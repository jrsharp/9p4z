/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P Server - Serves a RAM-backed filesystem over UART
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/ramfs.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_ninep_uart)

static uint8_t rx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static struct ninep_transport g_transport;
static struct ninep_server g_server;
static struct ninep_ramfs g_ramfs;

static void setup_demo_filesystem(void)
{
	struct ninep_fs_node *root = g_ramfs.root;

	/* Create demo files */
	const char *hello_content = "Hello from Zephyr 9P server!\n";
	ninep_ramfs_create_file(&g_ramfs, root, "hello.txt",
	                         hello_content, strlen(hello_content));

	const char *readme_content =
		"9P Server on Zephyr RTOS\n"
		"=========================\n\n"
		"This is a demonstration 9P server running on Zephyr.\n"
		"It serves a RAM-backed filesystem over UART.\n\n"
		"Features:\n"
		"- File reading\n"
		"- Directory listing\n"
		"- Path navigation\n\n"
		"Try:\n"
		"  9p -a tcp!localhost!9999 ls /\n"
		"  9p -a tcp!localhost!9999 read /readme.txt\n";

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
		const char *version = "Zephyr 9P Server\n";
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

int main(void)
{
	const struct device *uart_dev;
	struct ninep_transport_uart_config uart_config;
	struct ninep_server_config server_config;
	int ret;

	LOG_INF("=== 9P Server ===");

	/* Initialize RAM filesystem */
	ret = ninep_ramfs_init(&g_ramfs);
	if (ret < 0) {
		LOG_ERR("Failed to initialize RAM FS: %d", ret);
		return -1;
	}

	/* Setup demo files */
	setup_demo_filesystem();

	/* Get UART device */
	uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -1;
	}

	/* Configure UART transport */
	uart_config.uart_dev = uart_dev;
	uart_config.rx_buf = rx_buffer;
	uart_config.rx_buf_size = sizeof(rx_buffer);

	/* Initialize transport */
	ret = ninep_transport_uart_init(&g_transport, &uart_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UART transport: %d", ret);
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

	LOG_INF("9P server running on UART1 (TCP port 9998)");
	LOG_INF("Shell available on UART0 (console)");
	LOG_INF("Type 'help' for available commands");

	/* Server runs in background via transport callbacks.
	 * Shell runs automatically on console (UART0).
	 * Just return and let the system run.
	 */
	return 0;
}
