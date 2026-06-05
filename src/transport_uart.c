/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_uart_transport, CONFIG_NINEP_LOG_LEVEL);

struct uart_transport_data {
	const struct device *uart_dev;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	size_t rx_offset;
	uint32_t expected_size;
	bool header_received;
#ifdef CONFIG_NINEP_UART_POLLING_MODE
	k_tid_t polling_tid;
	bool polling_active;
#endif
#ifdef CONFIG_NINEP_UART_DEFERRED_RX
	struct k_sem msg_sem;       /* ISR -> framer thread: bytes are available */
	struct ring_buf rx_ring;    /* ISR -> framer byte FIFO */
	bool proc_active;
	k_tid_t proc_tid;
#endif
};

static void uart_irq_handler(const struct device *dev, void *user_data)
{
	struct ninep_transport *transport = user_data;
	struct uart_transport_data *data = transport->priv_data;

	if (!uart_irq_update(dev)) {
		return;
	}

#ifdef CONFIG_NINEP_UART_DEFERRED_RX
	/*
	 * Deferred-RX: the ISR only moves bytes into the ring as fast as
	 * possible; the framer thread frames + dispatches off-ISR. This keeps
	 * the receive path drop-free for pipelined requests (the previous design
	 * dropped any byte that arrived while a message was being processed).
	 */
	bool got = false;

	while (uart_irq_rx_ready(dev)) {
		uint8_t buf[64];
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}

		uint32_t put = ring_buf_put(&data->rx_ring, buf, n);

		if (put < (uint32_t)n) {
			LOG_WRN("9P UART RX ring full, dropped %u bytes",
				(uint32_t)n - put);
		}
		got = true;
	}

	if (got) {
		k_sem_give(&data->msg_sem);
	}
#else
	uint8_t byte;

	while (uart_irq_rx_ready(dev)) {
		if (uart_fifo_read(dev, &byte, 1) != 1) {
			break;
		}

		/* Store received byte */
		if (data->rx_offset < data->rx_buf_size) {
			data->rx_buf[data->rx_offset++] = byte;
		} else {
			/* Buffer overflow - reset */
			data->rx_offset = 0;
			data->header_received = false;
			continue;
		}

		/* Parse header if we have enough bytes */
		if (!data->header_received && data->rx_offset >= 7) {
			struct ninep_msg_header hdr;

			if (ninep_parse_header(data->rx_buf, data->rx_offset, &hdr) == 0) {
				data->expected_size = hdr.size;
				data->header_received = true;
			} else {
				/* Invalid header - reset */
				data->rx_offset = 0;
				continue;
			}
		}

		/* Check if we have a complete message */
		if (data->header_received && data->rx_offset >= data->expected_size) {
			/* Deliver complete message (inline in ISR) */
			if (transport->recv_cb) {
				transport->recv_cb(transport, data->rx_buf,
				                   data->expected_size,
				                   transport->user_data);
			}

			/* Reset for next message */
			data->rx_offset = 0;
			data->header_received = false;
			data->expected_size = 0;
		}
	}
#endif
}

#ifdef CONFIG_NINEP_UART_POLLING_MODE
#define UART_POLLING_STACK_SIZE CONFIG_NINEP_UART_POLLING_STACK_SIZE
/* Lowest application priority: the loop must busy-poll (k_yield, no sleep) to
 * keep up with the line rate, so it must sit *below* the shell/log threads or
 * it starves them. It still gets the CPU whenever they are idle.
 */
#define UART_POLLING_PRIORITY (CONFIG_NUM_PREEMPT_PRIORITIES - 1)

static struct k_thread uart_polling_thread;
static K_THREAD_STACK_DEFINE(uart_polling_stack, UART_POLLING_STACK_SIZE);

