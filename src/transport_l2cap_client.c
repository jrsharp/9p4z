/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_l2cap_client.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
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
	uint16_t psm;
	uint16_t service_uuid;
	ninep_l2cap_client_state_cb_t state_cb;

	/* Synchronization */
	struct k_sem connect_sem;
	int connect_err;  /* Error from BLE connection callback */

	/* Deferred connection work (to avoid blocking BT thread) */
	struct k_work connect_work;
	bt_addr_le_t discovered_addr;
};

/* Define TX buffer pool for L2CAP SDUs */
#define TX_BUF_COUNT 4
#define TX_BUF_SIZE BT_L2CAP_SDU_BUF_SIZE(CONFIG_NINEP_MAX_MESSAGE_SIZE)
NET_BUF_POOL_DEFINE(l2cap_client_tx_pool, TX_BUF_COUNT, TX_BUF_SIZE,
                    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

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

	/* Clean up BLE connection to avoid EINVAL on reconnect */
	if (data->conn) {
		bt_conn_disconnect(data->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(data->conn);
		data->conn = NULL;
	}

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

/* BLE connection callbacks */
/* Forward declaration for accessing data from connection */
static struct l2cap_client_data *g_connecting_data = NULL;

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
	struct l2cap_client_data *data = g_connecting_data;
	char addr_str[BT_ADDR_LE_STR_LEN];
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);
	int ret;

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

	/* Store the connection reference */
	data->conn = bt_conn_ref(conn);
	LOG_INF("BLE connected successfully to %s", addr_str);

	/* Initialize L2CAP channel and connect immediately (like old code did)
	 * Note: Preserve rx_buf and rx_buf_size set during init - only zero the le struct */
	uint8_t *saved_rx_buf = data->channel.rx_buf;
	size_t saved_rx_buf_size = data->channel.rx_buf_size;
	memset(&data->channel, 0, sizeof(data->channel));
	data->channel.le.chan.ops = &l2cap_chan_ops;
	data->channel.le.rx.mtu = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	data->channel.transport = data->transport;
	data->channel.rx_buf = saved_rx_buf;
	data->channel.rx_buf_size = saved_rx_buf_size;

	LOG_INF("Initiating L2CAP channel to PSM 0x%04x", data->psm);
	ret = bt_l2cap_chan_connect(conn, &data->channel.le.chan, data->psm);
	if (ret < 0) {
		LOG_ERR("L2CAP connect failed: %d", ret);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(data->conn);
		data->conn = NULL;
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
	} else {
		LOG_INF("L2CAP connect initiated");
		/* l2cap_connected callback will set state to CONNECTED */
	}
}

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct l2cap_client_data *data = g_connecting_data;

	LOG_INF("BLE disconnected (reason: 0x%02x)", reason);

	/* Clean up connection reference if it matches ours.
	 * This handles cases where BLE disconnects before L2CAP is established,
	 * or after L2CAP has already cleaned up. */
	if (data && data->conn == conn) {
		bt_conn_unref(data->conn);
		data->conn = NULL;
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
	}
}

static struct bt_conn_cb conn_callbacks = {
	.connected = ble_connected,
	.disconnected = ble_disconnected,
};

static bool conn_cb_registered = false;

/* State management */
static void set_state(struct l2cap_client_data *data,
                      enum ninep_l2cap_client_state new_state)
{
	if (data->state != new_state) {
		LOG_INF("L2CAP client state: %d -> %d", data->state, new_state);
		data->state = new_state;

		if (data->state_cb) {
			data->state_cb(data->transport, new_state,
			               data->transport->user_data);
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

	/* Check if BT stack already has a connection to this address */
	existing = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr);
	if (existing) {
		LOG_WRN("Existing BT connection found, disconnecting first");
		bt_conn_disconnect(existing, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(existing);
		k_sleep(K_MSEC(200));
	}

	set_state(data, NINEP_L2CAP_CLIENT_CONNECTING);

	/* Set global for connection callback context */
	g_connecting_data = data;

	/* Create BLE connection - callbacks handle the rest:
	 * 1. ble_connected() will initiate L2CAP
	 * 2. l2cap_connected() will set state to CONNECTED */
	ret = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
	                        BT_LE_CONN_PARAM_DEFAULT, &data->conn);
	if (ret < 0) {
		LOG_ERR("Failed to create connection: %d", ret);
		g_connecting_data = NULL;
		set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
		return ret;
	}

	LOG_INF("BLE connection initiated, callbacks will handle L2CAP");
	return 0;  /* Success - connection in progress, callbacks handle the rest */
}

/* Scan callback - stored globally since BT scan API doesn't provide context */
static struct l2cap_client_data *g_scan_data = NULL;

/* Work handler for deferred connection (runs outside BT thread) */
static void connect_work_handler(struct k_work *work)
{
	struct l2cap_client_data *data = CONTAINER_OF(work, struct l2cap_client_data, connect_work);
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(&data->discovered_addr, addr_str, sizeof(addr_str));
	LOG_INF("Deferred connect starting to %s", addr_str);

	int ret = connect_to_device(data, &data->discovered_addr);
	if (ret < 0) {
		LOG_ERR("Deferred connect failed: %d", ret);
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

		uint8_t type = net_buf_simple_pull_u8(ad);
		len--;

		if ((type == BT_DATA_UUID16_ALL || type == BT_DATA_UUID16_SOME) &&
		    len >= 2) {
			for (int i = 0; i + 1 < len; i += 2) {
				uint16_t uuid = sys_get_le16(&ad->data[i]);
				if (uuid == data->service_uuid) {
					LOG_INF("Found target service 0x%04x at %s",
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
		/* Stop scanning and defer connection to work queue
		 * (can't block BT thread with semaphore wait) */
		bt_le_scan_stop();
		g_scan_data = NULL;

		/* Save address and schedule deferred connection */
		bt_addr_le_copy(&data->discovered_addr, addr);

		/* Cancel any stale work before submitting */
		k_work_cancel(&data->connect_work);

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

	LOG_INF("Starting scan for service UUID 0x%04x", data->service_uuid);

	set_state(data, NINEP_L2CAP_CLIENT_SCANNING);
	g_scan_data = data;

	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	ret = bt_le_scan_start(&scan_param, scan_cb);
	if (ret < 0) {
		LOG_ERR("Scan start failed: %d", ret);
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

	/* Cancel any pending connect work first */
	k_work_cancel(&data->connect_work);

	/* Stop scanning if active */
	bt_le_scan_stop();
	g_scan_data = NULL;

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

	set_state(data, NINEP_L2CAP_CLIENT_DISCONNECTED);
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

	if (!config->target_addr && config->service_uuid == 0) {
		LOG_ERR("Must provide either target_addr or service_uuid");
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
	data->service_uuid = config->service_uuid;
	data->state_cb = config->state_cb;
	data->transport = transport;

	/* Initialize channel */
	data->channel.rx_buf = config->rx_buf;
	data->channel.rx_buf_size = config->rx_buf_size;
	data->channel.transport = transport;

	/* Initialize semaphore and work item */
	k_sem_init(&data->connect_sem, 0, 1);
	k_work_init(&data->connect_work, connect_work_handler);

	/* Initialize transport */
	transport->ops = &l2cap_client_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

	LOG_INF("L2CAP client transport initialized (PSM: 0x%04x, RX buf: %zu bytes)",
	        config->psm, config->rx_buf_size);

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
