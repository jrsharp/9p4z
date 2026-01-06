/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
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

/*
 * Thread pool for 9P message processing.
 *
 * This is critical for true 9P multiplexing - we need MULTIPLE threads
 * so that a blocking read (like kbin waiting for key events) doesn't
 * block other 9P operations (like battery reads, DFU, etc.).
 *
 * Architecture:
 * - N worker threads wait on a message queue
 * - When a 9P message arrives, it's queued
 * - The next available thread picks it up and processes it
 * - If one thread blocks, others continue processing new requests
 *
 * Note: Reduced from 3 threads/4KB stacks to fit in ESP32 DRAM constraints.
 * 1024 bytes is tight but sufficient for simple 9P operations.
 * TODO: Make these configurable via Kconfig.
 */
#define NINEP_THREAD_POOL_SIZE 1
#define NINEP_THREAD_STACK_SIZE 1024
#define NINEP_THREAD_PRIORITY 5
#define NINEP_MSG_QUEUE_SIZE 4

/* Work item for thread pool - owns a COPY of the message data */
struct ninep_work_item {
	struct l2cap_9p_chan *channel;
	size_t msg_len;
	uint8_t *msg_buf;  /* Allocated copy of message - worker must free */
};

/* Message queue for thread pool */
K_MSGQ_DEFINE(ninep_msg_queue, sizeof(struct ninep_work_item), NINEP_MSG_QUEUE_SIZE, 4);

/* Thread pool stacks and threads */
K_THREAD_STACK_ARRAY_DEFINE(ninep_thread_stacks, NINEP_THREAD_POOL_SIZE, NINEP_THREAD_STACK_SIZE);
static struct k_thread ninep_threads[NINEP_THREAD_POOL_SIZE];
static bool ninep_thread_pool_started = false;

/* RX state machine states */
enum l2cap_rx_state {
	RX_WAIT_SIZE,   /* Waiting for 4-byte size field */
	RX_WAIT_MSG     /* Waiting for message body */
};

/* Maximum concurrent L2CAP channels per PSM (reduced for ESP32 DRAM) */
#define MAX_L2CAP_CHANNELS 1

/* L2CAP channel structure */
struct l2cap_9p_chan {
	struct bt_l2cap_le_chan le;
	struct ninep_transport *transport;
	uint8_t *rx_buf;           /* Buffer for assembling incoming message */
	size_t rx_buf_size;
	size_t rx_len;             /* Current position in rx_buf */
	uint32_t rx_expected;      /* Expected total message size */
	enum l2cap_rx_state rx_state;
	bool in_use;               /* Track if this channel slot is allocated */
};

/* Transport private data */
struct l2cap_transport_data {
	struct bt_l2cap_server server;
	struct l2cap_9p_chan channels[MAX_L2CAP_CHANNELS];  /* Support multiple channels */
	uint8_t *rx_buf_pool;  /* RX buffer pool (divided among channels) */
	size_t rx_buf_size_per_channel;
	uint8_t active_channels;  /* Count of active connections */
	struct l2cap_9p_chan *current_rx_chan;  /* Channel currently processing a request */
	struct ninep_transport *transport;  /* Backpointer to parent transport */
#if NINEP_NCS_BUILD
	struct net_buf_pool tx_pool;  /* TX buffer pool for NCS */
#endif
};

/* Define TX buffer pool for L2CAP SDUs (reduced for ESP32 DRAM) */
#define TX_BUF_COUNT 2
#define TX_BUF_SIZE BT_L2CAP_SDU_BUF_SIZE(CONFIG_NINEP_MAX_MESSAGE_SIZE)
NET_BUF_POOL_DEFINE(l2cap_tx_pool, TX_BUF_COUNT, TX_BUF_SIZE, CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

/* Forward declarations */
static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                        struct bt_l2cap_chan **chan);

/* Thread pool worker - waits on message queue and processes 9P messages */
static void ninep_thread_pool_worker(void *arg1, void *arg2, void *arg3)
{
	int thread_id = (int)(intptr_t)arg1;
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("9P worker thread %d started", thread_id);

	while (1) {
		struct ninep_work_item item;

		/* Wait for work item from queue */
		int ret = k_msgq_get(&ninep_msg_queue, &item, K_FOREVER);
		if (ret != 0) {
			LOG_ERR("Thread %d: msgq_get failed: %d", thread_id, ret);
			continue;
		}

		/* Validate work item */
		if (!item.msg_buf || item.msg_len == 0) {
			LOG_ERR("Thread %d: invalid work item (buf=%p, len=%zu)",
			        thread_id, item.msg_buf, item.msg_len);
			continue;
		}

		struct l2cap_9p_chan *ch = item.channel;
		struct ninep_transport *transport = ch->transport;
		struct l2cap_transport_data *data = transport->priv_data;

		LOG_INF("Thread %d: processing 9P msg %zu bytes, type=0x%02x",
		        thread_id, item.msg_len, item.msg_buf[4]);

		/*
		 * Set current channel for response routing.
		 * NOTE: With single L2CAP channel (typical case), this is safe.
		 * For multi-channel, there's a theoretical race but responses
		 * still go to a valid connected channel.
		 */
		data->current_rx_chan = ch;

		/* Deliver to 9P layer - may block (e.g., kbin read) */
		if (transport->recv_cb) {
			transport->recv_cb(transport, item.msg_buf, item.msg_len, transport->user_data);
		}

		/* Free the message buffer - we own it */
		k_free(item.msg_buf);

		LOG_DBG("Thread %d: done processing", thread_id);
	}
}

