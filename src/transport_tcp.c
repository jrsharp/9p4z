/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_tcp.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_tcp_transport, CONFIG_NINEP_LOG_LEVEL);

#define TCP_RECV_THREAD_STACK_SIZE 4096
#define TCP_RECV_THREAD_PRIORITY 5

struct tcp_transport_data {
	int listen_sock;
	int client_sock;
	uint16_t port;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	k_tid_t recv_tid;
	bool active;
	struct k_thread recv_thread;
	k_thread_stack_t recv_stack[K_KERNEL_STACK_LEN(TCP_RECV_THREAD_STACK_SIZE)];
};

static void tcp_recv_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct ninep_transport *transport = arg1;
	struct tcp_transport_data *data = transport->priv_data;
	size_t rx_offset = 0;
	uint32_t expected_size = 0;
	bool header_received = false;

	LOG_INF("TCP receive thread started");

	while (data->active) {
		/* Wait for client connection if not connected */
		if (data->client_sock < 0) {
			struct sockaddr_in client_addr;
			socklen_t client_addr_len = sizeof(client_addr);

			LOG_INF("Waiting for client connection on port %d", data->port);
			data->client_sock = zsock_accept(data->listen_sock,
			                            (struct sockaddr *)&client_addr,
			                            &client_addr_len);
			if (data->client_sock < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					k_sleep(K_MSEC(100));
					continue;
				}
				LOG_ERR("Accept failed: %d", errno);
				k_sleep(K_SECONDS(1));
				continue;
			}

			LOG_INF("Client connected from %d.%d.%d.%d",
			        ((uint8_t *)&client_addr.sin_addr)[0],
			        ((uint8_t *)&client_addr.sin_addr)[1],
			        ((uint8_t *)&client_addr.sin_addr)[2],
			        ((uint8_t *)&client_addr.sin_addr)[3]);

			/* Reset receive state for new connection */
			rx_offset = 0;
			expected_size = 0;
			header_received = false;
		}

		/* Read data from client */
		uint8_t byte;
		int ret = zsock_recv(data->client_sock, &byte, 1, 0);

		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				k_sleep(K_MSEC(10));
				continue;
			}
			LOG_ERR("Receive error: %d", errno);
			zsock_close(data->client_sock);
			data->client_sock = -1;
			continue;
		} else if (ret == 0) {
			LOG_INF("Client disconnected");
			zsock_close(data->client_sock);
			data->client_sock = -1;
			continue;
		}

		/* Store received byte */
		if (rx_offset < data->rx_buf_size) {
			data->rx_buf[rx_offset++] = byte;
		} else {
			/* Buffer overflow - reset */
			LOG_WRN("RX buffer overflow, resetting");
			rx_offset = 0;
			header_received = false;
			continue;
		}

		/* Parse header if we have enough bytes */
		if (!header_received && rx_offset >= 7) {
			struct ninep_msg_header hdr;

			if (ninep_parse_header(data->rx_buf, rx_offset, &hdr) == 0) {
				expected_size = hdr.size;
				header_received = true;
				LOG_DBG("Header received: size=%u type=%u tag=%u",
				        hdr.size, hdr.type, hdr.tag);
			} else {
				/* Invalid header - reset */
				LOG_WRN("Invalid header, resetting");
				rx_offset = 0;
				continue;
			}
		}

		/* Check if we have a complete message */
		if (header_received && rx_offset >= expected_size) {
			LOG_DBG("Complete message received: %u bytes", expected_size);

			/* Deliver complete message */
			if (transport->recv_cb) {
				transport->recv_cb(transport, data->rx_buf,
				                   expected_size, transport->user_data);
			}

			/* Reset for next message */
			rx_offset = 0;
			header_received = false;
			expected_size = 0;
		}
	}

	LOG_INF("TCP receive thread exiting");
}

static int tcp_send(struct ninep_transport *transport, const uint8_t *buf,
                    size_t len)
{
	struct tcp_transport_data *data = transport->priv_data;

	if (data->client_sock < 0) {
		return -ENOTCONN;
	}

	int ret = zsock_send(data->client_sock, buf, len, 0);

	if (ret < 0) {
		LOG_ERR("Send failed: %d", errno);
		return -errno;
	}

	LOG_DBG("Sent %d bytes", ret);
	return ret;
}

static int tcp_start(struct ninep_transport *transport)
{
	struct tcp_transport_data *data = transport->priv_data;
	struct sockaddr_in addr;
	int ret;

	/* Create listen socket */
	data->listen_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (data->listen_sock < 0) {
		LOG_ERR("Failed to create socket: %d", errno);
		return -errno;
	}

	/* Allow reuse of address */
	int opt = 1;

	ret = zsock_setsockopt(data->listen_sock, SOL_SOCKET, SO_REUSEADDR,
	                 &opt, sizeof(opt));
	if (ret < 0) {
		LOG_WRN("Failed to set SO_REUSEADDR: %d", errno);
	}

	/* Bind to port */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(data->port);

	ret = zsock_bind(data->listen_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		LOG_ERR("Failed to bind to port %d: %d", data->port, errno);
		zsock_close(data->listen_sock);
		return -errno;
	}

	/* Listen for connections */
	ret = zsock_listen(data->listen_sock, 1);
	if (ret < 0) {
		LOG_ERR("Failed to listen: %d", errno);
		zsock_close(data->listen_sock);
		return -errno;
	}

	LOG_INF("Listening on port %d", data->port);

	/* Start receive thread */
	data->active = true;
	data->client_sock = -1;
	data->recv_tid = k_thread_create(&data->recv_thread, data->recv_stack,
	                                  K_KERNEL_STACK_SIZEOF(data->recv_stack),
	                                  tcp_recv_thread_fn, transport, NULL, NULL,
	                                  TCP_RECV_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(data->recv_tid, "9p_tcp_recv");

	return 0;
}

static int tcp_stop(struct ninep_transport *transport)
{
	struct tcp_transport_data *data = transport->priv_data;

	data->active = false;

	if (data->recv_tid) {
		k_thread_join(data->recv_tid, K_FOREVER);
	}

	if (data->client_sock >= 0) {
		zsock_close(data->client_sock);
		data->client_sock = -1;
	}

	if (data->listen_sock >= 0) {
		zsock_close(data->listen_sock);
		data->listen_sock = -1;
	}

	LOG_INF("TCP transport stopped");
	return 0;
}

static const struct ninep_transport_ops tcp_transport_ops = {
	.send = tcp_send,
	.start = tcp_start,
	.stop = tcp_stop,
};

int ninep_tcp_transport_init(struct ninep_transport *transport,
                              const struct ninep_tcp_config *config,
                              ninep_transport_recv_cb_t recv_cb,
                              void *user_data)
{
	if (!transport || !config) {
		return -EINVAL;
	}

	/* Allocate private data */
	struct tcp_transport_data *data = k_malloc(sizeof(*data));

	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));

	/* Allocate receive buffer */
	data->rx_buf = k_malloc(config->rx_buf_size);
	if (!data->rx_buf) {
		k_free(data);
		return -ENOMEM;
	}

	data->rx_buf_size = config->rx_buf_size;
	data->port = config->port ? config->port : 564;  /* Default 9P port */
	data->listen_sock = -1;
	data->client_sock = -1;

	/* Initialize transport */
	transport->ops = &tcp_transport_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

	LOG_INF("TCP transport initialized (port=%d, buf_size=%zu)",
	        data->port, data->rx_buf_size);

	return 0;
}
