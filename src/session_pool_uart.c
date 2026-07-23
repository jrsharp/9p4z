/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/session_pool_uart.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_session_pool_uart, CONFIG_NINEP_LOG_LEVEL);

/* How often to sample the DTR line for connect/disconnect transitions. */
#define DTR_POLL_INTERVAL_MS 200

/*
 * Dedicated work queue for 9P message processing, mirroring the L2CAP pool.
 * Decouples 9P processing (filesystem I/O, locks, response TX) from the UART
 * ISR — the ISR only accumulates bytes and submits work.
 */
#ifndef CONFIG_NINEP_SESSION_PROC_STACK_SIZE
#define CONFIG_NINEP_SESSION_PROC_STACK_SIZE 8192
#endif
static K_THREAD_STACK_DEFINE(uart_proc_stack, CONFIG_NINEP_SESSION_PROC_STACK_SIZE);
static struct k_work_q uart_proc_wq;
static bool uart_proc_wq_started;

/* --- Transport ops (per session) ----------------------------------------- */

static int uart_session_send(struct ninep_transport *transport, const uint8_t *buf,
			     size_t len)
{
	struct uart_session_chan *ch = transport->priv_data;

	if (!ch || !ch->session || ch->session->state != NINEP_SESSION_CONNECTED) {
		return -ENOTCONN;
	}

	/* Runs on the proc work queue thread (not ISR), so spinning poll_out
	 * is safe; CDC-ACM blocks until the host drains its IN endpoint. */
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(ch->uart_dev, buf[i]);
	}

	LOG_DBG("sent %zu bytes on session %d", len, ch->session->session_id);
	return len;
}

static int uart_session_get_mtu(struct ninep_transport *transport)
{
	struct uart_session_chan *ch = transport->priv_data;

	if (!ch) {
		return -EINVAL;
	}

	/* The RX buffer bounds the largest message we can receive. */
	return (int)ch->rx_buf_size;
}

static int uart_session_stop(struct ninep_transport *transport)
{
	struct uart_session_chan *ch = transport->priv_data;

	if (ch && ch->uart_dev) {
		uart_irq_rx_disable(ch->uart_dev);
	}
	return 0;
}

static const struct ninep_transport_ops uart_session_transport_ops = {
	.send = uart_session_send,
	.stop = uart_session_stop,
	.get_mtu = uart_session_get_mtu,
	/* start not needed — RX is enabled by the pool on DTR assert */
};

/* --- 9P message processing (work queue) ----------------------------------- */

/*
 * Framer: drain the RX ring, assembling and dispatching as many complete 9P
 * messages as are available. Runs on the work queue (off the ISR), so recv_cb
 * may block/take locks. Looping here is what makes the server robust to
 * pipelining -- several requests buffered in the ring are all processed,
 * none dropped. The ISR is the sole producer into the ring and this is the
 * sole consumer, so ring_buf needs no extra lock; the framing state
 * (rx_buf/rx_len/rx_expected/rx_state) is touched only here.
 */
static void uart_session_process_work_handler(struct k_work *work)
{
	struct uart_session_chan *ch =
		CONTAINER_OF(work, struct uart_session_chan, process_work);

	for (;;) {
		struct ninep_session *session = ch->session;

		/* Session may have been freed (DTR dropped) mid-drain. */
		if (!session || session->state != NINEP_SESSION_CONNECTED) {
			return;
		}

		if (ch->rx_state == UART_RX_WAIT_SIZE) {
			ch->rx_len += ring_buf_get(&ch->rx_ring,
						   &ch->rx_buf[ch->rx_len],
						   4 - ch->rx_len);
			if (ch->rx_len < 4) {
				return;   /* size field incomplete; await more bytes */
			}
			ch->rx_expected = ch->rx_buf[0] | (ch->rx_buf[1] << 8) |
					  (ch->rx_buf[2] << 16) |
					  ((uint32_t)ch->rx_buf[3] << 24);
			if (ch->rx_expected < 7 || ch->rx_expected > ch->rx_buf_size) {
				LOG_ERR("session %d: bad msg size %u; resyncing",
					session->session_id, ch->rx_expected);
				ch->rx_len = 0;   /* drop and re-hunt a size field */
				continue;
			}
			ch->rx_state = UART_RX_WAIT_DATA;
		}

		/* UART_RX_WAIT_DATA: pull the rest of the body. */
		ch->rx_len += ring_buf_get(&ch->rx_ring, &ch->rx_buf[ch->rx_len],
					   ch->rx_expected - ch->rx_len);
		if (ch->rx_len < ch->rx_expected) {
			return;   /* body incomplete; await more bytes */
		}

		struct ninep_transport *transport = &session->transport;

		LOG_DBG("processing 9P message: %u bytes on session %d",
			ch->rx_len, session->session_id);
		if (transport->recv_cb) {
			transport->recv_cb(transport, ch->rx_buf, ch->rx_len,
					   transport->user_data);
		}

		/* Ready for the next message in the ring. */
		ch->rx_len = 0;
		ch->rx_expected = 0;
		ch->rx_state = UART_RX_WAIT_SIZE;
	}
}

