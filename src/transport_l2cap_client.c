/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_l2cap_client.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#if defined(CONFIG_BT_GATT_CLIENT)
#include <zephyr/bluetooth/gatt.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* Platform detection: NCS vs mainline Zephyr vs ESP32 */
#if defined(CONFIG_NRF_MODEM_LIB) || defined(CONFIG_NCS_BOOT_BANNER)
#define NINEP_NCS_BUILD 1
#define NINEP_ESP32_BUILD 0
#include <zephyr/bluetooth/buf.h>
#include <zephyr/net_buf.h>
#elif defined(CONFIG_SOC_SERIES_ESP32S3) || defined(CONFIG_SOC_SERIES_ESP32)
#define NINEP_NCS_BUILD 0
#define NINEP_ESP32_BUILD 1
#include <zephyr/net_buf.h>
#else
#define NINEP_NCS_BUILD 0
#define NINEP_ESP32_BUILD 0
#include <zephyr/net_buf.h>
#endif

LOG_MODULE_REGISTER(ninep_l2cap_client, CONFIG_NINEP_LOG_LEVEL);

#if defined(CONFIG_BT_GATT_CLIENT)
/* 9PIS Transport Information Characteristic UUID: 39500004-feed-4a91-ba88-a1e0f6e4c001 */
#define BT_UUID_9PIS_TRANSPORT_VAL \
	BT_UUID_128_ENCODE(0x39500004, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)
static struct bt_uuid_128 uuid_9pis_transport = BT_UUID_INIT_128(BT_UUID_9PIS_TRANSPORT_VAL);

/* 9PIS Features Characteristic UUID: 39500002-feed-4a91-ba88-a1e0f6e4c001 */
#define BT_UUID_9PIS_FEATURES_VAL \
	BT_UUID_128_ENCODE(0x39500002, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)
static struct bt_uuid_128 uuid_9pis_features = BT_UUID_INIT_128(BT_UUID_9PIS_FEATURES_VAL);
#endif /* CONFIG_BT_GATT_CLIENT */

/* Forward declarations for global state tracking.
 * g_connecting_data is set during connection establishment.
 * g_active_data persists after connection for disconnect handling.
 * g_scan_data tracks the client during BLE scanning. */
static struct l2cap_client_data *g_connecting_data;
static struct l2cap_client_data *g_active_data;
static struct l2cap_client_data *g_scan_data;

/* RX state machine states */
enum l2cap_rx_state {
	RX_WAIT_SIZE,   /* Waiting for 4-byte size field */
	RX_WAIT_MSG     /* Waiting for message body */
};

/* L2CAP channel structure */
struct l2cap_client_chan {
	struct bt_l2cap_le_chan le;
	struct ninep_transport *transport;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	size_t rx_len;
	uint32_t rx_expected;
	enum l2cap_rx_state rx_state;
};

/* Transport private data */
struct l2cap_client_data {
	struct l2cap_client_chan channel;
	struct bt_conn *conn;
	enum ninep_l2cap_client_state state;
	struct ninep_transport *transport;

	/* Configuration (copied to avoid lifetime issues) */
	bt_addr_le_t target_addr;
	bool has_target_addr;
	bt_addr_le_t filter_addr;    /* MAC filter for UUID scanning */
	bool has_filter_addr;        /* If true, only connect to filter_addr during scan */
	bool use_accept_list;        /* If true, use BLE filter accept list during scan */
	uint16_t psm;
	uint8_t service_uuid128[16];   /* 128-bit UUID in little-endian */
	bool has_uuid128;              /* true if using 128-bit UUID */
	uint16_t service_uuid16;       /* fallback 16-bit UUID */
	ninep_l2cap_client_state_cb_t state_cb;
	void *state_cb_user_data;      /* user_data for state callback (preserved from init) */

	/* 9PIS GATT discovery (requires CONFIG_BT_GATT_CLIENT) */
	bool discover_9pis;            /* true to read PSM from 9PIS GATT */
	const char *required_features; /* If set, verify features contains this string */
#if defined(CONFIG_BT_GATT_CLIENT)
	struct bt_gatt_read_params read_params;
	char transport_info_buf[64];   /* Buffer for transport info string */
	char features_buf[128];        /* Buffer for features string */
	uint16_t discovered_psm;       /* PSM from 9PIS, 0 if not discovered */
	bool features_verified;        /* true after features check passed */
#endif

	/* Synchronization */
	struct k_sem connect_sem;
	int connect_err;  /* Error from BLE connection callback */

	/* Deferred connection work (to avoid blocking BT thread) */
	struct k_work connect_work;
	bt_addr_le_t discovered_addr;

	/* Deferred state callback work (to avoid calling from BLE callback context)
	 * Uses a message queue to preserve state transition order when multiple
	 * state changes happen before work runs (e.g., quick disconnect+reconnect) */
	struct k_work state_work;
	struct k_msgq state_queue;
	char state_queue_buffer[4 * sizeof(enum ninep_l2cap_client_state)];
};

