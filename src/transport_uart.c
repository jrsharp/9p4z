/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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
};

static void uart_irq_handler(const struct device *dev, void *user_data)
{
	struct ninep_transport *transport = user_data;
	struct uart_transport_data *data = transport->priv_data;
	uint8_t byte;

	if (!uart_irq_update(dev)) {
		return;
	}

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
			/* Deliver complete message */
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
}

#ifdef CONFIG_NINEP_UART_POLLING_MODE
#define UART_POLLING_STACK_SIZE 1024
#define UART_POLLING_PRIORITY 5

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
				} else {
					/* Invalid header - reset */
					data->rx_offset = 0;
					continue;
				}
			}

			/* Check if complete message received */
			if (data->header_received && data->rx_offset >= data->expected_size) {
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

static int uart_send(struct ninep_transport *transport, const uint8_t *buf,
                     size_t len)
{
	struct uart_transport_data *data = transport->priv_data;

	if (!data || !data->uart_dev) {
		return -EINVAL;
	}

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

#ifdef CONFIG_NINEP_UART_POLLING_MODE
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
	/* Enable UART interrupts */
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

#ifdef CONFIG_NINEP_UART_POLLING_MODE
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