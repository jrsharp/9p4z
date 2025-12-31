/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_CLIENT_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_CLIENT_H_

#include <zephyr/9p/transport.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport_l2cap_client 9P L2CAP Client Transport
 * @ingroup ninep_transport
 * @{
 */

/**
 * @brief L2CAP client connection state
 */
enum ninep_l2cap_client_state {
	NINEP_L2CAP_CLIENT_DISCONNECTED,
	NINEP_L2CAP_CLIENT_SCANNING,
	NINEP_L2CAP_CLIENT_CONNECTING,
	NINEP_L2CAP_CLIENT_CONNECTED,
};

/**
 * @brief L2CAP client state change callback
 *
 * @param transport Transport instance
 * @param state New connection state
 * @param user_data User context pointer
 */
typedef void (*ninep_l2cap_client_state_cb_t)(struct ninep_transport *transport,
                                               enum ninep_l2cap_client_state state,
                                               void *user_data);

/**
 * @brief L2CAP client transport configuration
 *
 * This transport operates in CLIENT mode, initiating a BLE connection
 * to a 9P server and establishing an L2CAP channel for 9P communication.
 *
 * Two connection modes are supported:
 * 1. Direct connect: Provide target_addr to connect to a known device
 * 2. Scan and connect: Set target_addr to NULL and provide service_uuid
 *    to scan for devices advertising the specified service
 */
struct ninep_transport_l2cap_client_config {
	/** Target device address (NULL to scan for service_uuid) */
	const bt_addr_le_t *target_addr;
	/** L2CAP PSM to connect to on the server */
	uint16_t psm;
	/** Service UUID to scan for (used when target_addr is NULL) */
	uint16_t service_uuid;
	/** Receive buffer */
	uint8_t *rx_buf;
	/** Receive buffer size */
	size_t rx_buf_size;
	/** Optional state change callback */
	ninep_l2cap_client_state_cb_t state_cb;
};

/**
 * @brief Initialize L2CAP client transport
 *
 * Initializes the transport for client mode operation. Call
 * ninep_transport_start() to begin scanning/connecting.
 *
 * @param transport Transport instance
 * @param config L2CAP client configuration
 * @param recv_cb Receive callback (called when server sends 9P response)
 * @param user_data User context pointer
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_l2cap_client_init(struct ninep_transport *transport,
                                       const struct ninep_transport_l2cap_client_config *config,
                                       ninep_transport_recv_cb_t recv_cb,
                                       void *user_data);

/**
 * @brief Get current connection state
 *
 * @param transport Transport instance
 * @return Current connection state
 */
enum ninep_l2cap_client_state ninep_transport_l2cap_client_get_state(
	struct ninep_transport *transport);

/**
 * @brief Get BLE connection handle
 *
 * @param transport Transport instance
 * @return BLE connection handle, or NULL if not connected
 */
struct bt_conn *ninep_transport_l2cap_client_get_conn(
	struct ninep_transport *transport);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_CLIENT_H_ */
