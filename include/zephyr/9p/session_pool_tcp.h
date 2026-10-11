/*
 * Copyright (c) 2026 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_SESSION_POOL_TCP_H_
#define ZEPHYR_INCLUDE_9P_SESSION_POOL_TCP_H_

#include <zephyr/9p/session_pool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Multi-client 9P over TCP
 *
 * The TCP counterpart of the L2CAP session pool. One listening socket and
 * an accept thread; every accepted connection gets a pooled session — its
 * own transport, its own 9P server (so fids and tags never collide between
 * clients) and its own receive thread — over one shared filesystem.
 *
 * transport_tcp.c serves exactly one client: a second connection completes
 * its TCP handshake in the listen backlog and then hears nothing until the
 * first leaves, which a client sees as a hang. Here a second client is
 * served, and one past the pool is refused at once (the socket is closed),
 * so it fails fast instead.
 *
 * One pool per image (its thread stacks are static, sized by
 * CONFIG_NINEP_TCP_MAX_SESSIONS).
 */
struct ninep_session_pool_tcp_config {
	uint16_t port;                      /* TCP port (0 = 564) */
	int max_sessions;                   /* <= CONFIG_NINEP_TCP_MAX_SESSIONS */
	size_t rx_buf_size_per_session;    /* largest message accepted */
	struct ninep_fs_ops *fs_ops;       /* filesystem (shared) */
	void *fs_context;                   /* filesystem context (shared) */
	const struct ninep_auth_config *auth_config;  /* optional (shared) */
	const char *version;                /* NULL = "9P2000" */
};

struct ninep_session_pool_tcp;

/**
 * @brief Set up the pool. Allocates per-session receive buffers.
 * @return the pool, or NULL (bad config, out of memory, already created)
 */
struct ninep_session_pool_tcp *ninep_session_pool_tcp_create(
	const struct ninep_session_pool_tcp_config *config);

/** @brief Bind, listen and start accepting. */
int ninep_session_pool_tcp_start(struct ninep_session_pool_tcp *pool);

/** @brief Stop accepting and drop every client. */
void ninep_session_pool_tcp_stop(struct ninep_session_pool_tcp *pool);

/** @brief Clients connected right now. */
int ninep_session_pool_tcp_clients(struct ninep_session_pool_tcp *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_SESSION_POOL_TCP_H_ */