/* Define TX buffer pool for L2CAP SDUs */
#define TX_BUF_COUNT 4
#define TX_BUF_SIZE BT_L2CAP_SDU_BUF_SIZE(CONFIG_NINEP_MAX_MESSAGE_SIZE)
NET_BUF_POOL_DEFINE(l2cap_client_tx_pool, TX_BUF_COUNT, TX_BUF_SIZE,
                    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

/* Low-latency connection parameters for keyboard input
 * Min interval: 6 × 1.25ms = 7.5ms (BLE minimum)
 * Max interval: 9 × 1.25ms = 11.25ms
 * Latency: 0 (respond to every event)
 * Timeout: 100 × 10ms = 1000ms
 */
static const struct bt_le_conn_param conn_param_low_latency = {
	.interval_min = 6,
	.interval_max = 9,
	.latency = 0,
	.timeout = 100,
};

/* Forward declarations */
static void set_state(struct l2cap_client_data *data,
                      enum ninep_l2cap_client_state new_state);
static int start_scan(struct l2cap_client_data *data);
static int connect_to_device(struct l2cap_client_data *data,
                             const bt_addr_le_t *addr);

/* L2CAP channel callbacks */
static void l2cap_connected(struct bt_l2cap_chan *chan)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_client_chan *ch = CONTAINER_OF(le_chan, struct l2cap_client_chan, le);
#else
	struct l2cap_client_chan *ch = CONTAINER_OF(chan, struct l2cap_client_chan, le.chan);
#endif
	struct l2cap_client_data *data = ch->transport->priv_data;

	LOG_DBG("l2cap_connected: chan=%p ch=%p ch->transport=%p priv_data=%p",
		chan, ch, ch->transport, data);
	LOG_DBG("  state_cb_user_data=%p state_cb=%p",
		data->state_cb_user_data, data->state_cb);

	LOG_INF("L2CAP channel connected (MTU: RX=%u, TX=%u, credits: RX=%d, TX=%d)",
	        ch->le.rx.mtu, ch->le.tx.mtu,
	        (int)atomic_get(&ch->le.rx.credits),
	        (int)atomic_get(&ch->le.tx.credits));
	LOG_INF("  RX CID=0x%04x, TX CID=0x%04x",
	        ch->le.rx.cid, ch->le.tx.cid);

	/* Reset RX state machine */
	ch->rx_len = 0;
	ch->rx_expected = 0;
	ch->rx_state = RX_WAIT_SIZE;

	/* Note: On mainline Zephyr, bt_l2cap_chan_recv_complete() requires a buffer
	 * and is meant to be called after recv() returns -EINPROGRESS. The initial
	 * 1 credit should be enough for the keyboard to send Rversion response. */

	set_state(data, NINEP_L2CAP_CLIENT_CONNECTED);
	k_sem_give(&data->connect_sem);
}

static void l2cap_disconnected(struct bt_l2cap_chan *chan)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_client_chan *ch = CONTAINER_OF(le_chan, struct l2cap_client_chan, le);
#else
	struct l2cap_client_chan *ch = CONTAINER_OF(chan, struct l2cap_client_chan, le.chan);
#endif
	struct l2cap_client_data *data = ch->transport->priv_data;

	LOG_INF("L2CAP channel disconnected");

	/* Reset state */
	ch->rx_len = 0;
	ch->rx_expected = 0;
	ch->rx_state = RX_WAIT_SIZE;

	/* DON'T clean up BLE connection here - let ble_disconnected() handle it.
	 * If we unref here, ble_disconnected() won't be able to properly clean up
	 * and the connection will remain in the BT stack's internal list. */

	/* DON'T clear g_active_data - ble_disconnected() needs it */

	set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
}

static int l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_client_chan *ch = CONTAINER_OF(le_chan, struct l2cap_client_chan, le);
#else
	struct l2cap_client_chan *ch = CONTAINER_OF(chan, struct l2cap_client_chan, le.chan);
#endif
	struct ninep_transport *transport = ch->transport;

	LOG_DBG("L2CAP recv: %u bytes", buf->len);

	/* Process all data in the buffer */
	while (buf->len > 0) {
		if (ch->rx_state == RX_WAIT_SIZE) {
			/* Reading 4-byte size field */
			size_t need = 4 - ch->rx_len;
			size_t copy = MIN(need, buf->len);

			memcpy(&ch->rx_buf[ch->rx_len], buf->data, copy);
			net_buf_pull(buf, copy);
			ch->rx_len += copy;

			if (ch->rx_len == 4) {
				/* Parse size field (little-endian) */
				ch->rx_expected = ch->rx_buf[0] |
				                  (ch->rx_buf[1] << 8) |
				                  (ch->rx_buf[2] << 16) |
				                  (ch->rx_buf[3] << 24);

				LOG_DBG("Message size: %u bytes", ch->rx_expected);

				/* Validate size */
				if (ch->rx_expected < 7 || ch->rx_expected > ch->rx_buf_size) {
					LOG_ERR("Invalid message size: %u (max: %zu)",
					        ch->rx_expected, ch->rx_buf_size);
					ch->rx_len = 0;
					ch->rx_state = RX_WAIT_SIZE;
					return -EINVAL;
				}

				/* Transition to message body state */
				ch->rx_state = RX_WAIT_MSG;
			}
		} else {
			/* Reading message body */
			size_t need = ch->rx_expected - ch->rx_len;
			size_t copy = MIN(need, buf->len);

			memcpy(&ch->rx_buf[ch->rx_len], buf->data, copy);
			net_buf_pull(buf, copy);
			ch->rx_len += copy;

			if (ch->rx_len == ch->rx_expected) {
				/* Complete message received */
				LOG_DBG("Complete message received: %u bytes (type=%u)",
				        ch->rx_len, ch->rx_buf[4]);

				/* Deliver to 9P layer */
				if (transport->recv_cb) {
					transport->recv_cb(transport, ch->rx_buf,
					                   ch->rx_len, transport->user_data);
				}

				/* Reset for next message */
				ch->rx_len = 0;
				ch->rx_expected = 0;
				ch->rx_state = RX_WAIT_SIZE;
			}
		}
	}

	/*
	 * Grant a credit back to the server after processing received data.
	 * This is needed on mainline Zephyr for credit-based flow control.
	 * ESP32 and NCS may handle credits differently.
	 */
#if !NINEP_NCS_BUILD && !NINEP_ESP32_BUILD
	bt_l2cap_chan_recv_complete(chan, buf);
#endif

	return 0;
}

#if NINEP_NCS_BUILD || NINEP_ESP32_BUILD
/* NCS and ESP32: .sent callback has no status parameter */
static void l2cap_sent(struct bt_l2cap_chan *chan)
{
	LOG_DBG("L2CAP data transmitted");
}
#else
/* Mainline Zephyr: .sent callback has status parameter */
static void l2cap_sent(struct bt_l2cap_chan *chan, int status)
{
	if (status < 0) {
		LOG_ERR("L2CAP send failed: %d", status);
	} else {
		LOG_DBG("L2CAP sent successfully");
	}
}
#endif