static void uart_polling_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct ninep_transport *transport = arg1;
	struct uart_transport_data *data = transport->priv_data;
	uint8_t byte;

	while (data->polling_active) {
		/* Poll for received data */
		int ret = uart_poll_in(data->uart_dev, &byte);
		if (ret == 0) {
			LOG_DBG("RX byte: 0x%02x (offset=%zu)",
				byte, data->rx_offset);
			/* Process received byte (same logic as IRQ handler) */
			if (data->rx_offset < data->rx_buf_size) {
				data->rx_buf[data->rx_offset++] = byte;
			} else {
				/* Buffer overflow - reset */
				data->rx_offset = 0;
				data->header_received = false;
				continue;
			}

			/* Parse header if we have enough bytes */
			if (!data->header_received && data->rx_offset >= 7) {
				struct ninep_msg_header hdr;

				if (ninep_parse_header(data->rx_buf, data->rx_offset, &hdr) == 0) {
					data->expected_size = hdr.size;
					data->header_received = true;
					LOG_DBG("UART RX header: size=%u type=%u tag=%u",
						hdr.size, hdr.type, hdr.tag);
				} else {
					/* Invalid header - reset */
					LOG_WRN("UART RX invalid header after %zu bytes",
						data->rx_offset);
					data->rx_offset = 0;
					continue;
				}
			}

			/* Check if complete message received */
			if (data->header_received && data->rx_offset >= data->expected_size) {
				LOG_DBG("UART RX %zu bytes", data->rx_offset);
				if (transport->recv_cb) {
					transport->recv_cb(transport, data->rx_buf,
					                  data->rx_offset,
					                  transport->user_data);
				}

				/* Reset for next message */
				data->rx_offset = 0;
				data->header_received = false;
				data->expected_size = 0;
			}
		} else {
			/* No data available, yield to other threads */
			k_yield();
		}
	}
}
#endif /* CONFIG_NINEP_UART_POLLING_MODE */

#ifdef CONFIG_NINEP_UART_DEFERRED_RX
#define UART_PROC_STACK_SIZE CONFIG_NINEP_UART_POLLING_STACK_SIZE
/* Sane default: responsive enough to answer 9P promptly, but the thread blocks
 * on msg_sem between messages so it never busy-starves real-time peers. */
#define UART_PROC_PRIORITY   K_PRIO_PREEMPT(6)

static struct k_thread uart_proc_thread;
static K_THREAD_STACK_DEFINE(uart_proc_stack, UART_PROC_STACK_SIZE);
/* Backing store for the deferred-RX ring (singleton, like the proc thread). */
static uint8_t uart_rx_ring_mem[CONFIG_NINEP_UART_DEFERRED_RX_RING_SIZE];

/*
 * Framer: woken when the ISR adds bytes, it drains the ring and dispatches as
 * many complete 9P messages as are available, then blocks again. Looping here
 * is what makes the receive path robust to pipelining -- several requests
 * buffered in the ring are all delivered, none dropped. The ISR is the sole
 * producer into the ring and this is the sole consumer (ring_buf needs no extra
 * lock); the framing state (rx_buf/rx_offset/header_received/expected_size) is
 * touched only here.
 */
static void uart_proc_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct ninep_transport *transport = arg1;
	struct uart_transport_data *data = transport->priv_data;

	while (data->proc_active) {
		if (k_sem_take(&data->msg_sem, K_FOREVER) != 0) {
			continue;
		}
		if (!data->proc_active) {
			break;
		}

		for (;;) {
			/* Accumulate the header (>=7 bytes) to learn the size. */
			if (!data->header_received) {
				data->rx_offset += ring_buf_get(
					&data->rx_ring,
					&data->rx_buf[data->rx_offset],
					7 - data->rx_offset);
				if (data->rx_offset < 7) {
					break;   /* await more bytes */
				}

				struct ninep_msg_header hdr;

				if (ninep_parse_header(data->rx_buf, data->rx_offset,
						       &hdr) != 0 ||
				    hdr.size < 7 || hdr.size > data->rx_buf_size) {
					LOG_ERR("bad 9P header; resyncing");
					data->rx_offset = 0;   /* drop and re-hunt */
					continue;
				}
				data->expected_size = hdr.size;
				data->header_received = true;
			}

			/* Pull the rest of the body. */
			data->rx_offset += ring_buf_get(
				&data->rx_ring, &data->rx_buf[data->rx_offset],
				data->expected_size - data->rx_offset);
			if (data->rx_offset < data->expected_size) {
				break;   /* await more bytes */
			}

			if (transport->recv_cb) {
				transport->recv_cb(transport, data->rx_buf,
						   data->expected_size,
						   transport->user_data);
			}

			data->rx_offset = 0;
			data->header_received = false;
			data->expected_size = 0;
		}
	}
}
#endif /* CONFIG_NINEP_UART_DEFERRED_RX */