/* --- UART RX ISR ---------------------------------------------------------- */

static void uart_session_isr(const struct device *dev, void *user_data)
{
	struct uart_session_chan *ch = user_data;

	if (!uart_irq_update(dev)) {
		return;
	}

	bool got = false;

	/* The ISR's only job is to move bytes into the ring as fast as possible;
	 * framing + dispatch happen off-ISR in the work handler. This keeps the
	 * receive path drop-free for pipelined requests (the previous design
	 * dropped any byte that arrived while a message was being processed). */
	while (uart_irq_rx_ready(dev)) {
		uint8_t buf[64];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}

		uint32_t put = ring_buf_put(&ch->rx_ring, buf, n);

		if (put < (uint32_t)n) {
			/* Genuine sustained overload: the framer can't keep up
			 * (it normally drains far faster than the UART fills). */
			LOG_WRN("session %d: RX ring full, dropped %u bytes",
				ch->session->session_id, (uint32_t)n - put);
		}
		got = true;
	}

	if (got) {
		k_work_submit_to_queue(&uart_proc_wq, &ch->process_work);
	}
}

/* --- Connect / disconnect (DTR edges) ------------------------------------- */

static void uart_session_connect(struct ninep_session_pool_uart *up)
{
	struct ninep_session *session = ninep_session_alloc(up->pool);

	if (!session) {
		LOG_ERR("no free UART session");
		return;
	}

	struct uart_session_chan *ch = &up->channels[session->session_id];

	memset(ch, 0, sizeof(*ch));
	ch->session = session;
	ch->uart_dev = up->config.uart_dev;
	ch->rx_buf = up->rx_buf_pool +
		     (session->session_id * up->config.rx_buf_size_per_session);
	ch->rx_buf_size = up->config.rx_buf_size_per_session;
	ch->rx_state = UART_RX_WAIT_SIZE;
	ch->ring_buf_mem = up->ring_pool +
			   (session->session_id * CONFIG_NINEP_SESSION_UART_RX_RING_SIZE);
	ring_buf_init(&ch->rx_ring, CONFIG_NINEP_SESSION_UART_RX_RING_SIZE,
		      ch->ring_buf_mem);
	k_work_init(&ch->process_work, uart_session_process_work_handler);

	session->transport.ops = &uart_session_transport_ops;
	session->transport.priv_data = ch;
	session->transport_priv = ch;

	struct ninep_server_config server_config = {
		.fs_ops = up->pool->fs_ops,
		.fs_ctx = up->pool->fs_context,
		.auth_config = up->pool->auth_config,
	};

	int ret = ninep_server_init(&session->server, &server_config,
				    &session->transport);
	if (ret < 0) {
		LOG_ERR("server init failed for session %d: %d",
			session->session_id, ret);
		ninep_session_free(session);
		return;
	}

	/* Bind and enable interrupt-driven RX for this session. */
	uart_irq_rx_disable(up->config.uart_dev);
	uart_irq_callback_user_data_set(up->config.uart_dev, uart_session_isr, ch);
	uart_irq_rx_enable(up->config.uart_dev);

	ninep_session_connected(session);
	LOG_INF("UART 9P session %d connected (DTR asserted)", session->session_id);
}

