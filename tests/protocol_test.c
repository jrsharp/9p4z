/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/protocol.h>
#include <string.h>

ZTEST_SUITE(ninep_protocol, NULL, NULL, NULL, NULL, NULL);

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