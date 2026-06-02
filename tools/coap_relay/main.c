/*
 * CoAP-to-9P Relay for local testing of the cloud transport.
 *
 * Emulates the cloud-hosted CoAP server on your LAN so you can test
 * the CoAP client transport without deploying anything.
 *
 * Architecture:
 *   [Device]  --Observe GET-->  [this relay]  <--TCP--  [9P client]
 *             <--Notification--                --9P-->
 *             --POST(response)->               <--9P--
 *
 * Build:
 *   make                          (uses pkg-config for libcoap-3)
 *   gcc -O2 -o coap_relay main.c $(pkg-config --cflags --libs libcoap-3)
 *
 * Usage:
 *   ./coap_relay [-d device_id] [-c coap_port] [-p tcp_port]
 *
 * Then from another terminal:
 *   9p -a tcp!localhost!5640 ls
 *
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>

#include <coap3/coap.h>

#define MAX_9P_MSG  8192
#define TCP_PORT    5640

/* ---- relay state ---- */

static coap_context_t  *coap_ctx;
static coap_resource_t *inbox_resource;

static int tcp_listen_fd  = -1;
static int tcp_client_fd  = -1;

/* Next 9P request to push to the device via Observe notification */
static uint8_t pending_req[MAX_9P_MSG];
static size_t  pending_req_len;
static int     has_pending_req;

/* Latest 9P response received from the device via POST */
static uint8_t pending_resp[MAX_9P_MSG];
static size_t  pending_resp_len;
static int     has_pending_resp;

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) { (void)sig; running = 0; }

/* ---- CoAP handlers ---- */

/*
 * GET /device/{id}/inbox  (Observable)
 *
 * The device registers an Observe on this resource.  When a 9P request
 * arrives from the TCP client we call coap_resource_notify_observers(),
 * which re-invokes this handler.  We return the pending 9P request as
 * the notification payload.
 */
static void inbox_handler(coap_resource_t *resource,
                           coap_session_t *session,
                           const coap_pdu_t *request,
                           const coap_string_t *query,
                           coap_pdu_t *response)
{
	coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);

	if (has_pending_req && pending_req_len > 0) {
		coap_add_data_large_response(resource, session, request,
		                             response, query,
		                             COAP_MEDIATYPE_APPLICATION_OCTET_STREAM,
		                             -1, 0,
		                             pending_req_len, pending_req,
		                             NULL, NULL);
		printf("[relay] -> device: %zu bytes  (type=%u tag=%u)\n",
		       pending_req_len, pending_req[4],
		       (unsigned)(pending_req[5] | (pending_req[6] << 8)));
		has_pending_req = 0;
	}
	/* else: initial Observe registration – return empty 2.05 */
}

/*
 * POST /device/{id}/outbox
 *
 * The device POSTs each 9P response here.  We stash it and forward it
 * to the TCP client on the next main-loop iteration.
 */
static void outbox_handler(coap_resource_t *resource,
                            coap_session_t *session,
                            const coap_pdu_t *request,
                            const coap_string_t *query,
                            coap_pdu_t *response)
{
	(void)resource;

	size_t len;
	const uint8_t *data;
	size_t offset, total;

	if (coap_get_data_large(request, &len, &data, &offset, &total)) {
		if (len > 0 && len <= MAX_9P_MSG) {
			memcpy(pending_resp, data, len);
			pending_resp_len = len;
			has_pending_resp = 1;
			printf("[relay] <- device: %zu bytes  (type=%u tag=%u)\n",
			       len, data[4],
			       (unsigned)(data[5] | (data[6] << 8)));
		}
	}

	coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
}

/* ---- setup ---- */

static int setup_coap(const char *device_id, uint16_t port)
{
	coap_address_t addr;
	coap_address_init(&addr);
	addr.addr.sin.sin_family      = AF_INET;
	addr.addr.sin.sin_port        = htons(port);
	addr.addr.sin.sin_addr.s_addr = INADDR_ANY;

	coap_ctx = coap_new_context(NULL);
	if (!coap_ctx) {
		fprintf(stderr, "coap_new_context failed\n");
		return -1;
	}

	/* Single-body block mode: POST handler gets fully reassembled data */
	coap_context_set_block_mode(coap_ctx,
	        COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);

	coap_endpoint_t *ep = coap_new_endpoint(coap_ctx, &addr, COAP_PROTO_UDP);
	if (!ep) {
		fprintf(stderr, "coap_new_endpoint failed\n");
		coap_free_context(coap_ctx);
		return -1;
	}

	/* Inbox: Observable GET */
	static char inbox_path[128];
	snprintf(inbox_path, sizeof(inbox_path), "device/%s/inbox", device_id);

	static coap_str_const_t inbox_uri;
	inbox_uri.s      = (const uint8_t *)inbox_path;
	inbox_uri.length = strlen(inbox_path);

	inbox_resource = coap_resource_init(&inbox_uri, 0);
	coap_resource_set_get_observable(inbox_resource, 1);
	coap_register_request_handler(inbox_resource, COAP_REQUEST_GET,
	                              inbox_handler);
	coap_add_resource(coap_ctx, inbox_resource);

	/* Outbox: POST */
	static char outbox_path[128];
	snprintf(outbox_path, sizeof(outbox_path), "device/%s/outbox", device_id);

	static coap_str_const_t outbox_uri;
	outbox_uri.s      = (const uint8_t *)outbox_path;
	outbox_uri.length = strlen(outbox_path);

	coap_resource_t *outbox = coap_resource_init(&outbox_uri, 0);
	coap_register_request_handler(outbox, COAP_REQUEST_POST,
	                              outbox_handler);
	coap_add_resource(coap_ctx, outbox);

	printf("[relay] CoAP on UDP :%u\n", port);
	printf("        GET  (Observe) /%s\n", inbox_path);
	printf("        POST           /%s\n", outbox_path);
	return 0;
}

