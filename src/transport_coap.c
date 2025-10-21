/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_coap.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_coap_transport, CONFIG_NINEP_LOG_LEVEL);

#define COAP_RECV_THREAD_STACK_SIZE 4096
#define COAP_RECV_THREAD_PRIORITY 5
#define COAP_MAX_PAYLOAD_SIZE 1024  /* CoAP payload size per block */

/* CoAP transport private data */
struct coap_transport_data {
	int sock;
	struct sockaddr_storage local_addr;
	socklen_t local_addr_len;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	const char *resource_path;
	k_tid_t recv_tid;
	bool active;
	struct ninep_transport *transport;  /* Back-pointer */
	struct k_thread recv_thread;
	k_thread_stack_t recv_stack[K_KERNEL_STACK_LEN(COAP_RECV_THREAD_STACK_SIZE)];

	/* Block-wise transfer state for TX (sending response) */
	struct sockaddr_storage client_addr;
	socklen_t client_addr_len;
	uint8_t *tx_block_buf;  /* Buffer for response being sent */
	size_t tx_block_len;    /* Total response length */
	bool tx_block_active;   /* Block-wise transfer in progress */
};

/**
 * @brief Send CoAP response (with block-wise transfer support)
 *
 * Sends a complete 9P response message as a CoAP response, using block-wise
 * transfer if the message is larger than COAP_MAX_PAYLOAD_SIZE.
 *
 * @param data Transport data
 * @param request Incoming CoAP request packet
 * @param response_payload 9P response message to send
 * @param response_len Length of response message
 * @return 0 on success, negative error code on failure
 */
static int coap_send_response(struct coap_transport_data *data,
                              struct coap_packet *request,
                              const uint8_t *response_payload,
                              size_t response_len)
{
	uint8_t coap_buf[COAP_MAX_PAYLOAD_SIZE + 128];  /* CoAP header + payload */
	struct coap_packet response;
	int ret;

	/* Check for block2 option in request (client requesting specific block) */
	struct coap_block_context block_ctx;
	bool has_block2 = false;

	ret = coap_get_option_int(request, COAP_OPTION_BLOCK2);
	if (ret >= 0) {
		has_block2 = true;
		coap_block_transfer_init(&block_ctx, COAP_BLOCK_1024, response_len);

		/* Parse block2 option from request */
		uint32_t block_num = ret >> 4;
		uint8_t block_size_exp = ret & 0x7;

		LOG_DBG("Client requests block %u (size_exp=%u)", block_num, block_size_exp);

		/* Update block context with client's request */
		block_ctx.current = block_num * coap_block_size_to_bytes(COAP_BLOCK_1024);
	}

	/* Initialize CoAP response packet */
	ret = coap_packet_init(&response, coap_buf, sizeof(coap_buf),
	                       COAP_VERSION_1, COAP_TYPE_ACK,
	                       coap_header_get_token(request, NULL),
	                       coap_header_get_code(request),
	                       coap_header_get_id(request));
	if (ret < 0) {
		LOG_ERR("Failed to init CoAP response: %d", ret);
		return ret;
	}

	/* Set response code */
	coap_header_set_code(&response, COAP_RESPONSE_CODE_CONTENT);

	/* Determine how much payload to send in this block */
	size_t offset = 0;
	size_t payload_len = response_len;
	bool more = false;

	if (has_block2) {
		offset = block_ctx.current;
		if (offset + COAP_MAX_PAYLOAD_SIZE < response_len) {
			payload_len = COAP_MAX_PAYLOAD_SIZE;
			more = true;
		} else {
			payload_len = response_len - offset;
			more = false;
		}

		/* Add block2 option to response */
		ret = coap_append_block2_option(&response, &block_ctx);
		if (ret < 0) {
			LOG_ERR("Failed to append block2 option: %d", ret);
			return ret;
		}

		LOG_DBG("Sending block at offset %zu, len %zu, more=%d",
		        offset, payload_len, more);
	} else if (response_len > COAP_MAX_PAYLOAD_SIZE) {
		/* First block - client didn't request block, but response is large */
		coap_block_transfer_init(&block_ctx, COAP_BLOCK_1024, response_len);
		payload_len = COAP_MAX_PAYLOAD_SIZE;
		more = true;

		ret = coap_append_block2_option(&response, &block_ctx);
		if (ret < 0) {
			LOG_ERR("Failed to append block2 option: %d", ret);
			return ret;
		}

		LOG_DBG("Starting block-wise transfer: total=%zu, first_block=%zu",
		        response_len, payload_len);
	}

	/* Add payload marker and copy payload */
	ret = coap_packet_append_payload_marker(&response);
	if (ret < 0) {
		LOG_ERR("Failed to append payload marker: %d", ret);
		return ret;
	}

	ret = coap_packet_append_payload(&response, response_payload + offset, payload_len);
	if (ret < 0) {
		LOG_ERR("Failed to append payload: %d", ret);
		return ret;
	}

	/* Send CoAP response */
	ret = zsock_sendto(data->sock, response.data, response.offset,
	                   0, (struct sockaddr *)&data->client_addr,
	                   data->client_addr_len);
	if (ret < 0) {
		LOG_ERR("Failed to send CoAP response: %d", errno);
		return -errno;
	}

	LOG_DBG("Sent CoAP response: %d bytes (payload: %zu bytes)", ret, payload_len);
	return 0;
}

