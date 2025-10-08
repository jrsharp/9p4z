/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P All Transports Server - Demonstrates all transport types
 * simultaneously serving the same filesystem
 */

#include <zephyr/kernel.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/transport_tcp.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* Transport instances */
static struct ninep_transport uart_transport;
static struct ninep_transport tcp_transport;
static struct ninep_transport l2cap_transport;

/* Server instances (one per transport, all sharing same filesystem) */
static struct ninep_server uart_server;
static struct ninep_server tcp_server;
static struct ninep_server l2cap_server;

/* Shared filesystem */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[32];

/* RX buffers for each transport */
static uint8_t uart_rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t tcp_rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t l2cap_rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* Bluetooth advertising data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
	        sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Generator functions for synthetic files */
static int gen_hello(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	const char *msg = "Hello from 9P all transports sample!\n"
	                  "This filesystem is available via:\n"
	                  "- UART\n"
	                  "- TCP (port 564)\n"
	                  "- Bluetooth L2CAP (PSM 0x0009)\n";
	size_t len = strlen(msg);

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = MIN(len - offset, buf_size);
	memcpy(buf, msg + offset, to_copy);
	return to_copy;
}

static int gen_version(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char version[256];
	int len = snprintf(version, sizeof(version),
	                  "9p4z All Transports Server\n"
	                  "===========================\n"
	                  "Zephyr: %s\n"
	                  "Build: %s %s\n\n"
	                  "Active Transports:\n"
	                  "- UART: %s\n"
	                  "- TCP: port 564\n"
	                  "- L2CAP: PSM 0x%04x\n",
	                  KERNEL_VERSION_STRING, __DATE__, __TIME__,
	                  DEVICE_DT_NAME(DT_CHOSEN(zephyr_console)),
	                  CONFIG_NINEP_L2CAP_PSM);

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = MIN(len - offset, buf_size);
	memcpy(buf, version + offset, to_copy);
	return to_copy;
}

static int gen_uptime(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	uint64_t uptime_ms = k_uptime_get();
	uint64_t uptime_sec = uptime_ms / 1000;
	uint64_t hours = uptime_sec / 3600;
	uint64_t minutes = (uptime_sec % 3600) / 60;
	uint64_t seconds = uptime_sec % 60;

	char uptime_str[64];
	int len = snprintf(uptime_str, sizeof(uptime_str),
	                  "%llu:%02llu:%02llu (%llu ms)\n",
	                  hours, minutes, seconds, uptime_ms);

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = MIN(len - offset, buf_size);
	memcpy(buf, uptime_str + offset, to_copy);
	return to_copy;
}

/* Setup shared filesystem */
static int setup_filesystem(void)
{
	int ret;

	/* Initialize sysfs */
	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	if (ret < 0) {
		LOG_ERR("Failed to initialize sysfs: %d", ret);
		return ret;
	}

	/* Create /hello.txt */
	ret = ninep_sysfs_add_file(&sysfs, "hello.txt", gen_hello, NULL, 256);
	if (ret < 0) {
		LOG_ERR("Failed to add hello.txt: %d", ret);
		return ret;
	}

	/* Create /sys directory */
	ret = ninep_sysfs_add_dir(&sysfs, "sys");
	if (ret < 0) {
		LOG_ERR("Failed to add sys directory: %d", ret);
		return ret;
	}

	/* Create /sys/version */
	ret = ninep_sysfs_add_file(&sysfs, "sys/version", gen_version, NULL, 512);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/version: %d", ret);
		return ret;
	}

	/* Create /sys/uptime */
	ret = ninep_sysfs_add_file(&sysfs, "sys/uptime", gen_uptime, NULL, 64);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/uptime: %d", ret);
		return ret;
	}

	/* Create /docs directory */
	ret = ninep_sysfs_add_dir(&sysfs, "docs");
	if (ret < 0) {
		LOG_ERR("Failed to add docs directory: %d", ret);
		return ret;
	}

	LOG_INF("Filesystem setup complete");
	return 0;
}

