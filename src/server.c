/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/server.h>
#include <zephyr/9p/message.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_server, CONFIG_NINEP_LOG_LEVEL);

/* Helper to find FID */
static struct ninep_server_fid *find_fid(struct ninep_server *server, uint32_t fid)
{
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (server->fids[i].in_use && server->fids[i].fid == fid) {
			return &server->fids[i];
		}
	}
	return NULL;
}

/* Helper to allocate FID */
static struct ninep_server_fid *alloc_fid(struct ninep_server *server, uint32_t fid)
{
	/* Check if already exists */
	if (find_fid(server, fid)) {
		return NULL;
	}

	/* Find free slot */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		if (!server->fids[i].in_use) {
			server->fids[i].fid = fid;
			server->fids[i].in_use = true;
			server->fids[i].node = NULL;
			server->fids[i].iounit = 0;
			return &server->fids[i];
		}
	}
	return NULL;
}

/* Helper to free FID */
static void free_fid(struct ninep_server *server, uint32_t fid)
{
	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (sfid) {
		sfid->in_use = false;
		sfid->node = NULL;
	}
}

/* Send error response */
static void send_error(struct ninep_server *server, uint16_t tag, const char *error)
{
	int ret = ninep_build_rerror(server->tx_buf, sizeof(server->tx_buf),
	                               tag, error, strlen(error));
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Tversion */
static void handle_tversion(struct ninep_server *server, const uint8_t *msg, size_t len)
{
	uint32_t msize = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);
	uint16_t version_len = msg[11] | (msg[12] << 8);
	const char *version = (const char *)&msg[13];

	LOG_DBG("Tversion: msize=%u, version=%.*s", msize, version_len, version);

	/* Tversion flushes all server state - clear all fids */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		server->fids[i].in_use = false;
		server->fids[i].node = NULL;
	}
	LOG_DBG("All fids cleared for new session");

	/* Negotiate message size */
	if (msize > CONFIG_NINEP_MAX_MESSAGE_SIZE) {
		msize = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	}

	/* Check version */
	if (version_len != 6 || strncmp(version, "9P2000", 6) != 0) {
		send_error(server, NINEP_NOTAG, "unsupported version");
		return;
	}

	/* Send Rversion */
	int ret = ninep_build_rversion(server->tx_buf, sizeof(server->tx_buf),
	                                 NINEP_NOTAG, msize, "9P2000", 6);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Tattach */
static void handle_tattach(struct ninep_server *server, uint16_t tag,
                           const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);

	LOG_DBG("Tattach: fid=%u", fid);

	/* Validate pointers */
	if (!server) {
		LOG_ERR("server is NULL!");
		return;
	}
	LOG_DBG("server=%p", server);

	if (!server->config) {
		LOG_ERR("server->config is NULL!");
		send_error(server, tag, "server not configured");
		return;
	}
	LOG_DBG("config=%p", server->config);

	const struct ninep_fs_ops *ops = server->config->fs_ops;
	if (!ops) {
		LOG_ERR("fs_ops is NULL!");
		send_error(server, tag, "filesystem not configured");
		return;
	}
	LOG_DBG("fs_ops=%p", ops);

	if (!ops->get_root) {
		LOG_ERR("get_root is NULL!");
		send_error(server, tag, "get_root not implemented");
		return;
	}
	LOG_DBG("get_root=%p", ops->get_root);

	/* Allocate FID */
	struct ninep_server_fid *sfid = alloc_fid(server, fid);

	if (!sfid) {
		send_error(server, tag, "FID already in use");
		return;
	}

	/* Get root node */
	sfid->node = server->config->fs_ops->get_root(server->config->fs_ctx);
	if (!sfid->node) {
		free_fid(server, fid);
		send_error(server, tag, "cannot get root");
		return;
	}

	/* Send Rattach */
	int ret = ninep_build_rattach(server->tx_buf, sizeof(server->tx_buf),
	                                tag, &sfid->node->qid);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Twalk */