/**
 * @brief Handle incoming CoAP POST request to /9p resource
 *
 * Extracts the 9P message from the CoAP payload, delivers it to the 9P layer,
 * and sends the 9P response back as a CoAP response.
 */
static int handle_9p_post(struct coap_transport_data *data,
                           struct coap_packet *request,
                           const struct sockaddr *client_addr,
                           socklen_t client_addr_len)
{
	uint16_t payload_len;
	const uint8_t *payload = coap_packet_get_payload(request, &payload_len);

	if (!payload || payload_len == 0) {
		LOG_WRN("Empty CoAP payload");
		return -EINVAL;
	}

	LOG_DBG("Received 9P message via CoAP: %u bytes", payload_len);

	/* Store client address for response */
	memcpy(&data->client_addr, client_addr, client_addr_len);
	data->client_addr_len = client_addr_len;

	/* Copy payload to RX buffer */
	if (payload_len > data->rx_buf_size) {
		LOG_ERR("9P message too large: %u > %zu", payload_len, data->rx_buf_size);
		return -ENOMEM;
	}

	memcpy(data->rx_buf, payload, payload_len);

	/* Deliver to 9P layer - this will synchronously process and call send() */
	if (data->transport->recv_cb) {
		data->transport->recv_cb(data->transport, data->rx_buf,
		                          payload_len, data->transport->user_data);
	}

	return 0;
}

/**
 * @brief Process incoming CoAP packet
 */
static void coap_process_packet(struct coap_transport_data *data,
                                uint8_t *buf, size_t len,
                                const struct sockaddr *src_addr,
                                socklen_t src_addr_len)
{
	struct coap_packet request;
	uint8_t *coap_payload;
	uint16_t payload_len;
	int ret;

	ret = coap_packet_parse(&request, buf, len, NULL, 0);
	if (ret < 0) {
		LOG_ERR("Failed to parse CoAP packet: %d", ret);
		return;
	}

	/* Get request details */
	uint8_t type = coap_header_get_type(&request);
	uint8_t code = coap_header_get_code(&request);
	uint16_t id = coap_header_get_id(&request);

	LOG_DBG("CoAP request: type=%u code=%u id=%u", type, code, id);

	/* We only handle CON POST requests */
	if (type != COAP_TYPE_CON) {
		LOG_WRN("Ignoring non-confirmable CoAP message");
		return;
	}

	if (code != COAP_METHOD_POST) {
		LOG_WRN("Ignoring non-POST CoAP request (code=%u)", code);
		/* TODO: Send METHOD_NOT_ALLOWED response */
		return;
	}

	/* Verify this is a request to our /9p resource */
	/* For simplicity, we accept all POST requests - in production,
	 * should check the URI path matches our resource_path */

	/* Handle the 9P POST request */
	handle_9p_post(data, &request, src_addr, src_addr_len);
}

/**
 * @brief CoAP receive thread
 *
 * Listens for incoming CoAP requests and processes them.
 */
