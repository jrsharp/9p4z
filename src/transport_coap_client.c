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

/*
 * Thread pool for processing 9P messages received via CoAP Observe.
 *
 * Following the L2CAP transport pattern: Observe notifications arrive on the
 * Zephyr CoAP client thread.  We copy the payload and queue it so that the
 * CoAP thread is released immediately.  Worker threads pick up queued
 * messages, invoke the 9P server (which may block), and POST the response
 * back to the cloud.
 */

/* Work item queued from observe callback to worker thread */
struct coap_client_work_item {
	struct ninep_transport *transport;
	size_t msg_len;
	uint8_t *msg_buf;  /* Allocated copy — worker must free */
};

/* Context passed through coap_client_req for non-blocking POST */
struct coap_client_send_ctx {
	struct coap_client_transport_data *data;
	uint8_t *payload_copy;  /* Heap copy of tx_buf — freed in POST callback */
	size_t payload_len;
};

K_MSGQ_DEFINE(coap_client_msg_queue,
              sizeof(struct coap_client_work_item),
              CONFIG_NINEP_COAP_CLIENT_MSG_QUEUE_SIZE, 4);

K_THREAD_STACK_ARRAY_DEFINE(coap_client_thread_stacks,
                            CONFIG_NINEP_COAP_CLIENT_THREAD_POOL_SIZE,
                            CONFIG_NINEP_COAP_CLIENT_THREAD_STACK_SIZE);
static struct k_thread coap_client_threads[CONFIG_NINEP_COAP_CLIENT_THREAD_POOL_SIZE];
static bool coap_client_thread_pool_started;

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
};

/* Worker thread — dequeues messages and delivers them to the 9P server */
static void coap_client_thread_pool_worker(void *arg1, void *arg2, void *arg3)
{
	int thread_id = (int)(intptr_t)arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("CoAP 9P worker thread %d started", thread_id);

	while (1) {
		struct coap_client_work_item item;

		int ret = k_msgq_get(&coap_client_msg_queue, &item, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("Worker %d: msgq_get failed: %d", thread_id, ret);
			continue;
		}

		if (!item.msg_buf || item.msg_len == 0) {
			LOG_ERR("Worker %d: invalid work item", thread_id);
			continue;
		}

		LOG_INF("Worker %d: processing 9P msg %zu bytes, type=0x%02x",
		        thread_id, item.msg_len, item.msg_buf[4]);

		if (item.transport->recv_cb) {
			item.transport->recv_cb(item.transport, item.msg_buf,
			                        item.msg_len,
			                        item.transport->user_data);
		}

		k_free(item.msg_buf);

		LOG_DBG("Worker %d: done processing", thread_id);
	}
}

/* Start thread pool if not already running */
static void coap_client_start_thread_pool(void)
{
	if (coap_client_thread_pool_started) {
		return;
	}

	for (int i = 0; i < CONFIG_NINEP_COAP_CLIENT_THREAD_POOL_SIZE; i++) {
		k_thread_create(&coap_client_threads[i],
		                coap_client_thread_stacks[i],
		                K_THREAD_STACK_SIZEOF(coap_client_thread_stacks[i]),
		                coap_client_thread_pool_worker,
		                (void *)(intptr_t)i, NULL, NULL,
		                CONFIG_NINEP_COAP_CLIENT_THREAD_PRIORITY, 0,
		                K_NO_WAIT);

		char name[24];
		snprintf(name, sizeof(name), "coap_9p_worker_%d", i);
		k_thread_name_set(&coap_client_threads[i], name);
	}

	coap_client_thread_pool_started = true;
	LOG_INF("Started CoAP 9P thread pool: %d threads, stack=%d, prio=%d",
	        CONFIG_NINEP_COAP_CLIENT_THREAD_POOL_SIZE,
	        CONFIG_NINEP_COAP_CLIENT_THREAD_STACK_SIZE,
	        CONFIG_NINEP_COAP_CLIENT_THREAD_PRIORITY);
}

/**
 * @brief Callback for CoAP POST response (when sending 9P response to cloud)
 *
 * Frees the heap-allocated payload copy and context on completion or error.
 */
