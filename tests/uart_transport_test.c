/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/message.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <string.h>

#define UART_NODE DT_NODELABEL(uart_emul0)

static const struct device *uart_dev = DEVICE_DT_GET(UART_NODE);
static struct ninep_transport transport;
static uint8_t rx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t test_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* Callback state */
static bool recv_cb_called;
static uint8_t recv_cb_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static size_t recv_cb_len;

static void test_recv_callback(struct ninep_transport *t,
                                const uint8_t *buf, size_t len,
                                void *user_data)
{
	recv_cb_called = true;
	recv_cb_len = len;
	if (len <= sizeof(recv_cb_buffer)) {
		memcpy(recv_cb_buffer, buf, len);
	}
}

static void *uart_transport_setup(void)
{
	zassert_true(device_is_ready(uart_dev), "UART device not ready");

	memset(&transport, 0, sizeof(transport));
	memset(rx_buffer, 0, sizeof(rx_buffer));
	memset(test_buffer, 0, sizeof(test_buffer));
	recv_cb_called = false;
	recv_cb_len = 0;

	return &transport;
}

static void uart_transport_before(void *f)
{
	/* Reset state before each test */
	memset(&transport, 0, sizeof(transport));
	recv_cb_called = false;
	recv_cb_len = 0;
	memset(recv_cb_buffer, 0, sizeof(recv_cb_buffer));
}

ZTEST_SUITE(ninep_uart_transport, NULL, uart_transport_setup,
            uart_transport_before, NULL, NULL);

ZTEST(ninep_uart_transport, test_uart_init)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	int ret = ninep_transport_uart_init(&transport, &config,
	                                     test_recv_callback, NULL);
	zassert_equal(ret, 0, "UART transport init failed");
	zassert_not_null(transport.ops, "Transport ops not set");
	zassert_not_null(transport.recv_cb, "Recv callback not set");
}

ZTEST(ninep_uart_transport, test_uart_init_null_checks)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	/* NULL transport */
	zassert_not_equal(ninep_transport_uart_init(NULL, &config,
	                                              test_recv_callback, NULL), 0,
	                  "Should fail with NULL transport");

	/* NULL config */
	zassert_not_equal(ninep_transport_uart_init(&transport, NULL,
	                                              test_recv_callback, NULL), 0,
	                  "Should fail with NULL config");

	/* NULL UART device */
	config.uart_dev = NULL;
	zassert_not_equal(ninep_transport_uart_init(&transport, &config,
	                                              test_recv_callback, NULL), 0,
	                  "Should fail with NULL UART device");

	config.uart_dev = uart_dev;

	/* NULL RX buffer */
	config.rx_buf = NULL;
	zassert_not_equal(ninep_transport_uart_init(&transport, &config,
	                                              test_recv_callback, NULL), 0,
	                  "Should fail with NULL RX buffer");

	config.rx_buf = rx_buffer;

	/* Zero buffer size */
	config.rx_buf_size = 0;
	zassert_not_equal(ninep_transport_uart_init(&transport, &config,
	                                              test_recv_callback, NULL), 0,
	                  "Should fail with zero buffer size");
}

ZTEST(ninep_uart_transport, test_uart_send_simple)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);

	/* Build a simple Tversion message */
	int msg_len = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                    NINEP_NOTAG, 8192, "9P2000", 6);
	zassert_true(msg_len > 0, "Failed to build Tversion");

	/* Send via transport */
	int ret = ninep_transport_send(&transport, test_buffer, msg_len);
	zassert_equal(ret, msg_len, "Send failed");
}

ZTEST(ninep_uart_transport, test_uart_start_stop)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);

	/* Start transport */
	int ret = ninep_transport_start(&transport);
	zassert_equal(ret, 0, "Failed to start transport");

	/* Stop transport */
	ret = ninep_transport_stop(&transport);
	zassert_equal(ret, 0, "Failed to stop transport");
}

ZTEST(ninep_uart_transport, test_uart_loopback_simple)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Build a simple message */
	int msg_len = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                    NINEP_NOTAG, 8192, "9P2000", 6);

	/* Send - in loopback mode, should be received */
	ninep_transport_send(&transport, test_buffer, msg_len);

	/* Wait for IRQ processing (uart_emul triggers IRQ via work queue) */
	k_sleep(K_MSEC(100));

	/* Verify callback was called */
	zassert_true(recv_cb_called, "Receive callback not called");
	zassert_equal(recv_cb_len, msg_len, "Wrong received length");
	zassert_mem_equal(recv_cb_buffer, test_buffer, msg_len,
	                  "Received data mismatch");
}

