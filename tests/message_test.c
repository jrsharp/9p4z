/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/protocol.h>
#include <string.h>

/* Test fixture data */
struct message_fixture {
	uint8_t buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
};

static struct message_fixture fixture;

static void *message_setup(void)
{
	memset(&fixture, 0, sizeof(fixture));
	return &fixture;
}

ZTEST_SUITE(ninep_message, NULL, message_setup, NULL, NULL, NULL);

/* Helper to write uint32 at offset */
static void write_u32(uint8_t *buf, size_t *offset, uint32_t val)
{
	buf[(*offset)++] = val & 0xff;
	buf[(*offset)++] = (val >> 8) & 0xff;
	buf[(*offset)++] = (val >> 16) & 0xff;
	buf[(*offset)++] = (val >> 24) & 0xff;
}

/* Helper to read uint32 at offset */
static uint32_t read_u32(const uint8_t *buf, size_t *offset)
{
	uint32_t val = buf[*offset] |
	               (buf[*offset + 1] << 8) |
	               (buf[*offset + 2] << 16) |
	               (buf[*offset + 3] << 24);
	*offset += 4;
	return val;
}

ZTEST(ninep_message, test_tversion_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint32_t msize = 8192;
	const char *version = "9P2000";
	uint16_t version_len = 6;

	/* Calculate total message size */
	uint32_t msg_size = 7 + 4 + 2 + version_len; /* header + msize + strlen + string */

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TVERSION,
		.tag = NINEP_NOTAG,
	};

	/* Write header */
	zassert_equal(ninep_write_header(buf, sizeof(fixture.buffer), &hdr), 7,
	              "Failed to write header");
	offset = 7;

	/* Write msize */
	write_u32(buf, &offset, msize);

	/* Write version string */
	zassert_equal(ninep_write_string(buf, sizeof(fixture.buffer), &offset,
	                                  version, version_len), 0,
	              "Failed to write version string");

	/* Verify total size */
	zassert_equal(offset, msg_size, "Wrong total message size");

	/* Verify we can parse it back */
	struct ninep_msg_header parsed_hdr;
	zassert_equal(ninep_parse_header(buf, offset, &parsed_hdr), 0,
	              "Failed to parse header");
	zassert_equal(parsed_hdr.size, msg_size, "Wrong parsed size");
	zassert_equal(parsed_hdr.type, NINEP_TVERSION, "Wrong parsed type");
	zassert_equal(parsed_hdr.tag, NINEP_NOTAG, "Wrong parsed tag");

	/* Parse msize */
	offset = 7;
	uint32_t parsed_msize = read_u32(buf, &offset);
	zassert_equal(parsed_msize, msize, "Wrong parsed msize");

	/* Parse version string */
	const char *parsed_version;
	uint16_t parsed_version_len;
	zassert_equal(ninep_parse_string(buf, msg_size, &offset,
	                                  &parsed_version, &parsed_version_len), 0,
	              "Failed to parse version string");
	zassert_equal(parsed_version_len, version_len, "Wrong version length");
	zassert_mem_equal(parsed_version, version, version_len,
	                  "Wrong version string");
}

ZTEST(ninep_message, test_rversion_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint32_t msize = 8192;
	const char *version = "9P2000";
	uint16_t version_len = 6;

	uint32_t msg_size = 7 + 4 + 2 + version_len;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RVERSION,
		.tag = NINEP_NOTAG,
	};

	/* Write header */
	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	/* Write msize */
	write_u32(buf, &offset, msize);

	/* Write version string */
	ninep_write_string(buf, sizeof(fixture.buffer), &offset, version, version_len);

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, offset, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_RVERSION, "Wrong type");

	offset = 7;
	uint32_t parsed_msize = read_u32(buf, &offset);
	zassert_equal(parsed_msize, 8192, "Wrong msize");

	const char *parsed_version;
	uint16_t parsed_version_len;
	ninep_parse_string(buf, msg_size, &offset, &parsed_version, &parsed_version_len);
	zassert_mem_equal(parsed_version, "9P2000", 6, "Wrong version");
}

ZTEST(ninep_message, test_tattach_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint32_t fid = 0;
	uint32_t afid = NINEP_NOFID;
	const char *uname = "glenda";
	const char *aname = "";
	uint16_t uname_len = 6;
	uint16_t aname_len = 0;

	/* size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] */
	uint32_t msg_size = 7 + 4 + 4 + 2 + uname_len + 2 + aname_len;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TATTACH,
		.tag = 1,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	/* Write fid */
	write_u32(buf, &offset, fid);

	/* Write afid */
	write_u32(buf, &offset, afid);

	/* Write uname */
	ninep_write_string(buf, sizeof(fixture.buffer), &offset, uname, uname_len);

	/* Write aname */
	ninep_write_string(buf, sizeof(fixture.buffer), &offset, aname, aname_len);

	zassert_equal(offset, msg_size, "Wrong message size");

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_TATTACH, "Wrong type");
	zassert_equal(parsed_hdr.tag, 1, "Wrong tag");

	offset = 7;
	uint32_t parsed_fid = read_u32(buf, &offset);
	uint32_t parsed_afid = read_u32(buf, &offset);
	zassert_equal(parsed_fid, 0, "Wrong fid");
	zassert_equal(parsed_afid, NINEP_NOFID, "Wrong afid");

	const char *parsed_uname;
	uint16_t parsed_uname_len;
	ninep_parse_string(buf, msg_size, &offset, &parsed_uname, &parsed_uname_len);
	zassert_equal(parsed_uname_len, 6, "Wrong uname length");
	zassert_mem_equal(parsed_uname, "glenda", 6, "Wrong uname");

	const char *parsed_aname;
	uint16_t parsed_aname_len;
	ninep_parse_string(buf, msg_size, &offset, &parsed_aname, &parsed_aname_len);
	zassert_equal(parsed_aname_len, 0, "Wrong aname length");
}

