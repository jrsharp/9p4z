/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_COAP_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_COAP_H_

#include <zephyr/9p/transport.h>
#include <zephyr/net/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport_coap 9P CoAP Transport
 * @ingroup ninep_transport
 * @{
 */

/**
 * @brief CoAP transport configuration
 *
 * This transport tunnels 9P messages over CoAP using confirmable (CON) messages
 * for reliable delivery over UDP. Large messages are handled using RFC 7959
 * block-wise transfer when they exceed the CoAP MTU.
 *
 * The transport exposes a single CoAP resource (/9p) that accepts POST requests
 * containing 9P Tmessages and returns 9P Rmessages in the response.
 */
struct ninep_transport_coap_config {
	/** Local UDP address to bind to (IPv4 or IPv6) */
	struct sockaddr *local_addr;
	/** Maximum receive buffer size (should be >= CONFIG_NINEP_MAX_MESSAGE_SIZE) */
	size_t rx_buf_size;
	/** CoAP resource path (default: "/9p") */
	const char *resource_path;
};

/**
 * @brief Initialize CoAP transport
 *
 * Creates a CoAP transport that serves 9P over UDP using CoAP confirmable
 * messages for reliability. The transport registers a CoAP resource at the
 * specified path (default: "/9p") and handles 9P protocol messages via
 * POST requests.
 *
 * Block-wise transfer (RFC 7959) is automatically used for messages larger
 * than the CoAP MTU (~1KB).
 *
 * @param transport Transport instance
 * @param config CoAP configuration
 * @param recv_cb Receive callback (called when complete 9P message received)
 * @param user_data User context pointer
 * @return 0 on success, negative error code on failure
 */
int ninep_transport_coap_init(struct ninep_transport *transport,
                               const struct ninep_transport_coap_config *config,
                               ninep_transport_recv_cb_t recv_cb,
                               void *user_data);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_COAP_H_ */
