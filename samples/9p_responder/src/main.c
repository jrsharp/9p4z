/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Simple 9P responder - responds to Tversion and Tattach
 * Can be connected to Plan 9 port tools via QEMU serial
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/message.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ninep_responder, LOG_LEVEL_DBG);

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_console)

static uint8_t rx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t tx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static struct ninep_transport *g_transport;

/* Helper to extract uint32 from message */
static uint32_t get_u32(const uint8_t *buf, size_t offset)
{
	return buf[offset] |
	       (buf[offset + 1] << 8) |
	       (buf[offset + 2] << 16) |
	       (buf[offset + 3] << 24);
}

static void handle_tversion(struct ninep_transport *transport,
                            const struct ninep_msg_header *hdr,
                            const uint8_t *msg, size_t len)
{
	uint32_t client_msize;
	const char *version_str;
	uint16_t version_len;
	size_t offset = 7;  /* Skip header */
	int ret;

	LOG_INF("Handling Tversion");

	/* Parse client msize */
	if (len < 11) {
		LOG_ERR("Tversion too short");
		return;
	}
	client_msize = get_u32(msg, offset);
	offset += 4;

	/* Parse version string */
	ret = ninep_parse_string(msg, len, &offset, &version_str, &version_len);
	if (ret < 0) {
		LOG_ERR("Failed to parse version string");
		return;
	}

	LOG_INF("Client: msize=%u, version=%.*s", client_msize, version_len, version_str);

	/* Negotiate msize (use smaller of client's or our max) */
	uint32_t our_msize = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	uint32_t negotiated_msize = (client_msize < our_msize) ? client_msize : our_msize;

	/* Check version - we only support 9P2000 */
	const char *our_version = "9P2000";
	uint16_t our_version_len = 6;
	bool version_match = (version_len == 6 && memcmp(version_str, "9P2000", 6) == 0);

	if (!version_match) {
		LOG_WRN("Unsupported version, responding with 'unknown'");
		our_version = "unknown";
		our_version_len = 7;
	}

	/* Build Rversion response */
	int msg_len = ninep_build_rversion(tx_buffer, sizeof(tx_buffer),
	                                    hdr->tag, negotiated_msize,
	                                    our_version, our_version_len);
	if (msg_len < 0) {
		LOG_ERR("Failed to build Rversion");
		return;
	}

	/* Send response */
	ret = ninep_transport_send(transport, tx_buffer, msg_len);
	if (ret < 0) {
		LOG_ERR("Failed to send Rversion");
	} else {
		LOG_INF("Sent Rversion: msize=%u, version=%s", negotiated_msize, our_version);
	}
}

static void handle_tattach(struct ninep_transport *transport,
                           const struct ninep_msg_header *hdr,
                           const uint8_t *msg, size_t len)
{
	uint32_t fid, afid;
	const char *uname, *aname;
	uint16_t uname_len, aname_len;
	size_t offset = 7;
	int ret;

	LOG_INF("Handling Tattach");

	/* Parse Tattach fields */
	if (len < 15) {  /* Minimum: header + fid + afid */
		LOG_ERR("Tattach too short");
		return;
	}

	fid = get_u32(msg, offset);
	offset += 4;
	afid = get_u32(msg, offset);
	offset += 4;

	ret = ninep_parse_string(msg, len, &offset, &uname, &uname_len);
	if (ret < 0) {
		LOG_ERR("Failed to parse uname");
		return;
	}

	ret = ninep_parse_string(msg, len, &offset, &aname, &aname_len);
	if (ret < 0) {
		LOG_ERR("Failed to parse aname");
		return;
	}

	LOG_INF("Client attach: fid=%u, afid=%u, uname=%.*s, aname=%.*s",
	        fid, afid, uname_len, uname, aname_len, aname);

	/* Build Rattach response with a root qid */
	struct ninep_qid root_qid = {
		.type = NINEP_QTDIR,
		.version = 0,
		.path = 1,  /* Root directory */
	};

	int msg_len = ninep_build_rattach(tx_buffer, sizeof(tx_buffer), hdr->tag, &root_qid);
	if (msg_len < 0) {
		LOG_ERR("Failed to build Rattach");
		return;
	}

	/* Send response */
	ret = ninep_transport_send(transport, tx_buffer, msg_len);
	if (ret < 0) {
		LOG_ERR("Failed to send Rattach");
	} else {
		LOG_INF("Sent Rattach: qid.path=%llu", root_qid.path);
	}
}

