/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/protocol.h>
#include <string.h>

/* Test fixture data */
struct protocol_fixture {
	uint8_t buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
};

static struct protocol_fixture fixture;

/* Setup function - runs before each test */
static void *protocol_setup(void)
{
	memset(&fixture, 0, sizeof(fixture));
	return &fixture;
}

/* Teardown function - runs after each test */
static void protocol_teardown(void *f)
{
	/* No cleanup needed for now */
}

ZTEST_SUITE(ninep_protocol, NULL, protocol_setup, NULL, NULL, protocol_teardown);

ZTEST(ninep_protocol, test_header_parse)
{
	uint8_t buf[] = {
		0x13, 0x00, 0x00, 0x00,  /* size = 19 */
		0x64,                     /* type = Tversion (100) */
		0x01, 0x00,              /* tag = 1 */
	};
	struct ninep_msg_header hdr;

	zassert_equal(ninep_parse_header(buf, sizeof(buf), &hdr), 0,
	              "Failed to parse header");
	zassert_equal(hdr.size, 19, "Wrong size");
	zassert_equal(hdr.type, NINEP_TVERSION, "Wrong type");
	zassert_equal(hdr.tag, 1, "Wrong tag");
}

ZTEST(ninep_protocol, test_header_write)
{
	uint8_t buf[7];
	struct ninep_msg_header hdr = {
		.size = 19,
		.type = NINEP_TVERSION,
		.tag = 1,
	};

	zassert_equal(ninep_write_header(buf, sizeof(buf), &hdr), 7,
	              "Failed to write header");
	zassert_equal(buf[0], 0x13, "Wrong size byte 0");
	zassert_equal(buf[4], 0x64, "Wrong type");
	zassert_equal(buf[5], 0x01, "Wrong tag byte 0");
}

ZTEST(ninep_protocol, test_string_parse)
{
	uint8_t buf[] = {
		0x07, 0x00,              /* length = 7 */
		'9', 'P', '2', '0', '0', '0', '\0'
	};
	size_t offset = 0;
	const char *str;
	uint16_t str_len;

	zassert_equal(ninep_parse_string(buf, sizeof(buf), &offset, &str, &str_len),
	              0, "Failed to parse string");
	zassert_equal(str_len, 7, "Wrong string length");
	zassert_mem_equal(str, "9P2000", 6, "Wrong string content");
	zassert_equal(offset, 9, "Wrong offset");
}

ZTEST(ninep_protocol, test_string_write)
{
	uint8_t buf[16];
	size_t offset = 0;
	const char *str = "9P2000";

	zassert_equal(ninep_write_string(buf, sizeof(buf), &offset, str, 6),
	              0, "Failed to write string");
	zassert_equal(buf[0], 0x06, "Wrong length byte 0");
	zassert_equal(buf[1], 0x00, "Wrong length byte 1");
	zassert_mem_equal(&buf[2], "9P2000", 6, "Wrong string content");
	zassert_equal(offset, 8, "Wrong offset");
}

ZTEST(ninep_protocol, test_qid_parse)
{
	uint8_t buf[] = {
		0x80,                               /* type = QTDIR */
		0x01, 0x00, 0x00, 0x00,            /* version = 1 */
		0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* path = 0x42 */
	};
	size_t offset = 0;
	struct ninep_qid qid;

	zassert_equal(ninep_parse_qid(buf, sizeof(buf), &offset, &qid),
	              0, "Failed to parse qid");
	zassert_equal(qid.type, NINEP_QTDIR, "Wrong type");
	zassert_equal(qid.version, 1, "Wrong version");
	zassert_equal(qid.path, 0x42, "Wrong path");
	zassert_equal(offset, 13, "Wrong offset");
}

ZTEST(ninep_protocol, test_qid_write)
{
	uint8_t buf[13];
	size_t offset = 0;
	struct ninep_qid qid = {
		.type = NINEP_QTDIR,
		.version = 1,
		.path = 0x42,
	};

	zassert_equal(ninep_write_qid(buf, sizeof(buf), &offset, &qid),
	              0, "Failed to write qid");
	zassert_equal(buf[0], NINEP_QTDIR, "Wrong type");
	zassert_equal(buf[1], 0x01, "Wrong version byte 0");
	zassert_equal(buf[5], 0x42, "Wrong path byte 0");
	zassert_equal(offset, 13, "Wrong offset");
}

