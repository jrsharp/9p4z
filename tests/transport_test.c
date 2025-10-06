/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/protocol.h>
#include <string.h>

/* Mock transport implementation for testing */
struct mock_transport_data {
	uint8_t *last_sent_buf;
	size_t last_sent_len;
	bool started;
	bool stopped;
	int send_return_value;
};

static struct mock_transport_data mock_data;
static struct ninep_transport test_transport;
static bool recv_callback_called;
static uint8_t recv_callback_buf[256];
static size_t recv_callback_len;

static void test_recv_callback(struct ninep_transport *transport,
                               const uint8_t *buf, size_t len,
                               void *user_data)
{
	recv_callback_called = true;
	if (len <= sizeof(recv_callback_buf)) {
		memcpy(recv_callback_buf, buf, len);
		recv_callback_len = len;
	}
}

static int mock_send(struct ninep_transport *transport, const uint8_t *buf,
                     size_t len)
{
	struct mock_transport_data *data = transport->priv_data;

	if (data->send_return_value < 0) {
		return data->send_return_value;
	}

	data->last_sent_buf = (uint8_t *)buf;
	data->last_sent_len = len;

	return len;
}

static int mock_start(struct ninep_transport *transport)
{
	struct mock_transport_data *data = transport->priv_data;
	data->started = true;
	data->stopped = false;
	return 0;
}

static int mock_stop(struct ninep_transport *transport)
{
	struct mock_transport_data *data = transport->priv_data;
	data->stopped = true;
	data->started = false;
	return 0;
}

static const struct ninep_transport_ops mock_ops = {
	.send = mock_send,
	.start = mock_start,
	.stop = mock_stop,
};

static void *transport_setup(void)
{
	memset(&mock_data, 0, sizeof(mock_data));
	memset(&test_transport, 0, sizeof(test_transport));
	recv_callback_called = false;
	recv_callback_len = 0;

	test_transport.ops = &mock_ops;
	test_transport.priv_data = &mock_data;
	test_transport.recv_cb = test_recv_callback;
	test_transport.user_data = NULL;

	return &test_transport;
}

static void transport_before(void *f)
{
	memset(&mock_data, 0, sizeof(mock_data));
	recv_callback_called = false;
	recv_callback_len = 0;
}

ZTEST_SUITE(ninep_transport, NULL, transport_setup, transport_before, NULL, NULL);

ZTEST(ninep_transport, test_transport_send)
{
	uint8_t test_data[] = {0x01, 0x02, 0x03, 0x04};
	int ret;

	ret = ninep_transport_send(&test_transport, test_data, sizeof(test_data));
	zassert_equal(ret, sizeof(test_data), "Send failed");
	zassert_equal(mock_data.last_sent_len, sizeof(test_data), "Wrong sent length");
	zassert_equal(mock_data.last_sent_buf, test_data, "Wrong buffer pointer");
}

ZTEST(ninep_transport, test_transport_send_error)
{
	uint8_t test_data[] = {0x01, 0x02, 0x03};
	int ret;

	mock_data.send_return_value = -EIO;
	ret = ninep_transport_send(&test_transport, test_data, sizeof(test_data));
	zassert_equal(ret, -EIO, "Should return error code");
}

ZTEST(ninep_transport, test_transport_start_stop)
{
	int ret;

	zassert_false(mock_data.started, "Should not be started initially");

	ret = ninep_transport_start(&test_transport);
	zassert_equal(ret, 0, "Start failed");
	zassert_true(mock_data.started, "Should be started");

	ret = ninep_transport_stop(&test_transport);
	zassert_equal(ret, 0, "Stop failed");
	zassert_true(mock_data.stopped, "Should be stopped");
	zassert_false(mock_data.started, "Should not be started after stop");
}

ZTEST(ninep_transport, test_transport_receive_callback)
{
	uint8_t test_message[] = {
		0x13, 0x00, 0x00, 0x00,  /* size = 19 */
		0x65,                     /* type = Rversion */
		0x01, 0x00,              /* tag = 1 */
		0x00, 0x20, 0x00, 0x00,  /* msize */
		0x06, 0x00,              /* version length */
		'9', 'P', '2', '0', '0', '0'
	};

	/* Simulate receiving a message */
	test_transport.recv_cb(&test_transport, test_message,
	                       sizeof(test_message), NULL);

	zassert_true(recv_callback_called, "Callback not called");
	zassert_equal(recv_callback_len, sizeof(test_message), "Wrong message length");
	zassert_mem_equal(recv_callback_buf, test_message, sizeof(test_message),
	                  "Message content mismatch");
}

ZTEST(ninep_transport, test_transport_send_header)
{
	struct ninep_msg_header hdr = {
		.size = 19,
		.type = NINEP_TVERSION,
		.tag = 1,
	};
	uint8_t buf[7];
	int ret;

	ret = ninep_write_header(buf, sizeof(buf), &hdr);
	zassert_equal(ret, 7, "Failed to write header");

	ret = ninep_transport_send(&test_transport, buf, sizeof(buf));
	zassert_equal(ret, sizeof(buf), "Send failed");
	zassert_equal(mock_data.last_sent_len, 7, "Wrong sent length");
}

ZTEST(ninep_transport, test_transport_null_checks)
{
	uint8_t test_data[] = {0x01, 0x02};

	zassert_equal(ninep_transport_send(NULL, test_data, sizeof(test_data)),
	              -EINVAL, "Should fail with NULL transport");
	zassert_equal(ninep_transport_start(NULL), -EINVAL,
	              "Should fail with NULL transport");
	zassert_equal(ninep_transport_stop(NULL), -EINVAL,
	              "Should fail with NULL transport");
}

ZTEST(ninep_transport, test_transport_message_roundtrip)
{
	/* Create a complete Tversion message */
	uint8_t msg[19];
	struct ninep_msg_header hdr = {
		.size = 19,
		.type = NINEP_TVERSION,
		.tag = 1,
	};
	size_t offset = 0;

	/* Write header */
	ninep_write_header(msg, sizeof(msg), &hdr);
	offset = 7;

	/* Write msize (8192) */
	msg[offset++] = 0x00;
	msg[offset++] = 0x20;
	msg[offset++] = 0x00;
	msg[offset++] = 0x00;

	/* Write version string */
	ninep_write_string(msg, sizeof(msg), &offset, "9P2000", 6);

	/* Send via transport */
	int ret = ninep_transport_send(&test_transport, msg, sizeof(msg));
	zassert_equal(ret, sizeof(msg), "Send failed");

	/* Simulate receiving the same message */
	test_transport.recv_cb(&test_transport, msg, sizeof(msg), NULL);

	/* Verify callback received the message */
	zassert_true(recv_callback_called, "Callback not called");
	zassert_equal(recv_callback_len, sizeof(msg), "Wrong message length");

	/* Parse and verify header from received message */
	struct ninep_msg_header recv_hdr;
	ret = ninep_parse_header(recv_callback_buf, recv_callback_len, &recv_hdr);
	zassert_equal(ret, 0, "Failed to parse received header");
	zassert_equal(recv_hdr.size, 19, "Wrong size");
	zassert_equal(recv_hdr.type, NINEP_TVERSION, "Wrong type");
	zassert_equal(recv_hdr.tag, 1, "Wrong tag");
}