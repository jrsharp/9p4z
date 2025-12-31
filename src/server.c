/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/server.h>
#include <zephyr/9p/message.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>  /* For strncasecmp */

#ifdef __ZEPHYR__
#include <zephyr/kernel.h>       /* For k_uptime_get */
#include <zephyr/random/random.h> /* For sys_rand_get */
#else
#include <time.h>  /* For clock_gettime on Unix */
#include <zephyr/kernel.h>  /* For k_uptime_get_32 shim */
#endif

LOG_MODULE_REGISTER(ninep_server, CONFIG_NINEP_LOG_LEVEL);

/* Forward declarations */
static uint64_t get_current_time_ms(void);

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
			server->fids[i].uname[0] = '\0';  /* Empty by default */
			server->fids[i].is_auth_fid = false;  /* Regular fid, not auth */
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

	LOG_INF("Tversion: msize=%u, version=%.*s", msize, version_len, version);

	/* Tversion flushes all server state - clear all fids */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		server->fids[i].in_use = false;
		server->fids[i].node = NULL;
	}
	LOG_DBG("All fids cleared for new session");

	/* Negotiate message size - use minimum of client, config, and transport MTU */
	if (msize > CONFIG_NINEP_MAX_MESSAGE_SIZE) {
		msize = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	}

	/* Further limit by transport MTU if available */
	int transport_mtu = ninep_transport_get_mtu(server->transport);
	if (transport_mtu > 0 && (uint32_t)transport_mtu < msize) {
		LOG_INF("Limiting msize to transport MTU: %u -> %d", msize, transport_mtu);
		msize = transport_mtu;
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
	uint32_t afid = msg[11] | (msg[12] << 8) | (msg[13] << 16) | (msg[14] << 24);

	LOG_DBG("Tattach: fid=%u, afid=%u", fid, afid);

	/* Validate pointers */
	if (!server) {
		LOG_ERR("server is NULL!");
		return;
	}
	LOG_DBG("server=%p", server);

	const struct ninep_fs_ops *ops = server->config.fs_ops;
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

	/* Parse uname from Tattach message
	 * Format: size[4] type[1] tag[2] fid[4] afid[4] uname[s] aname[s]
	 * uname starts at offset 15 */
	uint16_t uname_len = 0;
	const char *uname = "anonymous";  /* Default if parsing fails */
	if (len > 17) {  /* Need at least 2 bytes for length */
		uname_len = msg[15] | (msg[16] << 8);
		if (len >= 17 + uname_len && uname_len > 0) {
			uname = (const char *)&msg[17];
		}
	}

	/* Check authentication requirement */
	const struct ninep_auth_config *auth = server->config.auth_config;
	if (auth && auth->required) {
		/* afid == NOFID means no auth attempted */
		if (afid == NINEP_NOFID) {
			send_error(server, tag, "authentication required");
			return;
		}

		/* Find and verify auth fid */
		struct ninep_server_fid *auth_fid = find_fid(server, afid);
		if (!auth_fid || !auth_fid->is_auth_fid) {
			send_error(server, tag, "invalid auth fid");
			return;
		}

		if (!auth_fid->auth.authenticated) {
			send_error(server, tag, "authentication incomplete");
			return;
		}

		/* Verify uname matches authenticated identity */
		if (uname_len > 0 && strncmp(uname, auth_fid->auth.claimed_identity, uname_len) != 0) {
			LOG_WRN("Tattach uname mismatch: claimed='%.*s', auth='%s'",
			        uname_len, uname, auth_fid->auth.claimed_identity);
			send_error(server, tag, "uname does not match authenticated identity");
			return;
		}

		/* Use authenticated identity as uname */
		uname = auth_fid->auth.claimed_identity;
		uname_len = strlen(auth_fid->auth.claimed_identity);

		LOG_INF("Authenticated attach for identity '%s'", uname);
	}

	/* Allocate FID */
	struct ninep_server_fid *sfid = alloc_fid(server, fid);

	if (!sfid) {
		send_error(server, tag, "FID already in use");
		return;
	}

	/* Store uname (truncate if necessary) */
	size_t copy_len = uname_len < sizeof(sfid->uname) - 1 ?
	                  uname_len : sizeof(sfid->uname) - 1;
	memcpy(sfid->uname, uname, copy_len);
	sfid->uname[copy_len] = '\0';

	LOG_INF("Tattach: fid=%u, uname='%s'", fid, sfid->uname);

	/* Get root node */
	sfid->node = server->config.fs_ops->get_root(server->config.fs_ctx);
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

	LOG_INF("Twalk: fid=%u, newfid=%u, nwname=%u", fid, newfid, nwname);

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
		/* Copy uname from parent fid */
		strncpy(new_sfid->uname, sfid->uname, sizeof(new_sfid->uname) - 1);
		new_sfid->uname[sizeof(new_sfid->uname) - 1] = '\0';

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
		node = server->config.fs_ops->walk(node, name, name_len,
		                                     server->config.fs_ctx);
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
	/* Copy uname from parent fid */
	strncpy(new_sfid->uname, sfid->uname, sizeof(new_sfid->uname) - 1);
	new_sfid->uname[sizeof(new_sfid->uname) - 1] = '\0';

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

	LOG_INF("Topen: fid=%u, mode=0x%02x", fid, mode);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid || !sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Open node */
	int ret = server->config.fs_ops->open(sfid->node, mode,
	                                        server->config.fs_ctx);
	if (ret < 0) {
		send_error(server, tag, "open failed");
		return;
	}

	/* Set iounit to a reasonable value for I/O operations */
	sfid->iounit = CONFIG_NINEP_MAX_MESSAGE_SIZE - 24; /* msize minus Twrite header */

	/* Send Ropen */
	ret = ninep_build_ropen(server->tx_buf, sizeof(server->tx_buf),
	                         tag, &sfid->node->qid, sfid->iounit);
	if (ret > 0) {
		LOG_INF("Sending Ropen: tag=%u, qid.type=%u, qid.path=0x%llx, iounit=%u, size=%d",
		        tag, sfid->node->qid.type, sfid->node->qid.path, sfid->iounit, ret);
		int send_ret = ninep_transport_send(server->transport, server->tx_buf, ret);
		if (send_ret < 0) {
			LOG_ERR("Failed to send Ropen: %d", send_ret);
		}
	} else {
		LOG_ERR("Failed to build Ropen: %d", ret);
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

	if (!sfid) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Handle auth fid read (returns challenge) */
	if (sfid->is_auth_fid) {
		/* Check challenge expiry (60 seconds) */
		uint64_t now = get_current_time_ms();
		if (now - sfid->auth.challenge_time > 60000) {
			send_error(server, tag, "authentication timeout");
			return;
		}

		/* Return the challenge */
		int bytes = 0;
		if (offset < NINEP_AUTH_CHALLENGE_SIZE) {
			bytes = NINEP_AUTH_CHALLENGE_SIZE - offset;
			if ((uint32_t)bytes > count) {
				bytes = count;
			}
			memcpy(&server->tx_buf[11], &sfid->auth.challenge[offset], bytes);
		}

		sfid->auth.challenge_issued = true;
		LOG_DBG("Auth read: returning %d bytes of challenge", bytes);

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
		return;
	}

	if (!sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Limit count to available buffer space */
	uint32_t max_data = sizeof(server->tx_buf) - 11; /* Header + count field */

	if (count > max_data) {
		count = max_data;
	}

	/* Read data */
	int bytes = server->config.fs_ops->read(sfid->node, offset,
	                                          &server->tx_buf[11], count,
	                                          sfid->uname, server->config.fs_ctx);
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
	int stat_len = server->config.fs_ops->stat(sfid->node, stat_buf,
	                                             sizeof(stat_buf),
	                                             server->config.fs_ctx);
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

/* Generate random challenge for auth */
static void generate_challenge(uint8_t *challenge, size_t len)
{
#ifdef __ZEPHYR__
	/* Use Zephyr random generator */
	sys_rand_get(challenge, len);
#else
	/* Unix fallback: use timer-based pseudo-random (not cryptographically secure) */
	uint32_t seed = (uint32_t)k_uptime_get_32();
	for (size_t i = 0; i < len; i++) {
		seed = seed * 1103515245 + 12345;
		challenge[i] = (seed >> 16) & 0xFF;
	}
#endif
}

/* Get current time in milliseconds for challenge expiry */
static uint64_t get_current_time_ms(void)
{
#ifdef __ZEPHYR__
	return k_uptime_get();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Handle Tauth - authentication start */
static void handle_tauth(struct ninep_server *server, uint16_t tag,
                         const uint8_t *msg, size_t len)
{
	/* Parse Tauth: size[4] type[1] tag[2] afid[4] uname[s] aname[s] */
	uint32_t afid = msg[7] | (msg[8] << 8) | (msg[9] << 16) | (msg[10] << 24);

	/* Parse uname (identity string) */
	uint16_t uname_len = 0;
	const char *uname = "";
	if (len > 13) {
		uname_len = msg[11] | (msg[12] << 8);
		if (len >= 13 + uname_len) {
			uname = (const char *)&msg[13];
		}
	}

	LOG_DBG("Tauth: afid=%u, uname='%.*s' (len=%u)", afid, uname_len, uname, uname_len);

	/* Check if auth is configured */
	const struct ninep_auth_config *auth = server->config.auth_config;
	if (!auth) {
		/* No auth configured - return error (authentication not required) */
		send_error(server, tag, "authentication not required");
		return;
	}

	/* Validate identity length (format validation is app's responsibility) */
	if (uname_len == 0 || uname_len >= NINEP_AUTH_IDENTITY_MAX) {
		LOG_WRN("Invalid identity length: %u", uname_len);
		send_error(server, tag, "invalid identity");
		return;
	}

	/* Allocate auth FID */
	struct ninep_server_fid *sfid = alloc_fid(server, afid);
	if (!sfid) {
		send_error(server, tag, "cannot allocate afid");
		return;
	}

	/* Mark as auth fid and initialize auth state */
	sfid->is_auth_fid = true;
	sfid->node = NULL;  /* Auth fids don't point to filesystem nodes */
	memset(&sfid->auth, 0, sizeof(sfid->auth));

	/* Store claimed identity */
	memcpy(sfid->auth.claimed_identity, uname, uname_len);
	sfid->auth.claimed_identity[uname_len] = '\0';

	/* Generate challenge */
	generate_challenge(sfid->auth.challenge, NINEP_AUTH_CHALLENGE_SIZE);
	sfid->auth.challenge_time = get_current_time_ms();
	sfid->auth.challenge_issued = false;
	sfid->auth.authenticated = false;

	LOG_INF("Tauth: generated challenge for identity '%s'", sfid->auth.claimed_identity);

	/* Build Rauth response */
	struct ninep_qid aqid = {
		.type = NINEP_QTAUTH,
		.version = 0,
		.path = (uint64_t)afid  /* Use afid as unique path */
	};

	int ret = ninep_build_rauth(server->tx_buf, sizeof(server->tx_buf), tag, &aqid);
	if (ret > 0) {
		ninep_transport_send(server->transport, server->tx_buf, ret);
	}
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
	if (!server->config.fs_ops->create) {
		send_error(server, tag, "create not supported");
		return;
	}

	/* Create new file/directory */
	struct ninep_fs_node *new_node = NULL;
	int ret = server->config.fs_ops->create(
		sfid->node, name, name_len, perm, mode, sfid->uname, &new_node, server->config.fs_ctx);

	if (ret < 0 || !new_node) {
		send_error(server, tag, "create failed");
		return;
	}

	/* Update FID to point to new node */
	sfid->node = new_node;
	sfid->iounit = 0;

	/* Send Rcreate */
	ret = ninep_build_rcreate(server->tx_buf, sizeof(server->tx_buf),
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
	if (!sfid) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Handle auth fid write (receives signature + pubkey from client) */
	if (sfid->is_auth_fid) {
		/* Must have read challenge first */
		if (!sfid->auth.challenge_issued) {
			send_error(server, tag, "must read challenge first");
			return;
		}

		/* Check challenge expiry (60 seconds) */
		uint64_t now = get_current_time_ms();
		if (now - sfid->auth.challenge_time > 60000) {
			send_error(server, tag, "authentication timeout");
			return;
		}

		/* Auth response format is application-defined
		 * Common format: signature[64] + pubkey[32] = 96 bytes
		 * But we don't assume - app callback parses the data */
		if (count < 2) {  /* Minimum sanity check */
			LOG_WRN("Auth response too short: %u bytes", count);
			send_error(server, tag, "invalid auth response");
			return;
		}

		/* Call application verify callback - app does ALL verification */
		const struct ninep_auth_config *auth = server->config.auth_config;
		if (!auth || !auth->verify_auth) {
			LOG_ERR("No auth verify callback configured");
			send_error(server, tag, "auth not configured");
			return;
		}

		/* Common layout for Ed25519: signature[64] + pubkey[32]
		 * App is free to interpret differently, but we provide common sizes */
		const size_t sig_size = 64;
		const size_t pubkey_size = 32;
		if (count < sig_size + pubkey_size) {
			LOG_WRN("Auth response size %u too small for sig+pubkey", count);
			send_error(server, tag, "invalid auth response size");
			return;
		}

		const uint8_t *signature = data;
		const uint8_t *pubkey = data + sig_size;

		/* App callback does ALL verification:
		 * - Verify identity matches pubkey (e.g., CGA = SHA256(pubkey)[:20])
		 * - Verify signature over challenge
		 * - Check if identity is authorized */
		int ret = auth->verify_auth(
			sfid->auth.claimed_identity,
			pubkey, pubkey_size,
			signature, sig_size,
			sfid->auth.challenge, NINEP_AUTH_CHALLENGE_SIZE,
			auth->auth_ctx);

		if (ret != 0) {
			LOG_WRN("Auth verification failed for identity '%s'",
			        sfid->auth.claimed_identity);
			send_error(server, tag, "authentication failed");
			return;
		}

		LOG_INF("Auth successful for identity '%s'", sfid->auth.claimed_identity);

		/* Mark as authenticated */
		sfid->auth.authenticated = true;

		/* Store authenticated identity in uname */
		strncpy(sfid->uname, sfid->auth.claimed_identity, sizeof(sfid->uname) - 1);
		sfid->uname[sizeof(sfid->uname) - 1] = '\0';

		/* Send Rwrite with bytes written */
		ret = ninep_build_rwrite(server->tx_buf, sizeof(server->tx_buf),
		                         tag, count);
		if (ret > 0) {
			ninep_transport_send(server->transport, server->tx_buf, ret);
		}
		return;
	}

	if (!sfid->node) {
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Check if filesystem supports write */
	if (!server->config.fs_ops->write) {
		send_error(server, tag, "write not supported");
		return;
	}

	/* Write data */
	int bytes = server->config.fs_ops->write(sfid->node, offset, data, count,
	                                           sfid->uname, server->config.fs_ctx);
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
	if (!server->config.fs_ops->remove) {
		send_error(server, tag, "remove not supported");
		return;
	}

	/* Remove file/directory */
	int ret = server->config.fs_ops->remove(sfid->node, server->config.fs_ctx);
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

	LOG_INF("Tclunk: fid=%u tag=%u", fid, tag);

	struct ninep_server_fid *sfid = find_fid(server, fid);

	if (!sfid) {
		LOG_WRN("Tclunk: unknown fid %u", fid);
		send_error(server, tag, "unknown fid");
		return;
	}

	/* Call filesystem clunk handler if available */
	if (server->config.fs_ops->clunk && sfid->node) {
		server->config.fs_ops->clunk(sfid->node, server->config.fs_ctx);
	}

	/* Free FID */
	free_fid(server, fid);

	/* Send Rclunk */
	int ret = ninep_build_rclunk(server->tx_buf, sizeof(server->tx_buf), tag);
	if (ret > 0) {
		int send_ret = ninep_transport_send(server->transport, server->tx_buf, ret);
		LOG_INF("Rclunk sent: tag=%u, ret=%d", tag, send_ret);
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

	LOG_INF("Received 9P message: type=%u, tag=%u, size=%u", hdr.type, hdr.tag, hdr.size);

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
	if (!server || !config) {
		return -EINVAL;
	}

	memset(server, 0, sizeof(*server));
	/* Copy config by value instead of storing pointer */
	memcpy(&server->config, config, sizeof(server->config));
	server->transport = transport;

	/* Set transport callback (only for network servers) */
	if (transport) {
		transport->recv_cb = server_recv_callback;
		transport->user_data = server;
		LOG_INF("9P server initialized (network transport)");
	} else {
		LOG_INF("9P server initialized (in-process)");
	}

	return 0;
}

void ninep_server_cleanup(struct ninep_server *server)
{
	if (!server) {
		return;
	}

	LOG_INF("Cleaning up 9P server - clunking open fids");

	/* Clunk all open fids to properly release filesystem resources */
	for (int i = 0; i < CONFIG_NINEP_MAX_FIDS; i++) {
		struct ninep_server_fid *sfid = &server->fids[i];
		if (sfid->in_use && sfid->node) {
			LOG_DBG("Cleanup: clunking fid %u node '%s'", sfid->fid, sfid->node->name);

			/* Call filesystem clunk handler if available */
			if (server->config.fs_ops && server->config.fs_ops->clunk) {
				server->config.fs_ops->clunk(sfid->node, server->config.fs_ctx);
			}

			sfid->node = NULL;
			sfid->in_use = false;
		}
	}

	LOG_INF("9P server cleanup complete");
}

int ninep_server_start(struct ninep_server *server)
{
	if (!server) {
		return -EINVAL;
	}

	/* In-process servers don't have a transport to start */
	if (!server->transport) {
		LOG_DBG("In-process server - no transport to start");
		return 0;
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
	if (!server) {
		return -EINVAL;
	}

	/* In-process servers don't have a transport to stop */
	if (!server->transport) {
		LOG_DBG("In-process server - no transport to stop");
		return 0;
	}

	int ret = ninep_transport_stop(server->transport);

	if (ret < 0) {
		LOG_ERR("Failed to stop transport: %d", ret);
		return ret;
	}

	LOG_INF("9P server stopped");
	return 0;
}
