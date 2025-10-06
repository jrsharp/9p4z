/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/9p/protocol.h>
#include <string.h>
#include <errno.h>

/* Helper macros for reading/writing little-endian values */
#define GET_U8(buf)  (*(uint8_t *)(buf))
#define GET_U16(buf) ((uint16_t)(GET_U8(buf)) | ((uint16_t)(GET_U8((buf)+1)) << 8))
#define GET_U32(buf) ((uint32_t)(GET_U16(buf)) | ((uint32_t)(GET_U16((buf)+2)) << 16))
#define GET_U64(buf) ((uint64_t)(GET_U32(buf)) | ((uint64_t)(GET_U32((buf)+4)) << 32))

#define PUT_U8(buf, val)  do { *(uint8_t *)(buf) = (val); } while (0)
#define PUT_U16(buf, val) do { \
	PUT_U8((buf), (val) & 0xff); \
	PUT_U8((buf)+1, ((val) >> 8) & 0xff); \
} while (0)
#define PUT_U32(buf, val) do { \
	PUT_U16((buf), (val) & 0xffff); \
	PUT_U16((buf)+2, ((val) >> 16) & 0xffff); \
} while (0)
#define PUT_U64(buf, val) do { \
	PUT_U32((buf), (val) & 0xffffffff); \
	PUT_U32((buf)+4, ((val) >> 32) & 0xffffffff); \
} while (0)

int ninep_parse_header(const uint8_t *buf, size_t len,
                       struct ninep_msg_header *hdr)
{
	if (!buf || !hdr) {
		return -EINVAL;
	}

	if (len < 7) {  /* minimum header size */
		return -EINVAL;
	}

	hdr->size = GET_U32(buf);
	hdr->type = GET_U8(buf + 4);
	hdr->tag = GET_U16(buf + 5);

	/* Validate message size */
	if (hdr->size < 7 || hdr->size > CONFIG_NINEP_MAX_MESSAGE_SIZE) {
		return -EINVAL;
	}

	return 0;
}

int ninep_write_header(uint8_t *buf, size_t len,
                       const struct ninep_msg_header *hdr)
{
	if (!buf || !hdr) {
		return -EINVAL;
	}

	if (len < 7) {
		return -EINVAL;
	}

	PUT_U32(buf, hdr->size);
	PUT_U8(buf + 4, hdr->type);
	PUT_U16(buf + 5, hdr->tag);

	return 7;
}

int ninep_parse_string(const uint8_t *buf, size_t len, size_t *offset,
                       const char **str, uint16_t *str_len)
{
	if (!buf || !offset || !str || !str_len) {
		return -EINVAL;
	}

	if (*offset + 2 > len) {
		return -EINVAL;
	}

	*str_len = GET_U16(buf + *offset);
	*offset += 2;

	if (*offset + *str_len > len) {
		return -EINVAL;
	}

	*str = (const char *)(buf + *offset);
	*offset += *str_len;

	return 0;
}

int ninep_write_string(uint8_t *buf, size_t len, size_t *offset,
                       const char *str, uint16_t str_len)
{
	if (!buf || !offset) {
		return -EINVAL;
	}

	if (*offset + 2 + str_len > len) {
		return -EINVAL;
	}

	PUT_U16(buf + *offset, str_len);
	*offset += 2;

	if (str && str_len > 0) {
		memcpy(buf + *offset, str, str_len);
		*offset += str_len;
	}

	return 0;
}

int ninep_parse_qid(const uint8_t *buf, size_t len, size_t *offset,
                    struct ninep_qid *qid)
{
	if (!buf || !offset || !qid) {
		return -EINVAL;
	}

	if (*offset + 13 > len) {  /* qid is 13 bytes */
		return -EINVAL;
	}

	qid->type = GET_U8(buf + *offset);
	qid->version = GET_U32(buf + *offset + 1);
	qid->path = GET_U64(buf + *offset + 5);
	*offset += 13;

	return 0;
}

int ninep_write_qid(uint8_t *buf, size_t len, size_t *offset,
                    const struct ninep_qid *qid)
{
	if (!buf || !offset || !qid) {
		return -EINVAL;
	}

	if (*offset + 13 > len) {
		return -EINVAL;
	}

	PUT_U8(buf + *offset, qid->type);
	PUT_U32(buf + *offset + 1, qid->version);
	PUT_U64(buf + *offset + 5, qid->path);
	*offset += 13;

	return 0;
}

int ninep_write_stat(uint8_t *buf, size_t len, size_t *offset,
                     const struct ninep_qid *qid, uint32_t mode,
                     uint64_t length, const char *name, uint16_t name_len)
{
	if (!buf || !offset || !qid || !name) {
		return -EINVAL;
	}

	/* Calculate stat size (excluding size field itself):
	 * type[2] + dev[4] + qid[13] + mode[4] + atime[4] + mtime[4] +
	 * length[8] + name[2+len] + uid[2+6] + gid[2+6] + muid[2+6]
	 */
	const char *uid = "zephyr";
	const char *gid = "zephyr";
	const char *muid = "zephyr";
	uint16_t uid_len = strlen(uid);
	uint16_t gid_len = strlen(gid);
	uint16_t muid_len = strlen(muid);

	uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
	                     (2 + name_len) + (2 + uid_len) +
	                     (2 + gid_len) + (2 + muid_len);

	/* Check buffer space: need stat_size + 2 (for the size field itself) */
	if (*offset + 2 + stat_size > len) {
		return -ENOSPC;
	}

	/* Write stat size */
	PUT_U16(buf + *offset, stat_size);
	*offset += 2;

	/* Write type and dev (kernel use, set to 0) */
	PUT_U16(buf + *offset, 0);
	*offset += 2;
	PUT_U32(buf + *offset, 0);
	*offset += 4;

	/* Write qid */
	int ret = ninep_write_qid(buf, len, offset, qid);
	if (ret < 0) {
		return ret;
	}

	/* Write mode */
	PUT_U32(buf + *offset, mode);
	*offset += 4;

	/* Write atime and mtime (set to 0 for now) */
	PUT_U32(buf + *offset, 0);
	*offset += 4;
	PUT_U32(buf + *offset, 0);
	*offset += 4;

	/* Write length */
	PUT_U64(buf + *offset, length);
	*offset += 8;

	/* Write strings */
	ret = ninep_write_string(buf, len, offset, name, name_len);
	if (ret < 0) {
		return ret;
	}

	ret = ninep_write_string(buf, len, offset, uid, uid_len);
	if (ret < 0) {
		return ret;
	}

	ret = ninep_write_string(buf, len, offset, gid, gid_len);
	if (ret < 0) {
		return ret;
	}

	ret = ninep_write_string(buf, len, offset, muid, muid_len);
	if (ret < 0) {
		return ret;
	}

	return 0;
}