/*
 * Multi-client 9P over TCP: a session per connection. See
 * session_pool_tcp.h for why transport_tcp.c's single client is not
 * enough for a server more than one machine talks to.
 *
 * Copyright (c) 2026 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/session_pool_tcp.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_tcp_pool, CONFIG_NINEP_LOG_LEVEL);

#define TCP_POOL_ACCEPT_STACK_SIZE 2048
#define TCP_POOL_PRIORITY          5

struct tcp_conn {
	struct ninep_session_pool_tcp *pool;
	struct ninep_session *session;
	struct k_mutex sock_lock;      /* send vs. close */
	int sock;
	uint8_t *rx_buf;
	volatile bool active;
	k_tid_t tid;                   /* receive thread; joined before reuse */
	struct k_thread thread;
	char peer[48];
};

struct ninep_session_pool_tcp {
	struct ninep_session_pool_tcp_config cfg;
	struct ninep_session_pool *base;
	int listen_sock;
	volatile bool running;
	k_tid_t accept_tid;
	struct k_thread accept_thread;
	struct tcp_conn conns[CONFIG_NINEP_TCP_MAX_SESSIONS];
};

K_KERNEL_STACK_ARRAY_DEFINE(tcp_conn_stacks, CONFIG_NINEP_TCP_MAX_SESSIONS,
			    CONFIG_NINEP_TCP_SESSION_STACK_SIZE);
K_KERNEL_STACK_DEFINE(tcp_accept_stack, TCP_POOL_ACCEPT_STACK_SIZE);

static struct ninep_session_pool_tcp the_pool;
static bool pool_created;

/* ---- a connection's transport ---- */

static int conn_send(struct ninep_transport *transport, const uint8_t *buf,
		     size_t len)
{
	struct tcp_conn *c = transport->priv_data;
	size_t off = 0;
	int ret = 0;

	k_mutex_lock(&c->sock_lock, K_FOREVER);
	/* All of it or an error: a message cut short leaves the stream out
	 * of step for good, and a non-blocking socket may take only part. */
	while (off < len) {
		if (c->sock < 0) {
			ret = -ENOTCONN;
			break;
		}

		int n = zsock_send(c->sock, buf + off, len - off, 0);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct zsock_pollfd pfd = {
					.fd = c->sock,
					.events = ZSOCK_POLLOUT,
				};

				zsock_poll(&pfd, 1, 100);
				continue;
			}
			ret = -errno;
			break;
		}
		off += (size_t)n;
	}
	k_mutex_unlock(&c->sock_lock);
	return ret < 0 ? ret : (int)len;
}

static void conn_recv_thread(void *a, void *b, void *d);

static int conn_start(struct ninep_transport *transport)
{
	struct tcp_conn *c = transport->priv_data;
	int idx = c->session->session_id;

	c->tid = k_thread_create(&c->thread, tcp_conn_stacks[idx],
				 K_KERNEL_STACK_SIZEOF(tcp_conn_stacks[idx]),
				 conn_recv_thread, c, NULL, NULL,
				 TCP_POOL_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(c->tid, "9p_tcp_sess");
	return 0;
}

/* Called by ninep_session_free(), usually from the connection's own
 * receive thread as it leaves; then it must not join itself. */
static int conn_stop(struct ninep_transport *transport)
{
	struct tcp_conn *c = transport->priv_data;

	c->active = false;
	k_mutex_lock(&c->sock_lock, K_FOREVER);
	if (c->sock >= 0) {
		zsock_close(c->sock);
		c->sock = -1;
	}
	k_mutex_unlock(&c->sock_lock);
	if (c->tid && c->tid != k_current_get()) {
		k_thread_join(c->tid, K_FOREVER);
		c->tid = NULL;
	}
	return 0;
}

static const struct ninep_transport_ops conn_ops = {
	.send = conn_send,
	.start = conn_start,
	.stop = conn_stop,
};

/* One connection's messages, framed by their size headers and handed to
 * its own server; the same loop as transport_tcp.c's. */
static void conn_recv_thread(void *a, void *b, void *d)
{
	ARG_UNUSED(b);
	ARG_UNUSED(d);
	struct tcp_conn *c = a;
	struct ninep_transport *transport = &c->session->transport;
	size_t rx_size = c->pool->cfg.rx_buf_size_per_session;
	size_t have = 0;
	uint32_t expected = 0;
	bool header = false;

	while (c->active) {
		size_t want = header ? expected : 7;
		int ret = zsock_recv(c->sock, c->rx_buf + have, want - have, 0);

		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct zsock_pollfd pfd = {
					.fd = c->sock,
					.events = ZSOCK_POLLIN,
				};

				zsock_poll(&pfd, 1, 100);
				continue;
			}
			LOG_INF("%s: receive error %d", c->peer, errno);
			break;
		}
		if (ret == 0) {
			LOG_INF("%s disconnected", c->peer);
			break;
		}
		have += (size_t)ret;

		if (!header && have >= 7) {
			struct ninep_msg_header hdr;

			if (ninep_parse_header(c->rx_buf, have, &hdr) != 0 ||
			    hdr.size < 7 || hdr.size > rx_size) {
				LOG_WRN("%s: bad message header (size %u), "
					"dropping the client", c->peer, hdr.size);
				break;
			}
			expected = hdr.size;
			header = true;
		}
		if (header && have >= expected) {
			if (transport->recv_cb) {
				transport->recv_cb(transport, c->rx_buf, expected,
						   transport->user_data);
			}
			have = 0;
			expected = 0;
			header = false;
		}
	}

	/* Clunks the client's fids (an open pcm closes), stops the
	 * transport (closing the socket) and hands the slot back. */
	ninep_session_free(c->session);
}

