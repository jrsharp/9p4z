/*
 * 9P Client Implementation
 *
 * Memory-efficient design using single shared response buffer and condvar.
 * Supports 64+ concurrent tags with minimal memory overhead.
 *
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/client.h>
#include <zephyr/9p/message.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_client, CONFIG_NINEP_LOG_LEVEL);

/*
 * Tag management - lightweight, no per-tag buffers
 */

/* Allocate a tag (caller must hold client->lock) */
static struct ninep_tag_entry *alloc_tag_locked(struct ninep_client *client, uint16_t *tag)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		if (!client->tags[i].in_use) {
			client->tags[i].in_use = true;
			client->tags[i].complete = false;
			client->tags[i].error = 0;
			client->tags[i].user_ctx = NULL;
			*tag = client->next_tag++;
			client->tags[i].tag = *tag;
			return &client->tags[i];
		}
	}
	return NULL;
}

/* Find tag entry by tag number (caller must hold lock) */
static struct ninep_tag_entry *find_tag_locked(struct ninep_client *client, uint16_t tag)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		if (client->tags[i].in_use && client->tags[i].tag == tag) {
			return &client->tags[i];
		}
	}
	return NULL;
}

/* Free a tag (caller must hold lock) */
static void free_tag_locked(struct ninep_client *client, uint16_t tag)
{
	struct ninep_tag_entry *entry = find_tag_locked(client, tag);
	if (entry) {
		entry->in_use = false;
	}
}

/*
 * FID management
 */

int ninep_client_alloc_fid(struct ninep_client *client, uint32_t *fid)
{
	k_mutex_lock(&client->lock, K_FOREVER);

	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!client->fids[i].in_use) {
			client->fids[i].in_use = true;
			client->fids[i].fid = client->next_fid++;
			*fid = client->fids[i].fid;
			k_mutex_unlock(&client->lock);
			return 0;
		}
	}

	k_mutex_unlock(&client->lock);
	return -ENOMEM;
}

void ninep_client_free_fid(struct ninep_client *client, uint32_t fid)
{
	k_mutex_lock(&client->lock, K_FOREVER);

	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (client->fids[i].in_use && client->fids[i].fid == fid) {
			client->fids[i].in_use = false;
			break;
		}
	}

	k_mutex_unlock(&client->lock);
}

static struct ninep_client_fid *find_fid_locked(struct ninep_client *client, uint32_t fid)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (client->fids[i].in_use && client->fids[i].fid == fid) {
			return &client->fids[i];
		}
	}
	return NULL;
}

/*
 * Response handling - single shared buffer, broadcast to all waiters
 */

static void client_recv_callback(struct ninep_transport *transport,
                                 const uint8_t *buf, size_t len, void *user_data)
{
	struct ninep_client *client = user_data;
	struct ninep_msg_header hdr;

	if (len < 7 || ninep_parse_header(buf, len, &hdr) < 0) {
		LOG_ERR("Invalid response message");
		return;
	}

	LOG_DBG("Received response: type=%u, tag=%u, size=%u", hdr.type, hdr.tag, hdr.size);

	k_mutex_lock(&client->lock, K_FOREVER);

	struct ninep_tag_entry *entry = find_tag_locked(client, hdr.tag);
	if (!entry) {
		LOG_WRN("No pending request for tag %u", hdr.tag);
		k_mutex_unlock(&client->lock);
		return;
	}

	/* Copy response to shared buffer */
	if (len <= sizeof(client->resp_buf)) {
		memcpy(client->resp_buf, buf, len);
		client->resp_len = len;
	} else {
		LOG_ERR("Response too large: %zu > %zu", len, sizeof(client->resp_buf));
		entry->error = -ENOMEM;
		entry->complete = true;
		k_condvar_broadcast(&client->resp_cv);
		k_mutex_unlock(&client->lock);
		return;
	}

	/* Handle error response */
	if (hdr.type == NINEP_RERROR) {
		size_t offset = 7;
		const char *ename;
		uint16_t ename_len;

		if (ninep_parse_string(buf, len, &offset, &ename, &ename_len) == 0) {
			LOG_ERR("9P error: %.*s", ename_len, ename);
		}
		entry->error = -EIO;
	} else {
		entry->error = 0;
	}

	entry->complete = true;

	/* Wake ALL waiters - they check if their tag completed */
	k_condvar_broadcast(&client->resp_cv);

	k_mutex_unlock(&client->lock);
}

