/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_H_

#include <zephyr/9p/transport.h>
#include <zephyr/bluetooth/l2cap.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport_l2cap 9P L2CAP Transport
 * @ingroup ninep_transport
 * @{
 */

/**
 * @brief L2CAP transport configuration
 */
struct ninep_transport_l2cap_config {
	uint16_t psm;               /* L2CAP PSM to listen on */
	uint8_t *rx_buf;            /* Receive buffer */
	size_t rx_buf_size;         /* Receive buffer size */
};

/**
 * @brief Initialize L2CAP transport
 *
 * Registers an L2CAP server on the specified PSM and sets up the
 * transport to handle incoming connections.
 *
 * @param transport Transport instance
 * @param config L2CAP configuration
 * @param recv_cb Receive callback
 * @param user_data User context pointer
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_l2cap_init(struct ninep_transport *transport,
                                const struct ninep_transport_l2cap_config *config,
                                ninep_transport_recv_cb_t recv_cb,
                                void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_H_ */