static void handle_twalk(struct ninep_server *server, uint16_t tag,
                         const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);
	uint32_t newfid = msg[11] | (msg[12] << 8) | (msg[13] << 16) | (msg[14] << 24);
	uint16_t nwname = msg[15] | (msg[16] << 8);

	LOG_DBG("Twalk: fid=%u, newfid=%u, nwname=%u", fid, newfid, nwname);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* If nwname is 0, clone the FID */
	if (nwname == 0) {
		struct ninep_server_fid *new_sfid = alloc_fid(server, newfid);

		if (!new_sfid) {
			send_error(server, tag, "cannot allocate newfid");
			return;
		}
		new_sfid->node = sfid->node;

		/* Send Rwalk with 0 qids */
		int ret = ninep_build_rwalk(server->tx_buf, sizeof(server->tx_buf),
		                             tag, 0, NULL);
		if (ret > 0) {
			ninep_transport_send(server->transport, server->tx_buf, ret);
		}
		return;
	}

	/* Walk path elements */
	struct ninep_fs_node *node = sfid->node;
	struct ninep_qid wqids[NINEP_MAX_WELEM];
	size_t offset = 17;

	for (int i = 0; i < nwname && i < NINEP_MAX_WELEM; i++) {
		uint16_t name_len = msg[offset] | (msg[offset + 1] << 8);
		const char *name = (const char *)&msg[offset + 2];

		offset += 2 + name_len;

		/* Walk to child */
		node = server->config->fs_ops->walk(node, name, name_len,
		                                     server->config->fs_ctx);
		if (!node) {
			send_error(server, tag, "file not found");
			return;
		}

		wqids[i] = node->qid;
	}

	/* Allocate new FID */
	struct ninep_server_fid *new_sfid = alloc_fid(server, newfid);

	if (!new_sfid) {
		send_error(server, tag, "cannot allocate newfid");
		return;
	}
	new_sfid->node = node;

	/* Send Rwalk */
	int ret = ninep_build_rwalk(server->tx_buf, sizeof(server->tx_buf),
	                             tag, nwname, wqids);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Topen */
static void handle_topen(struct ninep_server *server, uint16_t tag,
                         const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);
	uint8_t mode = msg[11];

	LOG_DBG("Topen: fid=%u, mode=%u", fid, mode);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Open node */
	int ret = server->config->fs_ops->open(sfid->node, mode,
	                                        server->config->fs_ctx);
	if (ret < 0) {
		send_error(server, tag, "open failed");
		return;
	}

	/* Set iounit (0 = use msize - header) */
	sfid->iounit = 0;

	/* Send Ropen */
	ret = ninep_build_ropen(server->tx_buf, sizeof(server->tx_buf),
	                         tag, &sfid->node->qid, sfid->iounit);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Tread */
static void handle_tread(struct ninep_server *server, uint16_t tag,
                         const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);
	uint64_t offset = msg[11] | ((uint64_t)msg[12] << 8) | ((uint64_t)msg[13] << 16) |
	                  ((uint64_t)msg[14] << 24) | ((uint64_t)msg[15] << 32) |
	                  ((uint64_t)msg[16] << 40) | ((uint64_t)msg[17] << 48) |
	                  ((uint64_t)msg[18] << 56);
	uint32_t count = msg[19] | (msg[20] << 8) | (msg[21] << 16) | (msg[22] << 24);

	LOG_DBG("Tread: fid=%u, offset=%llu, count=%u", fid, offset, count);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Limit count to available buffer space */
	uint32_t max_data = sizeof(server->tx_buf) - 11; /* Header + count field */

	if (count > max_data) {
		count = max_data;
	}

	/* Read data */
	int bytes = server->config->fs_ops->read(sfid->node, offset,
	                                          &server->tx_buf[11], count,
	                                          server->config->fs_ctx);
	if (bytes < 0) {
		send_error(server, tag, "read failed");
		return;
	}

	/* Build Rread header */
	uint32_t msg_size = 11 + bytes;

	server->tx_buf[0] = msg_size & 0xFF;
	server->tx_buf[1] = (msg_size >> 8) & 0xFF;
	server->tx_buf[2] = (msg_size >> 16) & 0xFF;
	server->tx_buf[3] = (msg_size >> 24) & 0xFF;
	server->tx_buf[4] = NINEP_RREAD;
	server->tx_buf[5] = tag & 0xFF;
	server->tx_buf[6] = (tag >> 8) & 0xFF;
	server->tx_buf[7] = bytes & 0xFF;
	server->tx_buf[8] = (bytes >> 8) & 0xFF;
	server->tx_buf[9] = (bytes >> 16) & 0xFF;
	server->tx_buf[10] = (bytes >> 24) & 0xFF;

	ninep_transport_send(server->transport, server->tx_buf, msg_size);
}

