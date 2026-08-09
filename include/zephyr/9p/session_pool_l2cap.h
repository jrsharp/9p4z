/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_SESSION_POOL_L2CAP_H_
#define ZEPHYR_INCLUDE_9P_SESSION_POOL_L2CAP_H_

#include <zephyr/9p/session_pool.h>
#include <zephyr/bluetooth/l2cap.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief L2CAP session pool for multi-client 9P servers
 *
 * Creates a 9P server on a single L2CAP PSM that supports multiple
 * concurrent clients. Each incoming L2CAP connection is assigned to
 * an independent session from the pool, preventing fid collisions.
 *
 * Example usage:
 *   struct ninep_session_pool_l2cap *pool;
 *   pool = ninep_session_pool_l2cap_create(&config);
 *   ninep_session_pool_l2cap_start(pool);
 *   // Accepts connections automatically
 *   ninep_session_pool_l2cap_stop(pool);
 */

/**
 * @brief L2CAP session pool configuration
 */
struct ninep_session_pool_l2cap_config {
	uint16_t psm;                       /* L2CAP PSM to listen on */
	int max_sessions;                   /* Maximum concurrent sessions */
	size_t rx_buf_size_per_session;    /* RX buffer size per session */
	struct ninep_fs_ops *fs_ops;       /* Filesystem operations (shared) */
	void *fs_context;                   /* Filesystem context (shared) */
	const struct ninep_auth_config *auth_config;  /* Optional auth config (shared) */
};

/**
 * @brief L2CAP session pool (opaque)
 */
struct ninep_session_pool_l2cap;

/**
 * @brief Statically declare L2CAP session pool storage
 *
 * Use this macro to declare static storage for a session pool.
 * This avoids heap allocation, which is preferred for embedded systems.
 *
 * Example:
 *   NINEP_SESSION_POOL_L2CAP_DEFINE(my_pool, 4, 8192);
 *   ninep_session_pool_l2cap_init(&my_pool, &config);
 *
 * @param name Variable name for the pool
 * @param num_sessions Maximum concurrent sessions
 * @param rx_buf_size RX buffer size per session
 */
#define NINEP_SESSION_POOL_L2CAP_DEFINE(name, num_sessions, rx_buf_size) \
	_NINEP_SESSION_POOL_L2CAP_DEFINE(name, num_sessions, rx_buf_size)

/**
 * @brief Initialize statically allocated L2CAP session pool
 *
 * Use with storage declared by NINEP_SESSION_POOL_L2CAP_DEFINE().
 * Does NOT use k_malloc().
 *
 * @param pool Statically allocated pool storage
 * @param config Pool configuration
 * @return 0 on success, negative errno on failure
 */
int ninep_session_pool_l2cap_init(struct ninep_session_pool_l2cap *pool,
                                   const struct ninep_session_pool_l2cap_config *config);

/**
 * @brief Create and initialize L2CAP session pool (dynamic allocation)
 *
 * Allocates and initializes a session pool for L2CAP-based 9P server.
 * Memory is allocated using k_malloc(). For embedded systems with limited
 * heap, prefer NINEP_SESSION_POOL_L2CAP_DEFINE() and ninep_session_pool_l2cap_init().
 *
 * @param config Pool configuration
 * @return Pointer to initialized pool, or NULL on failure
 */
struct ninep_session_pool_l2cap *ninep_session_pool_l2cap_create(
	const struct ninep_session_pool_l2cap_config *config);

/**
 * @brief Start accepting L2CAP connections
 *
 * Registers the L2CAP server and begins accepting connections.
 * Each incoming connection is automatically assigned to a session.
 *
 * @param pool L2CAP session pool
 * @return 0 on success, negative errno on failure
 */
int ninep_session_pool_l2cap_start(struct ninep_session_pool_l2cap *pool);

/**
 * @brief Stop accepting connections and disconnect all sessions
 *
 * Disconnects all active sessions. Note: Zephyr doesn't support
 * unregistering L2CAP servers, so the server remains registered.
 *
 * @param pool L2CAP session pool
 */
void ninep_session_pool_l2cap_stop(struct ninep_session_pool_l2cap *pool);

/**
 * @brief Destroy session pool and free memory
 *
 * Stops the pool and frees all allocated memory.
 *
 * @param pool L2CAP session pool
 */
void ninep_session_pool_l2cap_destroy(struct ninep_session_pool_l2cap *pool);

/**
 * @brief L2CAP channel for a single session
 * @internal
 */
