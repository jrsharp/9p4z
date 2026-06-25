/*
 * 9P transport over an Æther reliable-conversation device (/net/aether)
 *
 * Carries a 9P session over a Plan 9 /net/<proto> reliable-conversation device
 * conforming to aether(3): one 9P message == one reliable datagram; the device
 * owns fragmentation, ordering, retransmit and de-dup, so this transport is
 * thin -- it opens a conversation and shuttles bytes over its `data` file.
 *
 *   client / mounter (active):   connect <peer>  -- write T raw, read R raw
 *   server / exporter (passive): announce        -- read [src][T], reply [src][R]
 *
 * Namespace- and radio-agnostic: all file I/O (and the optional client fast
 * path) is injected via struct ninep_aether_io, so any aether(3) provider can
 * carry 9P through it.  See <zephyr/9p/transport_aether.h>.
 *
 * Copyright (c) 2024-2025 Jon Sharp
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <zephyr/9p/transport.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/transport_aether.h>

LOG_MODULE_REGISTER(ninep_aether_transport, CONFIG_NINEP_LOG_LEVEL);

/* RX thread stack: the server's recv_cb runs ninep_server_process_message()
 * (walks the namespace, builds an R-message, replies) -- give it room.  DRAM,
 * allocated on start via io->stack_alloc and freed on stop (Xtensa stacks
 * can't be PSRAM). */
#define AETHER_9P_RX_STACK 4096

/* struct ninep_aether_data is caller-allocated (see the header) so the host
 * controls placement.  The active client instance is tracked here so
 * ninep_aether_client_active() can answer without the caller's pointer. */
static struct ninep_aether_data *s_active_client;

static int aether_9p_send(struct ninep_transport *t, const uint8_t *buf, size_t len);
static int aether_9p_start(struct ninep_transport *t);
static int aether_9p_stop(struct ninep_transport *t);
static int aether_9p_get_mtu(struct ninep_transport *t);

static const struct ninep_transport_ops aether_9p_ops = {
	.send = aether_9p_send,
	.start = aether_9p_start,
	.stop = aether_9p_stop,
	.get_mtu = aether_9p_get_mtu,
};

/* Client fast path: the provider delivers a whole message from its RX context
 * (no dedicated thread).  The client's recv_cb (ninep_client: match the tag,
 * copy the response, wake the dialer) is shallow enough for that.  A connected
 * conversation delivers raw 9P messages (src is the bound peer; ignore it). */
static void aether_9p_client_krecv(const uint8_t src[6], const uint8_t *msg,
				   uint16_t len, void *user)
{
	ARG_UNUSED(src);
	struct ninep_aether_data *d = user;
	struct ninep_transport *t = d->transport;

	if (t->recv_cb) {
		t->recv_cb(t, msg, len, t->user_data);
	}
}

/* RX thread: one datagram per read -> one 9P message -> recv_cb.  Used by the
 * server always, and by a client whose provider offers no set_krecv fast path.
 * Exits on EOF (hangup wakes the blocked read) or error. */
static void aether_9p_rx_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct ninep_aether_data *d = p1;
	struct ninep_transport *t = d->transport;

	while (d->running) {
		int n = d->io->read(d->io_ctx, d->data_fd, d->rx_buf,
				    d->rx_buf_size);
		if (n <= 0) {
			if (n < 0 && d->running) {
				LOG_WRN("aether 9P RX read error %d", n);
			}
			break; /* EOF (hangup) or error */
		}

		const uint8_t *msg = d->rx_buf;
		size_t mlen = (size_t)n;

		if (d->server_mode) {
			/* announced reads are [6 src][T] -- learn the requester
			 * so the reply (.send) can address it. */
			if (n < AETHER_9P_HDR) {
				continue;
			}
			memcpy(d->cur_src, d->rx_buf, AETHER_9P_HDR);
			d->have_src = true;
			msg = d->rx_buf + AETHER_9P_HDR;
			mlen = (size_t)n - AETHER_9P_HDR;
		}

		if (t->recv_cb) {
			t->recv_cb(t, msg, mlen, t->user_data);
		}
	}

	LOG_INF("aether 9P RX thread exit (%s)",
		d->server_mode ? "server" : "client");
}