/*
 * Wait for a specific tag's response with timeout
 * Caller must hold lock on entry, lock is held on return
 */
static int wait_for_tag(struct ninep_client *client, struct ninep_tag_entry *entry,
                        uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (!entry->complete) {
		int64_t remaining = deadline - k_uptime_get();
		if (remaining <= 0) {
			return -ETIMEDOUT;
		}

		int ret = k_condvar_wait(&client->resp_cv, &client->lock,
		                         K_MSEC(remaining));
		if (ret == -EAGAIN) {
			/* Timeout */
			return -ETIMEDOUT;
		}
		/* Spurious wakeup or another tag completed - loop and check */
	}

	return entry->error;
}

/*
 * Client initialization
 */

int ninep_client_init(struct ninep_client *client,
                      const struct ninep_client_config *config,
                      struct ninep_transport *transport)
{
	if (!client || !config || !transport) {
		return -EINVAL;
	}

	memset(client, 0, sizeof(*client));
	client->config = config;
	client->transport = transport;
	client->msize = config->max_message_size;
	client->next_fid = 0;
	client->next_tag = 0;

	/* Initialize synchronization primitives */
	k_mutex_init(&client->lock);
	k_condvar_init(&client->resp_cv);

	/* Initialize tag entries (all start free) */
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		client->tags[i].in_use = false;
	}

	/* Set transport callback */
	transport->recv_cb = client_recv_callback;
	transport->user_data = client;

	/* Start transport */
	int ret = ninep_transport_start(transport);
	if (ret < 0) {
		LOG_ERR("Failed to start transport: %d", ret);
		return ret;
	}

	LOG_INF("9P client initialized (max %d tags, %d fids)",
	        CONFIG_NINEP_MAX_TAGS, CONFIG_NINEP_MAX_FIDS);
	return 0;
}

/*
 * 9P Protocol Operations
 */

int ninep_client_version(struct ninep_client *client)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tversion */
	int len = ninep_build_tversion(client->tx_buf, sizeof(client->tx_buf),
	                                NINEP_NOTAG, client->config->max_message_size,
	                                client->config->version,
	                                strlen(client->config->version));
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Override tag to NOTAG for version */
	entry->tag = NINEP_NOTAG;

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response (lock held) */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Version request failed: %d", ret);
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Rversion to get negotiated msize */
	if (client->resp_len >= 11) {
		client->msize = client->resp_buf[7] | (client->resp_buf[8] << 8) |
		                (client->resp_buf[9] << 16) | (client->resp_buf[10] << 24);
		LOG_INF("Negotiated msize: %u", client->msize);
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_attach(struct ninep_client *client, uint32_t *fid,
                        uint32_t afid, const char *uname, const char *aname)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;
	uint32_t allocated_fid;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Allocate FID inline */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!client->fids[i].in_use) {
			client->fids[i].in_use = true;
			client->fids[i].fid = client->next_fid++;
			allocated_fid = client->fids[i].fid;
			*fid = allocated_fid;
			goto fid_allocated;
		}
	}
	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return -ENOMEM;

fid_allocated:;

	/* Build Tattach */
	int len = ninep_build_tattach(client->tx_buf, sizeof(client->tx_buf),
	                               tag, allocated_fid, afid,
	                               uname, strlen(uname),
	                               aname, strlen(aname));
	if (len < 0) {
		struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Attach request failed: %d", ret);
		struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Rattach to get root qid */
	struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
	if (cfid && client->resp_len >= 20) {
		size_t offset = 7;
		ninep_parse_qid(client->resp_buf, client->resp_len, &offset, &cfid->qid);
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_walk(struct ninep_client *client, uint32_t fid,
                      uint32_t *newfid, const char *path)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;
	uint32_t allocated_fid;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Allocate new FID */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!client->fids[i].in_use) {
			client->fids[i].in_use = true;
			client->fids[i].fid = client->next_fid++;
			allocated_fid = client->fids[i].fid;
			*newfid = allocated_fid;
			goto fid_allocated;
		}
	}
	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return -ENOMEM;

