/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P TCP Server - Serves a RAM-backed filesystem over TCP/IP
 */

#include <zephyr/kernel.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/ramfs.h>
#include <zephyr/9p/transport_tcp.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_mgmt.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static struct ninep_transport g_transport;
static struct ninep_server g_server;
static struct ninep_ramfs g_ramfs;

static void setup_demo_filesystem(void)
{
	struct ninep_fs_node *root = g_ramfs.root;

	/* Create demo files */
	const char *hello_content = "Hello from Zephyr 9P TCP server!\n";
	ninep_ramfs_create_file(&g_ramfs, root, "hello.txt",
	                         hello_content, strlen(hello_content));

	const char *readme_content =
		"9P Server on Zephyr RTOS over TCP/IP\n"
		"======================================\n\n"
		"This is a demonstration 9P server running on Zephyr.\n"
		"It serves a RAM-backed filesystem over TCP/IP.\n\n"
		"Connection:\n"
		"  Address: tcp!192.0.2.1!564\n"
		"  Port:    564 (standard 9P port)\n\n"
		"Features:\n"
		"- File reading\n"
		"- Directory listing\n"
		"- Path navigation\n\n"
		"Try:\n"
		"  9p -a tcp!192.0.2.1!564 ls /\n"
		"  9p -a tcp!192.0.2.1!564 read /readme.txt\n";

	ninep_ramfs_create_file(&g_ramfs, root, "readme.txt",
	                         readme_content, strlen(readme_content));

	/* Create subdirectory with files */
	struct ninep_fs_node *docs = ninep_ramfs_create_dir(&g_ramfs, root, "docs");

	if (docs) {
		const char *doc1 = "This is document 1\n";
		const char *doc2 = "This is document 2\n";

		ninep_ramfs_create_file(&g_ramfs, docs, "doc1.txt", doc1, strlen(doc1));
		ninep_ramfs_create_file(&g_ramfs, docs, "doc2.txt", doc2, strlen(doc2));
	}

	/* System info directory */
	struct ninep_fs_node *sys = ninep_ramfs_create_dir(&g_ramfs, root, "sys");

	if (sys) {
		const char *version = "Zephyr 9P TCP Server\n";
		ninep_ramfs_create_file(&g_ramfs, sys, "version",
		                         version, strlen(version));

		const char *board = CONFIG_BOARD "\n";
		ninep_ramfs_create_file(&g_ramfs, sys, "board", board, strlen(board));
	}

	LOG_INF("Demo filesystem created");
	LOG_INF("  /hello.txt");
	LOG_INF("  /readme.txt");
	LOG_INF("  /docs/doc1.txt");
	LOG_INF("  /docs/doc2.txt");
	LOG_INF("  /sys/version");
	LOG_INF("  /sys/board");
}

static void print_network_info(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface available");
		return;
	}

	LOG_INF("Network interface: %p", iface);

	/* Print IPv4 addresses */
	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4) {
		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (ipv4->unicast[i].ipv4.addr_state != NET_ADDR_ANY_STATE) {
				char addr_str[NET_IPV4_ADDR_LEN];

				net_addr_ntop(AF_INET,
				              &ipv4->unicast[i].ipv4.address.in_addr,
				              addr_str, sizeof(addr_str));
				LOG_INF("  IPv4 address: %s", addr_str);
			}
		}
	}
}

int main(void)
{
	struct ninep_tcp_config tcp_config;
	struct ninep_server_config server_config;
	int ret;

	LOG_INF("=== 9P TCP Server ===");

	/* Wait a bit for network to come up */
	k_sleep(K_SECONDS(1));

	print_network_info();

	/* Initialize RAM filesystem */
	ret = ninep_ramfs_init(&g_ramfs);
	if (ret < 0) {
		LOG_ERR("Failed to initialize RAM FS: %d", ret);
		return -1;
	}

	/* Setup demo files */
	setup_demo_filesystem();

	/* Configure TCP transport */
	tcp_config.port = 564;  /* Standard 9P port */
	tcp_config.rx_buf_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;

	/* Initialize transport */
	ret = ninep_tcp_transport_init(&g_transport, &tcp_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize TCP transport: %d", ret);
		return -1;
	}

	/* Configure server */
	server_config.fs_ops = ninep_ramfs_get_ops();
	server_config.fs_ctx = &g_ramfs;
	server_config.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	server_config.version = "9P2000";

	/* Initialize server */
	ret = ninep_server_init(&g_server, &server_config, &g_transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize server: %d", ret);
		return -1;
	}

	/* Start server */
	ret = ninep_server_start(&g_server);
	if (ret < 0) {
		LOG_ERR("Failed to start server: %d", ret);
		return -1;
	}

	LOG_INF("9P server listening on tcp!192.0.2.1!564");
	LOG_INF("Connect with: 9p -a tcp!192.0.2.1!564 ls /");

	/* Server runs in background via transport callbacks.
	 * Shell runs automatically on console.
	 * Just return and let the system run.
	 */
	return 0;
}
