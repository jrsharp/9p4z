/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_9P_MESSAGE_H_
#define ZEPHYR_INCLUDE_9P_MESSAGE_H_

#include <zephyr/9p/protocol.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_message 9P Message Builders
 * @ingroup ninep
 * @{
 */

/**
 * @brief Build a Tversion message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag (should be NINEP_NOTAG for Tversion)
 * @param msize Maximum message size
 * @param version Protocol version string (e.g., "9P2000")
 * @param version_len Version string length
 * @return Number of bytes written, or negative error code
 */
int ninep_build_tversion(uint8_t *buf, size_t buf_len, uint16_t tag,
                         uint32_t msize, const char *version, uint16_t version_len);

/**
 * @brief Build an Rversion message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag (should be NINEP_NOTAG for Rversion)
 * @param msize Maximum message size
 * @param version Protocol version string
 * @param version_len Version string length
 * @return Number of bytes written, or negative error code
 */
int ninep_build_rversion(uint8_t *buf, size_t buf_len, uint16_t tag,
                         uint32_t msize, const char *version, uint16_t version_len);

/**
 * @brief Build a Tattach message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param fid File identifier to establish
 * @param afid Authentication fid (NINEP_NOFID if no auth)
 * @param uname User name
 * @param uname_len User name length
 * @param aname Attach name (filesystem/tree to attach to)
 * @param aname_len Attach name length
 * @return Number of bytes written, or negative error code
 */
int ninep_build_tattach(uint8_t *buf, size_t buf_len, uint16_t tag,
                        uint32_t fid, uint32_t afid,
                        const char *uname, uint16_t uname_len,
                        const char *aname, uint16_t aname_len);

/**
 * @brief Build an Rattach message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param qid Qid of the root
 * @return Number of bytes written, or negative error code
 */
int ninep_build_rattach(uint8_t *buf, size_t buf_len, uint16_t tag,
                        const struct ninep_qid *qid);

/**
 * @brief Build a Twalk message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param fid Current fid
 * @param newfid New fid to create
 * @param nwname Number of path elements
 * @param wnames Array of path element strings
 * @param wname_lens Array of path element lengths
 * @return Number of bytes written, or negative error code
 */
int ninep_build_twalk(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint32_t fid, uint32_t newfid,
                      uint16_t nwname, const char **wnames, const uint16_t *wname_lens);

/**
 * @brief Build an Rwalk message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param nwqid Number of qids
 * @param wqids Array of qids
 * @return Number of bytes written, or negative error code
 */
int ninep_build_rwalk(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint16_t nwqid, const struct ninep_qid *wqids);

/**
 * @brief Build a Topen message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param fid File identifier
 * @param mode Open mode (NINEP_OREAD, NINEP_OWRITE, etc.)
 * @return Number of bytes written, or negative error code
 */
int ninep_build_topen(uint8_t *buf, size_t buf_len, uint16_t tag,
                      uint32_t fid, uint8_t mode);

/**
 * @brief Build an Ropen message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param qid File qid
 * @param iounit I/O unit (0 for default)
 * @return Number of bytes written, or negative error code
 */
int ninep_build_ropen(uint8_t *buf, size_t buf_len, uint16_t tag,
                      const struct ninep_qid *qid, uint32_t iounit);

/**
 * @brief Build a Tclunk message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param fid File identifier to clunk
 * @return Number of bytes written, or negative error code
 */
int ninep_build_tclunk(uint8_t *buf, size_t buf_len, uint16_t tag, uint32_t fid);

/**
 * @brief Build an Rclunk message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @return Number of bytes written, or negative error code
 */
int ninep_build_rclunk(uint8_t *buf, size_t buf_len, uint16_t tag);

/**
 * @brief Build an Rerror message
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @param tag Message tag
 * @param ename Error message string
 * @param ename_len Error message length
 * @return Number of bytes written, or negative error code
 */
int ninep_build_rerror(uint8_t *buf, size_t buf_len, uint16_t tag,
                       const char *ename, uint16_t ename_len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_MESSAGE_H_ */
