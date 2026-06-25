/*
 * 9P transport over an Æther reliable-conversation device (/net/aether)
 *
 * Carries a 9P session over a Plan 9 /net/<proto> reliable-conversation
 * device that conforms to the aether(3) contract: one 9P message rides in one
 * reliable datagram, the device owns fragmentation/ordering/retransmit/de-dup,
 * so this transport is thin -- it opens a conversation and shuttles bytes over
 * its `data` file.
 *
 *   client / mounter (active):   connect <peer>  -- write T raw, read R raw
 *   server / exporter (passive): announce        -- read [src][T], reply [src][R]
 *
 * The transport is namespace- and radio-agnostic: the host injects the file
 * I/O (open/read/write/close on /net/aether) and, optionally, a fast client
 * receive hook, via struct ninep_aether_io.  Any provider of the aether(3)
 * device -- LoRa, DECT NR+, 802.15.4 -- can carry 9P through it unchanged.
 *
 * Copyright (c) 2024-2025 Jon Sharp
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_AETHER_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_AETHER_H_

#include <zephyr/kernel.h>
#include <zephyr/9p/transport.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Aether 9P msize (max 9P message size).  Sized so a whole 9P message rides in
 * ONE reliable datagram -- i.e. one LoRa packet -- avoiding fragmentation and
 * its reassembly latency on a slow half-duplex link.  The /net/aether device
 * fragments a write into chunks of (reliable MTU - 1) = 199 bytes (1-byte frag
 * header), so a 199-byte message is exactly one fragment. */
#define AETHER_9P_MSIZE 199

/* Announced-conversation framing: [6-byte src/dst][payload] (aether(3) §4). */
#define AETHER_9P_HDR 6

/* Transport read buffer size: a server read is [6-byte src][9P message]. */
#define AETHER_9P_RXBUF (AETHER_9P_MSIZE + AETHER_9P_HDR)

/**
 * Shallow client receive callback (aether(3) optional fast path).
 *
 * Delivers one whole 9P message from a connected conversation.  Runs in the
 * provider's receive context (e.g. a mesh workqueue), so it MUST be shallow --
 * it just hands the message to the transport's recv_cb.  @src is the datagram
 * source (the bound peer; ignored for a connected conversation).
 */
typedef void (*ninep_aether_krecv_t)(const uint8_t src[6], const uint8_t *msg,
				     uint16_t len, void *user);

/**
 * Host-provided I/O for the /net/aether conversation device.
 *
 * The transport never names a concrete namespace or radio; the host wires
 * these to its own /net/aether implementation.  `open/read/write/close` mirror
 * the file ops (fds index the host's namespace).  Return values follow the
 * usual convention: >= 0 on success (byte count for read/write), negative
 * errno on failure.
 */
struct ninep_aether_io {
	int (*open)(void *ctx, const char *path, uint8_t mode);
	int (*read)(void *ctx, int fd, void *buf, size_t len);
	int (*write)(void *ctx, int fd, const void *buf, size_t len);
	int (*close)(void *ctx, int fd);

	/* Optional client fast path: register @cb to receive messages on
	 * conversation @slot from the provider's RX context (no dedicated
	 * thread).  NULL => the client uses an RX thread, like the server. */
	int (*set_krecv)(void *ctx, int slot, ninep_aether_krecv_t cb,
			 void *user);

	/* DRAM thread-stack alloc/free for the server RX thread.  Required in
	 * server mode (and in client mode when set_krecv is NULL).  Allocated
	 * on start and freed on stop, so a passive transport pays nothing
	 * until it actually exports.  Must return DRAM (not PSRAM). */
	void *(*stack_alloc)(size_t size);
	void  (*stack_free)(void *stack);
};

/**
 * Per-instance transport state.  Caller-allocated so the host controls
 * placement: a client (which never spawns a thread, riding the set_krecv fast
 * path) is safe in PSRAM, while a server -- whose embedded k_thread must not
 * live in PSRAM on Xtensa -- belongs in DRAM.  Treat the fields as private.
 */
struct ninep_aether_data {
	struct ninep_transport *transport;
	const struct ninep_aether_io *io;
	void    *io_ctx;
	uint8_t  peer[6];
	bool     server_mode;

	int      slot;
	int      ctl_fd;
	int      data_fd;

	uint8_t *rx_buf;
	size_t   rx_buf_size;

	volatile bool running;
	bool     rx_threaded;
	struct k_thread rx_thread;
	void    *rx_stack;

	uint8_t  cur_src[6];
	bool     have_src;
	uint8_t  tx_buf[AETHER_9P_HDR + AETHER_9P_MSIZE];
};

/** Configuration for an Æther 9P transport instance. */
struct ninep_aether_config {
	struct ninep_aether_data *inst;    /* caller-allocated state (required) */
	const struct ninep_aether_io *io;  /* host I/O (required) */
	void    *io_ctx;                   /* opaque, passed to every io op */
	uint8_t  peer_addr[6];             /* connect target (client mode) */
	bool     server_mode;              /* true = announce, false = connect */
	uint8_t *rx_buf;                   /* read buffer (>= AETHER_9P_RXBUF) */
	size_t   rx_buf_size;
};

/**
 * Initialize an Æther 9P transport instance.
 *
 * @param transport  Transport struct to initialize
 * @param config     Configuration (host I/O, peer addr, mode, read buffer)
 * @return 0 on success, negative errno on failure
 */
int ninep_transport_aether_init(struct ninep_transport *transport,
				const struct ninep_aether_config *config);

/**
 * @return true if a client (mounter) Æther 9P conversation is active.
 *
 * A live client conversation saturates a half-duplex radio and can delay other
 * timed work; callers that must back off meanwhile (e.g. blocking HCI commands
 * on a timer) use this to gate themselves.
 */
bool ninep_aether_client_active(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_AETHER_H_ */
