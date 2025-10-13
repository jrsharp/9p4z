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

/* Helper: Allocate a tag */
static struct ninep_pending_req *alloc_tag(struct ninep_client *client, uint16_t *tag)
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

	/* Copy response to pending request buffer */
	if (req->resp_buf && len <= req->resp_max) {
		memcpy(req->resp_buf, buf, len);
		req->resp_len = len;
		req->error = 0;
	} else {
		req->error = -ENOMEM;
	}

	req->complete = true;
	k_sem_give(&req->sem);
}

/* Helper: Send request and wait for response */
static int send_and_wait(struct ninep_client *client, struct ninep_pending_req *req,
                        const uint8_t *msg, size_t len)
{
	int ret;

	/* Send request */
	ret = ninep_transport_send(client->transport, msg, len);
	if (ret < 0) {
		return ret;
	}

	/* Wait for response */
	ret = k_sem_take(&req->sem, K_MSEC(client->config->timeout_ms));
	if (ret < 0) {
		LOG_ERR("Request timeout");
		return -ETIMEDOUT;
	}

	return req->error;
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
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Tversion */
	int len = ninep_build_tversion(client->tx_buf, sizeof(client->tx_buf),
	                                NINEP_NOTAG, client->config->max_message_size,
	                                client->config->version, strlen(client->config->version));
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Override tag to NOTAG for version */
	req->tag = NINEP_NOTAG;

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

	/* Parse Rversion to get negotiated msize */
	if (req->resp_len >= 11) {
		client->msize = client->rx_buf[7] | (client->rx_buf[8] << 8) |
		                (client->rx_buf[9] << 16) | (client->rx_buf[10] << 24);
		LOG_INF("Negotiated msize: %u", client->msize);
	}

	free_tag(client, tag);
	return 0;
}

int ninep_client_attach(struct ninep_client *client, uint32_t *fid,
                        uint32_t afid, const char *uname, const char *aname)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Allocate FID */
	int ret = ninep_client_alloc_fid(client, fid);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

	/* Build Tattach */
	int len = ninep_build_tattach(client->tx_buf, sizeof(client->tx_buf),
	                               tag, *fid, afid,
	                               uname, strlen(uname),
	                               aname, strlen(aname));
	if (len < 0) {
		ninep_client_free_fid(client, *fid);
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		ninep_client_free_fid(client, *fid);
		free_tag(client, tag);
		return ret;
	}

	/* Parse Rattach to get root qid */
	struct ninep_client_fid *cfid = find_fid(client, *fid);
	if (cfid && req->resp_len >= 20) {
		size_t offset = 7;
		ninep_parse_qid(client->rx_buf, req->resp_len, &offset, &cfid->qid);
	}

	free_tag(client, tag);
	return 0;
}

int ninep_client_walk(struct ninep_client *client, uint32_t fid,
                      uint32_t *newfid, const char *path)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Allocate new FID */
	int ret = ninep_client_alloc_fid(client, newfid);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

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
	                             tag, fid, *newfid, nwname, wnames, wname_lens);
	if (len < 0) {
		ninep_client_free_fid(client, *newfid);
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		ninep_client_free_fid(client, *newfid);
		free_tag(client, tag);
		return ret;
	}

	/* Parse Rwalk to get final qid */
	struct ninep_client_fid *cfid = find_fid(client, *newfid);
	if (cfid && req->resp_len >= 9) {
		uint16_t nwqid = client->rx_buf[7] | (client->rx_buf[8] << 8);
		if (nwqid > 0) {
			size_t offset = 9 + (nwqid - 1) * 13;
			ninep_parse_qid(client->rx_buf, req->resp_len, &offset, &cfid->qid);
		}
	}

	free_tag(client, tag);
	return 0;
}

int ninep_client_open(struct ninep_client *client, uint32_t fid, uint8_t mode)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Topen */
	int len = ninep_build_topen(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, mode);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

	/* Parse Ropen to get iounit */
	struct ninep_client_fid *cfid = find_fid(client, fid);
	if (cfid && req->resp_len >= 24) {
		cfid->iounit = client->rx_buf[20] | (client->rx_buf[21] << 8) |
		               (client->rx_buf[22] << 16) | (client->rx_buf[23] << 24);
	}

	free_tag(client, tag);
	return 0;
}

int ninep_client_read(struct ninep_client *client, uint32_t fid,
                      uint64_t offset, uint8_t *buf, uint32_t count)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Tread */
	int len = ninep_build_tread(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid, offset, count);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

	/* Parse Rread and copy data */
	if (req->resp_len >= 11) {
		uint32_t data_count = client->rx_buf[7] | (client->rx_buf[8] << 8) |
		                      (client->rx_buf[9] << 16) | (client->rx_buf[10] << 24);

		if (data_count > count) {
			data_count = count;
		}

		memcpy(buf, &client->rx_buf[11], data_count);
		free_tag(client, tag);
		return data_count;
	}

	free_tag(client, tag);
	return -EIO;
}

int ninep_client_write(struct ninep_client *client, uint32_t fid,
                       uint64_t offset, const uint8_t *buf, uint32_t count)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Twrite */
	int len = ninep_build_twrite(client->tx_buf, sizeof(client->tx_buf),
	                              tag, fid, offset, count, buf);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

	/* Parse Rwrite to get bytes written */
	if (req->resp_len >= 11) {
		uint32_t written = client->rx_buf[7] | (client->rx_buf[8] << 8) |
		                   (client->rx_buf[9] << 16) | (client->rx_buf[10] << 24);
		free_tag(client, tag);
		return written;
	}

	free_tag(client, tag);
	return -EIO;
}

int ninep_client_stat(struct ninep_client *client, uint32_t fid,
                      struct ninep_stat *stat)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Tstat */
	int len = ninep_build_tstat(client->tx_buf, sizeof(client->tx_buf),
	                             tag, fid);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret < 0) {
		free_tag(client, tag);
		return ret;
	}

	/* Parse Rstat - this is simplified, full parsing needs stat structure parser */
	if (req->resp_len >= 9) {
		/* For now, just indicate success - full stat parsing TBD */
		free_tag(client, tag);
		return 0;
	}

	free_tag(client, tag);
	return -EIO;
}

int ninep_client_create(struct ninep_client *client, uint32_t fid,
                        const char *name, uint32_t perm, uint8_t mode)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Tcreate */
	int len = ninep_build_tcreate(client->tx_buf, sizeof(client->tx_buf),
	                               tag, fid, name, strlen(name), perm, mode);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	free_tag(client, tag);
	return ret;
}

int ninep_client_remove(struct ninep_client *client, uint32_t fid)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Tremove */
	int len = ninep_build_tremove(client->tx_buf, sizeof(client->tx_buf),
	                               tag, fid);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret == 0) {
		ninep_client_free_fid(client, fid);
	}
	free_tag(client, tag);
	return ret;
}

int ninep_client_clunk(struct ninep_client *client, uint32_t fid)
{
	uint16_t tag;
	struct ninep_pending_req *req = alloc_tag(client, &tag);

	if (!req) {
		return -ENOMEM;
	}

	req->resp_buf = client->rx_buf;
	req->resp_max = sizeof(client->rx_buf);

	/* Build Tclunk */
	int len = ninep_build_tclunk(client->tx_buf, sizeof(client->tx_buf),
	                              tag, fid);
	if (len < 0) {
		free_tag(client, tag);
		return len;
	}

	/* Send and wait */
	int ret = send_and_wait(client, req, client->tx_buf, len);
	if (ret == 0) {
		ninep_client_free_fid(client, fid);
	}
	free_tag(client, tag);
	return ret;
}