static struct bt_l2cap_chan_ops l2cap_chan_ops = {
	.connected = l2cap_connected,
	.disconnected = l2cap_disconnected,
	.recv = l2cap_recv,
	.sent = l2cap_sent,
};

#if defined(CONFIG_BT_GATT_CLIENT)
/*
 * Parse PSM from 9PIS Transport Information string.
 * Format: "l2cap:psm=0xNNNN,mtu=NNNN" or "l2cap:psm=0xNNNN"
 * Returns PSM value or 0 on parse failure.
 */
static uint16_t parse_transport_info_psm(const char *transport_info)
{
	const char *psm_str;
	char *end;
	unsigned long psm;

	if (!transport_info) {
		return 0;
	}

	/* Look for "psm=" */
	psm_str = strstr(transport_info, "psm=");
	if (!psm_str) {
		LOG_WRN("No 'psm=' found in transport info");
		return 0;
	}

	psm_str += 4;  /* Skip "psm=" */

	/* Parse hex or decimal */
	psm = strtoul(psm_str, &end, 0);
	if (end == psm_str || psm > 0xFFFF) {
		LOG_WRN("Invalid PSM value in transport info");
		return 0;
	}

	LOG_INF("Parsed PSM 0x%04lx from transport info", psm);
	return (uint16_t)psm;
}

/* Forward declarations */
static int start_l2cap_connect(struct l2cap_client_data *data);
static int start_transport_info_read(struct l2cap_client_data *data);

/*
 * GATT read callback for 9PIS Features characteristic
 * Verifies the device supports required features before proceeding
 */
