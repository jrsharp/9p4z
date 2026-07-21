/*
 * Copyright (c) 2026 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_NSFILE_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_NSFILE_H_

/**
 * @file
 * @brief 9P transport over an already-open namespace file (Plan 9 `mount fd`).
 *
 * Runs 9P over a bidirectional, message-preserving byte channel that is already
 * open in the thread's namespace -- i.e. a file where one write() sends one 9P
 * message and one read() returns one 9P message. The canonical use is a mesh
 * conversation's data node: `bind l2cap!dect-modem`, then
 *   fd = ns_open("/net/aether/<N>/data", ...)  (after clone + connect <addr>)
 * gives a channel that tunnels 9P to <addr>'s mesh 9P server. Feed the resulting
 * transport to ninep_client_init() + a fs_9p/remote_fs mount and you have
 * `mount aether!<addr>` -- a remote node's filesystem in the local namespace.
 *
 * This is the on-deck equivalent of the host tool `aether_conv --bridge`.
 *
 * Contract: the channel must be message-framed (one 9P message per read/write,
 * no reassembly) -- true for /net/aether/<N>/data, where each datagram carries
 * exactly one framed 9P message. msize must be clamped (via the client config)
 * to the channel's MTU so no message is split.
 */

#include <zephyr/9p/transport.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Transport-over-namespace-file instance. */
struct ninep_transport_nsfile {
	struct ninep_transport transport;   /* embed; client_init fills recv_cb/user_data */
	int fd;                             /* ns_open'd bidirectional 9P channel */
	int mtu;                            /* max single-message size (channel datagram cap) */

	/* One RX thread does blocking ns_read() and delivers each message off-thread
	 * via recv_cb (never inline with send), so the client is already waiting when
	 * its reply arrives -- same model as the UART transport's deferred RX. */
	struct k_thread rx_thread;
	k_tid_t rx_tid;
	k_thread_stack_t *rx_stack;
	size_t rx_stack_sz;
	volatile bool running;
	uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
};

/**
 * @brief Initialize a 9P transport over an open namespace-file channel.
 *
 * @param t         Transport instance (caller-owned storage).
 * @param fd        An ns_open()'d bidirectional 9P channel (e.g. a connected
 *                  /net/aether/<N>/data). The transport does not open or close it.
 * @param mtu       Max single-message size in bytes (the channel's datagram cap,
 *                  e.g. AETHER_MAX_PAYLOAD). 0 -> a conservative default.
 * @param rx_stack  Stack for the RX thread (K_THREAD_STACK_DEFINE'd by the caller;
 *                  one per concurrent mount).
 * @param rx_stack_sz sizeof that stack (K_THREAD_STACK_SIZEOF).
 * @return 0 on success, negative errno otherwise. After success pass
 *         &t->transport to ninep_client_init(); ninep_client_init calls ops->start
 *         which spawns the RX thread.
 */
int ninep_transport_nsfile_init(struct ninep_transport_nsfile *t, int fd, int mtu,
				k_thread_stack_t *rx_stack, size_t rx_stack_sz);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_NSFILE_H_ */