ZTEST(ninep_message, test_rattach_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;

	/* size[4] Rattach tag[2] qid[13] */
	uint32_t msg_size = 7 + 13;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RATTACH,
		.tag = 1,
	};

	struct ninep_qid qid = {
		.type = NINEP_QTDIR,
		.version = 0,
		.path = 1,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	ninep_write_qid(buf, sizeof(fixture.buffer), &offset, &qid);

	zassert_equal(offset, msg_size, "Wrong message size");

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_RATTACH, "Wrong type");

	offset = 7;
	struct ninep_qid parsed_qid;
	ninep_parse_qid(buf, msg_size, &offset, &parsed_qid);
	zassert_equal(parsed_qid.type, NINEP_QTDIR, "Wrong qid type");
	zassert_equal(parsed_qid.version, 0, "Wrong qid version");
	zassert_equal(parsed_qid.path, 1, "Wrong qid path");
}

ZTEST(ninep_message, test_twalk_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint32_t fid = 1;
	uint32_t newfid = 2;
	uint16_t nwname = 2;
	const char *wnames[] = {"usr", "glenda"};
	uint16_t wname_lens[] = {3, 6};

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	uint32_t msg_size = 7 + 4 + 4 + 2;
	for (int i = 0; i < nwname; i++) {
		msg_size += 2 + wname_lens[i];
	}

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TWALK,
		.tag = 2,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	write_u32(buf, &offset, fid);
	write_u32(buf, &offset, newfid);

	/* Write nwname */
	buf[offset++] = nwname & 0xff;
	buf[offset++] = (nwname >> 8) & 0xff;

	/* Write each path element */
	for (int i = 0; i < nwname; i++) {
		ninep_write_string(buf, sizeof(fixture.buffer), &offset,
		                   wnames[i], wname_lens[i]);
	}

	zassert_equal(offset, msg_size, "Wrong message size");

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_TWALK, "Wrong type");
	zassert_equal(parsed_hdr.tag, 2, "Wrong tag");
}

ZTEST(ninep_message, test_rwalk_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint16_t nwqid = 2;
	struct ninep_qid qids[] = {
		{.type = NINEP_QTDIR, .version = 0, .path = 10},
		{.type = NINEP_QTDIR, .version = 0, .path = 20},
	};

	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(qid[13]) */
	uint32_t msg_size = 7 + 2 + (nwqid * 13);

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RWALK,
		.tag = 2,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	/* Write nwqid */
	buf[offset++] = nwqid & 0xff;
	buf[offset++] = (nwqid >> 8) & 0xff;

	/* Write qids */
	for (int i = 0; i < nwqid; i++) {
		ninep_write_qid(buf, sizeof(fixture.buffer), &offset, &qids[i]);
	}

	zassert_equal(offset, msg_size, "Wrong message size");

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_RWALK, "Wrong type");

	offset = 7;
	uint16_t parsed_nwqid = buf[offset] | (buf[offset + 1] << 8);
	offset += 2;
	zassert_equal(parsed_nwqid, 2, "Wrong nwqid");

	for (int i = 0; i < nwqid; i++) {
		struct ninep_qid parsed_qid;
		ninep_parse_qid(buf, msg_size, &offset, &parsed_qid);
		zassert_equal(parsed_qid.path, qids[i].path, "Wrong qid path");
	}
}

ZTEST(ninep_message, test_topen_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint32_t fid = 1;
	uint8_t mode = NINEP_OREAD;

	/* size[4] Topen tag[2] fid[4] mode[1] */
	uint32_t msg_size = 7 + 4 + 1;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TOPEN,
		.tag = 3,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	write_u32(buf, &offset, fid);
	buf[offset++] = mode;

	zassert_equal(offset, msg_size, "Wrong message size");
}

ZTEST(ninep_message, test_ropen_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;

	/* size[4] Ropen tag[2] qid[13] iounit[4] */
	uint32_t msg_size = 7 + 13 + 4;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_ROPEN,
		.tag = 3,
	};

	struct ninep_qid qid = {
		.type = NINEP_QTFILE,
		.version = 0,
		.path = 42,
	};

	uint32_t iounit = 8192;

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	ninep_write_qid(buf, sizeof(fixture.buffer), &offset, &qid);
	write_u32(buf, &offset, iounit);

	zassert_equal(offset, msg_size, "Wrong message size");
}

