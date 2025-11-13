/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/session_pool_l2cap.h>
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

LOG_MODULE_REGISTER(ninep_session_pool_l2cap, CONFIG_NINEP_LOG_LEVEL);

/* RX state machine states */
enum l2cap_rx_state {
	RX_WAIT_SIZE,   /* Waiting for 4-byte size field */
	RX_WAIT_MSG     /* Waiting for message body */
};

/* L2CAP channel for a single session */
struct l2cap_session_chan {
	struct bt_l2cap_le_chan le;
	struct ninep_session *session;  /* Back-pointer to owning session */
	uint8_t *rx_buf;
	size_t rx_buf_size;
	size_t rx_len;
	uint32_t rx_expected;
	enum l2cap_rx_state rx_state;
};

#if NINEP_NCS_BUILD
/* NCS: Define TX buffer pool for L2CAP SDUs */
#define TX_BUF_COUNT 4
#define TX_BUF_SIZE BT_L2CAP_SDU_BUF_SIZE(CONFIG_NINEP_MAX_MESSAGE_SIZE)
NET_BUF_POOL_DEFINE(l2cap_session_tx_pool, TX_BUF_COUNT, TX_BUF_SIZE,
                    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
#endif

/* L2CAP session pool private data */
struct ninep_session_pool_l2cap {
	struct bt_l2cap_server server;
	struct ninep_session_pool *pool;  /* Generic session pool */
	struct ninep_session_pool_l2cap_config config;
	uint8_t *rx_buf_pool;  /* Large buffer divided among sessions */
	struct l2cap_session_chan *channels;  /* One channel struct per session */
};

/* Forward declarations */
static int l2cap_session_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                                 struct bt_l2cap_chan **chan);
static void l2cap_session_connected(struct bt_l2cap_chan *chan);
static void l2cap_session_disconnected(struct bt_l2cap_chan *chan);
static int l2cap_session_recv(struct bt_l2cap_chan *chan, struct net_buf *buf);

#if NINEP_NCS_BUILD
static void l2cap_session_sent(struct bt_l2cap_chan *chan);
#else
static void l2cap_session_sent(struct bt_l2cap_chan *chan, int status);
#endif

static struct bt_l2cap_chan_ops l2cap_session_chan_ops = {
	.connected = l2cap_session_connected,
	.disconnected = l2cap_session_disconnected,
	.recv = l2cap_session_recv,
	.sent = l2cap_session_sent,
};

/* Transport operations for session-based L2CAP */
static int l2cap_session_send(struct ninep_transport *transport, const uint8_t *buf,
                               size_t len);
static int l2cap_session_get_mtu(struct ninep_transport *transport);

static const struct ninep_transport_ops l2cap_session_transport_ops = {
	.send = l2cap_session_send,
	.get_mtu = l2cap_session_get_mtu,
	/* start/stop not needed - managed by session pool */
};

static void l2cap_session_connected(struct bt_l2cap_chan *chan)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_session_chan *ch = CONTAINER_OF(le_chan, struct l2cap_session_chan, le);
#else
	struct l2cap_session_chan *ch = CONTAINER_OF(chan, struct l2cap_session_chan, le.chan);
#endif

	LOG_INF("L2CAP session channel connected (MTU: RX=%u, TX=%u) for session %d",
	        ch->le.rx.mtu, ch->le.tx.mtu, ch->session->session_id);

	/* Reset RX state machine */
	ch->rx_len = 0;
	ch->rx_expected = 0;
	ch->rx_state = RX_WAIT_SIZE;

	/* Mark session as connected */
	ninep_session_connected(ch->session);
}

static void l2cap_session_disconnected(struct bt_l2cap_chan *chan)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_session_chan *ch = CONTAINER_OF(le_chan, struct l2cap_session_chan, le);
#else
	struct l2cap_session_chan *ch = CONTAINER_OF(chan, struct l2cap_session_chan, le.chan);
#endif

	LOG_INF("L2CAP session channel disconnected for session %d", ch->session->session_id);

	/* Free the session back to the pool */
	ninep_session_free(ch->session);
}

static int l2cap_session_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
#if NINEP_NCS_BUILD
	struct bt_l2cap_le_chan *le_chan = BT_L2CAP_LE_CHAN(chan);
	struct l2cap_session_chan *ch = CONTAINER_OF(le_chan, struct l2cap_session_chan, le);
