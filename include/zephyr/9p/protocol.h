/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_PROTOCOL_H_
#define ZEPHYR_INCLUDE_9P_PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_protocol 9P Protocol Definitions
 * @ingroup ninep
 * @{
 */

/* 9P2000 Protocol Version */
#define NINEP_VERSION "9P2000"

/* 9P Message Types */
enum ninep_msg_type {
	NINEP_TVERSION = 100,
	NINEP_RVERSION = 101,
	NINEP_TAUTH    = 102,
	NINEP_RAUTH    = 103,
	NINEP_TATTACH  = 104,
	NINEP_RATTACH  = 105,
	NINEP_TERROR   = 106,  /* illegal */
	NINEP_RERROR   = 107,
	NINEP_TFLUSH   = 108,
	NINEP_RFLUSH   = 109,
	NINEP_TWALK    = 110,
	NINEP_RWALK    = 111,
	NINEP_TOPEN    = 112,
	NINEP_ROPEN    = 113,
	NINEP_TCREATE  = 114,
	NINEP_RCREATE  = 115,
	NINEP_TREAD    = 116,
	NINEP_RREAD    = 117,
	NINEP_TWRITE   = 118,
	NINEP_RWRITE   = 119,
	NINEP_TCLUNK   = 120,
	NINEP_RCLUNK   = 121,
	NINEP_TREMOVE  = 122,
	NINEP_RREMOVE  = 123,
	NINEP_TSTAT    = 124,
	NINEP_RSTAT    = 125,
	NINEP_TWSTAT   = 126,
	NINEP_RWSTAT   = 127,
};

/* 9P Open/Create Modes */
#define NINEP_OREAD   0x00  /* open read-only */
#define NINEP_OWRITE  0x01  /* open write-only */
#define NINEP_ORDWR   0x02  /* open read-write */
#define NINEP_OEXEC   0x03  /* execute (== read but check execute permission) */
#define NINEP_OTRUNC  0x10  /* truncate file */
#define NINEP_OCEXEC  0x20  /* close on exec */
#define NINEP_ORCLOSE 0x40  /* remove on close */

/* 9P Qid Types */
#define NINEP_QTDIR    0x80  /* directory */
#define NINEP_QTAPPEND 0x40  /* append only */
#define NINEP_QTEXCL   0x20  /* exclusive use */
#define NINEP_QTMOUNT  0x10  /* mounted channel */
#define NINEP_QTAUTH   0x08  /* authentication file */
#define NINEP_QTTMP    0x04  /* temporary file (not backed up) */
#define NINEP_QTFILE   0x00  /* regular file */

/* 9P Directory Entry Mode Bits */
#define NINEP_DMDIR    0x80000000  /* directory */
#define NINEP_DMAPPEND 0x40000000  /* append only */
#define NINEP_DMEXCL   0x20000000  /* exclusive use */
#define NINEP_DMMOUNT  0x10000000  /* mounted channel */
#define NINEP_DMAUTH   0x08000000  /* authentication file */
#define NINEP_DMTMP    0x04000000  /* temporary file */
#define NINEP_DMREAD   0x4         /* readable */
#define NINEP_DMWRITE  0x2         /* writable */
#define NINEP_DMEXEC   0x1         /* executable */

/* Special fids and tags */
#define NINEP_NOFID  ((uint32_t)-1)
#define NINEP_NOTAG  ((uint16_t)-1)

/* Maximum element sizes */
#define NINEP_MAX_WELEM 16  /* max path elements in Twalk */

/**
 * @brief 9P Qid (unique file identifier)
 */
struct ninep_qid {
	uint8_t type;     /* file type */
	uint32_t version; /* version number for cache consistency */
	uint64_t path;    /* unique path identifier */
} __packed;

/**
 * @brief 9P Stat structure (file metadata)
 */
struct ninep_stat {
	uint16_t size;    /* total size of stat structure */
	uint16_t type;    /* server type */
	uint32_t dev;     /* server subtype */
	struct ninep_qid qid;
	uint32_t mode;    /* permissions and flags */
	uint32_t atime;   /* last access time */
	uint32_t mtime;   /* last modification time */
	uint64_t length;  /* file length */
	char *name;       /* file name */
	char *uid;        /* owner name */
	char *gid;        /* group name */
	char *muid;       /* last modifier name */
};

/**
 * @brief 9P Message header
 */
struct ninep_msg_header {
	uint32_t size;  /* total message size including header */
	uint8_t type;   /* message type */
	uint16_t tag;   /* message tag for matching requests/responses */
} __packed;

/**
 * @brief Parse a 9P message header from buffer
 *
 * @param buf Input buffer
 * @param len Buffer length
 * @param hdr Output header structure
 * @return 0 on success, negative error code on failure
 */
int ninep_parse_header(const uint8_t *buf, size_t len,
                       struct ninep_msg_header *hdr);

/**
 * @brief Serialize a 9P message header to buffer
 *
 * @param buf Output buffer
 * @param len Buffer length
 * @param hdr Header structure to serialize
 * @return Number of bytes written, or negative error code
 */
int ninep_write_header(uint8_t *buf, size_t len,
                       const struct ninep_msg_header *hdr);

/**
 * @brief Parse a string from 9P message
 *
 * @param buf Input buffer
 * @param len Buffer length
 * @param offset Current offset in buffer (updated)
 * @param str Output string pointer
 * @param str_len Output string length
 * @return 0 on success, negative error code on failure
 */
int ninep_parse_string(const uint8_t *buf, size_t len, size_t *offset,
                       const char **str, uint16_t *str_len);

/**
 * @brief Write a string to 9P message
 *
 * @param buf Output buffer
 * @param len Buffer length
 * @param offset Current offset in buffer (updated)
 * @param str String to write
 * @param str_len String length
 * @return 0 on success, negative error code on failure
 */
int ninep_write_string(uint8_t *buf, size_t len, size_t *offset,
                       const char *str, uint16_t str_len);

/**
 * @brief Parse a qid from 9P message
 *
 * @param buf Input buffer
 * @param len Buffer length
 * @param offset Current offset in buffer (updated)
 * @param qid Output qid structure
 * @return 0 on success, negative error code on failure
 */
int ninep_parse_qid(const uint8_t *buf, size_t len, size_t *offset,
                    struct ninep_qid *qid);

/**
 * @brief Write a qid to 9P message
 *
 * @param buf Output buffer
 * @param len Buffer length
 * @param offset Current offset in buffer (updated)
 * @param qid Qid structure to write
 * @return 0 on success, negative error code on failure
 */
int ninep_write_qid(uint8_t *buf, size_t len, size_t *offset,
                    const struct ninep_qid *qid);

/**
 * @brief Write a stat structure to 9P message
 *
 * @param buf Output buffer
 * @param len Buffer length
 * @param offset Current offset in buffer (updated)
 * @param qid File qid
 * @param mode File mode/permissions
 * @param length File length
 * @param name File name
 * @param name_len File name length
 * @return 0 on success, negative error code on failure
 */
int ninep_write_stat(uint8_t *buf, size_t len, size_t *offset,
                     const struct ninep_qid *qid, uint32_t mode,
                     uint64_t length, const char *name, uint16_t name_len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_PROTOCOL_H_ */