/* ---- the pool ---- */

static void accept_thread(void *a, void *b, void *d)
{
	ARG_UNUSED(b);
	ARG_UNUSED(d);
	struct ninep_session_pool_tcp *pool = a;

	while (pool->running) {
		struct sockaddr_storage addr;
		socklen_t alen = sizeof(addr);
		int s = zsock_accept(pool->listen_sock, (struct sockaddr *)&addr,
				     &alen);

		if (s < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				k_sleep(K_MSEC(100));
			} else if (pool->running) {
				LOG_ERR("accept: %d", errno);
				k_sleep(K_SECONDS(1));
			}
			continue;
		}

		char peer[48] = "?";

		if (addr.ss_family == AF_INET6) {
			struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&addr;
			char ip[INET6_ADDRSTRLEN];

			zsock_inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
			snprintf(peer, sizeof(peer), "[%s]:%u", ip,
				 ntohs(a6->sin6_port));
		} else {
			struct sockaddr_in *a4 = (struct sockaddr_in *)&addr;
			char ip[INET_ADDRSTRLEN];

			zsock_inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
			snprintf(peer, sizeof(peer), "%s:%u", ip,
				 ntohs(a4->sin_port));
		}

		struct ninep_session *ss = ninep_session_alloc(pool->base);

		if (!ss) {
			/* Refuse outright: a client left in the backlog waits
			 * for an answer that never comes. */
			LOG_WRN("%s refused: all %d sessions in use", peer,
				pool->cfg.max_sessions);
			zsock_close(s);
			continue;
		}

		struct tcp_conn *c = &pool->conns[ss->session_id];

		if (c->tid) {
			/* The slot's last thread freed the session as it
			 * left; make sure it has actually gone. */
			k_thread_join(c->tid, K_FOREVER);
			c->tid = NULL;
		}
		c->pool = pool;
		c->session = ss;
		c->sock = s;
		c->active = true;
		snprintf(c->peer, sizeof(c->peer), "%s", peer);

		ss->transport.ops = &conn_ops;
		ss->transport.priv_data = c;
		ss->transport_priv = c;

		struct ninep_server_config scfg = {
			.fs_ops = pool->cfg.fs_ops,
			.fs_ctx = pool->cfg.fs_context,
			.max_message_size = pool->cfg.rx_buf_size_per_session,
			.version = pool->cfg.version ? pool->cfg.version
						     : "9P2000",
			.auth_config = pool->cfg.auth_config,
		};
		int ret = ninep_server_init(&ss->server, &scfg, &ss->transport);

		if (ret == 0) {
			ret = ninep_server_start(&ss->server);
		}
		if (ret < 0) {
			LOG_ERR("%s: session setup failed: %d", peer, ret);
			c->active = false;
			zsock_close(s);
			c->sock = -1;
			ss->transport.ops = NULL;   /* nothing to stop */
			ninep_session_free(ss);
			continue;
		}
		ninep_session_connected(ss);
		LOG_INF("%s connected (session %d, %d in use)", peer,
			ss->session_id, ninep_session_pool_tcp_clients(pool));
	}
}