static int aether_9p_send(struct ninep_transport *t, const uint8_t *buf,
			  size_t len)
{
	struct ninep_aether_data *d = t->priv_data;
	int ret;

	if (!d || d->data_fd < 0) {
		return -ENOTCONN;
	}

	if (d->server_mode) {
		/* Reply to the in-flight requester: announced write is [dst][R]. */
		if (!d->have_src) {
			LOG_WRN("aether 9P server send with no pending source");
			return -ENOTCONN;
		}
		if (len > sizeof(d->tx_buf) - AETHER_9P_HDR) {
			return -EMSGSIZE;
		}
		memcpy(d->tx_buf, d->cur_src, AETHER_9P_HDR);
		memcpy(d->tx_buf + AETHER_9P_HDR, buf, len);
		ret = d->io->write(d->io_ctx, d->data_fd, d->tx_buf,
				   AETHER_9P_HDR + len);
	} else {
		/* Connected conversation: raw payload to the bound peer. */
		ret = d->io->write(d->io_ctx, d->data_fd, buf, len);
	}

	return (ret < 0) ? ret : 0;
}

static int aether_9p_spawn_rx(struct ninep_aether_data *d)
{
	if (!d->io->stack_alloc || !d->io->stack_free) {
		LOG_ERR("aether 9P: RX thread needs io->stack_alloc/free");
		return -ENOSYS;
	}
	d->rx_stack = d->io->stack_alloc(AETHER_9P_RX_STACK);
	if (!d->rx_stack) {
		return -ENOMEM;
	}
	d->running = true;
	d->rx_threaded = true;
	k_thread_create(&d->rx_thread, (k_thread_stack_t *)d->rx_stack,
			AETHER_9P_RX_STACK, aether_9p_rx_entry, d, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&d->rx_thread,
			  d->server_mode ? "aeth9p_srv" : "aeth9p_cli");
	return 0;
}

static int aether_9p_start(struct ninep_transport *t)
{
	struct ninep_aether_data *d = t->priv_data;
	char path[40];
	char cmd[40];
	char nbuf[8];

	if (!d || !d->rx_buf || !d->io) {
		return -EINVAL;
	}

	/* Idempotent: conn dials by calling ninep_client_init() -- which itself
	 * calls ninep_transport_start() -- and THEN calls ninep_transport_start()
	 * again explicitly.  Re-running here would allocate a second RX stack
	 * (leaking the first) and re-create the running thread.  Already
	 * running -> no-op. */
	if (d->running) {
		return 0;
	}

	/* Open + hold the clone fid (it rebinds to <N>/ctl and IS the
	 * conversation handle -- the conversation lives as long as it's open). */
	d->ctl_fd = d->io->open(d->io_ctx, "/net/aether/clone", NINEP_ORDWR);
	if (d->ctl_fd < 0) {
		LOG_ERR("aether 9P: /net/aether/clone open failed (%d) -- mesh up?",
			d->ctl_fd);
		return d->ctl_fd;
	}
	int n = d->io->read(d->io_ctx, d->ctl_fd, nbuf, sizeof(nbuf) - 1);
	if (n <= 0) {
		d->io->close(d->io_ctx, d->ctl_fd);
		d->ctl_fd = -1;
		return -EIO;
	}
	nbuf[n] = '\0';
	int slot = atoi(nbuf);

	/* Active open (connect <peer>) or passive open (announce). */
	int cl;
	if (d->server_mode) {
		cl = snprintf(cmd, sizeof(cmd), "announce");
	} else {
		cl = snprintf(cmd, sizeof(cmd),
			      "connect %02x:%02x:%02x:%02x:%02x:%02x",
			      d->peer[0], d->peer[1], d->peer[2],
			      d->peer[3], d->peer[4], d->peer[5]);
	}
	int wr = d->io->write(d->io_ctx, d->ctl_fd, cmd, cl);
	if (wr < 0) {
		LOG_ERR("aether 9P: ctl '%s' failed (%d)", cmd, wr);
		d->io->close(d->io_ctx, d->ctl_fd);
		d->ctl_fd = -1;
		return wr;
	}

	snprintf(path, sizeof(path), "/net/aether/%d/data", slot);
	d->data_fd = d->io->open(d->io_ctx, path, NINEP_ORDWR);
	if (d->data_fd < 0) {
		d->io->close(d->io_ctx, d->ctl_fd);
		d->ctl_fd = -1;
		return d->data_fd;
	}

	d->slot = slot;
	d->have_src = false;
	d->rx_threaded = false;

	int err = 0;
	if (!d->server_mode && d->io->set_krecv) {
		/* Shallow recv -> ride the provider's RX path, no thread. */
		err = d->io->set_krecv(d->io_ctx, slot, aether_9p_client_krecv, d);
		if (!err) {
			d->running = true;
		}
	} else {
		/* Server (deep reply path), or a client with no fast path. */
		err = aether_9p_spawn_rx(d);
	}
	if (err) {
		d->io->close(d->io_ctx, d->data_fd);
		d->io->close(d->io_ctx, d->ctl_fd);
		d->data_fd = d->ctl_fd = -1;
		d->slot = -1;
		return err;
	}

	LOG_INF("aether 9P transport started: conv %d (%s, msize=%d)",
		slot, d->server_mode ? "announce" : "connect", AETHER_9P_MSIZE);
	return 0;
}

