/*
 * Copyright (c) 2026 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P transport over an already-open namespace file -- see transport_nsfile.h.
 */

#include <zephyr/9p/transport_nsfile.h>
#include <zephyr/namespace/namespace.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_nsfile, CONFIG_NINEP_LOG_LEVEL);

#define NSFILE_RX_PRIORITY   K_PRIO_PREEMPT(7)
#define NSFILE_DEFAULT_MTU   448   /* fits a mesh datagram (AETHER_MAX_PAYLOAD-ish) */

/* send(): one 9P message -> one write on the channel. The channel is message-
 * framed (a datagram carries exactly one framed message), so a full write == one
 * message. A short/failed write is a transport error the client will surface. */
static int nsfile_send(struct ninep_transport *transport, const uint8_t *buf, size_t len)
{
	struct ninep_transport_nsfile *t =
		CONTAINER_OF(transport, struct ninep_transport_nsfile, transport);
	ssize_t w = ns_write(t->fd, buf, len);

	if (w < 0) {
		return (int)w;
	}
	if ((size_t)w != len) {
		LOG_WRN("short write %zd/%zu on nsfile channel", w, len);
		return -EIO;
	}
	return (int)len;
}

/* RX thread: blocking ns_read() returns one reply message per call (the channel
 * data node blocks until a datagram arrives). Deliver each off-thread via recv_cb
 * so the client is already waiting when its reply lands. A 0-length read means
 * "nothing yet" on a non-blocking channel -> brief yield; a negative read is a
 * transient error -> brief backoff (don't hot-spin). */
static void nsfile_rx_thread(void *a, void *b, void *c)
{
	struct ninep_transport_nsfile *t = a;

	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (t->running) {
		ssize_t n = ns_read(t->fd, t->rx_buf, sizeof(t->rx_buf));

		if (n > 0) {
			if (t->transport.recv_cb) {
				t->transport.recv_cb(&t->transport, t->rx_buf, (size_t)n,
						     t->transport.user_data);
			}
		} else if (n == 0) {
			k_msleep(2);   /* empty read: channel had no datagram queued */
		} else {
			if (!t->running) {
				break;     /* stop() closed/interrupted the read */
			}
			k_msleep(5);   /* transient read error */
		}
	}
	LOG_DBG("nsfile RX thread exit");
}

static int nsfile_start(struct ninep_transport *transport)
{
	struct ninep_transport_nsfile *t =
		CONTAINER_OF(transport, struct ninep_transport_nsfile, transport);

	if (t->running) {
		return 0;
	}
	t->running = true;
	t->rx_tid = k_thread_create(&t->rx_thread, t->rx_stack, t->rx_stack_sz,
				    nsfile_rx_thread, t, NULL, NULL,
				    NSFILE_RX_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(t->rx_tid, "9p_nsfile_rx");
	return 0;
}

static int nsfile_stop(struct ninep_transport *transport)
{
	struct ninep_transport_nsfile *t =
		CONTAINER_OF(transport, struct ninep_transport_nsfile, transport);

	if (!t->running) {
		return 0;
	}
	t->running = false;
	/* Unblock a thread parked in ns_read by closing the channel; the caller's
	 * fd is consumed here (matching the "transport owns the running channel"
	 * lifetime once started). */
	if (t->fd >= 0) {
		(void)ns_close(t->fd);
		t->fd = -1;
	}
	if (t->rx_tid) {
		(void)k_thread_join(&t->rx_thread, K_MSEC(500));
		t->rx_tid = NULL;
	}
	return 0;
}

static int nsfile_get_mtu(struct ninep_transport *transport)
{
	struct ninep_transport_nsfile *t =
		CONTAINER_OF(transport, struct ninep_transport_nsfile, transport);
	return t->mtu;
}

static const struct ninep_transport_ops nsfile_ops = {
	.send = nsfile_send,
	.start = nsfile_start,
	.stop = nsfile_stop,
	.get_mtu = nsfile_get_mtu,
};

int ninep_transport_nsfile_init(struct ninep_transport_nsfile *t, int fd, int mtu,
				k_thread_stack_t *rx_stack, size_t rx_stack_sz)
{
	if (!t || fd < 0 || !rx_stack || rx_stack_sz == 0) {
		return -EINVAL;
	}
	memset(t, 0, sizeof(*t));
	t->fd = fd;
	t->mtu = mtu > 0 ? mtu : NSFILE_DEFAULT_MTU;
	t->rx_stack = rx_stack;
	t->rx_stack_sz = rx_stack_sz;
	t->running = false;
	t->transport.ops = &nsfile_ops;
	t->transport.priv_data = NULL;
	/* recv_cb + user_data are filled by ninep_client_init(); it then calls
	 * ops->start (nsfile_start) to spawn the RX thread. */
	return 0;
}
