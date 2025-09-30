/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_UART_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_UART_H_

#include <zephyr/9p/transport.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport_uart 9P UART Transport
 * @ingroup ninep_transport
 * @{
 */

/**
 * @brief UART transport configuration
 */
struct ninep_transport_uart_config {
	const struct device *uart_dev;  /* UART device */
	uint8_t *rx_buf;                /* Receive buffer */
	size_t rx_buf_size;             /* Receive buffer size */
};

/**
 * @brief Initialize UART transport
 *
 * @param transport Transport instance
 * @param config UART configuration
 * @param recv_cb Receive callback
 * @param user_data User context pointer
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_uart_init(struct ninep_transport *transport,
                               const struct ninep_transport_uart_config *config,
                               ninep_transport_recv_cb_t recv_cb,
                               void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_UART_H_ */