static void uart_session_disconnect(struct ninep_session_pool_uart *up)
{
	/* Detach the ISR before tearing down any session — it dereferences the
	 * channel/session we are about to free. */
	uart_irq_rx_disable(up->config.uart_dev);

	/* Notify the app FIRST: a proxied op (e.g. a blocking /net/aether read the
	 * killed host client left outstanding) may be stuck on a downstream client,
	 * fouling it for the next session. Let the app reset it before we clunk the
	 * session's fids (which themselves proxy over that client). */
	if (up->config.on_disconnect) {
		up->config.on_disconnect(up->config.disconnect_ctx);
	}

	for (int i = 0; i < up->pool->max_sessions; i++) {
		struct ninep_session *session = ninep_session_get(up->pool, i);

		if (!session || session->state == NINEP_SESSION_FREE) {
			continue;
		}

		struct uart_session_chan *ch = &up->channels[i];

		/* Cancel a queued (not-yet-running) framer pass. If it is
		 * mid-drain it re-checks the session state each iteration and
		 * bails once we free it below. */
		(void)k_work_cancel(&ch->process_work);
		ring_buf_reset(&ch->rx_ring);

		/* Clunks all open fids and resets server + transport state. */
		ninep_session_free(session);
		LOG_INF("UART 9P session %d freed (DTR deasserted)", i);
	}
}

static void uart_dtr_poll(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct ninep_session_pool_uart *up =
		CONTAINER_OF(dwork, struct ninep_session_pool_uart, dtr_poll);

	uint32_t dtr = 0;
	int ret = uart_line_ctrl_get(up->config.uart_dev, UART_LINE_CTRL_DTR, &dtr);
	bool now = (ret == 0) && (dtr != 0);

	if (now != up->last_dtr) {
		up->last_dtr = now;
		if (now) {
			uart_session_connect(up);
		} else {
			uart_session_disconnect(up);
		}
	}

	if (up->running) {
		k_work_schedule(&up->dtr_poll, K_MSEC(DTR_POLL_INTERVAL_MS));
	}
}

/* --- Public API ----------------------------------------------------------- */

int ninep_session_pool_uart_init(struct ninep_session_pool_uart *up,
				 const struct ninep_session_pool_uart_config *config)
{
	if (!up || !config || !config->fs_ops || !config->uart_dev ||
	    config->max_sessions <= 0 || config->rx_buf_size_per_session == 0) {
		LOG_ERR("invalid arguments");
		return -EINVAL;
	}

	if (!up->pool || !up->rx_buf_pool || !up->channels) {
		LOG_ERR("storage not allocated (use NINEP_SESSION_POOL_UART_DEFINE)");
		return -EINVAL;
	}

	if (!device_is_ready(config->uart_dev)) {
		LOG_ERR("UART device %s not ready", config->uart_dev->name);
		return -ENODEV;
	}

	memcpy(&up->config, config, sizeof(*config));

	struct ninep_session_pool_config pool_config = {
		.max_sessions = config->max_sessions,
		.fs_ops = config->fs_ops,
		.fs_context = config->fs_context,
		.auth_config = config->auth_config,
	};

	int ret = ninep_session_pool_init(up->pool, &pool_config);

	if (ret < 0) {
		LOG_ERR("session pool init failed: %d", ret);
		return ret;
	}

	up->last_dtr = false;
	up->running = false;
	k_work_init_delayable(&up->dtr_poll, uart_dtr_poll);

	LOG_INF("UART 9P session pool init: %s, %d session(s), %zu bytes RX each",
		config->uart_dev->name, config->max_sessions,
		config->rx_buf_size_per_session);
	return 0;
}

int ninep_session_pool_uart_start(struct ninep_session_pool_uart *up)
{
	if (!up) {
		return -EINVAL;
	}

	if (!uart_proc_wq_started) {
		struct k_work_queue_config wq_cfg = {
			.name = "9p_uart_proc",
		};
		k_work_queue_start(&uart_proc_wq, uart_proc_stack,
				   K_THREAD_STACK_SIZEOF(uart_proc_stack),
				   K_PRIO_PREEMPT(8), &wq_cfg);
		uart_proc_wq_started = true;
		LOG_INF("9P UART processing work queue started (stack=%d, prio=%d)",
			CONFIG_NINEP_SESSION_PROC_STACK_SIZE, 8);
	}

	/* Make sure RX is quiet until a session is allocated. */
	uart_irq_rx_disable(up->config.uart_dev);

	up->last_dtr = false;
	up->running = true;
	k_work_schedule(&up->dtr_poll, K_MSEC(DTR_POLL_INTERVAL_MS));

	LOG_INF("UART 9P session pool started (DTR-gated) on %s",
		up->config.uart_dev->name);
	return 0;
}

void ninep_session_pool_uart_stop(struct ninep_session_pool_uart *up)
{
	if (!up) {
		return;
	}

	LOG_INF("stopping UART 9P session pool");

	up->running = false;
	(void)k_work_cancel_delayable(&up->dtr_poll);

	uart_session_disconnect(up);
	ninep_session_pool_disconnect_all(up->pool);
}