ZTEST(ninep_message, test_tclunk_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	uint32_t fid = 1;

	/* size[4] Tclunk tag[2] fid[4] */
	uint32_t msg_size = 7 + 4;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TCLUNK,
		.tag = 4,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	write_u32(buf, &offset, fid);

	zassert_equal(offset, msg_size, "Wrong message size");

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_TCLUNK, "Wrong type");
	zassert_equal(parsed_hdr.tag, 4, "Wrong tag");

	offset = 7;
	uint32_t parsed_fid = read_u32(buf, &offset);
	zassert_equal(parsed_fid, 1, "Wrong fid");
}

ZTEST(ninep_message, test_rclunk_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;

	/* size[4] Rclunk tag[2] */
	uint32_t msg_size = 7;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RCLUNK,
		.tag = 4,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	zassert_equal(offset, msg_size, "Wrong message size");
}

ZTEST(ninep_message, test_rerror_build)
{
	uint8_t *buf = fixture.buffer;
	size_t offset = 0;
	const char *ename = "file not found";
	uint16_t ename_len = 14;

	/* size[4] Rerror tag[2] ename[s] */
	uint32_t msg_size = 7 + 2 + ename_len;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RERROR,
		.tag = 5,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &hdr);
	offset = 7;

	ninep_write_string(buf, sizeof(fixture.buffer), &offset, ename, ename_len);

	zassert_equal(offset, msg_size, "Wrong message size");

	/* Parse and verify */
	struct ninep_msg_header parsed_hdr;
	ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(parsed_hdr.type, NINEP_RERROR, "Wrong type");

	offset = 7;
	const char *parsed_ename;
	uint16_t parsed_ename_len;
	ninep_parse_string(buf, msg_size, &offset, &parsed_ename, &parsed_ename_len);
	zassert_equal(parsed_ename_len, 14, "Wrong ename length");
	zassert_mem_equal(parsed_ename, "file not found", 14, "Wrong ename");
}

ZTEST(ninep_message, test_max_message_size)
{
	uint8_t *buf = fixture.buffer;

	/* Verify fixture buffer is large enough */
	zassert_true(sizeof(fixture.buffer) >= CONFIG_NINEP_MAX_MESSAGE_SIZE,
	             "Test buffer too small");

	/* Try to build a message at max size */
	uint32_t msg_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;

	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TWRITE,
		.tag = 1,
	};

	int ret = ninep_write_header(buf, msg_size, &hdr);
	zassert_equal(ret, 7, "Should succeed at max size");

	/* Parse it back */
	struct ninep_msg_header parsed_hdr;
	ret = ninep_parse_header(buf, msg_size, &parsed_hdr);
	zassert_equal(ret, 0, "Should parse at max size");
	zassert_equal(parsed_hdr.size, msg_size, "Wrong size");
}

ZTEST(ninep_message, test_version_negotiation_sequence)
{
	uint8_t *buf = fixture.buffer;
	size_t offset;

	/* Client sends Tversion with msize=8192 */
	struct ninep_msg_header tversion_hdr = {
		.size = 19,
		.type = NINEP_TVERSION,
		.tag = NINEP_NOTAG,
	};

	ninep_write_header(buf, sizeof(fixture.buffer), &tversion_hdr);
	offset = 7;
	write_u32(buf, &offset, 8192);
	ninep_write_string(buf, sizeof(fixture.buffer), &offset, "9P2000", 6);

	/* Verify Tversion is well-formed */
	struct ninep_msg_header parsed_thdr;
	ninep_parse_header(buf, 19, &parsed_thdr);
	zassert_equal(parsed_thdr.type, NINEP_TVERSION, "Wrong Tversion type");
	zassert_equal(parsed_thdr.tag, NINEP_NOTAG, "Tversion should use NOTAG");

	/* Server responds with Rversion (smaller msize=4096) */
	struct ninep_msg_header rversion_hdr = {
		.size = 19,
		.type = NINEP_RVERSION,
		.tag = NINEP_NOTAG,
	};

	memset(buf, 0, sizeof(fixture.buffer));
	ninep_write_header(buf, sizeof(fixture.buffer), &rversion_hdr);
	offset = 7;
	write_u32(buf, &offset, 4096);  /* Server negotiates smaller */
	ninep_write_string(buf, sizeof(fixture.buffer), &offset, "9P2000", 6);

	/* Verify Rversion */
	struct ninep_msg_header parsed_rhdr;
	ninep_parse_header(buf, 19, &parsed_rhdr);
	zassert_equal(parsed_rhdr.type, NINEP_RVERSION, "Wrong Rversion type");
	zassert_equal(parsed_rhdr.tag, NINEP_NOTAG, "Rversion should use NOTAG");

	offset = 7;
	uint32_t negotiated_msize = read_u32(buf, &offset);
	zassert_equal(negotiated_msize, 4096, "Should negotiate smaller msize");
}
