/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/client.h>
#include <zephyr/9p/message.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_client, CONFIG_NINEP_LOG_LEVEL);

/* Helper: Allocate a tag (caller must hold client->lock) */
static struct ninep_pending_req *alloc_tag_locked(struct ninep_client *client, uint16_t *tag)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		if (!client->pending[i].in_use) {
			client->pending[i].in_use = true;
			client->pending[i].complete = false;
			client->pending[i].error = 0;
			*tag = client->next_tag++;
			client->pending[i].tag = *tag;
			return &client->pending[i];
		}
	}
	return NULL;
}

/* Helper: Find pending request by tag */
static struct ninep_pending_req *find_pending(struct ninep_client *client, uint16_t tag)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		if (client->pending[i].in_use && client->pending[i].tag == tag) {
			return &client->pending[i];
		}
	}
	return NULL;
}

/* Helper: Free a tag */
static void free_tag(struct ninep_client *client, uint16_t tag)
{
	struct ninep_pending_req *req = find_pending(client, tag);

	if (req) {
		req->in_use = false;
	}
}

/* Helper: Allocate a FID */
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

/* Helper: Free a FID */
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

/* Helper: Find FID */
static struct ninep_client_fid *find_fid(struct ninep_client *client, uint32_t fid)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (client->fids[i].in_use && client->fids[i].fid == fid) {
			return &client->fids[i];
		}
	}
	return NULL;
}

/* Transport callback: Process incoming responses */
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

	struct ninep_pending_req *req = find_pending(client, hdr.tag);

	if (!req) {
		LOG_WRN("No pending request for tag %u", hdr.tag);
		return;
	}

	/* Handle error response */
	if (hdr.type == NINEP_RERROR) {
		size_t offset = 7;
		const char *ename;
		uint16_t ename_len;

		if (ninep_parse_string(buf, len, &offset, &ename, &ename_len) == 0) {
			LOG_ERR("Error response: %.*s", ename_len, ename);
			req->error = -EIO;
		} else {
			req->error = -EIO;
		}
		req->complete = true;
		k_sem_give(&req->sem);
		return;
	}

	/* Copy response to per-request buffer */
	if (len <= CONFIG_NINEP_RESP_BUF_SIZE) {
		memcpy(req->resp_data, buf, len);
		req->resp_len = len;
		req->error = 0;
	} else {
		LOG_ERR("Response too large: %zu > %d", len, CONFIG_NINEP_RESP_BUF_SIZE);
		req->error = -ENOMEM;
	}

	req->complete = true;
	k_sem_give(&req->sem);
}

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

	/* Initialize mutex */
	k_mutex_init(&client->lock);

	/* Initialize pending request semaphores */
	for (int i = 0; i < CONFIG_NINEP_MAX_TAGS; i++) {
		k_sem_init(&client->pending[i].sem, 0, 1);
		client->pending[i].in_use = false;
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

	LOG_INF("9P client initialized");
	return 0;
}

int ninep_client_version(struct ninep_client *client)
{
	uint16_t tag;
	struct ninep_pending_req *req;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tversion */
	int len = ninep_build_tversion(client->tx_buf, sizeof(client->tx_buf),
	                                NINEP_NOTAG, client->config->max_message_size,
	                                client->config->version, strlen(client->config->version));
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Override tag to NOTAG for version */
	req->tag = NINEP_NOTAG;

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Rversion from per-request buffer to get negotiated msize */
	k_mutex_lock(&client->lock, K_FOREVER);
	if (req->resp_len >= 11) {
		client->msize = req->resp_data[7] | (req->resp_data[8] << 8) |
		                (req->resp_data[9] << 16) | (req->resp_data[10] << 24);
		LOG_INF("Negotiated msize: %u", client->msize);
	}

	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_attach(struct ninep_client *client, uint32_t *fid,
                        uint32_t afid, const char *uname, const char *aname)
{
	uint16_t tag;
	struct ninep_pending_req *req;
	uint32_t allocated_fid;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Allocate FID inline (already have lock) */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!client->fids[i].in_use) {
			client->fids[i].in_use = true;
			client->fids[i].fid = client->next_fid++;
			allocated_fid = client->fids[i].fid;
			*fid = allocated_fid;
			goto fid_allocated;
		}
	}
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return -ENOMEM;

