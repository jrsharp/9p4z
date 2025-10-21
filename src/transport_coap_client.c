/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_coap_client.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_coap_client_transport, CONFIG_NINEP_LOG_LEVEL);

/* CoAP client transport private data */
struct coap_client_transport_data {
	struct coap_client client;
	int sock;
	struct sockaddr_storage server_addr;
	socklen_t server_addr_len;
	char device_id[64];
	char inbox_path[128];
	char outbox_path[128];
	uint8_t *rx_buf;
	size_t rx_buf_size;
	struct ninep_transport *transport;  /* Back-pointer */
	bool observe_active;

	/* Response handling */
	struct k_sem response_sent_sem;
	uint8_t *pending_response;
	size_t pending_response_len;
};

/**
 * @brief Callback for CoAP POST response (when sending 9P response to cloud)
 *
 * This is called when the cloud acknowledges our 9P response.
 */
static void response_post_cb(int16_t result_code, size_t offset,
                             const uint8_t *payload, size_t len,
                             bool last_block, void *user_data)
{
	struct coap_client_transport_data *data = user_data;

	if (result_code < 0) {
		LOG_ERR("Failed to send response to cloud: %d", result_code);
	} else {
		LOG_DBG("Response sent to cloud: code=%d", result_code);
	}

	/* Signal that response transmission is complete */
	k_sem_give(&data->response_sent_sem);
}

/**
 * @brief Callback for CoAP Observe notifications (9P requests from cloud)
 *
 * This is called by Zephyr's CoAP client when the cloud sends us a 9P request
 * via Observe notification. Zephyr handles all the complexity - we just get
 * the payload delivered here.
 */
static void observe_notification_cb(int16_t result_code, size_t offset,
                                    const uint8_t *payload, size_t len,
                                    bool last_block, void *user_data)
{
	struct coap_client_transport_data *data = user_data;

	if (result_code < 0) {
		LOG_ERR("Observe notification error: %d", result_code);
		return;
	}

	/* Handle blockwise transfer */
	if (!last_block) {
		/* This is a partial block - Zephyr is assembling the message.
		 * We'll get called again with the next block. */
		LOG_DBG("Received block at offset %zu, len %zu (more blocks coming)",
		        offset, len);

		/* Accumulate in rx_buf */
		if (offset + len <= data->rx_buf_size) {
			memcpy(data->rx_buf + offset, payload, len);
		} else {
			LOG_ERR("Message too large: %zu + %zu > %zu",
			        offset, len, data->rx_buf_size);
		}
		return;
	}

	/* Last block received - complete message */
	size_t total_len = offset + len;

	LOG_DBG("Received complete 9P request from cloud: %zu bytes", total_len);

	/* Copy final block if using blockwise transfer */
	if (offset > 0) {
		if (offset + len <= data->rx_buf_size) {
			memcpy(data->rx_buf + offset, payload, len);
			payload = data->rx_buf;
			len = total_len;
		} else {
			LOG_ERR("Message too large: %zu", total_len);
			return;
		}
	}

	/* Deliver to 9P server - this will synchronously call our send() */
	if (data->transport->recv_cb) {
		data->transport->recv_cb(data->transport, payload, len,
		                          data->transport->user_data);
	}
}

/**
 * @brief Send 9P response to cloud via CoAP POST
 *
 * This is called by the 9P server when it has a response to send back.
 * We POST it to the cloud's outbox path.
 */
static int coap_client_send(struct ninep_transport *transport,
                            const uint8_t *buf, size_t len)
{
	struct coap_client_transport_data *data = transport->priv_data;
	int ret;

	LOG_DBG("Sending 9P response to cloud: %zu bytes", len);

	/* Prepare CoAP POST request */
	struct coap_client_request req = {
		.method = COAP_METHOD_POST,
		.confirmable = true,
		.path = data->outbox_path,
		.fmt = COAP_CONTENT_FORMAT_APP_OCTET_STREAM,
		.payload = buf,
		.len = len,
		.cb = response_post_cb,
		.user_data = data,
	};

	/* Reset semaphore */
	k_sem_reset(&data->response_sent_sem);

	/* Send the request - Zephyr handles blockwise transfer automatically */
	ret = coap_client_req(&data->client, data->sock,
	                      (struct sockaddr *)&data->server_addr, &req, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to send CoAP request: %d", ret);
		return ret;
	}

	/* Wait for response to be sent (with timeout) */
	ret = k_sem_take(&data->response_sent_sem, K_SECONDS(30));
	if (ret < 0) {
		LOG_ERR("Timeout waiting for response confirmation");
		return -ETIMEDOUT;
	}

	return len;
}

/**
 * @brief Start CoAP client transport
 *
 * Connects to cloud CoAP server and registers for Observe notifications.
 */
