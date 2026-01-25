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
	NINEP_L2CAP_CLIENT_DISCOVERING,  /**< Reading 9PIS GATT attributes */
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
 * 2. Scan and connect: Set target_addr to NULL and provide service_uuid128
 *    (or service_uuid16) to scan for devices advertising the specified service
 */
struct ninep_transport_l2cap_client_config {
	/** Target device address (NULL to scan for service UUID) */
	const bt_addr_le_t *target_addr;
	/** L2CAP PSM to connect to on the server (fallback if discover_9pis fails) */
	uint16_t psm;
	/** 128-bit service UUID to scan for in little-endian (preferred) */
	const uint8_t *service_uuid128;
	/** 16-bit service UUID fallback (used if service_uuid128 is NULL) */
	uint16_t service_uuid16;
	/** Receive buffer */
	uint8_t *rx_buf;
	/** Receive buffer size */
	size_t rx_buf_size;
	/** Optional state change callback */
	ninep_l2cap_client_state_cb_t state_cb;
	/** Enable 9PIS GATT discovery to read PSM from Transport Info characteristic.
	 *  Requires CONFIG_BT_GATT_CLIENT. If discovery fails, falls back to psm. */
	bool discover_9pis;
	/** Required feature string (e.g., "kbd"). If set, transport verifies the
	 *  9PIS Features characteristic contains this string before connecting.
	 *  Requires CONFIG_BT_GATT_CLIENT and discover_9pis=true. */
	const char *required_features;
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

/**
 * @brief Set target device address for reconnection
 *
 * Updates the target address used when starting the transport. This allows
 * direct MAC address connection to a paired device instead of UUID scanning.
 * Call this before ninep_transport_start() to reconnect to a known device.
 *
 * @param transport Transport instance
 * @param addr Target device address (NULL to use UUID scanning)
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_l2cap_client_set_target(struct ninep_transport *transport,
					    const bt_addr_le_t *addr);

/**
 * @brief Set MAC address filter for UUID scanning
 *
 * When scanning for devices by service UUID, only connect to devices that
 * match this MAC address. This enables "paired device" behavior where we
 * scan for advertisements but only connect to a specific known device.
 *
 * Use this instead of set_target when you need to wait for the device to
 * start advertising (e.g., after deep sleep wake) rather than doing direct
 * connect which requires the device to already be connectable.
 *
 * @param transport Transport instance
 * @param addr Filter address (NULL to connect to any matching device)
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_l2cap_client_set_filter(struct ninep_transport *transport,
					    const bt_addr_le_t *addr);

/**
 * @brief Enable filter accept list scanning with IRK resolution
 *
 * When enabled, scanning uses the BLE filter accept list (whitelist).
 * The BLE stack automatically resolves Random Resolvable Addresses (RPA)
 * using stored IRKs from bonding. Only devices in the accept list (or
 * whose RPA resolves to an identity in the list) will be reported.
 *
 * This is required for reconnecting to bonded devices that use BLE privacy.
 * Before calling this, add the bonded device's identity address to the
 * accept list using bt_le_filter_accept_list_add().
 *
 * @param transport Transport instance
 * @param enable true to enable accept list scanning, false to disable
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_l2cap_client_set_accept_list(struct ninep_transport *transport,
						 bool enable);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_L2CAP_CLIENT_H_ */