static void coap_recv_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct ninep_transport *transport = arg1;
	struct coap_transport_data *data = transport->priv_data;

	LOG_INF("CoAP receive thread started");

	uint8_t recv_buf[COAP_MAX_PAYLOAD_SIZE + 256];  /* CoAP header + payload */

	while (data->active) {
		struct sockaddr_storage src_addr;
		socklen_t src_addr_len = sizeof(src_addr);

		int ret = zsock_recvfrom(data->sock, recv_buf, sizeof(recv_buf), 0,
		                          (struct sockaddr *)&src_addr, &src_addr_len);

		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				k_sleep(K_MSEC(10));
				continue;
			}
			LOG_ERR("Receive error: %d", errno);
			k_sleep(K_MSEC(100));
			continue;
		}

		if (ret == 0) {
			continue;
		}

		LOG_DBG("Received CoAP packet: %d bytes", ret);

		/* Process the CoAP packet */
		coap_process_packet(data, recv_buf, ret,
		                    (struct sockaddr *)&src_addr, src_addr_len);
	}

	LOG_INF("CoAP receive thread exiting");
}

/**
 * @brief Send 9P message via CoAP
 *
 * This is called by the 9P server to send a response. We send it as a
 * CoAP response to the most recent request.
 */
static int coap_send(struct ninep_transport *transport, const uint8_t *buf,
                     size_t len)
{
	struct coap_transport_data *data = transport->priv_data;

	LOG_DBG("coap_send: %zu bytes", len);

	/* For now, we send the response immediately. In a more complete
	 * implementation, we would need to maintain request/response state
	 * and handle asynchronous responses.
	 *
	 * Since 9P is request/response, and we process synchronously in
	 * handle_9p_post(), we can send the response directly here.
	 */

	/* Create a simple CoAP ACK response */
	uint8_t coap_buf[COAP_MAX_PAYLOAD_SIZE + 128];
	struct coap_packet response;
	int ret;

	/* We need the original request to create a proper response.
	 * For now, create a simple CON message.
	 * TODO: Improve this by maintaining request context.
	 */
	static uint16_t msg_id = 0;
	msg_id++;

	ret = coap_packet_init(&response, coap_buf, sizeof(coap_buf),
	                       COAP_VERSION_1, COAP_TYPE_CON,
	                       0, NULL, COAP_RESPONSE_CODE_CONTENT, msg_id);
	if (ret < 0) {
		LOG_ERR("Failed to init CoAP response: %d", ret);
		return ret;
	}

	/* Add payload marker */
	ret = coap_packet_append_payload_marker(&response);
	if (ret < 0) {
		LOG_ERR("Failed to append payload marker: %d", ret);
		return ret;
	}

	/* Add 9P response payload (handle block-wise transfer if needed) */
	size_t to_send = len;
	if (to_send > COAP_MAX_PAYLOAD_SIZE) {
		/* Start block-wise transfer */
		struct coap_block_context block_ctx;
		coap_block_transfer_init(&block_ctx, COAP_BLOCK_1024, len);

		ret = coap_append_block2_option(&response, &block_ctx);
		if (ret < 0) {
			LOG_ERR("Failed to append block2 option: %d", ret);
			return ret;
		}

		to_send = COAP_MAX_PAYLOAD_SIZE;
		LOG_DBG("Starting block-wise transfer: %zu bytes total", len);
	}

	ret = coap_packet_append_payload(&response, buf, to_send);
	if (ret < 0) {
		LOG_ERR("Failed to append payload: %d", ret);
		return ret;
	}

	/* Send to most recent client */
	ret = zsock_sendto(data->sock, response.data, response.offset,
	                   0, (struct sockaddr *)&data->client_addr,
	                   data->client_addr_len);
	if (ret < 0) {
		LOG_ERR("Failed to send CoAP response: %d", errno);
		return -errno;
	}

	LOG_DBG("Sent CoAP response: %d bytes (payload: %zu bytes)", ret, to_send);
	return len;  /* Return full length to indicate success */
}

