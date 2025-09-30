/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simple 9P UART echo sample - demonstrates basic transport functionality
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ninep_uart_echo, LOG_LEVEL_DBG);

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_console)

static uint8_t rx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];

static void message_received(struct ninep_transport *transport,
                             const uint8_t *buf, size_t len,
                             void *user_data)
{
	struct ninep_msg_header hdr;

	LOG_INF("Received message: %d bytes", len);

	/* Parse header */
	if (ninep_parse_header(buf, len, &hdr) == 0) {
		LOG_INF("Message type: %d, tag: %d, size: %d",
		        hdr.type, hdr.tag, hdr.size);

		/* Echo message back for now */
		ninep_transport_send(transport, buf, len);
	} else {
		LOG_ERR("Failed to parse message header");
	}
}

int main(void)
{
	const struct device *uart_dev;
	struct ninep_transport transport;
	struct ninep_transport_uart_config uart_config;
	int ret;

	LOG_INF("9P UART Echo Sample");

	uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -1;
	}

	/* Configure UART transport */
	uart_config.uart_dev = uart_dev;
	uart_config.rx_buf = rx_buffer;
	uart_config.rx_buf_size = sizeof(rx_buffer);

	/* Initialize transport */
	ret = ninep_transport_uart_init(&transport, &uart_config,
	                                message_received, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UART transport: %d", ret);
		return -1;
	}

	/* Start receiving */
	ret = ninep_transport_start(&transport);
	if (ret < 0) {
		LOG_ERR("Failed to start transport: %d", ret);
		return -1;
	}

	LOG_INF("9P UART transport started, waiting for messages...");

	/* Let the system run */
	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}