/* Start thread pool if not already running */
static void ninep_start_thread_pool(void)
{
	if (ninep_thread_pool_started) {
		return;
	}

	for (int i = 0; i < NINEP_THREAD_POOL_SIZE; i++) {
		k_thread_create(&ninep_threads[i],
		                ninep_thread_stacks[i],
		                K_THREAD_STACK_SIZEOF(ninep_thread_stacks[i]),
		                ninep_thread_pool_worker,
		                (void *)(intptr_t)i, NULL, NULL,
		                NINEP_THREAD_PRIORITY, 0, K_NO_WAIT);

		char name[16];
		snprintf(name, sizeof(name), "9p_worker_%d", i);
		k_thread_name_set(&ninep_threads[i], name);
	}

	ninep_thread_pool_started = true;
	LOG_INF("Started 9P thread pool: %d threads, stack=%d, prio=%d",
	        NINEP_THREAD_POOL_SIZE, NINEP_THREAD_STACK_SIZE, NINEP_THREAD_PRIORITY);
}

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

	LOG_INF("L2CAP channel connected (MTU: RX=%u, TX=%u, MPS: RX=%u, TX=%u)",
	        ch->le.rx.mtu, ch->le.tx.mtu, ch->le.rx.mps, ch->le.tx.mps);
	LOG_INF("  RX CID=0x%04x, TX CID=0x%04x", ch->le.rx.cid, ch->le.tx.cid);
	LOG_INF("  RX credits=%d, TX credits=%d",
	        (int)atomic_get(&ch->le.rx.credits), (int)atomic_get(&ch->le.tx.credits));
	LOG_INF("  recv callback=%p, chan ops=%p", chan->ops->recv, chan->ops);

	/* Reset RX state machine */
	ch->rx_len = 0;
	ch->rx_expected = 0;
	ch->rx_state = RX_WAIT_SIZE;
	ch->in_use = true;

	LOG_INF("Channel ready, initial credits=%d", (int)atomic_get(&ch->le.rx.credits));
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

	LOG_INF(">>> L2CAP RECV: %u bytes", buf->len);

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
				LOG_INF("Complete 9P message received: %u bytes (type=%u)",
				        ch->rx_len, ch->rx_buf[4]);

				/*
				 * CRITICAL: Allocate and copy message before queuing.
				 * We must not reference ch->rx_buf after resetting state,
				 * as new BLE packets could overwrite it before the worker
				 * thread processes this message.
				 */
				uint8_t *msg_copy = k_malloc(ch->rx_len);
				if (!msg_copy) {
					LOG_ERR("Failed to allocate %u bytes for message copy",
					        ch->rx_len);
					/* Reset for next message anyway - this one is lost */
					ch->rx_len = 0;
					ch->rx_expected = 0;
					ch->rx_state = RX_WAIT_SIZE;
					return -ENOMEM;
				}

				memcpy(msg_copy, ch->rx_buf, ch->rx_len);

				struct ninep_work_item item = {
					.channel = ch,
					.msg_len = ch->rx_len,
					.msg_buf = msg_copy,
				};

				/*
				 * Try to queue with a short timeout. We're in the BT RX thread,
				 * so we can't block forever, but a brief wait gives workers
				 * a chance to catch up if the queue is momentarily full.
				 */
				int ret = k_msgq_put(&ninep_msg_queue, &item, K_MSEC(100));
				if (ret != 0) {
					LOG_ERR("Failed to queue 9P message after 100ms: %d", ret);
					LOG_ERR("  Queue may be full (workers all blocked?) - message lost");
					k_free(msg_copy);
					/* Client will timeout waiting for response */
				}

				/* Reset for next message - safe now that data is copied */
				ch->rx_len = 0;
				ch->rx_expected = 0;
				ch->rx_state = RX_WAIT_SIZE;
			}
		}
	}

	return 0;
}

static void l2cap_sent(struct bt_l2cap_chan *chan)
{
	LOG_DBG("L2CAP sent successfully");
}