static int uart_send(struct ninep_transport *transport, const uint8_t *buf,
                     size_t len)
{
	struct uart_transport_data *data = transport->priv_data;

	if (!data || !data->uart_dev) {
		return -EINVAL;
	}

	LOG_DBG("UART TX %zu bytes", len);

	/* Send data via UART polling mode */
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(data->uart_dev, buf[i]);
	}

	return len;
}

static int uart_start(struct ninep_transport *transport)
{
	struct uart_transport_data *data = transport->priv_data;

	if (!data || !data->uart_dev) {
		return -EINVAL;
	}

#if defined(CONFIG_NINEP_UART_DEFERRED_RX)
	/* Interrupt RX (ISR fills the ring) + a dedicated off-ISR framer thread. */
	k_sem_init(&data->msg_sem, 0, 1);
	ring_buf_init(&data->rx_ring, sizeof(uart_rx_ring_mem), uart_rx_ring_mem);
	data->rx_offset = 0;
	data->header_received = false;
	data->proc_active = true;
	uart_irq_callback_user_data_set(data->uart_dev, uart_irq_handler,
	                                transport);
	uart_irq_rx_enable(data->uart_dev);
	data->proc_tid = k_thread_create(&uart_proc_thread, uart_proc_stack,
	                                 K_THREAD_STACK_SIZEOF(uart_proc_stack),
	                                 uart_proc_thread_fn,
	                                 transport, NULL, NULL,
	                                 UART_PROC_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(data->proc_tid, "uart_9p_proc");
#elif defined(CONFIG_NINEP_UART_POLLING_MODE)
	/* Start polling thread */
	data->polling_active = true;
	data->polling_tid = k_thread_create(&uart_polling_thread,
	                                    uart_polling_stack,
	                                    K_THREAD_STACK_SIZEOF(uart_polling_stack),
	                                    uart_polling_thread_fn,
	                                    transport, NULL, NULL,
	                                    UART_POLLING_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(data->polling_tid, "uart_poll");
#else
	/* Enable UART interrupts (recv_cb runs inline in the ISR) */
	uart_irq_callback_user_data_set(data->uart_dev, uart_irq_handler,
	                                transport);
	uart_irq_rx_enable(data->uart_dev);
#endif

	return 0;
}

static int uart_stop(struct ninep_transport *transport)
{
	struct uart_transport_data *data = transport->priv_data;

	if (!data || !data->uart_dev) {
		return -EINVAL;
	}

#if defined(CONFIG_NINEP_UART_DEFERRED_RX)
	/* Stop processing thread + disable UART interrupts */
	uart_irq_rx_disable(data->uart_dev);
	data->proc_active = false;
	k_sem_give(&data->msg_sem);   /* wake it so it can exit */
	k_thread_join(data->proc_tid, K_FOREVER);
#elif defined(CONFIG_NINEP_UART_POLLING_MODE)
	/* Stop polling thread */
	data->polling_active = false;
	k_thread_join(data->polling_tid, K_FOREVER);
#else
	/* Disable UART interrupts */
	uart_irq_rx_disable(data->uart_dev);
#endif

	return 0;
}

static const struct ninep_transport_ops uart_transport_ops = {
	.send = uart_send,
	.start = uart_start,
	.stop = uart_stop,
};

int ninep_transport_uart_init(struct ninep_transport *transport,
                               const struct ninep_transport_uart_config *config,
                               ninep_transport_recv_cb_t recv_cb,
                               void *user_data)
{
	struct uart_transport_data *data;

	if (!transport || !config || !config->uart_dev ||
	    !config->rx_buf || config->rx_buf_size == 0) {
		return -EINVAL;
	}

	/* Allocate private data */
	data = k_malloc(sizeof(*data));
	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));
	data->uart_dev = config->uart_dev;
	data->rx_buf = config->rx_buf;
	data->rx_buf_size = config->rx_buf_size;

	/* Initialize transport */
	transport->ops = &uart_transport_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

#ifdef CONFIG_NINEP_UART_POLLING_MODE
	/* In polling mode, ensure UART interrupts are disabled at hardware level */
	/* This prevents spurious interrupts when using polling */
	uart_irq_tx_disable(data->uart_dev);
	uart_irq_rx_disable(data->uart_dev);
#endif

	return 0;
}