static uint8_t features_read_cb(struct bt_conn *conn, uint8_t err,
				struct bt_gatt_read_params *params,
				const void *data, uint16_t length)
{
	struct l2cap_client_data *client_data = g_active_data;

	if (!client_data) {
		LOG_ERR("No active client data in features callback");
		return BT_GATT_ITER_STOP;
	}

	if (err) {
		LOG_WRN("Features read failed (err %d), assuming compatible", err);
		client_data->features_verified = true;
		goto read_transport;
	}

	if (!data) {
		/* Read complete */
		return BT_GATT_ITER_STOP;
	}

	/* Copy features string */
	size_t copy_len = MIN(length, sizeof(client_data->features_buf) - 1);
	memcpy(client_data->features_buf, data, copy_len);
	client_data->features_buf[copy_len] = '\0';

	LOG_INF("9PIS Features: %s", client_data->features_buf);

	/* Check if required features are present */
	if (client_data->required_features) {
		if (strstr(client_data->features_buf, client_data->required_features) == NULL) {
			LOG_WRN("Device features '%s' don't include required '%s' - disconnecting",
				client_data->features_buf, client_data->required_features);
			/* Disconnect and resume scanning */
			bt_conn_disconnect(client_data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			return BT_GATT_ITER_STOP;
		}
		LOG_INF("Required feature '%s' verified", client_data->required_features);
	}

	client_data->features_verified = true;

read_transport:
	/* Proceed to read transport info */
	start_transport_info_read(client_data);
	return BT_GATT_ITER_STOP;
}

/*
 * GATT read callback for 9PIS Transport Information characteristic
 */
static uint8_t transport_info_read_cb(struct bt_conn *conn, uint8_t err,
				      struct bt_gatt_read_params *params,
				      const void *data, uint16_t length)
{
	struct l2cap_client_data *client_data = g_active_data;

	if (!client_data) {
		LOG_ERR("No active client data in GATT callback");
		return BT_GATT_ITER_STOP;
	}

	if (err) {
		LOG_WRN("9PIS read failed (err %d), using fallback PSM 0x%04x",
			err, client_data->psm);
		goto connect_l2cap;
	}

	if (!data) {
		/* Read complete */
		LOG_DBG("9PIS read complete");
		return BT_GATT_ITER_STOP;
	}

	/* Copy transport info (ensure null termination) */
	size_t copy_len = MIN(length, sizeof(client_data->transport_info_buf) - 1);
	memcpy(client_data->transport_info_buf, data, copy_len);
	client_data->transport_info_buf[copy_len] = '\0';

	LOG_INF("9PIS Transport Info: %s", client_data->transport_info_buf);

	/* Parse PSM from transport info */
	client_data->discovered_psm = parse_transport_info_psm(client_data->transport_info_buf);

connect_l2cap:
	/* Proceed with L2CAP connection using discovered or fallback PSM */
	if (start_l2cap_connect(client_data) < 0) {
		set_state(client_data, NINEP_L2CAP_CLIENT_DISCONNECTED);
	}

	return BT_GATT_ITER_STOP;
}

/*
 * Start reading 9PIS Transport Information characteristic (step 2 of discovery)
 */
static int start_transport_info_read(struct l2cap_client_data *data)
{
	int ret;

	LOG_INF("Reading 9PIS Transport Info...");

	memset(&data->read_params, 0, sizeof(data->read_params));
	data->read_params.func = transport_info_read_cb;
	data->read_params.handle_count = 0;  /* Read by UUID */
	data->read_params.by_uuid.start_handle = 0x0001;
	data->read_params.by_uuid.end_handle = 0xFFFF;
	data->read_params.by_uuid.uuid = &uuid_9pis_transport.uuid;

	data->discovered_psm = 0;
	memset(data->transport_info_buf, 0, sizeof(data->transport_info_buf));

	ret = bt_gatt_read(data->conn, &data->read_params);
	if (ret < 0) {
		LOG_WRN("Transport info read failed: %d, using fallback PSM 0x%04x",
			ret, data->psm);
		/* Fall through to L2CAP connect anyway */
		if (start_l2cap_connect(data) < 0) {
			set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
		}
		return ret;
	}

	return 0;
}

/*
 * Start 9PIS GATT discovery
 * If required_features is set, reads Features characteristic first to verify
 * the device supports the required feature before connecting.
 */
static int start_9pis_discovery(struct l2cap_client_data *data)
{
	int ret;

	LOG_INF("Starting 9PIS GATT discovery...");
	set_state(data, NINEP_L2CAP_CLIENT_DISCOVERING);

	data->features_verified = false;
	memset(data->features_buf, 0, sizeof(data->features_buf));

	/* If required_features is set, read features first to verify compatibility */
	if (data->required_features) {
		LOG_INF("Verifying required feature: %s", data->required_features);

		memset(&data->read_params, 0, sizeof(data->read_params));
		data->read_params.func = features_read_cb;
		data->read_params.handle_count = 0;  /* Read by UUID */
		data->read_params.by_uuid.start_handle = 0x0001;
		data->read_params.by_uuid.end_handle = 0xFFFF;
		data->read_params.by_uuid.uuid = &uuid_9pis_features.uuid;

		ret = bt_gatt_read(data->conn, &data->read_params);
		if (ret < 0) {
			LOG_WRN("Features read failed: %d, skipping verification", ret);
			/* Fall through to transport info read */
			return start_transport_info_read(data);
		}
		return 0;  /* features_read_cb will call start_transport_info_read */
	}

	/* No features check needed, go directly to transport info */
	return start_transport_info_read(data);
}
#endif /* CONFIG_BT_GATT_CLIENT */

/*
 * Start L2CAP channel connection (called after BLE connect, optionally after 9PIS discovery)
 */
static int start_l2cap_connect(struct l2cap_client_data *data)
{
	int ret;
	uint16_t psm_to_use;

#if defined(CONFIG_BT_GATT_CLIENT)
	/* Use discovered PSM if available, otherwise fallback */
	psm_to_use = (data->discovered_psm != 0) ? data->discovered_psm : data->psm;
#else
	psm_to_use = data->psm;
#endif

	/* Validate PSM - 0 is not a valid L2CAP PSM */
	if (psm_to_use == 0) {
		LOG_ERR("Invalid PSM 0 - 9PIS discovery failed and no fallback PSM configured");
		bt_conn_disconnect(data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(data->conn);
		data->conn = NULL;
		return -EINVAL;
	}

	/* Request security/bonding BEFORE L2CAP connect.
	 * This triggers SMP pairing which exchanges IRK for address resolution.
	 * Must be done before L2CAP or security request may fail. */
	ret = bt_conn_set_security(data->conn, BT_SECURITY_L2);
	if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("Failed to request security: %d (continuing anyway)", ret);
	} else {
		LOG_INF("Security level L2 requested (bonding enabled)");
	}

	/* Initialize L2CAP channel - preserve rx_buf settings */
	uint8_t *saved_rx_buf = data->channel.rx_buf;
	size_t saved_rx_buf_size = data->channel.rx_buf_size;
	memset(&data->channel, 0, sizeof(data->channel));
	data->channel.le.chan.ops = &l2cap_chan_ops;
	data->channel.le.rx.mtu = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	data->channel.transport = data->transport;
	data->channel.rx_buf = saved_rx_buf;
	data->channel.rx_buf_size = saved_rx_buf_size;

	LOG_INF("Initiating L2CAP channel to PSM 0x%04x", psm_to_use);
	ret = bt_l2cap_chan_connect(data->conn, &data->channel.le.chan, psm_to_use);
	if (ret < 0) {
		LOG_ERR("L2CAP connect failed: %d", ret);
		bt_conn_disconnect(data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(data->conn);
		data->conn = NULL;
		return ret;
	}

	LOG_INF("L2CAP connect initiated");
	return 0;
}

/* BLE connection callbacks */

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
	struct l2cap_client_data *data = g_connecting_data;
	char addr_str[BT_ADDR_LE_STR_LEN];
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	LOG_INF("ble_connected callback: addr=%s err=%d data=%p", addr_str, err, data);

	if (!data) {
		LOG_WRN("ble_connected: no g_connecting_data, ignoring");
		return;
	}

	/* Check if this is our connection (we're in CONNECTING state) */
	if (data->state != NINEP_L2CAP_CLIENT_CONNECTING) {
		LOG_WRN("ble_connected: not in CONNECTING state (%d), ignoring", data->state);
		return;
	}

	g_connecting_data = NULL;

	if (err) {
		LOG_ERR("BLE connection failed: %d", err);
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
		return;
	}

	/* Store the connection reference and track active data for disconnect handling */
	data->conn = bt_conn_ref(conn);
	g_active_data = data;
	LOG_INF("BLE connected successfully to %s", addr_str);

#if defined(CONFIG_BT_GATT_CLIENT)
	/* If 9PIS discovery is enabled, read Transport Info to get PSM */
	if (data->discover_9pis) {
		int ret = start_9pis_discovery(data);
		if (ret == 0) {
			/* Callback will handle L2CAP connect */
			return;
		}
		/* Discovery failed, fall through to direct L2CAP connect */
		LOG_WRN("9PIS discovery failed, using configured PSM");
	}
#endif

	/* Direct L2CAP connect */
	if (start_l2cap_connect(data) < 0) {
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
	}
}

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
	/* Try g_connecting_data first (for disconnects during connection),
	 * then g_active_data (for disconnects after successful connection) */
	struct l2cap_client_data *data = g_connecting_data;
	if (!data) {
		data = g_active_data;
	}

	LOG_INF("BLE disconnected (reason: 0x%02x, data=%p)", reason, data);

	/* Clean up connection reference if it matches ours.
	 * This handles cases where BLE disconnects before L2CAP is established,
	 * or after L2CAP has already cleaned up. */
	if (data && data->conn == conn) {
		LOG_INF("Cleaning up BLE connection reference");
		bt_conn_unref(data->conn);
		data->conn = NULL;
		g_active_data = NULL;
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
	} else if (data && data->conn == NULL) {
		/* L2CAP already cleaned up, just clear active data */
		LOG_INF("BLE disconnected after L2CAP cleanup");
		g_active_data = NULL;
	}
}

