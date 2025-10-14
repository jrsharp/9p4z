/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * TCP Transport Integration Tests
 *
 * Tests the TCP transport with real sockets over localhost:
 * - IPv4 connectivity
 * - IPv6 connectivity (when available)
 * - Dual-stack operation
 * - Message send/receive
 * - Server/client integration
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/9p/transport_tcp.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/client.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_mgmt.h>

LOG_MODULE_REGISTER(tcp_transport_test, LOG_LEVEL_DBG);

#define TEST_PORT 9564
#define TEST_TIMEOUT_MS 5000

/* Test fixtures */
static struct ninep_transport server_transport;
static struct ninep_transport client_transport;
static struct ninep_server server;
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[8];

static uint8_t server_rx_buf[4096];

static bool server_ready;
static bool client_connected;
static K_SEM_DEFINE(server_ready_sem, 0, 1);
static K_SEM_DEFINE(client_ready_sem, 0, 1);

/* Simple file generator for testing */
static int gen_test_file(uint8_t *buf, size_t buf_size,
                         uint64_t offset, void *ctx)
{
	const char *content = "Hello from 9P TCP test!\n";
	size_t len = strlen(content);

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = len - offset;
	if (to_copy > buf_size) {
		to_copy = buf_size;
	}

	memcpy(buf, content + offset, to_copy);
	return to_copy;
}

/* Setup sysfs for server */
static void setup_test_filesystem(void)
{
	int ret;

	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	zassert_equal(ret, 0, "Failed to init sysfs: %d", ret);

	ret = ninep_sysfs_register_file(&sysfs, "/test.txt", gen_test_file, NULL);
	zassert_equal(ret, 0, "Failed to register file: %d", ret);
}

/* Wait for network to be ready */
static void wait_for_network(void)
{
	struct net_if *iface = net_if_get_default();

	zassert_not_null(iface, "No network interface");

	/* Wait for interface to come up */
	for (int i = 0; i < 50; i++) {
		if (net_if_is_up(iface)) {
			LOG_INF("Network interface is up");
			return;
		}
		k_sleep(K_MSEC(100));
	}

	zassert_true(false, "Network interface failed to come up");
}

/* Test: Initialize and start TCP transport (server) */
ZTEST(tcp_transport, test_init_and_start)
{
	struct ninep_tcp_config config = {
		.port = TEST_PORT,
		.rx_buf_size = sizeof(server_rx_buf),
	};

	wait_for_network();

	/* Initialize transport */
	int ret = ninep_tcp_transport_init(&server_transport, &config, NULL, NULL);
	zassert_equal(ret, 0, "Failed to init transport: %d", ret);

	/* Start listening */
	ret = server_transport.ops->start(&server_transport);
	zassert_equal(ret, 0, "Failed to start transport: %d", ret);

	/* Give it time to bind and listen */
	k_sleep(K_MSEC(500));

	/* Stop transport */
	ret = server_transport.ops->stop(&server_transport);
	zassert_equal(ret, 0, "Failed to stop transport: %d", ret);
}

/* Test: IPv4 connectivity */
ZTEST(tcp_transport, test_ipv4_connectivity)
{
	struct sockaddr_in addr;
	int sock;
	int ret;

	wait_for_network();

	/* Start server transport */
	struct ninep_tcp_config config = {
		.port = TEST_PORT,
		.rx_buf_size = sizeof(server_rx_buf),
	};

	ret = ninep_tcp_transport_init(&server_transport, &config, NULL, NULL);
	zassert_equal(ret, 0, "Failed to init server transport: %d", ret);

	ret = server_transport.ops->start(&server_transport);
	zassert_equal(ret, 0, "Failed to start server transport: %d", ret);

	k_sleep(K_MSEC(500));

	/* Connect via IPv4 */
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(sock >= 0, "Failed to create IPv4 socket: %d", errno);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT);
	zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	zassert_equal(ret, 0, "Failed to connect via IPv4: %d", errno);

	LOG_INF("IPv4 connection successful");

	zsock_close(sock);

	/* Stop server */
	server_transport.ops->stop(&server_transport);
}

#if defined(CONFIG_NET_IPV6)
/* Test: IPv6 connectivity */
ZTEST(tcp_transport, test_ipv6_connectivity)
{
	struct sockaddr_in6 addr;
	int sock;
	int ret;

	wait_for_network();

	/* Start server transport (dual-stack) */
	struct ninep_tcp_config config = {
		.port = TEST_PORT,
		.rx_buf_size = sizeof(server_rx_buf),
	};

	ret = ninep_tcp_transport_init(&server_transport, &config, NULL, NULL);
	zassert_equal(ret, 0, "Failed to init server transport: %d", ret);

	ret = server_transport.ops->start(&server_transport);
	zassert_equal(ret, 0, "Failed to start server transport: %d", ret);

	k_sleep(K_MSEC(500));

	/* Connect via IPv6 */
	sock = zsock_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(sock >= 0, "Failed to create IPv6 socket: %d", errno);

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(TEST_PORT);
	zsock_inet_pton(AF_INET6, "::1", &addr.sin6_addr);

	ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	zassert_equal(ret, 0, "Failed to connect via IPv6: %d", errno);

	LOG_INF("IPv6 connection successful");

	zsock_close(sock);

	/* Stop server */
	server_transport.ops->stop(&server_transport);
}