struct ninep_session_pool_tcp *ninep_session_pool_tcp_create(
	const struct ninep_session_pool_tcp_config *config)
{
	if (!config || !config->fs_ops || config->max_sessions <= 0 ||
	    config->max_sessions > CONFIG_NINEP_TCP_MAX_SESSIONS ||
	    config->rx_buf_size_per_session < 7 || pool_created) {
		return NULL;
	}

	struct ninep_session_pool_tcp *pool = &the_pool;

	memset(pool, 0, sizeof(*pool));
	pool->cfg = *config;
	pool->listen_sock = -1;
	pool->base = k_malloc(ninep_session_pool_size(config->max_sessions));
	if (!pool->base) {
		return NULL;
	}

	struct ninep_session_pool_config bcfg = {
		.max_sessions = config->max_sessions,
		.fs_ops = config->fs_ops,
		.fs_context = config->fs_context,
		.auth_config = config->auth_config,
	};

	if (ninep_session_pool_init(pool->base, &bcfg) < 0) {
		k_free(pool->base);
		return NULL;
	}
	for (int i = 0; i < config->max_sessions; i++) {
		struct tcp_conn *c = &pool->conns[i];

		k_mutex_init(&c->sock_lock);
		c->sock = -1;
		c->rx_buf = k_malloc(config->rx_buf_size_per_session);
		if (!c->rx_buf) {
			for (int j = 0; j < i; j++) {
				k_free(pool->conns[j].rx_buf);
			}
			k_free(pool->base);
			return NULL;
		}
	}
	pool_created = true;
	return pool;
}

int ninep_session_pool_tcp_start(struct ninep_session_pool_tcp *pool)
{
	uint16_t port = pool->cfg.port ? pool->cfg.port : 564;
	int opt = 1;
	int ret;

#if defined(CONFIG_NET_IPV6)
	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = htons(port),
	};

	pool->listen_sock = zsock_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (pool->listen_sock >= 0) {
		int off = 0;

		/* dual-stack: IPv4 clients arrive as mapped addresses */
		zsock_setsockopt(pool->listen_sock, IPPROTO_IPV6, IPV6_V6ONLY,
				 &off, sizeof(off));
	}
#else
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(port),
	};

	pool->listen_sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
	if (pool->listen_sock < 0) {
		LOG_ERR("socket: %d", errno);
		return -errno;
	}
	zsock_setsockopt(pool->listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt,
			 sizeof(opt));
	ret = zsock_bind(pool->listen_sock, (struct sockaddr *)&addr,
			 sizeof(addr));
	if (ret < 0) {
		LOG_ERR("bind to port %u: %d", port, errno);
		zsock_close(pool->listen_sock);
		pool->listen_sock = -1;
		return -errno;
	}
	/* Room in the backlog for every session to be arriving at once */
	ret = zsock_listen(pool->listen_sock, pool->cfg.max_sessions);
	if (ret < 0) {
		LOG_ERR("listen: %d", errno);
		zsock_close(pool->listen_sock);
		pool->listen_sock = -1;
		return -errno;
	}

	pool->running = true;
	pool->accept_tid = k_thread_create(&pool->accept_thread,
					   tcp_accept_stack,
					   K_KERNEL_STACK_SIZEOF(tcp_accept_stack),
					   accept_thread, pool, NULL, NULL,
					   TCP_POOL_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(pool->accept_tid, "9p_tcp_accept");
	LOG_INF("Listening on port %u for up to %d clients", port,
		pool->cfg.max_sessions);
	return 0;
}

void ninep_session_pool_tcp_stop(struct ninep_session_pool_tcp *pool)
{
	pool->running = false;
	if (pool->listen_sock >= 0) {
		zsock_close(pool->listen_sock);
		pool->listen_sock = -1;
	}
	if (pool->accept_tid) {
		k_thread_join(pool->accept_tid, K_FOREVER);
		pool->accept_tid = NULL;
	}
	/* Each receive thread frees its own session when its socket goes */
	for (int i = 0; i < pool->cfg.max_sessions; i++) {
		struct tcp_conn *c = &pool->conns[i];

		c->active = false;
		k_mutex_lock(&c->sock_lock, K_FOREVER);
		if (c->sock >= 0) {
			zsock_shutdown(c->sock, ZSOCK_SHUT_RDWR);
		}
		k_mutex_unlock(&c->sock_lock);
		if (c->tid) {
			k_thread_join(c->tid, K_FOREVER);
			c->tid = NULL;
		}
	}
}

int ninep_session_pool_tcp_clients(struct ninep_session_pool_tcp *pool)
{
	int n = 0;

	for (int i = 0; i < pool->cfg.max_sessions; i++) {
		if (pool->base->sessions[i].state == NINEP_SESSION_CONNECTED) {
			n++;
		}
	}
	return n;
}