static void ble_security_changed(struct bt_conn *conn, bt_security_t level,
				 enum bt_security_err err)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

	if (err) {
		LOG_ERR("Security change failed: %s level %u err %d", addr_str, level, err);
	} else {
		LOG_INF("Security changed: %s level %u", addr_str, level);
	}
}

static struct bt_conn_cb conn_callbacks = {
	.connected = ble_connected,
	.disconnected = ble_disconnected,
	.security_changed = ble_security_changed,
};

static bool conn_cb_registered = false;

/* State management - defer callback to work queue to avoid calling from BLE context */
static void set_state(struct l2cap_client_data *data,
                      enum ninep_l2cap_client_state new_state)
{
	if (data->state != new_state) {
		LOG_INF("L2CAP client state: %d -> %d", data->state, new_state);
		data->state = new_state;

		if (data->state_cb) {
			/* Queue state for deferred callback - BLE callbacks may be in
			 * ISR or BT thread context where mutex/condvar ops fail.
			 * Using a queue preserves ordering when multiple state changes
			 * happen before work runs (e.g., disconnect then quick reconnect). */
			int ret = k_msgq_put(&data->state_queue, &new_state, K_NO_WAIT);
			if (ret == 0) {
				k_work_submit(&data->state_work);
			} else {
				LOG_ERR("State queue full, dropping state %d", new_state);
			}
		}
	}
}

static int connect_l2cap(struct l2cap_client_data *data)
{
	int ret;

	LOG_INF("Connecting L2CAP channel to PSM 0x%04x", data->psm);

	/* Elevate security before L2CAP connection - keyboard likely requires encryption */
	ret = bt_conn_set_security(data->conn, BT_SECURITY_L2);
	if (ret < 0) {
		LOG_WRN("Failed to set security level: %d (continuing anyway)", ret);
	} else {
		LOG_INF("Security level set to L2 (encryption)");
	}

	/* Initialize channel structure */
	memset(&data->channel, 0, sizeof(data->channel));
	data->channel.le.chan.ops = &l2cap_chan_ops;
	data->channel.le.rx.mtu = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	data->channel.transport = data->transport;

	ret = bt_l2cap_chan_connect(data->conn, &data->channel.le.chan, data->psm);
	if (ret < 0) {
		LOG_ERR("L2CAP connect failed: %d", ret);
		return ret;
	}

	LOG_INF("L2CAP connect initiated");
	return 0;
}