ZTEST(ninep_uart_transport, test_uart_message_framing)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Build Tversion message */
	int msg_len = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                    NINEP_NOTAG, 8192, "9P2000", 6);

	/* Inject message byte-by-byte to test framing */
	for (int i = 0; i < msg_len; i++) {
		uart_emul_put_rx_data(uart_dev, &test_buffer[i], 1);

		/* After 7 bytes, header should be parsed */
		if (i == 6) {
			k_sleep(K_MSEC(10));
			/* Callback shouldn't be called yet */
			zassert_false(recv_cb_called, "Callback called too early");
		}
	}

	/* Wait for final IRQ processing */
	k_sleep(K_MSEC(100));

	/* Now callback should be called with complete message */
	zassert_true(recv_cb_called, "Callback not called");
	zassert_equal(recv_cb_len, msg_len, "Wrong message length");

	/* Verify header */
	struct ninep_msg_header hdr;
	ninep_parse_header(recv_cb_buffer, recv_cb_len, &hdr);
	zassert_equal(hdr.type, NINEP_TVERSION, "Wrong message type");
	zassert_equal(hdr.size, 19, "Wrong message size");
}

ZTEST(ninep_uart_transport, test_uart_multiple_messages)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Send first message */
	int msg1_len = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                     NINEP_NOTAG, 8192, "9P2000", 6);
	uart_emul_put_rx_data(uart_dev, test_buffer, msg1_len);
	k_sleep(K_MSEC(100));

	zassert_true(recv_cb_called, "First message not received");
	zassert_equal(recv_cb_len, msg1_len, "Wrong first message length");

	/* Reset for second message */
	recv_cb_called = false;
	recv_cb_len = 0;

	/* Send second message (different type) */
	struct ninep_qid qid = {.type = NINEP_QTDIR, .version = 0, .path = 1};
	int msg2_len = ninep_build_rattach(test_buffer, sizeof(test_buffer), 1, &qid);
	uart_emul_put_rx_data(uart_dev, test_buffer, msg2_len);
	k_sleep(K_MSEC(100));

	zassert_true(recv_cb_called, "Second message not received");
	zassert_equal(recv_cb_len, msg2_len, "Wrong second message length");

	/* Verify it's Rattach */
	struct ninep_msg_header hdr;
	ninep_parse_header(recv_cb_buffer, recv_cb_len, &hdr);
	zassert_equal(hdr.type, NINEP_RATTACH, "Wrong second message type");
}

ZTEST(ninep_uart_transport, test_uart_buffer_overflow_protection)
{
	/* Use a smaller buffer */
	uint8_t small_rx_buf[32];

	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = small_rx_buf,
		.rx_buf_size = sizeof(small_rx_buf),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Build a large message (Twalk with multiple path elements) */
	const char *wnames[] = {"test1", "test2", "test3", "test4"};
	uint16_t wname_lens[] = {5, 5, 5, 5};

	int msg_len = ninep_build_twalk(test_buffer, sizeof(test_buffer),
	                                 1, 1, 2, 4, wnames, wname_lens);
	zassert_true(msg_len > sizeof(small_rx_buf), "Test message too small");

	/* Send oversized message */
	uart_emul_put_rx_data(uart_dev, test_buffer, msg_len);
	k_sleep(K_MSEC(100));

	/* Callback should NOT be called due to overflow protection */
	zassert_false(recv_cb_called, "Callback called despite overflow");
}

ZTEST(ninep_uart_transport, test_uart_invalid_header_recovery)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Send garbage (invalid header) */
	uint8_t garbage[] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
	uart_emul_put_rx_data(uart_dev, garbage, sizeof(garbage));
	k_sleep(K_MSEC(100));

	/* Should not trigger callback */
	zassert_false(recv_cb_called, "Callback called for invalid header");

	/* Now send a valid message - transport should recover */
	int msg_len = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                    NINEP_NOTAG, 8192, "9P2000", 6);
	uart_emul_put_rx_data(uart_dev, test_buffer, msg_len);
	k_sleep(K_MSEC(100));

	/* This should succeed */
	zassert_true(recv_cb_called, "Transport did not recover");
	zassert_equal(recv_cb_len, msg_len, "Wrong recovered message length");
}

