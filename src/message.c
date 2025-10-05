/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/9p/message.h>
#include <zephyr/9p/protocol.h>
#include <string.h>
#include <errno.h>

/* Helper to write uint64 in little-endian */
static void write_u64_le(uint8_t *buf, size_t *offset, uint64_t val)
{
	buf[(*offset)++] = val & 0xff;
	buf[(*offset)++] = (val >> 8) & 0xff;
	buf[(*offset)++] = (val >> 16) & 0xff;
	buf[(*offset)++] = (val >> 24) & 0xff;
	buf[(*offset)++] = (val >> 32) & 0xff;
	buf[(*offset)++] = (val >> 40) & 0xff;
	buf[(*offset)++] = (val >> 48) & 0xff;
	buf[(*offset)++] = (val >> 56) & 0xff;
}

/* Helper to write uint32 in little-endian */
static void write_u32_le(uint8_t *buf, size_t *offset, uint32_t val)
{
	buf[(*offset)++] = val & 0xff;
	buf[(*offset)++] = (val >> 8) & 0xff;
	buf[(*offset)++] = (val >> 16) & 0xff;
	buf[(*offset)++] = (val >> 24) & 0xff;
}

/* Helper to write uint16 in little-endian */
static void write_u16_le(uint8_t *buf, size_t *offset, uint16_t val)
{
	buf[(*offset)++] = val & 0xff;
	buf[(*offset)++] = (val >> 8) & 0xff;
}

int ninep_build_tversion(uint8_t *buf, size_t buf_len, uint16_t tag,
                         uint32_t msize, const char *version, uint16_t version_len)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 2 + version_len;
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TVERSION,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, msize);
	ret = ninep_write_string(buf, buf_len, &offset, version, version_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

int ninep_build_rversion(uint8_t *buf, size_t buf_len, uint16_t tag,
                         uint32_t msize, const char *version, uint16_t version_len)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 2 + version_len;
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RVERSION,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, msize);
	ret = ninep_write_string(buf, buf_len, &offset, version, version_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

int ninep_build_tattach(uint8_t *buf, size_t buf_len, uint16_t tag,
                        uint32_t fid, uint32_t afid,
                        const char *uname, uint16_t uname_len,
                        const char *aname, uint16_t aname_len)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 4 + 2 + uname_len + 2 + aname_len;
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TATTACH,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, fid);
	write_u32_le(buf, &offset, afid);

	ret = ninep_write_string(buf, buf_len, &offset, uname, uname_len);
	if (ret < 0) {
		return ret;
	}

	ret = ninep_write_string(buf, buf_len, &offset, aname, aname_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

int ninep_build_rattach(uint8_t *buf, size_t buf_len, uint16_t tag,
                        const struct ninep_qid *qid)
{
	if (!buf || !qid || buf_len < 20) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 13;

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RATTACH,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	ret = ninep_write_qid(buf, buf_len, &offset, qid);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

int ninep_build_twalk(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint32_t fid, uint32_t newfid,
                      uint16_t nwname, const char **wnames, const uint16_t *wname_lens)
{
	if (!buf || buf_len < 7 || nwname > NINEP_MAX_WELEM) {
		return -EINVAL;
	}

	if (nwname > 0 && (!wnames || !wname_lens)) {
		return -EINVAL;
	}

	/* Calculate total size */
	uint32_t msg_size = 7 + 4 + 4 + 2;
	for (uint16_t i = 0; i < nwname; i++) {
		msg_size += 2 + wname_lens[i];
	}

	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TWALK,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, fid);
	write_u32_le(buf, &offset, newfid);
	write_u16_le(buf, &offset, nwname);

	for (uint16_t i = 0; i < nwname; i++) {
		ret = ninep_write_string(buf, buf_len, &offset,
		                         wnames[i], wname_lens[i]);
		if (ret < 0) {
			return ret;
		}
	}

	return offset;
}

int ninep_build_rwalk(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint16_t nwqid, const struct ninep_qid *wqids)
{
	if (!buf || buf_len < 7 || nwqid > NINEP_MAX_WELEM) {
		return -EINVAL;
	}

	if (nwqid > 0 && !wqids) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 2 + (nwqid * 13);
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RWALK,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u16_le(buf, &offset, nwqid);

	for (uint16_t i = 0; i < nwqid; i++) {
		ret = ninep_write_qid(buf, buf_len, &offset, &wqids[i]);
		if (ret < 0) {
			return ret;
		}
	}

	return offset;
}

int ninep_build_topen(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint32_t fid, uint8_t mode)
{
	if (!buf || buf_len < 12) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 1;

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TOPEN,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, fid);
	buf[offset++] = mode;

	return offset;
}