static int connect_to_device(struct l2cap_client_data *data,
                             const bt_addr_le_t *addr)
{
	int ret;
	char addr_str[BT_ADDR_LE_STR_LEN];
	struct bt_conn *existing;

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	LOG_INF("Connecting to %s", addr_str);

	/* Check if we already have a connection reference that wasn't cleaned up */
	if (data->conn) {
		LOG_WRN("Stale connection reference found, cleaning up");
		bt_conn_disconnect(data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(data->conn);
		data->conn = NULL;
		k_sleep(K_MSEC(100));
	}

	/* Check if BT stack already has a connection to this address.
	 * If so, wait for it to be fully removed before creating a new one.
	 * This handles the case where keyboard went to sleep and we're reconnecting. */
	existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
	if (existing) {
		LOG_WRN("Existing BT connection found, disconnecting first");
		bt_conn_disconnect(existing, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(existing);

		/* Wait for connection to be fully removed (up to 3 seconds) */
		int wait_count = 0;
		while (wait_count < 30) {
			k_sleep(K_MSEC(100));
			existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
			if (!existing) {
				LOG_INF("Old connection removed after %d ms", (wait_count + 1) * 100);
				break;
			}
			/* Connection still exists - try disconnecting again every 500ms */
			if (wait_count > 0 && (wait_count % 5) == 0) {
				LOG_WRN("Retrying disconnect after %d ms", wait_count * 100);
				bt_conn_disconnect(existing, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			}
			bt_conn_unref(existing);
			wait_count++;
		}
		if (wait_count >= 30) {
			/* Last resort: try to proceed anyway - the BT stack might
			 * handle the collision, or we'll fail cleanly */
			LOG_WRN("Old connection still exists after 3s, proceeding anyway");
		}
	}

	set_state(data, NINEP_L2CAP_CLIENT_CONNECTING);

	/* Set global for connection callback context */
	g_connecting_data = data;

	/* Create BLE connection with low-latency parameters for keyboard input.
	 * Callbacks handle the rest:
	 * 1. ble_connected() will initiate L2CAP
	 * 2. l2cap_connected() will set state to CONNECTED
	 *
	 * IMPORTANT: bt_conn_le_create() returns a reference that we must release.
	 * We'll take a new reference in ble_connected() when we actually need it.
	 * If we don't release here, we leak a reference and the connection can
	 * never be fully cleaned up. */
	struct bt_conn *conn = NULL;
	ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
	                        &conn_param_low_latency, &conn);
	if (ret < 0) {
		LOG_ERR("Failed to create connection: %d", ret);
		g_connecting_data = NULL;
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
		return ret;
	}

	/* Release the reference from bt_conn_le_create().
	 * The BT stack keeps its own internal reference until disconnect.
	 * We'll take a reference in ble_connected() callback. */
	if (conn) {
		bt_conn_unref(conn);
	}

	LOG_INF("BLE connection initiated, callbacks will handle L2CAP");
	return 0;  /* Success - connection in progress, callbacks handle the rest */
}

/* Work handler for deferred connection (runs outside BT thread) */
static void connect_work_handler(struct k_work *work)
{
	struct l2cap_client_data *data = CONTAINER_OF(work, struct l2cap_client_data, connect_work);
	char addr_str[BT_ADDR_LE_STR_LEN];
	int ret;

	/* Stop scanning first - must be done outside scan callback context
	 * to avoid ESP32 BLE controller issues */
	ret = bt_le_scan_stop();
	if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("Scan stop returned %d (continuing anyway)", ret);
	}

	bt_addr_le_to_str(&data->discovered_addr, addr_str, sizeof(addr_str));
	LOG_INF("Deferred connect starting to %s", addr_str);

	ret = connect_to_device(data, &data->discovered_addr);
	if (ret < 0) {
		LOG_ERR("Deferred connect failed: %d", ret);
	}
}

/* Work handler for deferred state callback (runs outside BT callback context) */
static void state_work_handler(struct k_work *work)
{
	struct l2cap_client_data *data = CONTAINER_OF(work, struct l2cap_client_data, state_work);
	enum ninep_l2cap_client_state state;

	LOG_DBG("state_work_handler: work=%p data=%p transport=%p state_cb_user_data=%p",
		work, data, data->transport, data->state_cb_user_data);

	/* Drain the queue - process all pending state transitions in order */
	while (k_msgq_get(&data->state_queue, &state, K_NO_WAIT) == 0) {
		LOG_DBG("Deferred state callback: state=%d", state);

		if (data->state_cb) {
			/* Use preserved state_cb_user_data, NOT transport->user_data
			 * (ninep_client_init overwrites transport->user_data with client ptr) */
			data->state_cb(data->transport, state, data->state_cb_user_data);
		}
	}
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                    struct net_buf_simple *ad)
{
	struct l2cap_client_data *data = g_scan_data;
	char addr_str[BT_ADDR_LE_STR_LEN];

	if (!data || data->state != NINEP_L2CAP_CLIENT_SCANNING) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	/* Only process connectable advertisements */
	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	LOG_DBG("Scan: %s (RSSI %d)", addr_str, rssi);

	/* Parse advertisement data for service UUID */
	bool found = false;
	while (ad->len > 1) {
		uint8_t len = net_buf_simple_pull_u8(ad);
		if (len == 0 || len > ad->len) {
			break;
		}

		uint8_t ad_type = net_buf_simple_pull_u8(ad);
		len--;

		/* Check for 128-bit UUID if we're looking for one */
		if (data->has_uuid128 &&
		    (ad_type == BT_DATA_UUID128_ALL || ad_type == BT_DATA_UUID128_SOME) &&
		    len >= 16) {
			/* Check each 128-bit UUID in the list */
			for (int i = 0; i + 15 < len; i += 16) {
				if (memcmp(&ad->data[i], data->service_uuid128, 16) == 0) {
					LOG_INF("Found 9PIS service at %s", addr_str);
					found = true;
					break;
				}
			}
		}

		/* Check for 16-bit UUID (fallback or primary if no 128-bit) */
		if (!found && data->service_uuid16 != 0 &&
		    (ad_type == BT_DATA_UUID16_ALL || ad_type == BT_DATA_UUID16_SOME) &&
		    len >= 2) {
			for (int i = 0; i + 1 < len; i += 2) {
				uint16_t uuid = sys_get_le16(&ad->data[i]);
				if (uuid == data->service_uuid16) {
					LOG_INF("Found service 0x%04x at %s",
					        uuid, addr_str);
					found = true;
					break;
				}
			}
		}

		if (found) {
			break;
		}

		net_buf_simple_pull(ad, len);
	}

	if (found) {
		/* Check MAC filter - if set, only connect to matching device */
		if (data->has_filter_addr) {
			char filter_str[BT_ADDR_LE_STR_LEN];
			bt_addr_le_to_str(&data->filter_addr, filter_str, sizeof(filter_str));
			LOG_INF("Checking filter: found=%s, want=%s", addr_str, filter_str);
			if (bt_addr_le_cmp(addr, &data->filter_addr) != 0) {
				LOG_INF("Skipping %s (doesn't match filter %s)", addr_str, filter_str);
				return;  /* Continue scanning for the right device */
			}
			LOG_INF("Found paired device %s", addr_str);
		}

		/* Check if we're already connected to this device */
		struct bt_conn *existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
		if (existing) {
			LOG_WRN("Device %s already connected, skipping", addr_str);
			bt_conn_unref(existing);
			return;  /* Continue scanning for other devices */
		}

		/* Mark that we found the target - the work handler will stop scan.
		 * Don't call bt_le_scan_stop() from scan callback context as it
		 * can cause ESP32 BLE controller to become unresponsive. */
		g_scan_data = NULL;

		/* Save address and schedule deferred connection */
		bt_addr_le_copy(&data->discovered_addr, addr);

		LOG_INF("Submitting connect work...");
		int ret = k_work_submit(&data->connect_work);
		if (ret == 0) {
			LOG_WRN("Connect work was already pending (ret=0)");
		} else if (ret == 1) {
			LOG_INF("Connect work submitted successfully");
		} else {
			LOG_ERR("Connect work submit failed: %d", ret);
		}
	}
}

static int start_scan(struct l2cap_client_data *data)
{
	int ret;

	/* Check if another transport is already scanning */
	if (g_scan_data != NULL && g_scan_data != data) {
		LOG_WRN("Another scan already in progress, cannot start new scan");
		return -EBUSY;
	}

	if (data->has_uuid128) {
		LOG_INF("Starting scan for 9PIS service (128-bit UUID, accept_list=%d)",
			data->use_accept_list);
	} else {
		LOG_INF("Starting scan for service UUID 0x%04x (accept_list=%d)",
			data->service_uuid16, data->use_accept_list);
	}

	set_state(data, NINEP_L2CAP_CLIENT_SCANNING);
	g_scan_data = data;

	/* Build scan options based on configuration.
	 * When use_accept_list is enabled, only devices in the BLE filter accept
	 * list (or whose RPA resolves to an identity in the list) are reported.
	 * This enables reconnection to bonded devices using BLE privacy/RPA. */
	uint32_t scan_options = BT_LE_SCAN_OPT_FILTER_DUPLICATE;
	if (data->use_accept_list) {
		scan_options |= BT_LE_SCAN_OPT_FILTER_ACCEPT_LIST;
		LOG_INF("Using filter accept list for IRK-based resolution");
	}

	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = scan_options,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	ret = bt_le_scan_start(&scan_param, scan_cb);
	if (ret < 0) {
		if (ret == -EAGAIN) {
			/* BT not ready yet - expected during early boot */
			LOG_WRN("Scan start: BT not ready (will retry)");
		} else {
			LOG_ERR("Scan start failed: %d", ret);
		}
		g_scan_data = NULL;
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
		return ret;
	}

	return 0;
}

/* Transport operations */
static int l2cap_client_send(struct ninep_transport *transport,
                             const uint8_t *buf, size_t len)
{
	struct l2cap_client_data *data = transport->priv_data;
	struct net_buf *msg_buf;
	int ret;

	LOG_DBG("L2CAP send: %zu bytes (type=%u)", len, len >= 5 ? buf[4] : 0);

	if (!data || data->state != NINEP_L2CAP_CLIENT_CONNECTED) {
		LOG_ERR("L2CAP send: not connected (state=%d)", data ? data->state : -1);
		return -ENOTCONN;
	}

	/* Log channel state for debugging */
	LOG_DBG("L2CAP TX state: mtu=%u, mps=%u, credits=%d",
	        data->channel.le.tx.mtu,
	        data->channel.le.tx.mps,
	        (int)atomic_get(&data->channel.le.tx.credits));

	/* HACK: If no credits, grant ourselves some to test sending */
	if (atomic_get(&data->channel.le.tx.credits) == 0) {
		LOG_WRN("No credits received! Forcing 10 credits for testing...");
		atomic_set(&data->channel.le.tx.credits, 10);
	}

	/* Allocate buffer */
	msg_buf = net_buf_alloc(&l2cap_client_tx_pool, K_MSEC(100));
	if (!msg_buf) {
		LOG_ERR("Failed to allocate net_buf");
		return -ENOMEM;
	}

	/* Reserve L2CAP header space */
	net_buf_reserve(msg_buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);

	/* Copy message data */
	net_buf_add_mem(msg_buf, buf, len);

	/* Send via L2CAP */
	ret = bt_l2cap_chan_send(&data->channel.le.chan, msg_buf);
	if (ret < 0) {
		LOG_ERR("bt_l2cap_chan_send failed: %d", ret);
		net_buf_unref(msg_buf);
		return ret;
	}

	LOG_DBG("L2CAP send queued (ret=%d)", ret);

	LOG_DBG("Sent %zu bytes via L2CAP", len);
	return len;
}

static int l2cap_client_start(struct ninep_transport *transport)
{
	struct l2cap_client_data *data = transport->priv_data;

	if (!data) {
		return -EINVAL;
	}

	/* If already scanning or connecting, treat as success (idempotent start) */
	if (data->state == NINEP_L2CAP_CLIENT_SCANNING ||
	    data->state == NINEP_L2CAP_CLIENT_CONNECTING ||
	    data->state == NINEP_L2CAP_CLIENT_CONNECTED) {
		LOG_DBG("Transport already active (state=%d)", data->state);
		return 0;
	}

	/* Register connection callbacks (once) */
	if (!conn_cb_registered) {
		bt_conn_cb_register(&conn_callbacks);
		conn_cb_registered = true;
	}

	if (data->has_target_addr) {
		/* Direct connect to known address */
		return connect_to_device(data, &data->target_addr);
	} else {
		/* Scan for service UUID */
		return start_scan(data);
	}
}

static int l2cap_client_stop(struct ninep_transport *transport)
{
	struct l2cap_client_data *data = transport->priv_data;

	if (!data) {
		return -EINVAL;
	}

	LOG_INF("L2CAP client stop (state=%d)", data->state);

	/* Cancel any pending work items and purge state queue to prevent
	 * stale callbacks. Note: we keep state_cb so it works on restart. */
	k_work_cancel(&data->connect_work);
	k_work_cancel(&data->state_work);
	k_msgq_purge(&data->state_queue);

	/* Stop scanning if active */
	if (g_scan_data == data) {
		bt_le_scan_stop();
		g_scan_data = NULL;
	}

	/* Clear global references to avoid stale pointers */
	if (g_connecting_data == data) {
		g_connecting_data = NULL;
	}
	if (g_active_data == data) {
		g_active_data = NULL;
	}

	/* Disconnect L2CAP channel */
	if (data->state == NINEP_L2CAP_CLIENT_CONNECTED) {
		bt_l2cap_chan_disconnect(&data->channel.le.chan);
	}

	/* Disconnect BLE */
	if (data->conn) {
		bt_conn_disconnect(data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(data->conn);
		data->conn = NULL;
	}

	/* Set state directly - don't use set_state() which would re-queue work
	 * as that would cause stale callbacks after stop returns */
	data->state = NINEP_L2CAP_CLIENT_DISCONNECTED;

	/* Don't free data - transport may be restarted for reconnection */
	return 0;
}

static int l2cap_client_get_mtu(struct ninep_transport *transport)
{
	struct l2cap_client_data *data = transport->priv_data;

	if (!data) {
		return -EINVAL;
	}

	if (data->state == NINEP_L2CAP_CLIENT_CONNECTED) {
		return data->channel.le.tx.mtu;
	}

	/* Not connected - return configured max */
	return CONFIG_NINEP_MAX_MESSAGE_SIZE;
}

static const struct ninep_transport_ops l2cap_client_ops = {
	.send = l2cap_client_send,
	.start = l2cap_client_start,
	.stop = l2cap_client_stop,
	.get_mtu = l2cap_client_get_mtu,
};

/* Public API */
int ninep_transport_l2cap_client_init(struct ninep_transport *transport,
                                       const struct ninep_transport_l2cap_client_config *config,
                                       ninep_transport_recv_cb_t recv_cb,
                                       void *user_data)
{
	struct l2cap_client_data *data;

	if (!transport || !config || !config->rx_buf || config->rx_buf_size == 0) {
		return -EINVAL;
	}

	if (!config->target_addr && !config->service_uuid128 && config->service_uuid16 == 0) {
		LOG_ERR("Must provide target_addr, service_uuid128, or service_uuid16");
		return -EINVAL;
	}

	/* Allocate private data */
	data = k_malloc(sizeof(*data));
	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));

	/* Copy configuration */
	if (config->target_addr) {
		bt_addr_le_copy(&data->target_addr, config->target_addr);
		data->has_target_addr = true;
	}
	data->psm = config->psm;
	if (config->service_uuid128) {
		memcpy(data->service_uuid128, config->service_uuid128, 16);
		data->has_uuid128 = true;
	}
	data->service_uuid16 = config->service_uuid16;
	data->state_cb = config->state_cb;
	data->state_cb_user_data = user_data;  /* Preserve for state callback (ninep_client_init overwrites transport->user_data) */
	data->discover_9pis = config->discover_9pis;
	data->required_features = config->required_features;
	data->transport = transport;

	/* Initialize channel */
	data->channel.rx_buf = config->rx_buf;
	data->channel.rx_buf_size = config->rx_buf_size;
	data->channel.transport = transport;

	/* Initialize semaphore and work items */
	k_sem_init(&data->connect_sem, 0, 1);
	k_work_init(&data->connect_work, connect_work_handler);
	k_work_init(&data->state_work, state_work_handler);
	k_msgq_init(&data->state_queue, data->state_queue_buffer,
	            sizeof(enum ninep_l2cap_client_state), 4);

	/* Initialize transport */
	transport->ops = &l2cap_client_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

	LOG_DBG("Transport init: transport=%p priv_data=%p state_cb_user_data=%p state_cb=%p",
		transport, data, data->state_cb_user_data, data->state_cb);

	if (data->has_uuid128) {
		LOG_INF("L2CAP client transport initialized (PSM: 0x%04x, 128-bit UUID, 9PIS: %s, RX buf: %zu bytes)",
		        config->psm, data->discover_9pis ? "yes" : "no", config->rx_buf_size);
	} else if (data->service_uuid16) {
		LOG_INF("L2CAP client transport initialized (PSM: 0x%04x, UUID16: 0x%04x, 9PIS: %s, RX buf: %zu bytes)",
		        config->psm, config->service_uuid16, data->discover_9pis ? "yes" : "no", config->rx_buf_size);
	} else {
		LOG_INF("L2CAP client transport initialized (PSM: 0x%04x, direct addr, 9PIS: %s, RX buf: %zu bytes)",
		        config->psm, data->discover_9pis ? "yes" : "no", config->rx_buf_size);
	}

	return 0;
}