static int setup_tcp(uint16_t port)
{
	tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_listen_fd < 0) return -1;

	int opt = 1;
	setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_port        = htons(port),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (bind(tcp_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		return -1;
	if (listen(tcp_listen_fd, 1) < 0)
		return -1;

	printf("[relay] TCP  on     :%u\n", port);
	return 0;
}

/* ---- I/O helpers ---- */

static int read_exact(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	while (n > 0) {
		ssize_t r = read(fd, p, n);
		if (r <= 0) return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static void try_accept(void)
{
	if (tcp_client_fd >= 0) return;

	struct pollfd pfd = { .fd = tcp_listen_fd, .events = POLLIN };
	if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		tcp_client_fd = accept(tcp_listen_fd, NULL, NULL);
		if (tcp_client_fd >= 0)
			printf("[relay] 9P client connected\n");
	}
}

/*
 * Read one complete 9P message from the TCP client (non-blocking check,
 * then blocking read of the body).  Queue it for the device.
 */
static void try_read_9p(void)
{
	if (tcp_client_fd < 0 || has_pending_req) return;

	struct pollfd pfd = { .fd = tcp_client_fd, .events = POLLIN };
	if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;

	uint8_t hdr[4];
	if (read_exact(tcp_client_fd, hdr, 4) < 0) {
		printf("[relay] 9P client disconnected\n");
		close(tcp_client_fd);
		tcp_client_fd = -1;
		return;
	}

	uint32_t size = (uint32_t)hdr[0]       | ((uint32_t)hdr[1] << 8) |
	                ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);

	if (size < 7 || size > MAX_9P_MSG) {
		printf("[relay] Bad 9P message size: %u\n", size);
		close(tcp_client_fd);
		tcp_client_fd = -1;
		return;
	}

	memcpy(pending_req, hdr, 4);
	if (read_exact(tcp_client_fd, pending_req + 4, size - 4) < 0) {
		printf("[relay] 9P read failed\n");
		close(tcp_client_fd);
		tcp_client_fd = -1;
		return;
	}

	pending_req_len  = size;
	has_pending_req  = 1;

	printf("[relay] <- client:  %u bytes  (type=%u tag=%u)\n",
	       size, pending_req[4],
	       (unsigned)(pending_req[5] | (pending_req[6] << 8)));

	/* Push to device via Observe notification */
	coap_resource_notify_observers(inbox_resource, NULL);
}

/* Forward a pending device response back to the TCP client */
static void try_send_response(void)
{
	if (!has_pending_resp || tcp_client_fd < 0) return;

	ssize_t w = write(tcp_client_fd, pending_resp, pending_resp_len);
	if (w < 0) {
		printf("[relay] TCP write failed: %s\n", strerror(errno));
	} else {
		printf("[relay] -> client:  %zd bytes\n", w);
	}
	has_pending_resp = 0;
}

/* ---- main ---- */

int main(int argc, char *argv[])
{
	const char *device_id = "test";
	uint16_t coap_port    = 5683;
	uint16_t tcp_port     = TCP_PORT;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			device_id = argv[++i];
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			coap_port = (uint16_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			tcp_port = (uint16_t)atoi(argv[++i]);
		else {
			printf("Usage: %s [-d device_id] [-c coap_port] [-p tcp_port]\n"
			       "  -d  Device ID to match (default: test)\n"
			       "  -c  CoAP UDP port      (default: 5683)\n"
			       "  -p  TCP 9P port        (default: 5640)\n",
			       argv[0]);
			return (strcmp(argv[i], "-h") == 0) ? 0 : 1;
		}
	}

	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	coap_startup();
	coap_set_log_level(COAP_LOG_WARN);

	printf("=== CoAP <-> 9P Relay ===\n");
	printf("Device ID: %s\n\n", device_id);

	if (setup_coap(device_id, coap_port) < 0) {
		fprintf(stderr, "CoAP setup failed\n");
		return 1;
	}
	if (setup_tcp(tcp_port) < 0) {
		fprintf(stderr, "TCP setup failed: %s\n", strerror(errno));
		return 1;
	}

	printf("\nReady.  Waiting for:\n");
	printf("  Device  -> Observe on UDP :%u\n", coap_port);
	printf("  Client  -> TCP :%u\n\n", tcp_port);

	while (running) {
		coap_io_process(coap_ctx, 50);   /* 50 ms CoAP tick */
		try_accept();
		try_read_9p();
		try_send_response();
	}

	printf("\nShutting down.\n");
	if (tcp_client_fd >= 0) close(tcp_client_fd);
	if (tcp_listen_fd >= 0) close(tcp_listen_fd);
	coap_free_context(coap_ctx);
	coap_cleanup();
	return 0;
}