#else
	struct l2cap_session_chan *ch = CONTAINER_OF(chan, struct l2cap_session_chan, le.chan);
#endif
	struct ninep_transport *transport = &ch->session->transport;

	LOG_DBG("L2CAP session recv: %u bytes for session %d", buf->len, ch->session->session_id);

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
static void l2cap_session_sent(struct bt_l2cap_chan *chan)
{
	LOG_DBG("L2CAP sent successfully");
}
#else
static void l2cap_session_sent(struct bt_l2cap_chan *chan, int status)
{
	if (status < 0) {
		LOG_ERR("L2CAP send failed: %d", status);
	} else {
		LOG_DBG("L2CAP sent successfully");
	}
}
#endif

static int l2cap_session_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                                 struct bt_l2cap_chan **chan)
{
	struct ninep_session_pool_l2cap *l2cap_pool =
		CONTAINER_OF(server, struct ninep_session_pool_l2cap, server);

	LOG_INF("L2CAP connection accepted, allocating session...");

	/* Allocate a session from the generic pool */
	struct ninep_session *session = ninep_session_alloc(l2cap_pool->pool);
	if (!session) {
		LOG_ERR("No available sessions");
		return -ENOMEM;
	}

	/* Get the L2CAP channel structure for this session */
	struct l2cap_session_chan *l2cap_chan = &l2cap_pool->channels[session->session_id];

	/* Initialize channel */
	memset(l2cap_chan, 0, sizeof(*l2cap_chan));
	l2cap_chan->le.chan.ops = &l2cap_session_chan_ops;
	l2cap_chan->session = session;
	l2cap_chan->rx_buf = l2cap_pool->rx_buf_pool +
	                     (session->session_id * l2cap_pool->config.rx_buf_size_per_session);
	l2cap_chan->rx_buf_size = l2cap_pool->config.rx_buf_size_per_session;
	l2cap_chan->rx_len = 0;
	l2cap_chan->rx_expected = 0;
	l2cap_chan->rx_state = RX_WAIT_SIZE;

	/* Initialize transport for this session */
	session->transport.ops = &l2cap_session_transport_ops;
	session->transport.priv_data = l2cap_chan;  /* Store channel in transport */

	/* Initialize 9P server for this session */
	struct ninep_server_config server_config = {
		.fs_ops = l2cap_pool->pool->fs_ops,
		.fs_ctx = l2cap_pool->pool->fs_context,
	};

	int ret = ninep_server_init(&session->server, &server_config, &session->transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9P server for session %d: %d",
		        session->session_id, ret);
		ninep_session_free(session);
		return ret;
	}

	LOG_INF("Assigned session %d to incoming L2CAP connection", session->session_id);

	*chan = &l2cap_chan->le.chan;
	return 0;
}