enum ninep_l2cap_client_state ninep_transport_l2cap_client_get_state(
	struct ninep_transport *transport)
{
	struct l2cap_client_data *data;

	if (!transport || !transport->priv_data) {
		return NINEP_L2CAP_CLIENT_DISCONNECTED;
	}

	data = transport->priv_data;
	return data->state;
}

struct bt_conn *ninep_transport_l2cap_client_get_conn(
	struct ninep_transport *transport)
{
	struct l2cap_client_data *data;

	if (!transport || !transport->priv_data) {
		return NULL;
	}

	data = transport->priv_data;
	return data->conn;
}

int ninep_transport_l2cap_client_set_target(struct ninep_transport *transport,
					    const bt_addr_le_t *addr)
{
	struct l2cap_client_data *data;

	if (!transport || !transport->priv_data) {
		return -EINVAL;
	}

	data = transport->priv_data;

	/* Only allow target change when disconnected */
	if (data->state != NINEP_L2CAP_CLIENT_DISCONNECTED) {
		LOG_WRN("Cannot change target while connected/connecting");
		return -EBUSY;
	}

	if (addr) {
		bt_addr_le_copy(&data->target_addr, addr);
		data->has_target_addr = true;
		LOG_INF("Target address set for direct connect");
	} else {
		memset(&data->target_addr, 0, sizeof(data->target_addr));
		data->has_target_addr = false;
		LOG_INF("Target address cleared, will use UUID scanning");
	}

	return 0;
}