static struct bt_l2cap_chan_ops l2cap_chan_ops = {
	.connected = l2cap_connected,
	.disconnected = l2cap_disconnected,
	.recv = l2cap_recv,
	.sent = l2cap_sent,
};

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
                        struct bt_l2cap_chan **chan)
{
	struct bt_conn_info info;

	LOG_INF(">>> L2CAP ACCEPT CALLBACK INVOKED <<<");
	LOG_INF("  server=%p, PSM=0x%04x", server, server->psm);

	if (bt_conn_get_info(conn, &info) == 0) {
		LOG_INF("  conn: role=%s, sec_level=%d",
		        info.role == BT_CONN_ROLE_CENTRAL ? "central" : "peripheral",
		        info.security.level);
	}

	struct l2cap_transport_data *data = CONTAINER_OF(server,
	                                                   struct l2cap_transport_data,
	                                                   server);

	LOG_INF("L2CAP connection request accepted, finding free channel slot");

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
	free_chan->le.rx.mtu = CONFIG_NINEP_MAX_MESSAGE_SIZE;  /* Required for L2CAP LE */
	free_chan->transport = data->transport;
	free_chan->rx_buf = data->rx_buf_pool + (free_chan - data->channels) * data->rx_buf_size_per_channel;
	free_chan->rx_buf_size = data->rx_buf_size_per_channel;
	free_chan->rx_len = 0;
	free_chan->rx_expected = 0;
	free_chan->rx_state = RX_WAIT_SIZE;
	free_chan->in_use = true;

	/* Set RX MTU for the peer to send to us */
	free_chan->le.rx.mtu = data->rx_buf_size_per_channel;

	LOG_INF("Assigned channel slot %d/%d (rx.mtu=%u)",
	        (int)(free_chan - data->channels), MAX_L2CAP_CHANNELS,
	        free_chan->le.rx.mtu);

	*chan = &free_chan->le.chan;
	return 0;
}

static int l2cap_send(struct ninep_transport *transport, const uint8_t *buf,
                      size_t len)
{
	struct l2cap_transport_data *data = transport->priv_data;
	struct net_buf *msg_buf;
	int ret;

	LOG_INF(">>> L2CAP SEND: %zu bytes, type=0x%02x", len, buf[4]);

	if (!data) {
		LOG_ERR("L2CAP send: no transport data");
		return -ENOTCONN;
	}

	/* Use the channel that's currently processing a request */
	struct l2cap_9p_chan *active_chan = data->current_rx_chan;

	if (!active_chan || !active_chan->in_use) {
		LOG_ERR("No active receive channel for response");
		return -ENOTCONN;
	}

	/* Allocate from application buffer pool */
	msg_buf = net_buf_alloc(&l2cap_tx_pool, K_FOREVER);
	if (!msg_buf) {
		LOG_ERR("Failed to allocate net_buf");
		return -ENOMEM;
	}
	/* Reserve L2CAP SDU headroom */
	net_buf_reserve(msg_buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);

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

	/* Verify BT is ready before registering */
	if (!bt_is_ready()) {
		LOG_ERR("BT not ready! Cannot register L2CAP server yet");
		return -EAGAIN;
	}

	/* Log server state BEFORE registration */
	LOG_INF("=== L2CAP SERVER REGISTRATION ===");
	LOG_INF("  BEFORE: server=%p, psm=0x%04x, sec_level=%d, accept=%p",
	        &data->server, data->server.psm, data->server.sec_level, data->server.accept);

	/* Register L2CAP server */
	ret = bt_l2cap_server_register(&data->server);

	/* Log result immediately */
	LOG_INF("  bt_l2cap_server_register() returned: %d", ret);

	if (ret < 0) {
		LOG_ERR("  FAILED to register L2CAP server: %d", ret);
		if (ret == -EINVAL) {
			LOG_ERR("  -EINVAL: Invalid params (PSM range? accept callback?)");
		} else if (ret == -EADDRINUSE) {
			LOG_ERR("  -EADDRINUSE: PSM already in use");
		} else if (ret == -ENOBUFS) {
			LOG_ERR("  -ENOBUFS: No buffer space");
		}
		return ret;
	}

	/* Log server state AFTER registration */
	LOG_INF("  AFTER: server=%p, psm=0x%04x (SUCCESS)", &data->server, data->server.psm);
	LOG_INF("=== L2CAP SERVER READY ON PSM 0x%04x ===", data->server.psm);

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

	/* Start 9P thread pool if not already running */
	ninep_start_thread_pool();

	/* Allocate private data */
	data = k_malloc(sizeof(*data));
	if (!data) {
		return -ENOMEM;
	}

	memset(data, 0, sizeof(*data));
	data->rx_buf_pool = config->rx_buf;
	data->rx_buf_size_per_channel = config->rx_buf_size / MAX_L2CAP_CHANNELS;
	data->active_channels = 0;
	data->current_rx_chan = NULL;
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
