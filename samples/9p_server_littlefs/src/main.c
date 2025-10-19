/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/9p/passthrough_fs.h>
#include <zephyr/9p/gatt_9pis.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* LittleFS mount point */
#define LITTLEFS_MOUNT_POINT "/lfs1"

/* LittleFS storage backend */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_mount = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
	.mnt_point = LITTLEFS_MOUNT_POINT,
};

/* 9P server instances */
static struct ninep_server server;
static struct ninep_transport transport;
static struct ninep_passthrough_fs passthrough_fs;

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

/* Setup initial filesystem contents */
static int setup_initial_files(void)
{
	struct fs_file_t file;
	int ret;

	LOG_INF("Setting up initial filesystem contents...");

	/* Create a welcome file */
	const char *welcome_path = LITTLEFS_MOUNT_POINT "/welcome.txt";
	const char *welcome_content =
		"Welcome to 9P over LittleFS!\n"
		"\n"
		"This filesystem is stored on flash memory and exposed\n"
		"via the 9P protocol over Bluetooth L2CAP.\n"
		"\n"
		"You can:\n"
		"- Read files like this one\n"
		"- Write new files\n"
		"- Create directories\n"
		"- Delete files and directories\n"
		"\n"
		"All changes are persisted to flash!\n";

	fs_file_t_init(&file);
	ret = fs_open(&file, welcome_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create welcome.txt: %d", ret);
		return ret;
	}

	if (ret != -EEXIST) {
		fs_write(&file, welcome_content, strlen(welcome_content));
		fs_close(&file);
		LOG_INF("Created: %s", welcome_path);
	}

	/* Create a docs directory */
	const char *docs_dir = LITTLEFS_MOUNT_POINT "/docs";
	ret = fs_mkdir(docs_dir);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create /docs directory: %d", ret);
		return ret;
	}

	if (ret != -EEXIST) {
		LOG_INF("Created: %s", docs_dir);
	}

	/* Create a README in docs */
	const char *readme_path = LITTLEFS_MOUNT_POINT "/docs/README.md";
	const char *readme_content =
		"# 9P over LittleFS Demo\n"
		"\n"
		"## About\n"
		"\n"
		"This is a demonstration of the 9p4z library exposing a\n"
		"LittleFS filesystem over Bluetooth L2CAP using the 9P protocol.\n"
		"\n"
		"## Features\n"
		"\n"
		"- **Persistent Storage**: All files are stored in flash memory\n"
		"- **Bluetooth Access**: Connect via BLE and access files wirelessly\n"
		"- **Standard Protocol**: Uses 9P2000 protocol\n"
		"- **Discoverable**: 9PIS GATT service for automatic discovery\n"
		"\n"
		"## Usage\n"
		"\n"
		"1. Connect to the device via Bluetooth\n"
		"2. Open L2CAP channel on PSM 0x0009\n"
		"3. Use any 9P client to access files\n"
		"\n"
		"Try creating files, directories, and exploring the filesystem!\n";

	fs_file_t_init(&file);
	ret = fs_open(&file, readme_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create README.md: %d", ret);
		return ret;
	}

	if (ret != -EEXIST) {
		fs_write(&file, readme_content, strlen(readme_content));
		fs_close(&file);
		LOG_INF("Created: %s", readme_path);
	}

	LOG_INF("Initial filesystem setup complete!");
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("9P LittleFS Server Sample");
	LOG_INF("=========================");

	/* Mount LittleFS */
	ret = fs_mount(&lfs_mount);
	if (ret < 0) {
		LOG_ERR("Failed to mount LittleFS: %d", ret);
		return 0;
	}

	LOG_INF("LittleFS mounted at %s", LITTLEFS_MOUNT_POINT);

	/* Setup initial files (if needed) */
	ret = setup_initial_files();
	if (ret < 0) {
		LOG_ERR("Failed to setup initial files: %d", ret);
		return 0;
	}

	/* Initialize Bluetooth */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

#ifdef CONFIG_NINEP_GATT_9PIS
	/* Initialize 9P Information Service (9PIS) for discoverability */
	struct ninep_9pis_config gatt_config = {
		.service_description = "9P File Server (LittleFS)",
		.service_features = "file-sharing,read-write,persistent-storage",
		.transport_info = "l2cap:psm=0x0009,mtu=4096",
		.app_store_link = "https://9p4z.org/clients",
		.protocol_version = "9P2000;9p4z;1.0.0",
	};

	ret = ninep_9pis_init(&gatt_config);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9PIS GATT service: %d", ret);
		return 0;
	}

	LOG_INF("9PIS GATT service initialized - device discoverable!");
#endif

	/* Start advertising */
	ret = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
	                                       BT_GAP_ADV_FAST_INT_MAX_2, NULL),
	                      ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("Advertising failed to start (err %d)", ret);
		return 0;
	}

	LOG_INF("Advertising started");

	/* Initialize passthrough filesystem */
	ret = ninep_passthrough_fs_init(&passthrough_fs, LITTLEFS_MOUNT_POINT);
	if (ret < 0) {
		LOG_ERR("Failed to initialize passthrough FS: %d", ret);
		return 0;
	}

	LOG_INF("Passthrough filesystem initialized");

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

	/* Initialize 9P server with passthrough filesystem */
	struct ninep_server_config server_config = {
		.fs_ops = ninep_passthrough_fs_get_ops(),
		.fs_ctx = &passthrough_fs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &server_config, &transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9P server: %d", ret);
		return 0;
	}

	LOG_INF("9P server initialized");

	/* Start server */
	ret = ninep_server_start(&server);
	if (ret < 0) {
		LOG_ERR("Failed to start server: %d", ret);
		return 0;
	}

	LOG_INF("=====================================================");
	LOG_INF("9P LittleFS Server running!");
	LOG_INF("=====================================================");
	LOG_INF("Mount point: %s", LITTLEFS_MOUNT_POINT);
	LOG_INF("L2CAP PSM:   0x%04x", CONFIG_NINEP_L2CAP_PSM);
	LOG_INF("=====================================================");
	LOG_INF("Connect via Bluetooth and access files wirelessly!");
	LOG_INF("All files are stored in flash and persist across reboots.");
	LOG_INF("=====================================================");

	return 0;
}