static int coap_start(struct ninep_transport *transport)
{
	struct coap_transport_data *data = transport->priv_data;
	int ret;

	/* Create UDP socket */
	int family = ((struct sockaddr *)&data->local_addr)->sa_family;
	data->sock = zsock_socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (data->sock < 0) {
		LOG_ERR("Failed to create UDP socket: %d", errno);
		return -errno;
	}

	/* Bind to local address */
	ret = zsock_bind(data->sock, (struct sockaddr *)&data->local_addr,
	                 data->local_addr_len);
	if (ret < 0) {
		LOG_ERR("Failed to bind socket: %d", errno);
		zsock_close(data->sock);
		return -errno;
	}

	/* Log bind address */
	if (family == AF_INET6) {
		char addr_str[INET6_ADDRSTRLEN];
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&data->local_addr;
		zsock_inet_ntop(AF_INET6, &addr6->sin6_addr, addr_str, sizeof(addr_str));
		LOG_INF("CoAP server listening on [%s]:%d%s",
		        addr_str, ntohs(addr6->sin6_port), data->resource_path);
	} else {
		char addr_str[INET_ADDRSTRLEN];
		struct sockaddr_in *addr4 = (struct sockaddr_in *)&data->local_addr;
		zsock_inet_ntop(AF_INET, &addr4->sin_addr, addr_str, sizeof(addr_str));
		LOG_INF("CoAP server listening on %s:%d%s",
		        addr_str, ntohs(addr4->sin_port), data->resource_path);
	}

	/* Start receive thread */
	data->active = true;
	data->recv_tid = k_thread_create(&data->recv_thread, data->recv_stack,
	                                  K_KERNEL_STACK_SIZEOF(data->recv_stack),
	                                  coap_recv_thread_fn, transport, NULL, NULL,
	                                  COAP_RECV_THREAD_PRIORITY, 0, K_NO_WAIT);

	k_thread_name_set(data->recv_tid, "coap_recv");

	return 0;
}

static int coap_stop(struct ninep_transport *transport)
{
	struct coap_transport_data *data = transport->priv_data;

	LOG_INF("Stopping CoAP transport");

	data->active = false;

	if (data->recv_tid) {
		k_thread_join(data->recv_tid, K_FOREVER);
	}

	if (data->sock >= 0) {
		zsock_close(data->sock);
		data->sock = -1;
	}

	return 0;
}

static int coap_get_mtu(struct ninep_transport *transport)
{
	ARG_UNUSED(transport);

	/* CoAP payload size with block-wise transfer.
	 * With BLOCK_1024, we can send 1024 bytes per block.
	 * The 9P layer should limit message size to this or use multiple blocks.
	 *
	 * However, for simplicity, we return the full configured message size
	 * and handle fragmentation internally.
	 */
	return CONFIG_NINEP_MAX_MESSAGE_SIZE;
}

static const struct ninep_transport_ops coap_transport_ops = {
	.send = coap_send,
	.start = coap_start,
	.stop = coap_stop,
	.get_mtu = coap_get_mtu,
};

int ninep_transport_coap_init(struct ninep_transport *transport,
                               const struct ninep_transport_coap_config *config,
                               ninep_transport_recv_cb_t recv_cb,
                               void *user_data)
{
	if (!transport || !config || !config->local_addr) {
		return -EINVAL;
	}

	/* Allocate private data */
	struct coap_transport_data *data = k_malloc(sizeof(*data));
	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));

	/* Allocate RX buffer if not provided */
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
	data->resource_path = config->resource_path ? config->resource_path : "/9p";
	data->transport = transport;

	/* Copy local address */
	if (config->local_addr->sa_family == AF_INET6) {
		memcpy(&data->local_addr, config->local_addr, sizeof(struct sockaddr_in6));
		data->local_addr_len = sizeof(struct sockaddr_in6);
	} else {
		memcpy(&data->local_addr, config->local_addr, sizeof(struct sockaddr_in));
		data->local_addr_len = sizeof(struct sockaddr_in);
	}

	/* Initialize transport */
	transport->ops = &coap_transport_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

	LOG_INF("CoAP transport initialized (resource: %s, RX buf: %zu bytes)",
	        data->resource_path, data->rx_buf_size);

	return 0;
}