static void response_post_cb(int16_t result_code, size_t offset,
                             const uint8_t *payload, size_t len,
                             bool last_block, void *user_data)
{
	struct coap_client_send_ctx *ctx = user_data;

	if (result_code < 0) {
		LOG_ERR("Failed to send response to cloud: %d", result_code);
	} else {
		LOG_DBG("Response sent to cloud: code=%d", result_code);
	}

	if (last_block || result_code < 0) {
		if (ctx) {
			k_free(ctx->payload_copy);
			k_free(ctx);
		}
	}
}

/**
 * @brief Callback for CoAP Observe notifications (9P requests from cloud)
 *
 * Copies the complete message and queues it for a worker thread.
 * Returns immediately so the Zephyr CoAP client thread is not blocked.
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

	/* Handle blockwise transfer — accumulate partial blocks */
	if (!last_block) {
		LOG_DBG("Received block at offset %zu, len %zu (more blocks coming)",
		        offset, len);

		if (offset + len <= data->rx_buf_size) {
			memcpy(data->rx_buf + offset, payload, len);
		} else {
			LOG_ERR("Message too large: %zu + %zu > %zu",
			        offset, len, data->rx_buf_size);
		}
		return;
	}

	/* Last block — assemble complete message */
	size_t total_len = offset + len;
	const uint8_t *complete_msg;

	if (offset > 0) {
		/* Blockwise: copy final block into rx_buf */
		if (offset + len <= data->rx_buf_size) {
			memcpy(data->rx_buf + offset, payload, len);
			complete_msg = data->rx_buf;
		} else {
			LOG_ERR("Message too large: %zu", total_len);
			return;
		}
	} else {
		/* Single-block message — payload is already complete */
		complete_msg = payload;
		total_len = len;
	}

	LOG_DBG("Received complete 9P request from cloud: %zu bytes", total_len);

	/* Allocate a copy so the CoAP client thread can return immediately */
	uint8_t *msg_copy = k_malloc(total_len);
	if (!msg_copy) {
		LOG_ERR("Failed to allocate %zu bytes for message copy", total_len);
		return;
	}
	memcpy(msg_copy, complete_msg, total_len);

	struct coap_client_work_item item = {
		.transport = data->transport,
		.msg_len = total_len,
		.msg_buf = msg_copy,
	};

	int ret = k_msgq_put(&coap_client_msg_queue, &item, K_MSEC(100));
	if (ret != 0) {
		LOG_ERR("Failed to queue 9P message: %d (queue full?)", ret);
		k_free(msg_copy);
	}
}

/**
 * @brief Send 9P response to cloud via CoAP POST (non-blocking)
 *
 * Copies the payload from the server's tx_buf into a heap allocation so
 * the server can release its tx_buf_mutex immediately.  The copy is freed
 * in response_post_cb when the cloud acknowledges the POST.
 */
static int coap_client_send(struct ninep_transport *transport,
                            const uint8_t *buf, size_t len)
{
	struct coap_client_transport_data *data = transport->priv_data;

	LOG_DBG("Sending 9P response to cloud: %zu bytes", len);

	/* Allocate context + payload copy */
	struct coap_client_send_ctx *ctx = k_malloc(sizeof(*ctx));
	if (!ctx) {
		LOG_ERR("Failed to allocate send context");
		return -ENOMEM;
	}

	ctx->payload_copy = k_malloc(len);
	if (!ctx->payload_copy) {
		LOG_ERR("Failed to allocate %zu bytes for payload copy", len);
		k_free(ctx);
		return -ENOMEM;
	}

	memcpy(ctx->payload_copy, buf, len);
	ctx->payload_len = len;
	ctx->data = data;

	/* Prepare CoAP POST with the heap copy as payload */
	struct coap_client_request req = {
		.method = COAP_METHOD_POST,
		.confirmable = true,
		.path = data->outbox_path,
		.fmt = COAP_CONTENT_FORMAT_APP_OCTET_STREAM,
		.payload = ctx->payload_copy,
		.len = len,
		.cb = response_post_cb,
		.user_data = ctx,
	};

	int ret = coap_client_req(&data->client, data->sock,
	                          (struct sockaddr *)&data->server_addr, &req, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to send CoAP request: %d", ret);
		k_free(ctx->payload_copy);
		k_free(ctx);
		return ret;
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

	/* Drain any pending messages */
	k_msgq_purge(&coap_client_msg_queue);

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

	/* Start thread pool before first use */
	coap_client_start_thread_pool();

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
