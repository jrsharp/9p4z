/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/message.h>
#include <zephyr/9p/protocol.h>
#include <string.h>

static uint8_t test_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];

static void *builder_setup(void)
{
	memset(test_buffer, 0, sizeof(test_buffer));
	return test_buffer;
}

ZTEST_SUITE(ninep_message_builder, NULL, builder_setup, NULL, NULL, NULL);

ZTEST(ninep_message_builder, test_build_tversion)
{
	int ret = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                NINEP_NOTAG, 8192, "9P2000", 6);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 19, "Wrong message size");

	/* Verify we can parse it */
	struct ninep_msg_header hdr;
	zassert_equal(ninep_parse_header(test_buffer, ret, &hdr), 0, "Parse failed");
	zassert_equal(hdr.type, NINEP_TVERSION, "Wrong type");
	zassert_equal(hdr.tag, NINEP_NOTAG, "Wrong tag");
	zassert_equal(hdr.size, 19, "Wrong size");
}

ZTEST(ninep_message_builder, test_build_rversion)
{
	int ret = ninep_build_rversion(test_buffer, sizeof(test_buffer),
	                                NINEP_NOTAG, 4096, "9P2000", 6);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 19, "Wrong message size");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_RVERSION, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_tattach)
{
	int ret = ninep_build_tattach(test_buffer, sizeof(test_buffer),
	                               1, 0, NINEP_NOFID,
	                               "glenda", 6, "", 0);
	zassert_true(ret > 0, "Build failed");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_TATTACH, "Wrong type");
	zassert_equal(hdr.tag, 1, "Wrong tag");
}

ZTEST(ninep_message_builder, test_build_rattach)
{
	struct ninep_qid qid = {
		.type = NINEP_QTDIR,
		.version = 0,
		.path = 1,
	};

	int ret = ninep_build_rattach(test_buffer, sizeof(test_buffer), 1, &qid);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 20, "Wrong message size");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_RATTACH, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_twalk)
{
	const char *wnames[] = {"usr", "glenda"};
	const uint16_t wname_lens[] = {3, 6};

	int ret = ninep_build_twalk(test_buffer, sizeof(test_buffer),
	                             2, 1, 2, 2, wnames, wname_lens);
	zassert_true(ret > 0, "Build failed");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_TWALK, "Wrong type");
	zassert_equal(hdr.tag, 2, "Wrong tag");
}

ZTEST(ninep_message_builder, test_build_rwalk)
{
	struct ninep_qid qids[] = {
		{.type = NINEP_QTDIR, .version = 0, .path = 10},
		{.type = NINEP_QTDIR, .version = 0, .path = 20},
	};

	int ret = ninep_build_rwalk(test_buffer, sizeof(test_buffer), 2, 2, qids);
	zassert_true(ret > 0, "Build failed");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_RWALK, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_topen)
{
	int ret = ninep_build_topen(test_buffer, sizeof(test_buffer),
	                             3, 1, NINEP_OREAD);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 12, "Wrong message size");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_TOPEN, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_ropen)
{
	struct ninep_qid qid = {
		.type = NINEP_QTFILE,
		.version = 0,
		.path = 42,
	};

	int ret = ninep_build_ropen(test_buffer, sizeof(test_buffer), 3, &qid, 8192);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 24, "Wrong message size");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_ROPEN, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_tclunk)
{
	int ret = ninep_build_tclunk(test_buffer, sizeof(test_buffer), 4, 1);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 11, "Wrong message size");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_TCLUNK, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_rclunk)
{
	int ret = ninep_build_rclunk(test_buffer, sizeof(test_buffer), 4);
	zassert_true(ret > 0, "Build failed");
	zassert_equal(ret, 7, "Wrong message size");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_RCLUNK, "Wrong type");
}

ZTEST(ninep_message_builder, test_build_rerror)
{
	int ret = ninep_build_rerror(test_buffer, sizeof(test_buffer),
	                              5, "file not found", 14);
	zassert_true(ret > 0, "Build failed");

	struct ninep_msg_header hdr;
	ninep_parse_header(test_buffer, ret, &hdr);
	zassert_equal(hdr.type, NINEP_RERROR, "Wrong type");
	zassert_equal(hdr.tag, 5, "Wrong tag");
}

ZTEST(ninep_message_builder, test_builder_null_checks)
{
	zassert_true(ninep_build_tversion(NULL, 100, 0, 8192, "9P2000", 6) < 0,
	             "Should fail with NULL buffer");
	zassert_true(ninep_build_rattach(test_buffer, 100, 1, NULL) < 0,
	             "Should fail with NULL qid");
	zassert_true(ninep_build_ropen(test_buffer, 100, 1, NULL, 8192) < 0,
	             "Should fail with NULL qid");
}

ZTEST(ninep_message_builder, test_builder_buffer_too_small)
{
	uint8_t small_buf[5];

	zassert_true(ninep_build_tversion(small_buf, sizeof(small_buf),
	                                   0, 8192, "9P2000", 6) < 0,
	             "Should fail with small buffer");
}

ZTEST(ninep_message_builder, test_build_twalk_max_elements)
{
	const char *wnames[NINEP_MAX_WELEM];
	uint16_t wname_lens[NINEP_MAX_WELEM];

	/* Fill with max elements */
	for (int i = 0; i < NINEP_MAX_WELEM; i++) {
		wnames[i] = "x";
		wname_lens[i] = 1;
	}

	int ret = ninep_build_twalk(test_buffer, sizeof(test_buffer),
	                             1, 1, 2, NINEP_MAX_WELEM, wnames, wname_lens);
	zassert_true(ret > 0, "Should succeed with max elements");

	/* Try with one more than max */
	ret = ninep_build_twalk(test_buffer, sizeof(test_buffer),
	                        1, 1, 2, NINEP_MAX_WELEM + 1, wnames, wname_lens);
	zassert_true(ret < 0, "Should fail with too many elements");
}

ZTEST(ninep_message_builder, test_version_negotiation_roundtrip)
{
	/* Client builds Tversion */
	int ret = ninep_build_tversion(test_buffer, sizeof(test_buffer),
	                                NINEP_NOTAG, 8192, "9P2000", 6);
	zassert_true(ret > 0, "Tversion build failed");

	uint8_t *response_buf = test_buffer + 100;  /* Use different part of buffer */

	/* Server builds Rversion */
	ret = ninep_build_rversion(response_buf, sizeof(test_buffer) - 100,
	                            NINEP_NOTAG, 4096, "9P2000", 6);
	zassert_true(ret > 0, "Rversion build failed");

	/* Verify both messages */
	struct ninep_msg_header thdr, rhdr;
	ninep_parse_header(test_buffer, 19, &thdr);
	ninep_parse_header(response_buf, 19, &rhdr);

	zassert_equal(thdr.type, NINEP_TVERSION, "Wrong Tversion type");
	zassert_equal(rhdr.type, NINEP_RVERSION, "Wrong Rversion type");
	zassert_equal(thdr.tag, NINEP_NOTAG, "Tversion should use NOTAG");
	zassert_equal(rhdr.tag, NINEP_NOTAG, "Rversion should use NOTAG");
}
