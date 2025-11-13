/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_SESSION_POOL_H_
#define ZEPHYR_INCLUDE_9P_SESSION_POOL_H_

#include <zephyr/9p/transport.h>
#include <zephyr/9p/server.h>
#include <zephyr/kernel.h>

/**
 * @brief Generic session pool for multi-client 9P servers
 *
 * Transport-agnostic session management. Each session represents an
 * independent 9P connection with its own transport and server instance,
 * preventing fid namespace collisions between clients.
 *
 * Usage:
 * 1. Transport-specific code creates session pool
 * 2. On incoming connection: allocate session, wire up transport
 * 3. On disconnect: free session
 */

/**
 * @brief Session state
 */
enum ninep_session_state {
	NINEP_SESSION_FREE = 0,      /* Session available for allocation */
	NINEP_SESSION_ALLOCATED,     /* Session allocated but not connected */
	NINEP_SESSION_CONNECTED,     /* Session active with client connected */
	NINEP_SESSION_DISCONNECTING, /* Session being torn down */
};

/**
 * @brief Single 9P session
 *
 * Represents one client connection with independent transport + server.
 */
struct ninep_session {
	struct ninep_transport transport;   /* Transport for this session */
	struct ninep_server server;         /* 9P server for this session */
	enum ninep_session_state state;     /* Current session state */
	void *transport_priv;               /* Transport-specific private data */
	int session_id;                     /* Session index in pool */
};

/**
 * @brief Session pool
 *
 * Manages a pool of independent 9P sessions.
 * Size is determined at initialization time.
 */
struct ninep_session_pool {
	int max_sessions;                   /* Maximum concurrent sessions */
	struct k_mutex lock;                /* Protects session allocation */
	struct ninep_fs_ops *fs_ops;       /* Shared filesystem operations */
	void *fs_context;                   /* Shared filesystem context */
	struct ninep_session sessions[];    /* Variable-length array of sessions */
};

/**
 * @brief Session pool configuration
 */
struct ninep_session_pool_config {
	int max_sessions;                   /* Maximum concurrent sessions */
	struct ninep_fs_ops *fs_ops;       /* Filesystem operations (shared) */
	void *fs_context;                   /* Filesystem context (shared) */
};

/**
 * @brief Calculate required size for session pool
 *
 * @param max_sessions Maximum concurrent sessions
 * @return Size in bytes needed for the pool structure
 */
static inline size_t ninep_session_pool_size(int max_sessions)
{
	return sizeof(struct ninep_session_pool) +
	       (max_sessions * sizeof(struct ninep_session));
}

/**
 * @brief Initialize session pool
 *
 * @param pool Session pool memory (caller-allocated using ninep_session_pool_size())
 * @param config Pool configuration
 * @return 0 on success, negative errno on failure
 */
int ninep_session_pool_init(struct ninep_session_pool *pool,
                              const struct ninep_session_pool_config *config);

/**
 * @brief Allocate a free session from the pool
 *
 * Finds and marks a session as allocated. Caller must initialize
 * the transport and complete setup before marking as connected.
 *
 * @param pool Session pool
 * @return Pointer to allocated session, or NULL if pool is full
 */
struct ninep_session *ninep_session_alloc(struct ninep_session_pool *pool);

/**
 * @brief Mark session as connected
 *
 * Called by transport-specific code after transport is initialized
 * and ready to handle requests.
 *
 * @param session Session to mark as connected
 */
void ninep_session_connected(struct ninep_session *session);

/**
 * @brief Free a session back to the pool
 *
 * Tears down the 9P server and transport, then marks session as free.
 *
 * @param session Session to free
 */
void ninep_session_free(struct ninep_session *session);

/**
 * @brief Get session by ID
 *
 * @param pool Session pool
 * @param session_id Session index
 * @return Pointer to session, or NULL if invalid ID
 */
struct ninep_session *ninep_session_get(struct ninep_session_pool *pool, int session_id);

/**
 * @brief Disconnect all active sessions
 *
 * Tears down all connected sessions and marks them as free.
 *
 * @param pool Session pool
 */
void ninep_session_pool_disconnect_all(struct ninep_session_pool *pool);

#endif /* ZEPHYR_INCLUDE_9P_SESSION_POOL_H_ */