struct l2cap_session_chan {
	struct bt_l2cap_le_chan le;
	struct ninep_session *session;
	struct ninep_session_pool_l2cap *pool;  /* Owning pool; the TX pool is shared */
	uint8_t *rx_buf;
	size_t rx_buf_size;
	size_t rx_len;
	uint32_t rx_expected;
	enum { RX_WAIT_SIZE, RX_WAIT_DATA, RX_PROCESSING } rx_state;
	struct k_work process_work;  /* Async 9P processing off BT RX thread */
	uint32_t process_len;        /* Length of message to process */

	/* Bytes that arrived while rx_buf was busy being processed.
	 *
	 * An L2CAP channel is a byte stream, so a pipelining client (iOS
	 * writes 9P messages into an NSOutputStream and CoreBluetooth packs
	 * them as it likes) routinely puts several messages in one SDU.
	 * Only the first can be parsed into rx_buf while the work queue
	 * owns it; the rest wait here and are parsed the moment it frees.
	 * Discarding them instead loses whole requests silently and the
	 * client sits on its timeout. */
	uint8_t rx_pending[CONFIG_NINEP_L2CAP_RX_PENDING_SIZE];
	size_t rx_pending_len;
	struct k_mutex rx_lock;      /* rx_* state: BT RX thread vs work queue */

	/* TX health. A wedged TX path accepts sends forever and completes
	 * none of them, so the only honest signal is completions: in_flight
	 * says whether we are owed any, last_done_ms when one last arrived. */
	atomic_t tx_in_flight;
	uint32_t tx_last_done_ms;    /* k_uptime_get_32() at last sent callback */
	uint32_t tx_sends;           /* Cumulative, for the stats file */
	uint32_t tx_completions;
	bool tx_wedged;              /* Latched: teardown already requested */
	struct k_work tx_recover_work;  /* Disconnects off the noticing thread */
};

/**
 * @brief Snapshot of a session's TX health
 *
 * A wedge is invisible from the client side — the link stays up and
 * reads simply never answer — so the counters are worth surfacing
 * somewhere a field unit can be asked about them.
 */
struct ninep_l2cap_tx_stats {
	uint32_t sends;         /* SDUs handed to bt_l2cap_chan_send() */
	uint32_t completions;   /* sent callbacks received */
	uint32_t in_flight;     /* sends - completions */
	uint32_t idle_ms;       /* Since the last completion, 0 if none owed */
	bool wedged;            /* Teardown was triggered by the detector */
};

/**
 * @brief Read TX health for one session
 *
 * @param pool L2CAP session pool
 * @param session_id Session index
 * @param stats Filled in on success
 * @return 0 on success, -EINVAL if the session id is out of range
 */
int ninep_session_pool_l2cap_tx_stats(struct ninep_session_pool_l2cap *pool,
                                       int session_id,
                                       struct ninep_l2cap_tx_stats *stats);

/**
 * @brief L2CAP session pool structure
 * @internal Exposed only for static allocation macro
 */
struct ninep_session_pool_l2cap {
	struct bt_l2cap_server server;
	struct ninep_session_pool *pool;
	struct ninep_session_pool_l2cap_config config;
	uint8_t *rx_buf_pool;
	struct l2cap_session_chan *channels;
};

/**
 * @brief Static allocation macro implementation
 * @internal
 */
/*
 * The storage is a byte array sized by the same formula the heap path
 * uses, not a hand-written copy of struct ninep_session_pool. A copy
 * silently undersizes the array the moment a field is added to the real
 * struct — which had already happened for auth_config, putting the last
 * session's tail past the end of the array.
 */
#define _NINEP_SESSION_POOL_L2CAP_DEFINE(name, num_sessions, rx_buf_size) \
	static uint8_t _##name##_rx_pool[(num_sessions) * (rx_buf_size)]; \
	static struct l2cap_session_chan _##name##_channels[num_sessions]; \
	static uint8_t _##name##_session_pool_storage[ \
		sizeof(struct ninep_session_pool) + \
		(num_sessions) * sizeof(struct ninep_session)] \
		__aligned(__alignof__(struct ninep_session_pool)); \
	static struct ninep_session_pool_l2cap name = { \
		.pool = (struct ninep_session_pool *)_##name##_session_pool_storage, \
		.rx_buf_pool = _##name##_rx_pool, \
		.channels = _##name##_channels, \
	}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_SESSION_POOL_L2CAP_H_ */
