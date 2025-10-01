/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_9P_TRANSPORT_H_
#define ZEPHYR_INCLUDE_9P_TRANSPORT_H_

#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_transport 9P Transport Abstraction
 * @ingroup ninep
 * @{
 */

struct ninep_transport;

/**
 * @brief Transport receive callback
 *
 * Called when a complete 9P message has been received.
 *
 * @param transport Transport instance
 * @param buf Buffer containing received message
 * @param len Length of received message
 * @param user_data User context pointer
 */
typedef void (*ninep_transport_recv_cb_t)(struct ninep_transport *transport,
                                          const uint8_t *buf, size_t len,
                                          void *user_data);

/**
 * @brief Transport operations
 */
struct ninep_transport_ops {
	/**
	 * @brief Send a message
	 *
	 * @param transport Transport instance
	 * @param buf Buffer containing message to send
	 * @param len Length of message
	 * @return Number of bytes sent, or negative error code
	 */
	int (*send)(struct ninep_transport *transport, const uint8_t *buf,
	            size_t len);

	/**
	 * @brief Start receiving messages
	 *
	 * @param transport Transport instance
	 * @return 0 on success, negative error code on failure
	 */
	int (*start)(struct ninep_transport *transport);

	/**
	 * @brief Stop receiving messages
	 *
	 * @param transport Transport instance
	 * @return 0 on success, negative error code on failure
	 */
	int (*stop)(struct ninep_transport *transport);
};

/**
 * @brief Transport instance
 */
struct ninep_transport {
	const struct ninep_transport_ops *ops;
	ninep_transport_recv_cb_t recv_cb;
	void *user_data;
	void *priv_data;  /* transport-specific private data */
};

/**
 * @brief Initialize a transport
 *
 * @param transport Transport instance
 * @param ops Transport operations
 * @param recv_cb Receive callback
 * @param user_data User context pointer
 */
void ninep_transport_init(struct ninep_transport *transport,
                          const struct ninep_transport_ops *ops,
                          ninep_transport_recv_cb_t recv_cb,
                          void *user_data);

/**
 * @brief Send a message via transport
 *
 * @param transport Transport instance
 * @param buf Buffer containing message to send
 * @param len Length of message
 * @return Number of bytes sent, or negative error code
 */
static inline int ninep_transport_send(struct ninep_transport *transport,
                                       const uint8_t *buf, size_t len)
{
	if (!transport || !transport->ops || !transport->ops->send) {
		return -EINVAL;
	}
	return transport->ops->send(transport, buf, len);
}

/**
 * @brief Start receiving messages
 *
 * @param transport Transport instance
 * @return 0 on success, negative error code on failure
 */
static inline int ninep_transport_start(struct ninep_transport *transport)
{
	if (!transport || !transport->ops || !transport->ops->start) {
		return -EINVAL;
	}
	return transport->ops->start(transport);
}

/**
 * @brief Stop receiving messages
 *
 * @param transport Transport instance
 * @return 0 on success, negative error code on failure
 */
static inline int ninep_transport_stop(struct ninep_transport *transport)
{
	if (!transport || !transport->ops || !transport->ops->stop) {
		return -EINVAL;
	}
	return transport->ops->stop(transport);
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_TRANSPORT_H_ */