int ninep_transport_l2cap_client_set_filter(struct ninep_transport *transport,
					    const bt_addr_le_t *addr)
{
	struct l2cap_client_data *data;

	if (!transport || !transport->priv_data) {
		return -EINVAL;
	}

	data = transport->priv_data;

	/* Only allow filter change when disconnected */
	if (data->state != NINEP_L2CAP_CLIENT_DISCONNECTED) {
		LOG_WRN("Cannot change filter while connected/connecting");
		return -EBUSY;
	}

	if (addr) {
		char addr_str[BT_ADDR_LE_STR_LEN];
		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		bt_addr_le_copy(&data->filter_addr, addr);
		data->has_filter_addr = true;
		LOG_INF("Scan filter set: only connect to %s", addr_str);
	} else {
		memset(&data->filter_addr, 0, sizeof(data->filter_addr));
		data->has_filter_addr = false;
		LOG_INF("Scan filter cleared, will connect to any matching device");
	}

	return 0;
}

int ninep_transport_l2cap_client_set_accept_list(struct ninep_transport *transport,
						 bool enable)
{
	struct l2cap_client_data *data;

	if (!transport || !transport->priv_data) {
		return -EINVAL;
	}

	data = transport->priv_data;

	/* Only allow change when disconnected */
	if (data->state != NINEP_L2CAP_CLIENT_DISCONNECTED) {
		LOG_WRN("Cannot change accept list mode while connected/connecting");
		return -EBUSY;
	}

	data->use_accept_list = enable;
	LOG_INF("Accept list scanning %s", enable ? "enabled" : "disabled");

	return 0;
}