static void handle_tclunk(struct ninep_transport *transport,
                          const struct ninep_msg_header *hdr,
                          const uint8_t *msg, size_t len)
{
	uint32_t fid;
	int ret;

	if (len < 11) {
		LOG_ERR("Tclunk too short");
		return;
	}

	fid = get_u32(msg, 7);
	LOG_INF("Handling Tclunk: fid=%u", fid);

	/* Build Rclunk response */
	int msg_len = ninep_build_rclunk(tx_buffer, sizeof(tx_buffer), hdr->tag);
	if (msg_len < 0) {
		LOG_ERR("Failed to build Rclunk");
		return;
	}

	/* Send response */
	ret = ninep_transport_send(transport, tx_buffer, msg_len);
	if (ret < 0) {
		LOG_ERR("Failed to send Rclunk");
	} else {
		LOG_INF("Sent Rclunk");
	}
}

static void message_received(struct ninep_transport *transport,
                             const uint8_t *buf, size_t len,
                             void *user_data)
{
	struct ninep_msg_header hdr;
	int ret;

	LOG_INF("Received message: %d bytes", len);
	LOG_HEXDUMP_DBG(buf, len < 32 ? len : 32, "Message:");

	/* Parse header */
	ret = ninep_parse_header(buf, len, &hdr);
	if (ret < 0) {
		LOG_ERR("Failed to parse message header");
		return;
	}

	LOG_INF("Message: type=%d, tag=%u, size=%u", hdr.type, hdr.tag, hdr.size);

	/* Dispatch based on message type */
	switch (hdr.type) {
	case NINEP_TVERSION:
		handle_tversion(transport, &hdr, buf, len);
		break;

	case NINEP_TATTACH:
		handle_tattach(transport, &hdr, buf, len);
		break;

	case NINEP_TCLUNK:
		handle_tclunk(transport, &hdr, buf, len);
		break;

	case NINEP_TWALK:
		LOG_WRN("Twalk not implemented yet");
		break;

	case NINEP_TOPEN:
		LOG_WRN("Topen not implemented yet");
		break;

	case NINEP_TREAD:
		LOG_WRN("Tread not implemented yet");
		break;

	case NINEP_TWRITE:
		LOG_WRN("Twrite not implemented yet");
		break;

	default:
		LOG_ERR("Unsupported message type: %d", hdr.type);
		/* Should send Rerror here */
		break;
	}
}

int main(void)
{
	const struct device *uart_dev;
	struct ninep_transport transport;
	struct ninep_transport_uart_config uart_config;
	int ret;

	LOG_INF("=== 9P Responder Sample ===");
	LOG_INF("Max message size: %d", CONFIG_NINEP_MAX_MESSAGE_SIZE);

	uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -1;
	}

	/* Configure UART transport */
	uart_config.uart_dev = uart_dev;
	uart_config.rx_buf = rx_buffer;
	uart_config.rx_buf_size = sizeof(rx_buffer);

	/* Initialize transport */
	ret = ninep_transport_uart_init(&transport, &uart_config,
	                                message_received, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UART transport: %d", ret);
		return -1;
	}

	g_transport = &transport;

	/* Start receiving */
	ret = ninep_transport_start(&transport);
	if (ret < 0) {
		LOG_ERR("Failed to start transport: %d", ret);
		return -1;
	}

	LOG_INF("9P responder ready - waiting for connections...");
	LOG_INF("Supported: Tversion, Tattach, Tclunk");

	/* Keep running */
	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
