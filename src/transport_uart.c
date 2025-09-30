/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

struct uart_transport_data {
	const struct device *uart_dev;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	size_t rx_offset;
	uint32_t expected_size;
	bool header_received;
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

	/* Enable UART interrupts */
	uart_irq_callback_user_data_set(data->uart_dev, uart_irq_handler,
	                                transport);
	uart_irq_rx_enable(data->uart_dev);

	return 0;
}

static int uart_stop(struct ninep_transport *transport)
{
	struct uart_transport_data *data = transport->priv_data;

	if (!data || !data->uart_dev) {
		return -EINVAL;
	}

	/* Disable UART interrupts */
	uart_irq_rx_disable(data->uart_dev);

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

	return 0;
}