static int aether_9p_stop(struct ninep_transport *t)
{
	struct ninep_aether_data *d = t->priv_data;

	if (!d) {
		return -EINVAL;
	}

	d->running = false;

	/* Client fast path: stop provider delivery before tearing down. */
	if (!d->server_mode && !d->rx_threaded && d->slot >= 0 &&
	    d->io->set_krecv) {
		d->io->set_krecv(d->io_ctx, d->slot, NULL, NULL);
	}
	/* hangup tears down the conversation; for an RX thread it also wakes the
	 * blocked read with EOF so the thread can exit. */
	if (d->ctl_fd >= 0) {
		d->io->write(d->io_ctx, d->ctl_fd, "hangup", 6);
	}
	if (d->rx_threaded) {
		k_thread_join(&d->rx_thread, K_MSEC(2000));
	}
	if (d->data_fd >= 0) {
		d->io->close(d->io_ctx, d->data_fd);
		d->data_fd = -1;
	}
	if (d->ctl_fd >= 0) {
		d->io->close(d->io_ctx, d->ctl_fd);
		d->ctl_fd = -1;
	}
	if (d->rx_stack) {
		d->io->stack_free(d->rx_stack);
		d->rx_stack = NULL;
	}
	d->rx_threaded = false;
	d->slot = -1;

	LOG_INF("aether 9P transport stopped");
	return 0;
}

static int aether_9p_get_mtu(struct ninep_transport *t)
{
	ARG_UNUSED(t);
	return AETHER_9P_MSIZE;
}

bool ninep_aether_client_active(void)
{
	return s_active_client && s_active_client->running;
}

int ninep_transport_aether_init(struct ninep_transport *transport,
				const struct ninep_aether_config *config)
{
	if (!transport || !config || !config->inst || !config->io ||
	    !config->rx_buf) {
		return -EINVAL;
	}
	if (!config->io->open || !config->io->read || !config->io->write ||
	    !config->io->close) {
		return -EINVAL;
	}
	if (config->rx_buf_size < AETHER_9P_HDR + AETHER_9P_MSIZE) {
		LOG_ERR("aether 9P: rx_buf too small (%zu < %d)",
			config->rx_buf_size, AETHER_9P_HDR + AETHER_9P_MSIZE);
		return -EINVAL;
	}

	struct ninep_aether_data *d = config->inst;

	memset(d, 0, sizeof(*d));
	memcpy(d->peer, config->peer_addr, 6);
	d->io = config->io;
	d->io_ctx = config->io_ctx;
	d->server_mode = config->server_mode;
	d->rx_buf = config->rx_buf;
	d->rx_buf_size = config->rx_buf_size;
	d->transport = transport;
	d->slot = -1;
	d->ctl_fd = -1;
	d->data_fd = -1;

	transport->ops = &aether_9p_ops;
	transport->recv_cb = NULL;  /* set by ninep_client_init / ninep_server_init */
	transport->user_data = NULL;
	transport->priv_data = d;

	if (!config->server_mode) {
		s_active_client = d;
	}

	LOG_INF("aether 9P transport initialized (%s, msize=%d)",
		config->server_mode ? "server" : "client", AETHER_9P_MSIZE);
	return 0;
}