/* Initialize UART transport and server */
static int init_uart_server(void)
{
	int ret;

	const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	struct ninep_transport_uart_config uart_config = {
		.uart_dev = uart_dev,
		.rx_buf = uart_rx_buf,
		.rx_buf_size = sizeof(uart_rx_buf),
	};

	ret = ninep_transport_uart_init(&uart_transport, &uart_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UART transport: %d", ret);
		return ret;
	}

	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&uart_server, &server_config, &uart_transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UART server: %d", ret);
		return ret;
	}

	ret = ninep_server_start(&uart_server);
	if (ret < 0) {
		LOG_ERR("Failed to start UART server: %d", ret);
		return ret;
	}

	LOG_INF("UART transport initialized on %s", uart_dev->name);
	return 0;
}

/* Initialize TCP transport and server */
static int init_tcp_server(void)
{
	int ret;

	struct ninep_tcp_config tcp_config = {
		.port = 564,
		.rx_buf_size = sizeof(tcp_rx_buf),
	};

	ret = ninep_tcp_transport_init(&tcp_transport, &tcp_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize TCP transport: %d", ret);
		return ret;
	}

	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&tcp_server, &server_config, &tcp_transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize TCP server: %d", ret);
		return ret;
	}

	ret = ninep_server_start(&tcp_server);
	if (ret < 0) {
		LOG_ERR("Failed to start TCP server: %d", ret);
		return ret;
	}

	LOG_INF("TCP transport initialized on port 564");
	return 0;
}

/* Initialize L2CAP transport and server */
static int init_l2cap_server(void)
{
	int ret;

	/* Initialize Bluetooth */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return ret;
	}

	LOG_INF("Bluetooth initialized");

	/* Start advertising */
	ret = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("Advertising failed to start (err %d)", ret);
		return ret;
	}

	LOG_INF("Bluetooth advertising started");

	struct ninep_transport_l2cap_config l2cap_config = {
		.psm = CONFIG_NINEP_L2CAP_PSM,
		.rx_buf = l2cap_rx_buf,
		.rx_buf_size = sizeof(l2cap_rx_buf),
	};

	ret = ninep_transport_l2cap_init(&l2cap_transport, &l2cap_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize L2CAP transport: %d", ret);
		return ret;
	}

	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&l2cap_server, &server_config, &l2cap_transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize L2CAP server: %d", ret);
		return ret;
	}

	ret = ninep_server_start(&l2cap_server);
	if (ret < 0) {
		LOG_ERR("Failed to start L2CAP server: %d", ret);
		return ret;
	}

	LOG_INF("L2CAP transport initialized on PSM 0x%04x", CONFIG_NINEP_L2CAP_PSM);
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("===========================================");
	LOG_INF("9P All Transports Server");
	LOG_INF("===========================================");

	/* Setup shared filesystem */
	ret = setup_filesystem();
	if (ret < 0) {
		LOG_ERR("Failed to setup filesystem: %d", ret);
		return 0;
	}

	/* Initialize all transports */
	LOG_INF("Initializing transports...");

	ret = init_uart_server();
	if (ret < 0) {
		LOG_WRN("UART transport not available (continuing without it)");
	}

	ret = init_tcp_server();
	if (ret < 0) {
		LOG_WRN("TCP transport not available (continuing without it)");
	}

	ret = init_l2cap_server();
	if (ret < 0) {
		LOG_WRN("L2CAP transport not available (continuing without it)");
	}

	LOG_INF("===========================================");
	LOG_INF("Server ready!");
	LOG_INF("===========================================");
	LOG_INF("UART:  Connect via serial console");
	LOG_INF("TCP:   9p -a tcp!<IP>!564 ls /");
	LOG_INF("L2CAP: Use iOS 9p4i app, PSM 0x%04x", CONFIG_NINEP_L2CAP_PSM);
	LOG_INF("===========================================");

	/* All servers run in background via transport callbacks */
	return 0;
}