/* Handle Tstat */
static void handle_tstat(struct ninep_server *server, uint16_t tag,
                         const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);

	LOG_DBG("Tstat: fid=%u", fid);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Get stat from filesystem (use space after header + nstat field) */
	uint8_t stat_buf[256];
	int stat_len = server->config->fs_ops->stat(sfid->node, stat_buf,
	                                             sizeof(stat_buf),
	                                             server->config->fs_ctx);
	if (stat_len < 0) {
		send_error(server, tag, "stat failed");
		return;
	}

	/* Build Rstat */
	int ret = ninep_build_rstat(server->tx_buf, sizeof(server->tx_buf),
	                             tag, stat_buf, stat_len);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	} else {
		send_error(server, tag, "rstat build failed");
	}
}

/* Handle Tauth */
static void handle_tauth(struct ninep_server *server, uint16_t tag,
                         const uint8_t *msg, size_t len)
{
	LOG_DBG("Tauth: authentication not required");
	/* We don't support authentication - return error */
	send_error(server, tag, "authentication not required");
}

/* Handle Tflush */
static void handle_tflush(struct ninep_server *server, uint16_t tag,
                          const uint8_t *msg, size_t len)
{
	uint16_t oldtag = msg[7] | (msg[8] << 8);
	LOG_DBG("Tflush: oldtag=%u", oldtag);

	/* Simple implementation: just acknowledge the flush */
	/* A full implementation would cancel pending operations for oldtag */
	int ret = ninep_build_rflush(server->tx_buf, sizeof(server->tx_buf), tag);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Tcreate */
static void handle_tcreate(struct ninep_server *server, uint16_t tag,
                           const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);
	uint16_t name_len = msg[11] | (msg[12] << 8);
	const char *name = (const char *)&msg[13];
	uint32_t perm = msg[13 + name_len] | (msg[14 + name_len] << 8) |
	                (msg[15 + name_len] << 16) | (msg[16 + name_len] << 24);
	uint8_t mode = msg[17 + name_len];

	LOG_DBG("Tcreate: fid=%u, name=%.*s, perm=0x%x, mode=%u",
	        fid, name_len, name, perm, mode);

	struct ninep_server_fid *sfid = find_fid(server, fid);
	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Check if filesystem supports create */
	if (!server->config->fs_ops->create) {
		send_error(server, tag, "create not supported");
		return;
	}

	/* Create new file/directory */
	struct ninep_fs_node *new_node = server->config->fs_ops->create(
		sfid->node, name, name_len, perm, mode, server->config->fs_ctx);

	if (!new_node) {
		send_error(server, tag, "create failed");
		return;
	}

	/* Update FID to point to new node */
	sfid->node = new_node;
	sfid->iounit = 0;

	/* Send Rcreate */
	int ret = ninep_build_rcreate(server->tx_buf, sizeof(server->tx_buf),
	                               tag, &new_node->qid, sfid->iounit);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	} else {
		send_error(server, tag, "rcreate build failed");
	}
}

/* Handle Twrite */
static void handle_twrite(struct ninep_server *server, uint16_t tag,
                          const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);
	uint64_t offset = msg[11] | ((uint64_t)msg[12] << 8) | ((uint64_t)msg[13] << 16) |
	                  ((uint64_t)msg[14] << 24) | ((uint64_t)msg[15] << 32) |
	                  ((uint64_t)msg[16] << 40) | ((uint64_t)msg[17] << 48) |
	                  ((uint64_t)msg[18] << 56);
	uint32_t count = msg[19] | (msg[20] << 8) | (msg[21] << 16) | (msg[22] << 24);
	const uint8_t *data = &msg[23];

	LOG_DBG("Twrite: fid=%u, offset=%llu, count=%u", fid, offset, count);

	struct ninep_server_fid *sfid = find_fid(server, fid);
	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Check if filesystem supports write */
	if (!server->config->fs_ops->write) {
		send_error(server, tag, "write not supported");
		return;
	}

	/* Write data */
	int bytes = server->config->fs_ops->write(sfid->node, offset, data, count,
	                                           server->config->fs_ctx);
	if (bytes < 0) {
		send_error(server, tag, "write failed");
		return;
	}

	/* Send Rwrite */
	int ret = ninep_build_rwrite(server->tx_buf, sizeof(server->tx_buf),
	                              tag, bytes);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	} else {
		send_error(server, tag, "rwrite build failed");
	}
}

/* Handle Tremove */
static void handle_tremove(struct ninep_server *server, uint16_t tag,
                           const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);

	LOG_DBG("Tremove: fid=%u", fid);

	struct ninep_server_fid *sfid = find_fid(server, fid);
	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Check if filesystem supports remove */
	if (!server->config->fs_ops->remove) {
		send_error(server, tag, "remove not supported");
		return;
	}

	/* Remove file/directory */
	int ret = server->config->fs_ops->remove(sfid->node, server->config->fs_ctx);
	if (ret < 0) {
		send_error(server, tag, "remove failed");
		return;
	}

	/* Free FID */
	free_fid(server, fid);

	/* Send Rremove */
	ret = ninep_build_rremove(server->tx_buf, sizeof(server->tx_buf), tag);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Handle Twstat */
