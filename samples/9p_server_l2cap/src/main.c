/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/9p/sysfs.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* 9P server instance */
static struct ninep_server server;
static struct ninep_transport transport;

/* Sysfs instance */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[32];

/* RX buffer for L2CAP transport */
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* Bluetooth advertising data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Bluetooth connection callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_ERR("Failed to connect to %s (err %u)", addr, err);
		return;
	}

	LOG_INF("Connected: %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason %u)", addr, reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Generate hello.txt content */
static int gen_hello(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	const char *msg = "Hello from 9P over L2CAP!\n";
	size_t msg_len = strlen(msg);

	if (offset >= msg_len) {
		return 0;
	}

	size_t remaining = msg_len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, msg + offset, to_copy);
	return to_copy;
}

/* Generate sys/version content */
static int gen_version(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char version[128];
	int len = snprintf(version, sizeof(version),
	                  "9p4z L2CAP Server\nZephyr: %s\nBuild: %s %s\n",
	                  KERNEL_VERSION_STRING, __DATE__, __TIME__);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, version + offset, to_copy);
	return to_copy;
}

/* Generate sys/uptime content */
static int gen_uptime(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	uint64_t uptime_ms = k_uptime_get();
	uint64_t uptime_sec = uptime_ms / 1000;
	uint64_t hours = uptime_sec / 3600;
	uint64_t minutes = (uptime_sec % 3600) / 60;
	uint64_t seconds = uptime_sec % 60;

	char uptime_str[64];
	int len = snprintf(uptime_str, sizeof(uptime_str),
	                  "%llu:%02llu:%02llu\n",
	                  hours, minutes, seconds);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, uptime_str + offset, to_copy);
	return to_copy;
}

/* Setup synthetic filesystem */
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
	ret = ninep_sysfs_register_file(&sysfs, "hello.txt", gen_hello, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add hello.txt: %d", ret);
		return ret;
	}

	/* Create /sys directory */
	ret = ninep_sysfs_register_dir(&sysfs, "sys");
	if (ret < 0) {
		LOG_ERR("Failed to add sys directory: %d", ret);
		return ret;
	}

	/* Create /sys/version */
	ret = ninep_sysfs_register_file(&sysfs, "sys/version", gen_version, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/version: %d", ret);
		return ret;
	}

	/* Create /sys/uptime */
	ret = ninep_sysfs_register_file(&sysfs, "sys/uptime", gen_uptime, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/uptime: %d", ret);
		return ret;
	}

	/* Create /docs directory */
	ret = ninep_sysfs_register_dir(&sysfs, "docs");
	if (ret < 0) {
		LOG_ERR("Failed to add docs directory: %d", ret);
		return ret;
	}

	LOG_INF("Filesystem setup complete");
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("9P L2CAP Server Sample");
	LOG_INF("======================");

	/* Initialize Bluetooth */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	/* Start advertising */
	ret = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("Advertising failed to start (err %d)", ret);
		return 0;
	}

	LOG_INF("Advertising started");

	/* Setup filesystem */
	ret = setup_filesystem();
	if (ret < 0) {
		LOG_ERR("Failed to setup filesystem: %d", ret);
		return 0;
	}

	/* Initialize L2CAP transport */
	struct ninep_transport_l2cap_config l2cap_config = {
		.psm = CONFIG_NINEP_L2CAP_PSM,
		.rx_buf = rx_buf,
		.rx_buf_size = sizeof(rx_buf),
	};

	ret = ninep_transport_l2cap_init(&transport, &l2cap_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize L2CAP transport: %d", ret);
		return 0;
	}

	LOG_INF("L2CAP transport initialized");

	/* Initialize 9P server with sysfs */
	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &server_config, &transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9P server: %d", ret);
		return 0;
	}

	LOG_INF("9P server initialized");

	/* Start server (starts transport and begins accepting messages) */
	ret = ninep_server_start(&server);
	if (ret < 0) {
		LOG_ERR("Failed to start server: %d", ret);
		return 0;
	}

	LOG_INF("L2CAP server started on PSM 0x%04x", CONFIG_NINEP_L2CAP_PSM);
	LOG_INF("Waiting for iOS client connection...");

	/* Server runs in background via callbacks */
	return 0;
}