ZTEST(ninep_uart_transport, test_uart_large_message)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Build a Twalk with max path elements */
	const char *wnames[NINEP_MAX_WELEM];
	uint16_t wname_lens[NINEP_MAX_WELEM];

	for (int i = 0; i < NINEP_MAX_WELEM; i++) {
		wnames[i] = "testdir";
		wname_lens[i] = 7;
	}

	int msg_len = ninep_build_twalk(test_buffer, sizeof(test_buffer),
	                                 1, 1, 2, NINEP_MAX_WELEM,
	                                 wnames, wname_lens);
	zassert_true(msg_len > 0, "Failed to build large Twalk");

	/* Send large message */
	uart_emul_put_rx_data(uart_dev, test_buffer, msg_len);
	k_sleep(K_MSEC(200));

	zassert_true(recv_cb_called, "Large message not received");
	zassert_equal(recv_cb_len, msg_len, "Wrong large message length");

	/* Verify it's a valid Twalk */
	struct ninep_msg_header hdr;
	ninep_parse_header(recv_cb_buffer, recv_cb_len, &hdr);
	zassert_equal(hdr.type, NINEP_TWALK, "Wrong message type");
}

ZTEST(ninep_uart_transport, test_uart_version_negotiation_sequence)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Client sends Tversion */
	int tversion_len = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                         NINEP_NOTAG, 8192, "9P2000", 6);
	uart_emul_put_rx_data(uart_dev, test_buffer, tversion_len);
	k_sleep(K_MSEC(100));

	zassert_true(recv_cb_called, "Tversion not received");

	/* Verify it's Tversion */
	struct ninep_msg_header thdr;
	ninep_parse_header(recv_cb_buffer, recv_cb_len, &thdr);
	zassert_equal(thdr.type, NINEP_TVERSION, "Not a Tversion");
	zassert_equal(thdr.tag, NINEP_NOTAG, "Tversion should use NOTAG");

	/* Reset for response */
	recv_cb_called = false;
	recv_cb_len = 0;

	/* Server responds with Rversion */
	int rversion_len = ninep_build_rversion(test_buffer, sizeof(test_buffer),
	                                         NINEP_NOTAG, 8192, "9P2000", 6);
	uart_emul_put_rx_data(uart_dev, test_buffer, rversion_len);
	k_sleep(K_MSEC(100));

	zassert_true(recv_cb_called, "Rversion not received");

	/* Verify it's Rversion */
	struct ninep_msg_header rhdr;
	ninep_parse_header(recv_cb_buffer, recv_cb_len, &rhdr);
	zassert_equal(rhdr.type, NINEP_RVERSION, "Not an Rversion");
	zassert_equal(rhdr.tag, NINEP_NOTAG, "Rversion should use NOTAG");
}

ZTEST(ninep_uart_transport, test_uart_bidirectional_communication)
{
	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	ninep_transport_uart_init(&transport, &config, test_recv_callback, NULL);
	ninep_transport_start(&transport);

	/* Build Tattach */
	int tattach_len = ninep_build_tattach(test_buffer, sizeof(test_buffer),
	                                       1, 0, NINEP_NOFID,
	                                       "glenda", 6, "", 0);

	/* Send via transport (TX) */
	int ret = ninep_transport_send(&transport, test_buffer, tattach_len);
	zassert_equal(ret, tattach_len, "Send failed");

	/* In loopback mode, should also receive (RX) */
	k_sleep(K_MSEC(100));

	zassert_true(recv_cb_called, "Loopback receive failed");
	zassert_equal(recv_cb_len, tattach_len, "Wrong loopback length");

	/* Verify message integrity */
	struct ninep_msg_header hdr;
	ninep_parse_header(recv_cb_buffer, recv_cb_len, &hdr);
	zassert_equal(hdr.type, NINEP_TATTACH, "Wrong loopback message type");
	zassert_equal(hdr.tag, 1, "Wrong loopback tag");
}