static void handle_twstat(struct ninep_server *server, uint16_t tag,
                          const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);

	LOG_DBG("Twstat: fid=%u", fid);

	struct ninep_server_fid *sfid = find_fid(server, fid);
	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Most embedded filesystems don't support metadata modification */
	send_error(server, tag, "wstat not supported");
}

/* Handle Tclunk */
static void handle_tclunk(struct ninep_server *server, uint16_t tag,
                          const uint8_t *msg, size_t len)
{
	uint32_t fid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);

	LOG_DBG("Tclunk: fid=%u", fid);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Free FID */
	free_fid(server, fid);

	/* Send Rclunk */
	int ret = ninep_build_rclunk(server->tx_buf, sizeof(server->tx_buf), tag);

	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
}

/* Message dispatcher */
void ninep_server_process_message(struct ninep_server *server,
                                   const uint8_t *msg, size_t len)
{
	if (len < 7) {
		LOG_ERR("Message too short");
		return;
	}

	struct ninep_msg_header hdr;

	if (ninep_parse_header(msg, len, &hdr) < 0) {
		LOG_ERR("Failed to parse header");
		return;
	}

	LOG_DBG("Received message: type=%u, tag=%u, size=%u", hdr.type, hdr.tag, hdr.size);

	switch (hdr.type) {
	case NINEP_TVERSION:
		handle_tversion(server, msg, len);
		break;
	case NINEP_TAUTH:
		handle_tauth(server, hdr.tag, msg, len);
		break;
	case NINEP_TATTACH:
		handle_tattach(server, hdr.tag, msg, len);
		break;
	case NINEP_TFLUSH:
		handle_tflush(server, hdr.tag, msg, len);
		break;
	case NINEP_TWALK:
		handle_twalk(server, hdr.tag, msg, len);
		break;
	case NINEP_TOPEN:
		handle_topen(server, hdr.tag, msg, len);
		break;
	case NINEP_TCREATE:
		handle_tcreate(server, hdr.tag, msg, len);
		break;
	case NINEP_TREAD:
		handle_tread(server, hdr.tag, msg, len);
		break;
	case NINEP_TWRITE:
		handle_twrite(server, hdr.tag, msg, len);
		break;
	case NINEP_TCLUNK:
		handle_tclunk(server, hdr.tag, msg, len);
		break;
	case NINEP_TREMOVE:
		handle_tremove(server, hdr.tag, msg, len);
		break;
	case NINEP_TSTAT:
		handle_tstat(server, hdr.tag, msg, len);
		break;
	case NINEP_TWSTAT:
		handle_twstat(server, hdr.tag, msg, len);
		break;
	default:
		LOG_WRN("Unhandled message type: %u", hdr.type);
		send_error(server, hdr.tag, "operation not supported");
		break;
	}
}

/* Transport callback */
static void server_recv_callback(struct ninep_transport *transport,
                                 const uint8_t *buf, size_t len, void *user_data)
{
	struct ninep_server *server = user_data;

	ninep_server_process_message(server, buf, len);
}

int ninep_server_init(struct ninep_server *server,
                      const struct ninep_server_config *config,
                      struct ninep_transport *transport)
{
	if (!server || !config || !transport) {
		return -EINVAL;
	}

	memset(server, 0, sizeof(*server));
	server->config = config;
	server->transport = transport;

	/* Set transport callback */
	transport->recv_cb = server_recv_callback;
	transport->user_data = server;

	LOG_INF("9P server initialized");
	return 0;
}

int ninep_server_start(struct ninep_server *server)
{
	if (!server || !server->transport) {
		return -EINVAL;
	}

	int ret = ninep_transport_start(server->transport);

	if (ret < 0) {
		LOG_ERR("Failed to start transport: %d", ret);
		return ret;
	}

	LOG_INF("9P server started");
	return 0;
}

int ninep_server_stop(struct ninep_server *server)
{
	if (!server || !server->transport) {
		return -EINVAL;
	}

	int ret = ninep_transport_stop(server->transport);

	if (ret < 0) {
		LOG_ERR("Failed to stop transport: %d", ret);
		return ret;
	}

	LOG_INF("9P server stopped");
	return 0;
}