/* Error handling tests */
ZTEST(ninep_protocol, test_header_parse_invalid_size)
{
	uint8_t buf[5] = {0};  /* Too small for header */
	struct ninep_msg_header hdr;

	zassert_not_equal(ninep_parse_header(buf, sizeof(buf), &hdr), 0,
	                  "Should fail with buffer too small");
}

ZTEST(ninep_protocol, test_header_parse_null_params)
{
	uint8_t buf[7] = {0x13, 0x00, 0x00, 0x00, 0x64, 0x01, 0x00};
	struct ninep_msg_header hdr;

	zassert_not_equal(ninep_parse_header(NULL, sizeof(buf), &hdr), 0,
	                  "Should fail with NULL buffer");
	zassert_not_equal(ninep_parse_header(buf, sizeof(buf), NULL), 0,
	                  "Should fail with NULL header");
}

ZTEST(ninep_protocol, test_header_parse_invalid_message_size)
{
	uint8_t buf[] = {
		0x03, 0x00, 0x00, 0x00,  /* size = 3 (less than header size) */
		0x64,
		0x01, 0x00,
	};
	struct ninep_msg_header hdr;

	zassert_not_equal(ninep_parse_header(buf, sizeof(buf), &hdr), 0,
	                  "Should fail with invalid message size");
}

ZTEST(ninep_protocol, test_string_parse_overflow)
{
	uint8_t buf[] = {
		0xFF, 0xFF,  /* length = 65535 (way too large) */
		'X'
	};
	size_t offset = 0;
	const char *str;
	uint16_t str_len;

	zassert_not_equal(ninep_parse_string(buf, sizeof(buf), &offset, &str, &str_len),
	                  0, "Should fail with string overflow");
}

ZTEST(ninep_protocol, test_string_write_empty)
{
	uint8_t buf[16];
	size_t offset = 0;

	zassert_equal(ninep_write_string(buf, sizeof(buf), &offset, NULL, 0),
	              0, "Should succeed with empty string");
	zassert_equal(buf[0], 0x00, "Length should be 0");
	zassert_equal(buf[1], 0x00, "Length should be 0");
	zassert_equal(offset, 2, "Offset should be 2");
}

ZTEST(ninep_protocol, test_roundtrip_header)
{
	uint8_t buf[7];
	struct ninep_msg_header hdr_out = {
		.size = 1234,
		.type = NINEP_TWALK,
		.tag = 42,
	};
	struct ninep_msg_header hdr_in;

	zassert_equal(ninep_write_header(buf, sizeof(buf), &hdr_out), 7,
	              "Failed to write header");
	zassert_equal(ninep_parse_header(buf, sizeof(buf), &hdr_in), 0,
	              "Failed to parse header");

	zassert_equal(hdr_in.size, hdr_out.size, "Size mismatch");
	zassert_equal(hdr_in.type, hdr_out.type, "Type mismatch");
	zassert_equal(hdr_in.tag, hdr_out.tag, "Tag mismatch");
}

ZTEST(ninep_protocol, test_roundtrip_qid)
{
	uint8_t buf[13];
	size_t offset_out = 0, offset_in = 0;
	struct ninep_qid qid_out = {
		.type = NINEP_QTFILE,
		.version = 0x12345678,
		.path = 0xDEADBEEFCAFEBABE,
	};
	struct ninep_qid qid_in;

	zassert_equal(ninep_write_qid(buf, sizeof(buf), &offset_out, &qid_out),
	              0, "Failed to write qid");
	zassert_equal(ninep_parse_qid(buf, sizeof(buf), &offset_in, &qid_in),
	              0, "Failed to parse qid");

	zassert_equal(qid_in.type, qid_out.type, "Type mismatch");
	zassert_equal(qid_in.version, qid_out.version, "Version mismatch");
	zassert_equal(qid_in.path, qid_out.path, "Path mismatch");
}