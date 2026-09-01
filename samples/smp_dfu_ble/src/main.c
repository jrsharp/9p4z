/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal SMP DFU Server over Bluetooth GATT — the "traditional" firmware
 * update path, and the deliberate twin of samples/9p_dfu_l2cap.
 *
 * Same board, same MCUboot, same slot layout, same signed image. The only
 * variable is the DFU mechanism above the BLE link:
 *
 *   9p_dfu_l2cap : 9P2000 over an L2CAP CoC       -> write a file
 *   smp_dfu_ble  : SMP/CBOR over GATT (MCUmgr)    -> img upload, img confirm, os reset
 *
 * Read this file next to samples/9p_dfu_l2cap/src/main.c. The difference in
 * what the *application* has to say is small; the difference in what the
 * *client* has to know is not. See doc/DFU_COMPARISON.md.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/dfu/mcuboot.h>

#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * Bluetooth advertising data.
 *
 * The 9P twin advertises the 9PIS service UUID, whose characteristics tell a
 * client the PSM and MTU to use — i.e. the transport is self-describing. Here
 * we advertise the SMP service UUID (8D53DC1D-1DB7-4CD3-868B-8A527460AA84,
 * little-endian) which is a bare "I speak SMP" flag: the client is expected to
 * already know the protocol, the command groups and the CBOR schemas.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		0x84, 0xaa, 0x60, 0x74, 0x52, 0x8a, 0x8b, 0x86,
		0xd3, 0x4c, 0xb7, 0x1d, 0x1d, 0xdc, 0x53, 0x8d),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
	        sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ------------------------------------------------------------------------ */
/* Device-side transfer instrumentation                                      */
/*                                                                           */
/* Emits the same BENCH lines as the 9P twin so the two can be timed by the  */
/* board itself, with no host-side connect/scan overhead in the number.      */
/* ------------------------------------------------------------------------ */

static int64_t bench_start_ms;
static size_t bench_last_off;
static size_t bench_total;

static void bench_begin(void)
{
	bench_start_ms = k_uptime_get();
	bench_last_off = 0;
	bench_total = 0;
	printk("BENCH mech=smp-gatt event=start\n");
}

static void bench_end(const char *how)
{
	int64_t dt = k_uptime_get() - bench_start_ms;

	if (bench_start_ms == 0) {
		return;
	}
	if (dt <= 0) {
		dt = 1;
	}
	printk("BENCH mech=smp-gatt event=%s bytes=%u ms=%lld Bps=%lld\n",
	       how, (unsigned int)bench_total, dt,
	       ((int64_t)bench_total * 1000) / dt);
	bench_start_ms = 0;
}

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

/*
 * MCUmgr DFU notification hook — the SMP counterpart of the 9P twin's
 * ninep_dfu status_cb. Note that where the 9P side gets a byte count handed to
 * it by the DFU module, here we have to reconstruct progress from the offset
 * carried in each upload request.
 */
static enum mgmt_cb_return dfu_hook(uint32_t event, enum mgmt_cb_return prev_status,
                                    int32_t *rc, uint16_t *group, bool *abort_more,
                                    void *data, size_t data_size)
{
	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		LOG_INF("DFU: upload started");
		bench_begin();
		break;

	case MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK: {
		const struct img_mgmt_upload_check *check = data;

		if (data != NULL && data_size >= sizeof(*check) &&
		    check->req != NULL) {
			size_t off = check->req->off;

			if (off != SIZE_MAX && off >= bench_last_off) {
				bench_total = off + check->req->img_data.len;
				bench_last_off = off;
			}
		}
		break;
	}

	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		LOG_INF("DFU: complete! Confirm and reset to apply.");
		bench_end("complete");
		break;

	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		LOG_ERR("DFU: stopped/aborted");
		bench_end("aborted");
		break;

	default:
		break;
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback dfu_callbacks = {
	.callback = dfu_hook,
	.event_id = (MGMT_EVT_OP_IMG_MGMT_DFU_STARTED |
	             MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK |
	             MGMT_EVT_OP_IMG_MGMT_DFU_PENDING |
	             MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED),
};

int main(void)
{
	int ret;

	LOG_INF("SMP DFU Server starting...");

	/* Check if we need to confirm the running image.
	 * The 9P twin points you at a file to write (/dev/confirm). Here the
	 * equivalent is the `img confirm` mcumgr command — there is no
	 * namespace to discover it from.
	 */
	if (!boot_is_img_confirmed()) {
		LOG_WRN("Image not confirmed - will revert on next reboot!");
		LOG_WRN("Send `mcumgr image confirm <hash>` to make permanent");
	}

	mgmt_callback_register(&dfu_callbacks);

	/* Initialize Bluetooth. The SMP GATT service registers itself at boot
	 * (MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION, default y), so unlike
	 * the 9P twin there is no server/transport to bring up here.
	 */
	ret = bt_enable(NULL);
	if (ret < 0) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return ret;
	}
	LOG_INF("Bluetooth initialized");

	ret = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
	                                       BT_GAP_ADV_FAST_INT_MIN_2,
	                                       BT_GAP_ADV_FAST_INT_MAX_2, NULL),
	                      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret < 0) {
		LOG_ERR("Advertising failed to start: %d", ret);
		return ret;
	}

	LOG_INF("SMP DFU server advertising as '%s'", CONFIG_BT_DEVICE_NAME);
	LOG_INF("Upload: mcumgr image upload / smpmgr upload");

	return 0;
}
