/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

/* Platform detection: NCS vs mainline Zephyr */
#if defined(CONFIG_NRF_MODEM_LIB) || defined(CONFIG_NCS_BOOT_BANNER)
#define NINEP_NCS_BUILD 1
#include <zephyr/bluetooth/buf.h>
#include <zephyr/net_buf.h>
#else
#define NINEP_NCS_BUILD 0
#include <zephyr/net/buf.h>
#endif

LOG_MODULE_REGISTER(ninep_l2cap_transport, CONFIG_NINEP_LOG_LEVEL);

/* RX state machine states */
enum l2cap_rx_state {
	RX_WAIT_SIZE,   /* Waiting for 4-byte size field */
	RX_WAIT_MSG     /* Waiting for message body */
};

/* Maximum concurrent L2CAP channels per PSM */
#define MAX_L2CAP_CHANNELS 4

/* L2CAP channel structure */
struct l2cap_9p_chan {
	struct bt_l2cap_le_chan le;
	struct ninep_transport *transport;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	size_t rx_len;
	uint32_t rx_expected;
	enum l2cap_rx_state rx_state;
	bool in_use;  /* Track if this channel slot is allocated */
};

/* Transport private data */
struct l2cap_transport_data {
	struct bt_l2cap_server server;
	struct l2cap_9p_chan channels[MAX_L2CAP_CHANNELS];  /* Support multiple channels */
	uint8_t *rx_buf_pool;  /* RX buffer pool (divided among channels) */
	size_t rx_buf_size_per_channel;
	uint8_t active_channels;  /* Count of active connections */
	struct ninep_transport *transport;  /* Backpointer to parent transport */
#if NINEP_NCS_BUILD
	struct net_buf_pool tx_pool;  /* TX buffer pool for NCS */
#endif
};

#if NINEP_NCS_BUILD
/* NCS: Define TX buffer pool for L2CAP SDUs */
#define TX_BUF_COUNT 4
#define TX_BUF_SIZE BT_L2CAP_SDU_BUF_SIZE(CONFIG_NINEP_MAX_MESSAGE_SIZE)
NET_BUF_POOL_DEFINE(l2cap_tx_pool, TX_BUF_COUNT, TX_BUF_SIZE, CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
#endif

/* Forward declarations */
static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                        struct bt_l2cap_chan **chan);

static void l2cap_connected(struct bt_l2cap_chan *chan)
{
#if NINEP_NCS_BUILD
	/* NCS: Two-step CONTAINER_OF via BT_L2CAP_LE_CHAN macro */
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_9p_chan *ch = CONTAINER_OF(le_chan, struct l2cap_9p_chan, le);
#else
	/* Mainline: Direct CONTAINER_OF */
	struct l2cap_9p_chan *ch = CONTAINER_OF(chan, struct l2cap_9p_chan, le.chan);
#endif

	LOG_INF("L2CAP channel connected (MTU: RX=%u, TX=%u)",
	        ch->le.rx.mtu, ch->le.tx.mtu);

	/* Reset RX state machine */
	ch->rx_len = 0;
	ch->rx_expected = 0;
	ch->rx_state = RX_WAIT_SIZE;
	ch->in_use = true;
}

static void l2cap_disconnected(struct bt_l2cap_chan *chan)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_9p_chan *ch = CONTAINER_OF(le_chan, struct l2cap_9p_chan, le);
#else
	struct l2cap_9p_chan *ch = CONTAINER_OF(chan, struct l2cap_9p_chan, le.chan);
#endif

	LOG_INF("L2CAP channel disconnected");

	/* Reset state */
	ch->rx_len = 0;
	ch->rx_expected = 0;
	ch->rx_state = RX_WAIT_SIZE;
	ch->in_use = false;
}

static int l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_9p_chan *ch = CONTAINER_OF(le_chan, struct l2cap_9p_chan, le);
#else
	struct l2cap_9p_chan *ch = CONTAINER_OF(chan, struct l2cap_9p_chan, le.chan);
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
					/* Reset and skip this message */
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
				LOG_DBG("Complete message received: %u bytes", ch->rx_len);

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

#if NINEP_NCS_BUILD
/* NCS: .sent callback has no status parameter */
static void l2cap_sent(struct bt_l2cap_chan *chan)
{
	LOG_DBG("L2CAP sent successfully");
}
#else
/* Mainline: .sent callback has status parameter */
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

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                        struct bt_l2cap_chan **chan)
{
	struct l2cap_transport_data *data = CONTAINER_OF(server,
	                                                   struct l2cap_transport_data,
	                                                   server);

	LOG_INF("L2CAP connection accepted");

	/* Find a free channel slot */
	struct l2cap_9p_chan *free_chan = NULL;
	for (int i = 0; i < MAX_L2CAP_CHANNELS; i++) {
		if (!data->channels[i].in_use) {
			free_chan = &data->channels[i];
			break;
		}
	}

	if (!free_chan) {
		LOG_WRN("All channel slots in use (%d/%d), rejecting connection",
		        MAX_L2CAP_CHANNELS, MAX_L2CAP_CHANNELS);
		return -ENOMEM;
	}

	/* Initialize channel */
	memset(free_chan, 0, sizeof(*free_chan));
	free_chan->le.chan.ops = &l2cap_chan_ops;
	free_chan->transport = data->transport;
	free_chan->rx_buf = data->rx_buf_pool + (free_chan - data->channels) * data->rx_buf_size_per_channel;
	free_chan->rx_buf_size = data->rx_buf_size_per_channel;
	free_chan->rx_len = 0;
	free_chan->rx_expected = 0;
	free_chan->rx_state = RX_WAIT_SIZE;
	free_chan->in_use = true;

	LOG_INF("Assigned channel slot %d/%d", (int)(free_chan - data->channels), MAX_L2CAP_CHANNELS);

	*chan = &free_chan->le.chan;
	return 0;
}

