/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_TCP_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_TCP_H_

#include <zephyr/9p/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport_tcp 9P TCP Transport
 * @ingroup ninep_transport
 * @{
 */

/**
 * @brief TCP transport configuration
 */
struct ninep_tcp_config {
	/** Port to listen on (default: 564) */
	uint16_t port;
	/** Maximum receive buffer size */
	size_t rx_buf_size;
};

/**
 * @brief Initialize TCP transport
 *
 * @param transport Transport instance
 * @param config TCP configuration
 * @param recv_cb Receive callback
 * @param user_data User context pointer
 * @return 0 on success, negative error code on failure
 */
int ninep_tcp_transport_init(struct ninep_transport *transport,
                              const struct ninep_tcp_config *config,
                              ninep_transport_recv_cb_t recv_cb,
                              void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_TCP_H_ */
