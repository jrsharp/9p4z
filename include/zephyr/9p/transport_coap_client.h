/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_COAP_CLIENT_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_COAP_CLIENT_H_

#include <zephyr/9p/transport.h>
#include <zephyr/net/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport_coap_client 9P CoAP Client Transport
 * @ingroup ninep_transport
 * @{
 */

/**
 * @brief CoAP client transport configuration
 *
 * This transport operates in CLIENT mode, connecting to a cloud-hosted CoAP
 * server. It uses CoAP Observe (RFC 7641) to receive 9P requests from the
 * cloud as push notifications, enabling NAT traversal.
 *
 * Architecture:
 *   [Device CoAP Client] --Observe--> [Cloud CoAP Server] <--9P--> [Users]
 *
 * The device initiates the connection, avoiding NAT issues. The cloud server
 * sends 9P requests as CoAP Observe notifications, and the device responds
 * via separate CoAP POST requests.
 *
 * Use cases:
 *   - Device behind home/office NAT
 *   - Remote access from anywhere
 *   - IoT cloud platforms
 *   - Mobile app gateway scenarios
 */
struct ninep_transport_coap_client_config {
	/** Cloud CoAP server address (IPv4 or IPv6) */
	struct sockaddr *server_addr;
	/** Size of server_addr structure */
	socklen_t server_addr_len;
	/** Device identifier (used in resource path) */
	const char *device_id;
	/** CoAP resource path for inbox (default: "/device/{id}/inbox") */
	const char *inbox_path;
	/** CoAP resource path for outbox (default: "/device/{id}/outbox") */
	const char *outbox_path;
	/** Maximum receive buffer size */
	size_t rx_buf_size;
};

/**
 * @brief Initialize CoAP client transport
 *
 * Creates a CoAP client transport that connects to a cloud CoAP server
 * and registers for Observe notifications. The device acts as a CoAP client,
 * initiating the connection to traverse NAT.
 *
 * When the cloud has a 9P request for this device, it sends it as a CoAP
 * Observe notification. The device processes it and POSTs the response back.
 *
 * Zephyr's CoAP client library handles:
 *   - Connection management
 *   - Observe registration and renewals
 *   - Notification reception
 *   - Block-wise transfer (for large messages)
 *   - Deduplication
 *
 * @param transport Transport instance
 * @param config CoAP client configuration
 * @param recv_cb Receive callback (called when cloud sends 9P request)
 * @param user_data User context pointer
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_coap_client_init(struct ninep_transport *transport,
                                      const struct ninep_transport_coap_client_config *config,
                                      ninep_transport_recv_cb_t recv_cb,
                                      void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_COAP_CLIENT_H_ */
