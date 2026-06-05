/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_SESSION_POOL_UART_H_
#define ZEPHYR_INCLUDE_9P_SESSION_POOL_UART_H_

#include <zephyr/9p/session_pool.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

/**
 * @brief UART / USB-CDC-ACM session pool for stream-transport 9P servers
 *
 * A raw byte stream (UART or USB CDC ACM) carries no intrinsic connect /
 * disconnect events, so a single long-lived ninep_server bound to a stream
 * WEDGES when one client disconnects and the next attaches: the stale RX
 * accumulator and fid namespace from the previous client are never reset.
 *
 * This pool fixes that by treating the CDC-ACM **DTR** modem line as the
 * connect / disconnect signal: a host opening the port asserts DTR, closing it
 * deasserts DTR. Each DTR rising edge allocates a fresh session (fresh RX state
 * machine + fresh ninep_server with its own fid namespace); each falling edge
 * frees the session (clunking all open fids and resetting RX state).
 *
 * A single UART device carries a single stream, so a UART pool is normally
 * sized @c max_sessions = 1. The pool abstraction is retained so the SAME
 * shared @ref ninep_fs_ops can be served simultaneously by this pool and by
 * @ref ninep_session_pool_l2cap (BLE), giving a multi-transport re-export
 * server with one filesystem behind it.
 *
 * Like the L2CAP pool, completed 9P messages are dispatched on a dedicated work
 * queue rather than in the UART ISR, so filesystem I/O may block and take locks
 * without stalling the receive path.
 */

/**
 * @brief UART session pool configuration
 */
struct ninep_session_pool_uart_config {
	const struct device *uart_dev;     /* UART / CDC-ACM device */
	int max_sessions;                  /* concurrent sessions (1 per stream) */
	size_t rx_buf_size_per_session;    /* RX buffer size per session */
	struct ninep_fs_ops *fs_ops;       /* filesystem operations (shared) */
	void *fs_context;                  /* filesystem context (shared) */
	const struct ninep_auth_config *auth_config;  /* optional auth (shared) */
};

/**
 * @brief Per-session RX state machine for a stream transport
 * @internal
 *
 * The ISR only drains the UART FIFO into @ref rx_ring (a byte FIFO); a framer
 * on the work queue pulls whole 9P messages out of the ring into @ref rx_buf
 * and dispatches them in a loop. Decoupling intake from framing means a
 * pipelined request (9P allows many outstanding tags) is never dropped because
 * a previous one is still being processed.
 */
struct uart_session_chan {
	struct ninep_session *session;
	const struct device *uart_dev;
	uint8_t *rx_buf;             /* assembly buffer: one message at a time */
	size_t rx_buf_size;
	size_t rx_len;               /* bytes assembled into rx_buf so far */
	uint32_t rx_expected;        /* full message size once the size field is in */
	enum {
		UART_RX_WAIT_SIZE = 0,  /* accumulating the 4-byte size field */
		UART_RX_WAIT_DATA,      /* accumulating the message body */
	} rx_state;
	struct ring_buf rx_ring;     /* ISR -> framer byte FIFO */
	uint8_t *ring_buf_mem;       /* backing store for rx_ring */
	struct k_work process_work;  /* framer: drains rx_ring, dispatches messages */
};

/**
 * @brief UART session pool structure
 * @internal Exposed only for the static-allocation macro.
 */
struct ninep_session_pool_uart {
	struct ninep_session_pool *pool;
	struct ninep_session_pool_uart_config config;
	uint8_t *rx_buf_pool;
	uint8_t *ring_pool;                /* per-session RX ring backing store */
	struct uart_session_chan *channels;
	struct k_work_delayable dtr_poll;  /* polls DTR for connect/disconnect */
	bool last_dtr;                     /* last observed DTR level */
	bool running;                      /* poll loop active */
};

/**
 * @brief Statically declare UART session pool storage.
 *
 * Avoids heap allocation. The backing pool layout matches
 * @ref ninep_session_pool exactly (including auth_config) so the storage may be
 * initialized by the standard @ref ninep_session_pool_init.
 *
 * Example:
 *   NINEP_SESSION_POOL_UART_DEFINE(fw_uart_pool, 1, 8192);
 *   struct ninep_session_pool_uart_config cfg = { ... };
 *   ninep_session_pool_uart_init(&fw_uart_pool, &cfg);
 *   ninep_session_pool_uart_start(&fw_uart_pool);
 *
 * @param name         Variable name for the pool
 * @param num_sessions Maximum concurrent sessions
 * @param rx_buf_size  RX buffer size per session
 */
#define NINEP_SESSION_POOL_UART_DEFINE(name, num_sessions, rx_buf_size) \
	_NINEP_SESSION_POOL_UART_DEFINE(name, num_sessions, rx_buf_size)

/**
 * @brief Initialize a statically allocated UART session pool.
 *
 * Use with storage declared by NINEP_SESSION_POOL_UART_DEFINE().
 *
 * @param pool   Statically allocated pool storage
 * @param config Pool configuration
 * @return 0 on success, negative errno on failure
 */
int ninep_session_pool_uart_init(struct ninep_session_pool_uart *pool,
				 const struct ninep_session_pool_uart_config *config);

/**
 * @brief Start the DTR-driven connect/disconnect loop.
 *
 * Begins polling the DTR line; sessions are allocated/freed as the host opens
 * and closes the port.
 *
 * @param pool UART session pool
 * @return 0 on success, negative errno on failure
 */
int ninep_session_pool_uart_start(struct ninep_session_pool_uart *pool);

/**
 * @brief Stop the poll loop and free any active session.
 *
 * @param pool UART session pool
 */
void ninep_session_pool_uart_stop(struct ninep_session_pool_uart *pool);

/**
 * @brief Static allocation macro implementation
 * @internal
 */
#define _NINEP_SESSION_POOL_UART_DEFINE(name, num_sessions, rx_buf_size) \
	static uint8_t _##name##_rx_pool[(num_sessions) * (rx_buf_size)]; \
	static uint8_t _##name##_ring_pool[(num_sessions) * \
		CONFIG_NINEP_SESSION_UART_RX_RING_SIZE]; \
	static struct uart_session_chan _##name##_channels[num_sessions]; \
	static struct { \
		int max_sessions; \
		struct k_mutex lock; \
		struct ninep_fs_ops *fs_ops; \
		void *fs_context; \
		const struct ninep_auth_config *auth_config; \
		struct ninep_session sessions[num_sessions]; \
	} _##name##_session_pool_storage; \
	static struct ninep_session_pool_uart name = { \
		.pool = (struct ninep_session_pool *)&_##name##_session_pool_storage, \
		.rx_buf_pool = _##name##_rx_pool, \
		.ring_pool = _##name##_ring_pool, \
		.channels = _##name##_channels, \
	}

#endif /* ZEPHYR_INCLUDE_9P_SESSION_POOL_UART_H_ */