/* Test: Dual-stack - IPv4-mapped IPv6 address */
ZTEST(tcp_transport, test_dual_stack)
{
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	int sock4, sock6;
	int ret;

	wait_for_network();

	/* Start server transport (dual-stack) */
	struct ninep_tcp_config config = {
		.port = TEST_PORT,
		.rx_buf_size = sizeof(server_rx_buf),
	};

	ret = ninep_tcp_transport_init(&server_transport, &config, NULL, NULL);
	zassert_equal(ret, 0, "Failed to init server transport: %d", ret);

	ret = server_transport.ops->start(&server_transport);
	zassert_equal(ret, 0, "Failed to start server transport: %d", ret);

	k_sleep(K_MSEC(500));

	/* Connect via IPv4 */
	sock4 = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(sock4 >= 0, "Failed to create IPv4 socket");

	memset(&addr4, 0, sizeof(addr4));
	addr4.sin_family = AF_INET;
	addr4.sin_port = htons(TEST_PORT);
	zsock_inet_pton(AF_INET, "127.0.0.1", &addr4.sin_addr);

	ret = zsock_connect(sock4, (struct sockaddr *)&addr4, sizeof(addr4));
	zassert_equal(ret, 0, "Failed to connect via IPv4");

	LOG_INF("IPv4 connection successful");

	/* Close first connection */
	zsock_close(sock4);
	k_sleep(K_MSEC(200));

	/* Connect via native IPv6 */
	sock6 = zsock_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(sock6 >= 0, "Failed to create IPv6 socket");

	memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_port = htons(TEST_PORT);
	zsock_inet_pton(AF_INET6, "::1", &addr6.sin6_addr);

	ret = zsock_connect(sock6, (struct sockaddr *)&addr6, sizeof(addr6));
	zassert_equal(ret, 0, "Failed to connect via IPv6");

	LOG_INF("IPv6 connection successful - dual-stack verified");

	zsock_close(sock6);

	/* Stop server */
	server_transport.ops->stop(&server_transport);
}
#endif /* CONFIG_NET_IPV6 */

/* Test: Full server/client integration */
ZTEST(tcp_transport, test_server_client_integration)
{
	int ret;

	wait_for_network();
	setup_test_filesystem();

	/* Configure and start server */
	struct ninep_tcp_config server_config = {
		.port = TEST_PORT,
		.rx_buf_size = sizeof(server_rx_buf),
	};

	ret = ninep_tcp_transport_init(&server_transport, &server_config, NULL, NULL);
	zassert_equal(ret, 0, "Failed to init server transport: %d", ret);

	struct ninep_server_config srv_cfg = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = 8192,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &srv_cfg, &server_transport);
	zassert_equal(ret, 0, "Failed to init server: %d", ret);

	ret = ninep_server_start(&server);
	zassert_equal(ret, 0, "Failed to start server: %d", ret);

	/* Give server time to start listening */
	k_sleep(K_SECONDS(1));

	/* Create client socket and connect */
	int client_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	zassert_true(client_sock >= 0, "Failed to create client socket");

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT);
	zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	ret = zsock_connect(client_sock, (struct sockaddr *)&addr, sizeof(addr));
	zassert_equal(ret, 0, "Failed to connect client: %d", errno);

	LOG_INF("Client connected to server");

	/* Send a simple Tversion message */
	uint8_t msg[] = {
		/* Header: size=19, type=100 (Tversion), tag=0 */
		19, 0, 0, 0,  /* size (little-endian) */
		100,          /* Tversion */
		0, 0,         /* tag */
		/* msize=8192 */
		0x00, 0x20, 0x00, 0x00,
		/* version string "9P2000" (length=6) */
		6, 0,         /* string length */
		'9', 'P', '2', '0', '0', '0'
	};

	ssize_t sent = zsock_send(client_sock, msg, sizeof(msg), 0);
	zassert_equal(sent, sizeof(msg), "Failed to send message");

	LOG_INF("Sent Tversion message");

	/* Receive Rversion response */
	uint8_t response[256];
	ssize_t received = zsock_recv(client_sock, response, sizeof(response), 0);
	zassert_true(received > 0, "Failed to receive response");

	LOG_INF("Received response: %d bytes", received);

	/* Verify it's an Rversion (type=101) */
	zassert_true(received >= 7, "Response too short");
	zassert_equal(response[4], 101, "Expected Rversion (101), got %d", response[4]);

	LOG_INF("Server/client integration test successful!");

	/* Cleanup */
	zsock_close(client_sock);
	ninep_server_stop(&server);
}

/* Test suite setup/teardown */
static void *tcp_transport_setup(void)
{
	LOG_INF("TCP transport test suite starting");
	server_ready = false;
	client_connected = false;
	return NULL;
}

static void tcp_transport_before(void *f)
{
	/* Reset state before each test */
	memset(&server_transport, 0, sizeof(server_transport));
	memset(&client_transport, 0, sizeof(client_transport));
}

static void tcp_transport_after(void *f)
{
	/* Cleanup after each test */
	k_sleep(K_MSEC(200));  /* Let sockets fully close */
}

ZTEST_SUITE(tcp_transport, NULL, tcp_transport_setup,
            tcp_transport_before, tcp_transport_after, NULL);
