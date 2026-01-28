/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal 9P DFU Server over Bluetooth L2CAP
 *
 * Provides firmware update capability via 9P filesystem.
 * Exposes /dev/firmware for MCUboot image uploads.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/dfu/mcuboot.h>

#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/dfu.h>

#ifdef CONFIG_NINEP_GATT_9PIS
#include <zephyr/9p/gatt_9pis.h>
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* 9P server and transport */
static struct ninep_server server;
static struct ninep_transport transport;
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* Sysfs for virtual files */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[8];

/* DFU instance */
static struct ninep_dfu dfu;

/* Bluetooth advertising data - include 9PIS service UUID for discoverability */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
#ifdef CONFIG_NINEP_GATT_9PIS
	/* Advertise 9PIS service UUID so iOS can filter for 9P devices */
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		0x01, 0xc0, 0xe4, 0xf6, 0xe0, 0xa1, 0x88, 0xba,
		0x91, 0x4a, 0xed, 0xfe, 0x01, 0x00, 0x50, 0x39),  /* 39500001-feed-4a91-ba88-a1e0f6e4c001 */
#endif
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
	        sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Connection callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}
	LOG_INF("Connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (reason %u)", reason);

	/* Restart advertising */
	int ret = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
	                                           BT_GAP_ADV_FAST_INT_MIN_2,
	                                           BT_GAP_ADV_FAST_INT_MAX_2, NULL),
	                          ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret) {
		LOG_ERR("Failed to restart advertising: %d", ret);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Sysfs: device name */
static int gen_name(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	const char *name = CONFIG_BT_DEVICE_NAME "\n";
	size_t len = strlen(name);

	if (offset >= len) {
		return 0;
	}
	size_t to_copy = MIN(len - offset, buf_size);
	memcpy(buf, name + offset, to_copy);
	return to_copy;
}

/* Sysfs: reboot command */
static int write_reboot(const uint8_t *buf, uint32_t count, uint64_t offset, void *ctx)
{
	LOG_INF("Reboot requested via 9P");
	k_sleep(K_MSEC(100)); /* Let logs flush */
	sys_reboot(SYS_REBOOT_COLD);
	return count; /* Never reached */
}

/* Sysfs: confirm image */
static int write_confirm(const uint8_t *buf, uint32_t count, uint64_t offset, void *ctx)
{
	int ret = ninep_dfu_confirm();
	if (ret == 0) {
		LOG_INF("Image confirmed via 9P");
	}
	return ret < 0 ? ret : count;
}

/* DFU status callback (optional) */
static void dfu_status(enum ninep_dfu_state state, uint32_t bytes, int err)
{
	switch (state) {
	case NINEP_DFU_ERASING:
		LOG_INF("DFU: erasing flash...");
		break;
	case NINEP_DFU_RECEIVING:
		/* Progress logged by DFU module */
		break;
	case NINEP_DFU_COMPLETE:
		LOG_INF("DFU: complete! Reboot to apply.");
		break;
	case NINEP_DFU_ERROR:
		LOG_ERR("DFU: error %d", err);
		break;
	default:
		break;
	}
}

/* Initialize sysfs with system files */
static int init_sysfs(void)
{
	int ret;

	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	if (ret < 0) {
		LOG_ERR("Failed to init sysfs: %d", ret);
		return ret;
	}

	/* /name - device name */
	ret = ninep_sysfs_register_file(&sysfs, "name", gen_name, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register /name: %d", ret);
		return ret;
	}

	/* /dev/reboot - write to reboot */
	ret = ninep_sysfs_register_writable_file(&sysfs, "dev/reboot",
	                                          NULL, write_reboot, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register /dev/reboot: %d", ret);
		return ret;
	}

	/* /dev/confirm - write to confirm image */
	ret = ninep_sysfs_register_writable_file(&sysfs, "dev/confirm",
	                                          NULL, write_confirm, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register /dev/confirm: %d", ret);
		return ret;
	}

	/* /dev/firmware - DFU endpoint */
	struct ninep_dfu_config dfu_cfg = {
		.path = "dev/firmware",
		.status_cb = dfu_status,
	};
	ret = ninep_dfu_init(&dfu, &sysfs, &dfu_cfg);
	if (ret < 0) {
		LOG_ERR("Failed to init DFU: %d", ret);
		return ret;
	}

	LOG_INF("Sysfs initialized");
	return 0;
}

/* Initialize 9P server */
static int init_9p_server(void)
{
	int ret;

	/* Initialize L2CAP transport */
	struct ninep_transport_l2cap_config l2cap_config = {
		.psm = CONFIG_NINEP_L2CAP_PSM,
		.rx_buf = rx_buf,
		.rx_buf_size = sizeof(rx_buf),
	};

	ret = ninep_transport_l2cap_init(&transport, &l2cap_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to init L2CAP transport: %d", ret);
		return ret;
	}

	/* Initialize server with sysfs backend */
	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &server_config, &transport);
	if (ret < 0) {
		LOG_ERR("Failed to init 9P server: %d", ret);
		return ret;
	}

	/* Start the server (registers L2CAP PSM and starts listening) */
	ret = ninep_server_start(&server);
	if (ret < 0) {
		LOG_ERR("Failed to start 9P server: %d", ret);
		return ret;
	}

	LOG_INF("9P server listening on L2CAP PSM 0x%04x", CONFIG_NINEP_L2CAP_PSM);
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("9P DFU Server starting...");

	/* Check if we need to confirm the running image */
	if (!boot_is_img_confirmed()) {
		LOG_WRN("Image not confirmed - will revert on next reboot!");
		LOG_WRN("Write to /dev/confirm to make permanent");
	}

	/* Initialize Bluetooth */
	ret = bt_enable(NULL);
	if (ret < 0) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth initialized");

	/* Initialize sysfs (including DFU) */
	ret = init_sysfs();
	if (ret < 0) {
		return ret;
	}

	/* Initialize 9P server */
	ret = init_9p_server();
	if (ret < 0) {
		return ret;
	}

#ifdef CONFIG_NINEP_GATT_9PIS
	/* Register 9PIS GATT service for discovery */
	char transport_info[32];
	snprintf(transport_info, sizeof(transport_info),
	         "l2cap:psm=0x%04x", CONFIG_NINEP_L2CAP_PSM);

	struct ninep_9pis_config gatt_config = {
		.service_description = "9P DFU Server",
		.service_features = "dfu,firmware-update",
		.transport_info = transport_info,
		.protocol_version = "9P2000",
	};
	ret = ninep_9pis_init(&gatt_config);
	if (ret < 0) {
		LOG_WRN("Failed to register 9PIS GATT: %d", ret);
	}
#endif

	/* Start advertising */
	ret = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
	                                       BT_GAP_ADV_FAST_INT_MIN_2,
	                                       BT_GAP_ADV_FAST_INT_MAX_2, NULL),
	                      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret < 0) {
		LOG_ERR("Advertising failed to start: %d", ret);
		return ret;
	}
	LOG_INF("Advertising started");

	LOG_INF("Ready! Files: /dev/firmware, /dev/reboot, /dev/confirm, /name");

	/* Server runs via L2CAP callbacks - just idle here */
	while (1) {
		k_sleep(K_SECONDS(60));
	}

	return 0;
}
