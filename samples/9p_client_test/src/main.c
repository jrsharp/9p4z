/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Automated end-to-end test for 9P client
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ztest.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/message.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ninep_test, LOG_LEVEL_INF);

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_ninep_uart)
#define TEST_TIMEOUT_MS 5000

static uint8_t rx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t tx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t response_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static struct ninep_transport g_transport;

static bool response_ready = false;
static size_t response_len = 0;
static K_SEM_DEFINE(response_sem, 0, 1);

/* Transport receive callback */
static void message_received(struct ninep_transport *transport,
                             const uint8_t *buf, size_t len,
                             void *user_data)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(user_data);

	if (len > sizeof(response_buffer)) {
		LOG_ERR("Response too large: %d bytes", len);
		return;
	}

	memcpy(response_buffer, buf, len);
	response_len = len;
	response_ready = true;

	k_sem_give(&response_sem);
}

/* Send request and wait for response */
static int send_and_wait(const uint8_t *req, size_t req_len, uint8_t *resp,
                        size_t *resp_len, uint32_t timeout_ms)
{
	response_ready = false;
	response_len = 0;

	int ret = ninep_transport_send(&g_transport, req, req_len);
	if (ret < 0) {
		LOG_ERR("Send failed: %d", ret);
		return ret;
	}

	ret = k_sem_take(&response_sem, K_MSEC(timeout_ms));
	if (ret < 0) {
		LOG_ERR("Timeout waiting for response");
		return ret;
	}

	if (!response_ready || response_len == 0) {
		LOG_ERR("No response received");
		return -ENODATA;
	}

	*resp_len = response_len;
	memcpy(resp, response_buffer, response_len);

	return 0;
}

/* Test: Initialize transport */
ZTEST(ninep_e2e, test_00_init_transport)
{
	const struct device *uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

	zassert_true(device_is_ready(uart_dev), "UART device not ready");

	struct ninep_transport_uart_config config = {
		.uart_dev = uart_dev,
		.rx_buf = rx_buffer,
		.rx_buf_size = sizeof(rx_buffer),
	};

	int ret = ninep_transport_uart_init(&g_transport, &config,
	                                   message_received, NULL);
	zassert_equal(ret, 0, "Transport init failed: %d", ret);

	ret = ninep_transport_start(&g_transport);
	zassert_equal(ret, 0, "Transport start failed: %d", ret);

	LOG_INF("✓ Transport initialized and started");
}

/* Test: Version negotiation */
ZTEST(ninep_e2e, test_01_version)
{
	uint8_t resp[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	size_t resp_len;

	/* Build Tversion request */
	size_t tx_len = ninep_build_version(tx_buffer, sizeof(tx_buffer),
	                                    1, /* tag */
	                                    8192, /* msize */
	                                    "9P2000");
	zassert_true(tx_len > 0, "Failed to build Tversion");

	LOG_INF("Sending Tversion...");
	int ret = send_and_wait(tx_buffer, tx_len, resp, &resp_len, TEST_TIMEOUT_MS);
	zassert_equal(ret, 0, "Tversion failed: %d", ret);

	/* Parse response */
	struct ninep_msg_header hdr;
	ret = ninep_parse_header(resp, resp_len, &hdr);
	zassert_equal(ret, 0, "Failed to parse Rversion header");
	zassert_equal(hdr.type, NINEP_RVERSION, "Expected Rversion, got %d", hdr.type);
	zassert_equal(hdr.tag, 1, "Tag mismatch");

	LOG_INF("✓ Version negotiation successful");
}

/* Test: Attach to root */
ZTEST(ninep_e2e, test_02_attach)
{
	uint8_t resp[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	size_t resp_len;

	/* Build Tattach request */
	size_t tx_len = ninep_build_attach(tx_buffer, sizeof(tx_buffer),
	                                   2, /* tag */
	                                   1, /* fid */
	                                   NINEP_NOFID, /* afid */
	                                   "nobody", /* uname */
	                                   ""); /* aname */
	zassert_true(tx_len > 0, "Failed to build Tattach");

	LOG_INF("Sending Tattach...");
	int ret = send_and_wait(tx_buffer, tx_len, resp, &resp_len, TEST_TIMEOUT_MS);
	zassert_equal(ret, 0, "Tattach failed: %d", ret);

	/* Parse response */
	struct ninep_msg_header hdr;
	ret = ninep_parse_header(resp, resp_len, &hdr);
	zassert_equal(ret, 0, "Failed to parse Rattach header");
	zassert_equal(hdr.type, NINEP_RATTACH, "Expected Rattach, got %d", hdr.type);
	zassert_equal(hdr.tag, 2, "Tag mismatch");

	LOG_INF("✓ Attach successful");
}

/* Test: Walk to a path */
ZTEST(ninep_e2e, test_03_walk)
{
	uint8_t resp[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	size_t resp_len;

	/* Build Twalk request - walk from root fid (1) to test.txt */
	const char *wnames[] = {"test.txt"};
	size_t tx_len = ninep_build_walk(tx_buffer, sizeof(tx_buffer),
	                                 3, /* tag */
	                                 1, /* fid (root) */
	                                 2, /* newfid */
	                                 wnames, 1);
	zassert_true(tx_len > 0, "Failed to build Twalk");

	LOG_INF("Sending Twalk for test.txt...");
	int ret = send_and_wait(tx_buffer, tx_len, resp, &resp_len, TEST_TIMEOUT_MS);

	/* Note: This will fail if 9P server not connected or test.txt doesn't exist */
	if (ret == -ETIMEDOUT) {
		ztest_test_skip();
		return;
	}

	zassert_equal(ret, 0, "Twalk failed: %d", ret);

	struct ninep_msg_header hdr;
	ret = ninep_parse_header(resp, resp_len, &hdr);
	zassert_equal(ret, 0, "Failed to parse Rwalk header");
	zassert_equal(hdr.type, NINEP_RWALK, "Expected Rwalk, got %d", hdr.type);

	LOG_INF("✓ Walk successful");
}

/* Test: Stat a file */
ZTEST(ninep_e2e, test_04_stat)
{
	uint8_t resp[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	size_t resp_len;

	/* Build Tstat request for root fid */
	size_t tx_len = ninep_build_tstat(tx_buffer, sizeof(tx_buffer),
	                                  4, /* tag */
	                                  1); /* fid (root) */
	zassert_true(tx_len > 0, "Failed to build Tstat");

	LOG_INF("Sending Tstat...");
	int ret = send_and_wait(tx_buffer, tx_len, resp, &resp_len, TEST_TIMEOUT_MS);

	if (ret == -ETIMEDOUT) {
		ztest_test_skip();
		return;
	}

	zassert_equal(ret, 0, "Tstat failed: %d", ret);

	struct ninep_msg_header hdr;
	ret = ninep_parse_header(resp, resp_len, &hdr);
	zassert_equal(ret, 0, "Failed to parse Rstat header");
	zassert_equal(hdr.type, NINEP_RSTAT, "Expected Rstat, got %d", hdr.type);

	LOG_INF("✓ Stat successful");
}

/* Test suite setup */
static void *ninep_e2e_setup(void)
{
	LOG_INF("=== 9P End-to-End Test Suite ===");
	LOG_INF("This test requires a 9P server connected to uart1");
	LOG_INF("Start server with: 9pex ~/9p-test | 9pserve unix!/tmp/9p.sock");
	return NULL;
}

ZTEST_SUITE(ninep_e2e, NULL, ninep_e2e_setup, NULL, NULL, NULL);