fid_allocated:;

	/* Parse path into elements */
	const char *wnames[NINEP_MAX_WELEM];
	uint16_t wname_lens[NINEP_MAX_WELEM];
	uint16_t nwname = 0;

	const char *p = path;
	while (*p && nwname < NINEP_MAX_WELEM) {
		while (*p == '/') p++;
		if (!*p) break;

		const char *start = p;
		while (*p && *p != '/') p++;

		wnames[nwname] = start;
		wname_lens[nwname] = p - start;
		nwname++;
	}

	/* Build Twalk */
	int len = ninep_build_twalk(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, allocated_fid, nwname, wnames, wname_lens);
	if (len < 0) {
		struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Walk request failed: %d", ret);
		struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Rwalk to get final qid */
	struct ninep_client_fid *cfid = find_fid_locked(client, allocated_fid);
	if (cfid && client->resp_len >= 9) {
		uint16_t nwqid = client->resp_buf[7] | (client->resp_buf[8] << 8);
		if (nwqid > 0) {
			size_t offset = 9 + (nwqid - 1) * 13;
			ninep_parse_qid(client->resp_buf, client->resp_len, &offset, &cfid->qid);
		}
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_open(struct ninep_client *client, uint32_t fid, uint8_t mode)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Topen */
	int len = ninep_build_topen(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, mode);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Open request failed: %d", ret);
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Ropen to get iounit */
	struct ninep_client_fid *cfid = find_fid_locked(client, fid);
	if (cfid && client->resp_len >= 24) {
		cfid->iounit = client->resp_buf[20] | (client->resp_buf[21] << 8) |
		               (client->resp_buf[22] << 16) | (client->resp_buf[23] << 24);
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_read(struct ninep_client *client, uint32_t fid,
                      uint64_t offset, uint8_t *buf, uint32_t count)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;
	int result;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tread */
	int len = ninep_build_tread(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, offset, count);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Read request failed: %d", ret);
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Rread and copy data to caller's buffer */
	if (client->resp_len >= 11) {
		uint32_t data_count = client->resp_buf[7] | (client->resp_buf[8] << 8) |
		                      (client->resp_buf[9] << 16) | (client->resp_buf[10] << 24);

		if (data_count > count) {
			data_count = count;
		}

		memcpy(buf, &client->resp_buf[11], data_count);
		result = data_count;
	} else {
		result = -EIO;
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_write(struct ninep_client *client, uint32_t fid,
                       uint64_t offset, const uint8_t *buf, uint32_t count)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;
	int result;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Twrite */
	int len = ninep_build_twrite(client->tx_buf, sizeof(client->tx_buf),
	                              tag, fid, offset, count, buf);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Write request failed: %d", ret);
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Rwrite */
	if (client->resp_len >= 11) {
		result = client->resp_buf[7] | (client->resp_buf[8] << 8) |
		         (client->resp_buf[9] << 16) | (client->resp_buf[10] << 24);
	} else {
		result = -EIO;
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_stat(struct ninep_client *client, uint32_t fid,
                      struct ninep_stat *stat)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tstat */
	int len = ninep_build_tstat(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret < 0) {
		LOG_ERR("Stat request failed: %d", ret);
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Parse Rstat - simplified for now */
	int result = (client->resp_len >= 9) ? 0 : -EIO;

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_create(struct ninep_client *client, uint32_t fid,
                        const char *name, uint32_t perm, uint8_t mode)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tcreate */
	int len = ninep_build_tcreate(client->tx_buf, sizeof(client->tx_buf),
	                               tag, fid, name, strlen(name), perm, mode);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return ret;
}

int ninep_client_remove(struct ninep_client *client, uint32_t fid)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tremove */
	int len = ninep_build_tremove(client->tx_buf, sizeof(client->tx_buf),
	                               tag, fid);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret == 0) {
		/* Free FID on success */
		struct ninep_client_fid *cfid = find_fid_locked(client, fid);
		if (cfid) cfid->in_use = false;
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return ret;
}

int ninep_client_clunk(struct ninep_client *client, uint32_t fid)
{
	uint16_t tag;
	struct ninep_tag_entry *entry;

	k_mutex_lock(&client->lock, K_FOREVER);

	entry = alloc_tag_locked(client, &tag);
	if (!entry) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tclunk */
	int len = ninep_build_tclunk(client->tx_buf, sizeof(client->tx_buf),
	                              tag, fid);
	if (len < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag_locked(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Wait for response */
	ret = wait_for_tag(client, entry, client->config->timeout_ms);
	if (ret == 0) {
		/* Free FID on success */
		struct ninep_client_fid *cfid = find_fid_locked(client, fid);
		if (cfid) cfid->in_use = false;
	}

	free_tag_locked(client, tag);
	k_mutex_unlock(&client->lock);
	return ret;
}