static int l2cap_session_send(struct ninep_transport *transport, const uint8_t *buf,
                               size_t len)
{
	struct l2cap_session_chan *chan = transport->priv_data;
	struct net_buf *msg_buf;
	int ret;

	if (!chan) {
		return -ENOTCONN;
	}

	LOG_DBG("Sending %zu bytes on session %d", len, chan->session->session_id);

#if NINEP_NCS_BUILD
	/* NCS: Allocate from application buffer pool */
	msg_buf = net_buf_alloc(&l2cap_session_tx_pool, K_FOREVER);
	if (!msg_buf) {
		LOG_ERR("Failed to allocate net_buf");
		return -ENOMEM;
	}
	/* Reserve L2CAP SDU headroom */
	net_buf_reserve(msg_buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
#else
	/* Mainline: Allocate from channel's built-in buffer pool */
	msg_buf = net_buf_alloc(&chan->le.tx.seg.pool, K_FOREVER);
	if (!msg_buf) {
		LOG_ERR("Failed to allocate net_buf");
		return -ENOMEM;
	}
#endif

	/* Copy message data to net_buf */
	net_buf_add_mem(msg_buf, buf, len);

	/* Send via L2CAP channel */
	ret = bt_l2cap_chan_send(&chan->le.chan, msg_buf);
	if (ret < 0) {
		LOG_ERR("bt_l2cap_chan_send failed: %d", ret);
		net_buf_unref(msg_buf);
		return ret;
	}

	LOG_DBG("Sent %zu bytes via L2CAP on session %d", len, chan->session->session_id);
	return len;
}

static int l2cap_session_get_mtu(struct ninep_transport *transport)
{
	struct l2cap_session_chan *chan = transport->priv_data;

	if (!chan) {
		return -EINVAL;
	}

	/* Return TX MTU if connected */
	return chan->le.tx.mtu;
}

struct ninep_session_pool_l2cap *ninep_session_pool_l2cap_create(
	const struct ninep_session_pool_l2cap_config *config)
{
	struct ninep_session_pool_l2cap *l2cap_pool;
	struct ninep_session_pool *pool;
	int ret;

	if (!config || !config->fs_ops || config->max_sessions <= 0 ||
	    config->rx_buf_size_per_session == 0) {
		LOG_ERR("Invalid configuration");
		return NULL;
	}

	/* Allocate L2CAP pool structure */
	l2cap_pool = k_malloc(sizeof(*l2cap_pool));
	if (!l2cap_pool) {
		LOG_ERR("Failed to allocate L2CAP pool");
		return NULL;
	}

	memset(l2cap_pool, 0, sizeof(*l2cap_pool));
	memcpy(&l2cap_pool->config, config, sizeof(*config));

	/* Allocate generic session pool */
	size_t pool_size = ninep_session_pool_size(config->max_sessions);
	pool = k_malloc(pool_size);
	if (!pool) {
		LOG_ERR("Failed to allocate session pool");
		k_free(l2cap_pool);
		return NULL;
	}

	/* Initialize generic session pool */
	struct ninep_session_pool_config pool_config = {
		.max_sessions = config->max_sessions,
		.fs_ops = config->fs_ops,
		.fs_context = config->fs_context,
	};

	ret = ninep_session_pool_init(pool, &pool_config);
	if (ret < 0) {
		LOG_ERR("Failed to initialize session pool: %d", ret);
		k_free(pool);
		k_free(l2cap_pool);
		return NULL;
	}

	l2cap_pool->pool = pool;

	/* Allocate RX buffer pool (divided among sessions) */
	size_t total_rx_buf = config->max_sessions * config->rx_buf_size_per_session;
	l2cap_pool->rx_buf_pool = k_malloc(total_rx_buf);
	if (!l2cap_pool->rx_buf_pool) {
		LOG_ERR("Failed to allocate RX buffer pool");
		k_free(pool);
		k_free(l2cap_pool);
		return NULL;
	}

	/* Allocate L2CAP channel structures (one per session) */
	l2cap_pool->channels = k_malloc(config->max_sessions * sizeof(struct l2cap_session_chan));
	if (!l2cap_pool->channels) {
		LOG_ERR("Failed to allocate channel structures");
		k_free(l2cap_pool->rx_buf_pool);
		k_free(pool);
		k_free(l2cap_pool);
		return NULL;
	}

	/* Initialize L2CAP server */
	l2cap_pool->server.psm = config->psm;
	l2cap_pool->server.accept = l2cap_session_accept;
	l2cap_pool->server.sec_level = BT_SECURITY_L1; /* No encryption required */

	LOG_INF("L2CAP session pool created: PSM 0x%04x, %d sessions, %zu bytes RX per session",
	        config->psm, config->max_sessions, config->rx_buf_size_per_session);

	return l2cap_pool;
}

int ninep_session_pool_l2cap_start(struct ninep_session_pool_l2cap *pool)
{
	int ret;

	if (!pool) {
		return -EINVAL;
	}

	/* Register L2CAP server */
	ret = bt_l2cap_server_register(&pool->server);
	if (ret < 0) {
		LOG_ERR("Failed to register L2CAP server: %d", ret);
		return ret;
	}

	LOG_INF("L2CAP session pool started on PSM 0x%04x", pool->config.psm);
	return 0;
}

void ninep_session_pool_l2cap_stop(struct ninep_session_pool_l2cap *pool)
{
	if (!pool) {
		return;
	}

	LOG_INF("Stopping L2CAP session pool");

	/* Disconnect all sessions */
	ninep_session_pool_disconnect_all(pool->pool);

	/* Note: Zephyr doesn't provide bt_l2cap_server_unregister(),
	 * so the server remains registered */
}

void ninep_session_pool_l2cap_destroy(struct ninep_session_pool_l2cap *pool)
{
	if (!pool) {
		return;
	}

	LOG_INF("Destroying L2CAP session pool");

	ninep_session_pool_l2cap_stop(pool);

	/* Free allocated memory */
	k_free(pool->channels);
	k_free(pool->rx_buf_pool);
	k_free(pool->pool);
	k_free(pool);
}