static int l2cap_send(struct ninep_transport *transport, const uint8_t *buf,
                      size_t len)
{
	struct l2cap_transport_data *data = transport->priv_data;
	struct net_buf *msg_buf;
	int ret;

	if (!data) {
		return -ENOTCONN;
	}

	/* Find first active channel to send on */
	struct l2cap_9p_chan *active_chan = NULL;
	for (int i = 0; i < MAX_L2CAP_CHANNELS; i++) {
		if (data->channels[i].in_use) {
			active_chan = &data->channels[i];
			break;
		}
	}

	if (!active_chan) {
		return -ENOTCONN;
	}

#if NINEP_NCS_BUILD
	/* NCS: Allocate from application buffer pool */
	msg_buf = net_buf_alloc(&l2cap_tx_pool, K_FOREVER);
	if (!msg_buf) {
		LOG_ERR("Failed to allocate net_buf");
		return -ENOMEM;
	}
	/* Reserve L2CAP SDU headroom */
	net_buf_reserve(msg_buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
#else
	/* Mainline: Allocate from channel's built-in buffer pool */
	msg_buf = net_buf_alloc(&active_chan->le.tx.seg.pool, K_FOREVER);
	if (!msg_buf) {
		LOG_ERR("Failed to allocate net_buf");
		return -ENOMEM;
	}
#endif

	/* Copy message data to net_buf */
	net_buf_add_mem(msg_buf, buf, len);

	/* Send via L2CAP channel */
	ret = bt_l2cap_chan_send(&active_chan->le.chan, msg_buf);
	if (ret < 0) {
		LOG_ERR("bt_l2cap_chan_send failed: %d", ret);
		net_buf_unref(msg_buf);
		return ret;
	}

	LOG_DBG("Sent %zu bytes via L2CAP", len);
	return len;
}

static int l2cap_start(struct ninep_transport *transport)
{
	struct l2cap_transport_data *data = transport->priv_data;
	int ret;

	if (!data) {
		return -EINVAL;
	}

	/* Register L2CAP server */
	ret = bt_l2cap_server_register(&data->server);
	if (ret < 0) {
		LOG_ERR("Failed to register L2CAP server: %d", ret);
		return ret;
	}

	LOG_INF("L2CAP server registered on PSM 0x%04x", data->server.psm);
	return 0;
}

static int l2cap_stop(struct ninep_transport *transport)
{
	struct l2cap_transport_data *data = transport->priv_data;
	int ret;

	if (!data) {
		return -EINVAL;
	}

	/* Disconnect all active channels */
	for (int i = 0; i < MAX_L2CAP_CHANNELS; i++) {
		if (data->channels[i].in_use) {
			ret = bt_l2cap_chan_disconnect(&data->channels[i].le.chan);
			if (ret < 0) {
				LOG_WRN("Failed to disconnect L2CAP channel %d: %d", i, ret);
			}
		}
	}

	/* Note: Zephyr doesn't provide bt_l2cap_server_unregister(),
	 * so we can't fully stop the server. Just mark as stopped.
	 */
	LOG_INF("L2CAP transport stopped");
	return 0;
}

static int l2cap_get_mtu(struct ninep_transport *transport)
{
	struct l2cap_transport_data *data = transport->priv_data;

	if (!data) {
		return -EINVAL;
	}

	/* Return TX MTU of first active channel, otherwise return configured MTU */
	for (int i = 0; i < MAX_L2CAP_CHANNELS; i++) {
		if (data->channels[i].in_use) {
			return data->channels[i].le.tx.mtu;
		}
	}

	/* Not connected yet - return configured MTU from prj.conf */
	return CONFIG_NINEP_L2CAP_MTU;
}

static const struct ninep_transport_ops l2cap_transport_ops = {
	.send = l2cap_send,
	.start = l2cap_start,
	.stop = l2cap_stop,
	.get_mtu = l2cap_get_mtu,
};

int ninep_transport_l2cap_init(struct ninep_transport *transport,
                                const struct ninep_transport_l2cap_config *config,
                                ninep_transport_recv_cb_t recv_cb,
                                void *user_data)
{
	struct l2cap_transport_data *data;

	if (!transport || !config || !config->rx_buf || config->rx_buf_size == 0) {
		return -EINVAL;
	}

	/* Allocate private data */
	data = k_malloc(sizeof(*data));
	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));
	data->rx_buf_pool = config->rx_buf;
	data->rx_buf_size_per_channel = config->rx_buf_size / MAX_L2CAP_CHANNELS;
	data->active_channels = 0;
	data->transport = transport;

	/* Initialize all channel slots as unused */
	for (int i = 0; i < MAX_L2CAP_CHANNELS; i++) {
		data->channels[i].in_use = false;
	}

	/* Initialize L2CAP server */
	data->server.psm = config->psm;
	data->server.accept = l2cap_accept;
	data->server.sec_level = BT_SECURITY_L1; /* No encryption required */

	/* Initialize transport */
	transport->ops = &l2cap_transport_ops;
	transport->recv_cb = recv_cb;
	transport->user_data = user_data;
	transport->priv_data = data;

	LOG_INF("L2CAP transport initialized (PSM: 0x%04x, RX buf: %zu bytes, %d channels)",
	        config->psm, config->rx_buf_size, MAX_L2CAP_CHANNELS);

	return 0;
}