static int coap_client_start(struct ninep_transport *transport)
{
	struct coap_client_transport_data *data = transport->priv_data;
	int ret;

	/* Initialize CoAP client */
	ret = coap_client_init(&data->client, "9p_coap_client");
	if (ret < 0) {
		LOG_ERR("Failed to initialize CoAP client: %d", ret);
		return ret;
	}

	/* Create UDP socket */
	int family = ((struct sockaddr *)&data->server_addr)->sa_family;
	data->sock = zsock_socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (data->sock < 0) {
		LOG_ERR("Failed to create socket: %d", errno);
		return -errno;
	}

	/* Log connection info */
	if (family == AF_INET6) {
		char addr_str[INET6_ADDRSTRLEN];
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&data->server_addr;
		zsock_inet_ntop(AF_INET6, &addr6->sin6_addr, addr_str, sizeof(addr_str));
		LOG_INF("Connecting to cloud CoAP server: [%s]:%d",
		        addr_str, ntohs(addr6->sin6_port));
	} else {
		char addr_str[INET_ADDRSTRLEN];
		struct sockaddr_in *addr4 = (struct sockaddr_in *)&data->server_addr;
		zsock_inet_ntop(AF_INET, &addr4->sin_addr, addr_str, sizeof(addr_str));
		LOG_INF("Connecting to cloud CoAP server: %s:%d",
		        addr_str, ntohs(addr4->sin_port));
	}

	/* Set up Observe option */
	struct coap_client_option observe_opt = {
		.code = COAP_OPTION_OBSERVE,
		.len = 1,
		.value = {0},  /* 0 = register for observe */
	};

	/* Prepare Observe GET request */
	struct coap_client_request req = {
		.method = COAP_METHOD_GET,
		.confirmable = true,
		.path = data->inbox_path,
		.cb = observe_notification_cb,
		.options = &observe_opt,
		.num_options = 1,
		.user_data = data,
	};

	/* Register for Observe - Zephyr handles everything from here! */
	ret = coap_client_req(&data->client, data->sock,
	                      (struct sockaddr *)&data->server_addr, &req, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register Observe: %d", ret);
		zsock_close(data->sock);
		return ret;
	}

	data->observe_active = true;

	LOG_INF("Successfully registered for Observe on %s", data->inbox_path);
	LOG_INF("Device ready to receive 9P requests from cloud");

	return 0;
}

/**
 * @brief Stop CoAP client transport
 *
 * Cancels Observe registration and closes connection.
 */
static int coap_client_stop(struct ninep_transport *transport)
{
	struct coap_client_transport_data *data = transport->priv_data;

	LOG_INF("Stopping CoAP client transport");

	if (data->observe_active) {
		/* Cancel all requests (including Observe) */
		coap_client_cancel_requests(&data->client);
		data->observe_active = false;
	}

	if (data->sock >= 0) {
		zsock_close(data->sock);
		data->sock = -1;
	}

	return 0;
}

/**
 * @brief Get transport MTU
 *
 * With CoAP client and blockwise transfer, we can handle the full message size.
 * Zephyr's CoAP client handles fragmentation automatically.
 */
static int coap_client_get_mtu(struct ninep_transport *transport)
{
	ARG_UNUSED(transport);
	return CONFIG_NINEP_MAX_MESSAGE_SIZE;
}

static const struct ninep_transport_ops coap_client_transport_ops = {
	.send = coap_client_send,
	.start = coap_client_start,
	.stop = coap_client_stop,
	.get_mtu = coap_client_get_mtu,
};

int ninep_transport_coap_client_init(struct ninep_transport *transport,
                                      const struct ninep_transport_coap_client_config *config,
                                      ninep_transport_recv_cb_t recv_cb,
                                      void *user_data)
{
	if (!transport || !config || !config->server_addr || !config->device_id) {
		return -EINVAL;
	}

	/* Allocate private data */
	struct coap_client_transport_data *data = k_malloc(sizeof(*data));
	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));

	/* Allocate RX buffer */
	if (config->rx_buf_size == 0) {
		k_free(data);
		return -EINVAL;
	}

	data->rx_buf = k_malloc(config->rx_buf_size);
	if (!data->rx_buf) {
		k_free(data);
		return -ENOMEM;
	}

	data->rx_buf_size = config->rx_buf_size;
	data->sock = -1;
	data->transport = transport;

	/* Initialize semaphore for response synchronization */
	k_sem_init(&data->response_sent_sem, 0, 1);

	/* Copy server address */
	memcpy(&data->server_addr, config->server_addr, config->server_addr_len);
	data->server_addr_len = config->server_addr_len;

	/* Copy device ID */
	strncpy(data->device_id, config->device_id, sizeof(data->device_id) - 1);

	/* Build resource paths */
	if (config->inbox_path) {
		strncpy(data->inbox_path, config->inbox_path, sizeof(data->inbox_path) - 1);
	} else {
		snprintf(data->inbox_path, sizeof(data->inbox_path),
		         "/device/%s/inbox", data->device_id);
	}

	if (config->outbox_path) {
		strncpy(data->outbox_path, config->outbox_path, sizeof(data->outbox_path) - 1);
	} else {
		snprintf(data->outbox_path, sizeof(data->outbox_path),
		         "/device/%s/outbox", data->device_id);
	}

	/* Initialize transport */
	transport->ops = &coap_client_transport_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

	LOG_INF("CoAP client transport initialized");
	LOG_INF("  Device ID: %s", data->device_id);
	LOG_INF("  Inbox:  %s", data->inbox_path);
	LOG_INF("  Outbox: %s", data->outbox_path);
	LOG_INF("  RX buffer: %zu bytes", data->rx_buf_size);

	return 0;
}