fid_allocated:;

	/* Build Tattach */
	int len = ninep_build_tattach(client->tx_buf, sizeof(client->tx_buf),
	                               tag, allocated_fid, afid,
	                               uname, strlen(uname),
	                               aname, strlen(aname));
	if (len < 0) {
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Rattach from per-request buffer to get root qid */
	k_mutex_lock(&client->lock, K_FOREVER);
	struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
	if (cfid && req->resp_len >= 20) {
		size_t offset = 7;
		ninep_parse_qid(req->resp_data, req->resp_len, &offset, &cfid->qid);
	}

	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_walk(struct ninep_client *client, uint32_t fid,
                      uint32_t *newfid, const char *path)
{
	uint16_t tag;
	struct ninep_pending_req *req;
	uint32_t allocated_fid;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Allocate new FID (already have lock) */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!client->fids[i].in_use) {
			client->fids[i].in_use = true;
			client->fids[i].fid = client->next_fid++;
			allocated_fid = client->fids[i].fid;
			*newfid = allocated_fid;
			goto fid_allocated;
		}
	}
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return -ENOMEM;

fid_allocated:;

	/* Parse path into elements */
	const char *wnames[NINEP_MAX_WELEM];
	uint16_t wname_lens[NINEP_MAX_WELEM];
	uint16_t nwname = 0;

	const char *p = path;
	while (*p && nwname < NINEP_MAX_WELEM) {
		/* Skip leading slashes */
		while (*p == '/') {
			p++;
		}
		if (!*p) {
			break;
		}

		/* Find element */
		const char *start = p;
		while (*p && *p != '/') {
			p++;
		}

		wnames[nwname] = start;
		wname_lens[nwname] = p - start;
		nwname++;
	}

	/* Build Twalk */
	int len = ninep_build_twalk(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, allocated_fid, nwname, wnames, wname_lens);
	if (len < 0) {
		/* Free FID inline (already have lock) */
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
		if (cfid) cfid->in_use = false;
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Rwalk from per-request buffer to get final qid */
	k_mutex_lock(&client->lock, K_FOREVER);
	struct ninep_client_fid *cfid = find_fid(client, allocated_fid);
	if (cfid && req->resp_len >= 9) {
		uint16_t nwqid = req->resp_data[7] | (req->resp_data[8] << 8);
		if (nwqid > 0) {
			size_t offset = 9 + (nwqid - 1) * 13;
			ninep_parse_qid(req->resp_data, req->resp_len, &offset, &cfid->qid);
		}
	}

	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_open(struct ninep_client *client, uint32_t fid, uint8_t mode)
{
	uint16_t tag;
	struct ninep_pending_req *req;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Topen */
	int len = ninep_build_topen(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, mode);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Ropen from per-request buffer to get iounit */
	k_mutex_lock(&client->lock, K_FOREVER);
	struct ninep_client_fid *cfid = find_fid(client, fid);
	if (cfid && req->resp_len >= 24) {
		cfid->iounit = req->resp_data[20] | (req->resp_data[21] << 8) |
		               (req->resp_data[22] << 16) | (req->resp_data[23] << 24);
	}

	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return 0;
}

int ninep_client_read(struct ninep_client *client, uint32_t fid,
                      uint64_t offset, uint8_t *buf, uint32_t count)
{
	uint16_t tag;
	struct ninep_pending_req *req;
	int result;

	/* Lock only during TX (build + send) */
	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tread */
	int len = ninep_build_tread(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, offset, count);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock BEFORE waiting - allows concurrent requests! */
	k_mutex_unlock(&client->lock);

	/* Wait for response - other threads can send requests while we wait */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Rread from per-request buffer and copy data */
	if (req->resp_len >= 11) {
		uint32_t data_count = req->resp_data[7] | (req->resp_data[8] << 8) |
		                      (req->resp_data[9] << 16) | (req->resp_data[10] << 24);

		if (data_count > count) {
			data_count = count;
		}

		memcpy(buf, &req->resp_data[11], data_count);
		result = data_count;
	} else {
		result = -EIO;
	}

	k_mutex_lock(&client->lock, K_FOREVER);
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_write(struct ninep_client *client, uint32_t fid,
                       uint64_t offset, const uint8_t *buf, uint32_t count)
{
	uint16_t tag;
	struct ninep_pending_req *req;
	int result;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Twrite */
	int len = ninep_build_twrite(client->tx_buf, sizeof(client->tx_buf),
	                              tag, fid, offset, count, buf);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Rwrite from per-request buffer */
	if (req->resp_len >= 11) {
		result = req->resp_data[7] | (req->resp_data[8] << 8) |
		         (req->resp_data[9] << 16) | (req->resp_data[10] << 24);
	} else {
		result = -EIO;
	}

	k_mutex_lock(&client->lock, K_FOREVER);
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_stat(struct ninep_client *client, uint32_t fid,
                      struct ninep_stat *stat)
{
	uint16_t tag;
	struct ninep_pending_req *req;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tstat */
	int len = ninep_build_tstat(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	if (req->error) {
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return req->error;
	}

	/* Parse Rstat - this is simplified, full parsing needs stat structure parser */
	int result = (req->resp_len >= 9) ? 0 : -EIO;

	k_mutex_lock(&client->lock, K_FOREVER);
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_create(struct ninep_client *client, uint32_t fid,
                        const char *name, uint32_t perm, uint8_t mode)
{
	uint16_t tag;
	struct ninep_pending_req *req;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tcreate */
	int len = ninep_build_tcreate(client->tx_buf, sizeof(client->tx_buf),
	                               tag, fid, name, strlen(name), perm, mode);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	int result = req->error;
	k_mutex_lock(&client->lock, K_FOREVER);
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_remove(struct ninep_client *client, uint32_t fid)
{
	uint16_t tag;
	struct ninep_pending_req *req;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tremove */
	int len = ninep_build_tremove(client->tx_buf, sizeof(client->tx_buf),
	                               tag, fid);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	k_mutex_lock(&client->lock, K_FOREVER);
	if (req->error == 0) {
		/* Free FID inline (already have lock) */
		struct ninep_client_fid *cfid = find_fid(client, fid);
		if (cfid) cfid->in_use = false;
	}

	int result = req->error;
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}

int ninep_client_clunk(struct ninep_client *client, uint32_t fid)
{
	uint16_t tag;
	struct ninep_pending_req *req;

	k_mutex_lock(&client->lock, K_FOREVER);

	req = alloc_tag_locked(client, &tag);
	if (!req) {
		k_mutex_unlock(&client->lock);
		return -ENOMEM;
	}

	/* Build Tclunk */
	int len = ninep_build_tclunk(client->tx_buf, sizeof(client->tx_buf),
	                              tag, fid);
	if (len < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return len;
	}

	/* Send request */
	int ret = ninep_transport_send(client->transport, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return ret;
	}

	/* Release lock before waiting */
	k_mutex_unlock(&client->lock);

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout (tag=%u)", tag);
		k_mutex_lock(&client->lock, K_FOREVER);
		free_tag(client, tag);
		k_mutex_unlock(&client->lock);
		return -ETIMEDOUT;
	}

	k_mutex_lock(&client->lock, K_FOREVER);
	if (req->error == 0) {
		/* Free FID inline (already have lock) */
		struct ninep_client_fid *cfid = find_fid(client, fid);
		if (cfid) cfid->in_use = false;
	}

	int result = req->error;
	free_tag(client, tag);
	k_mutex_unlock(&client->lock);
	return result;
}