int ninep_build_ropen(uint8_t *buf, size_t buf_len, uint16_t tag,
                      const struct ninep_qid *qid, uint32_t iounit)
{
	if (!buf || !qid || buf_len < 24) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 13 + 4;

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_ROPEN,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	ret = ninep_write_qid(buf, buf_len, &offset, qid);
	if (ret < 0) {
		return ret;
	}

	write_u32_le(buf, &offset, iounit);

	return offset;
}

int ninep_build_tclunk(uint8_t *buf, size_t buf_len, uint16_t tag, uint32_t fid)
{
	if (!buf || buf_len < 11) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4;

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TCLUNK,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, fid);

	return offset;
}

int ninep_build_rclunk(uint8_t *buf, size_t buf_len, uint16_t tag)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7;

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RCLUNK,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	return offset;
}

int ninep_build_tread(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint32_t fid, uint64_t offset, uint32_t count)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 8 + 4;  /* header + fid + offset + count */
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t pos = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TREAD,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	pos = 7;

	write_u32_le(buf, &pos, fid);
	write_u64_le(buf, &pos, offset);
	write_u32_le(buf, &pos, count);

	return pos;
}

int ninep_build_tstat(uint8_t *buf, size_t buf_len, uint16_t tag, uint32_t fid)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4;  /* header + fid */
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t pos = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TSTAT,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	pos = 7;

	write_u32_le(buf, &pos, fid);

	return pos;
}

int ninep_build_twrite(uint8_t *buf, size_t buf_len, uint16_t tag,
                       uint32_t fid, uint64_t offset, uint32_t count,
                       const uint8_t *data)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 8 + 4 + count;  /* header + fid + offset + count + data */
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t pos = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TWRITE,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	pos = 7;

	write_u32_le(buf, &pos, fid);
	write_u64_le(buf, &pos, offset);
	write_u32_le(buf, &pos, count);

	/* Write data */
	if (count > 0 && data) {
		memcpy(&buf[pos], data, count);
		pos += count;
	}

	return pos;
}

int ninep_build_tcreate(uint8_t *buf, size_t buf_len, uint16_t tag,
                        uint32_t fid, const char *name, uint16_t name_len,
                        uint32_t perm, uint8_t mode)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4 + 2 + name_len + 4 + 1;
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TCREATE,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, fid);

	ret = ninep_write_string(buf, buf_len, &offset, name, name_len);
	if (ret < 0) {
		return ret;
	}

	write_u32_le(buf, &offset, perm);
	buf[offset++] = mode;

	return offset;
}

int ninep_build_tremove(uint8_t *buf, size_t buf_len, uint16_t tag, uint32_t fid)
{
	if (!buf || buf_len < 11) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 4;

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_TREMOVE,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	write_u32_le(buf, &offset, fid);

	return offset;
}

int ninep_build_rerror(uint8_t *buf, size_t buf_len, uint16_t tag,
                       const char *ename, uint16_t ename_len)
{
	if (!buf || buf_len < 7) {
		return -EINVAL;
	}

	uint32_t msg_size = 7 + 2 + ename_len;
	if (buf_len < msg_size) {
		return -ENOSPC;
	}

	size_t offset = 0;
	struct ninep_msg_header hdr = {
		.size = msg_size,
		.type = NINEP_RERROR,
		.tag = tag,
	};

	int ret = ninep_write_header(buf, buf_len, &hdr);
	if (ret < 0) {
		return ret;
	}
	offset = 7;

	ret = ninep_write_string(buf, buf_len, &offset, ename, ename_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}
