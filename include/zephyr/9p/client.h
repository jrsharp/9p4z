/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_CLIENT_H_
#define ZEPHYR_INCLUDE_9P_CLIENT_H_

#include <zephyr/9p/protocol.h>
#include <zephyr/9p/transport.h>
#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_client 9P Client
 * @ingroup ninep
 * @{
 */

/* Forward declarations */
struct ninep_client;

/**
 * @brief Client FID entry (tracks opened files)
 */
struct ninep_client_fid {
	uint32_t fid;
	struct ninep_qid qid;
	bool in_use;
	uint32_t iounit;
};

/**
 * @brief Pending request structure (for request/response matching)
 *
 * Each pending request has its own response buffer, enabling true
 * concurrent 9P operations across multiple threads. The 9P protocol
 * uses tags to multiplex requests - this structure tracks each
 * outstanding request until its response arrives.
 */
struct ninep_pending_req {
	uint16_t tag;
	bool in_use;
	bool complete;
	int error;
	struct k_sem sem;

	/* Per-request response buffer for concurrent operations */
	uint8_t resp_data[CONFIG_NINEP_RESP_BUF_SIZE];
	size_t resp_len;
};

/**
 * @brief 9P client configuration
 */
struct ninep_client_config {
	uint32_t max_message_size;
	const char *version;
	uint32_t timeout_ms;  /* Request timeout in milliseconds */
};

/**
 * @brief 9P client instance
 *
 * Thread-safe 9P client supporting concurrent operations. Multiple threads
 * can issue 9P requests simultaneously - the client serializes TX (message
 * building and sending) but allows concurrent waits for responses.
 */
struct ninep_client {
	const struct ninep_client_config *config;
	struct ninep_transport *transport;

	/* FID table */
	struct ninep_client_fid fids[CONFIG_NINEP_MAX_FIDS];

	/* Pending requests table - each slot has its own response buffer */
	struct ninep_pending_req pending[CONFIG_NINEP_MAX_TAGS];

	/* TX buffer - protected by lock during build+send */
	uint8_t tx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

	/* State */
	uint32_t msize;  /* Negotiated max message size */
	uint32_t next_fid;
	uint16_t next_tag;

	/* Synchronization - held only during TX (build + send) */
	struct k_mutex lock;
};

/**
 * @brief Initialize 9P client
 *
 * @param client Client instance
 * @param config Client configuration
 * @param transport Transport layer
 * @return 0 on success, negative error code on failure
 */
int ninep_client_init(struct ninep_client *client,
                      const struct ninep_client_config *config,
                      struct ninep_transport *transport);

/**
 * @brief Negotiate version with server (Tversion/Rversion)
 *
 * @param client Client instance
 * @return 0 on success, negative error code on failure
 */
int ninep_client_version(struct ninep_client *client);

/**
 * @brief Attach to server root (Tattach/Rattach)
 *
 * @param client Client instance
 * @param fid Output: allocated FID for root
 * @param afid Authentication FID (NINEP_NOFID for no auth)
 * @param uname User name
 * @param aname Attach name (filesystem to attach)
 * @return 0 on success, negative error code on failure
 */
int ninep_client_attach(struct ninep_client *client, uint32_t *fid,
                        uint32_t afid, const char *uname, const char *aname);

/**
 * @brief Walk to path (Twalk/Rwalk)
 *
 * @param client Client instance
 * @param fid Starting FID
 * @param newfid Output: new FID (or same as fid for clone)
 * @param path Path to walk (e.g., "dir/subdir/file")
 * @return 0 on success, negative error code on failure
 */
int ninep_client_walk(struct ninep_client *client, uint32_t fid,
                      uint32_t *newfid, const char *path);

/**
 * @brief Open file (Topen/Ropen)
 *
 * @param client Client instance
 * @param fid FID to open
 * @param mode Open mode (NINEP_OREAD, NINEP_OWRITE, etc.)
 * @return 0 on success, negative error code on failure
 */
int ninep_client_open(struct ninep_client *client, uint32_t fid, uint8_t mode);

/**
 * @brief Read from file (Tread/Rread)
 *
 * @param client Client instance
 * @param fid FID to read from
 * @param offset Byte offset
 * @param buf Output buffer
 * @param count Bytes to read
 * @return Number of bytes read, or negative error code
 */
int ninep_client_read(struct ninep_client *client, uint32_t fid,
                      uint64_t offset, uint8_t *buf, uint32_t count);

/**
 * @brief Write to file (Twrite/Rwrite)
 *
 * @param client Client instance
 * @param fid FID to write to
 * @param offset Byte offset
 * @param buf Data to write
 * @param count Bytes to write
 * @return Number of bytes written, or negative error code
 */
int ninep_client_write(struct ninep_client *client, uint32_t fid,
                       uint64_t offset, const uint8_t *buf, uint32_t count);

/**
 * @brief Get file stat (Tstat/Rstat)
 *
 * @param client Client instance
 * @param fid FID to stat
 * @param stat Output stat structure
 * @return 0 on success, negative error code on failure
 */
int ninep_client_stat(struct ninep_client *client, uint32_t fid,
                      struct ninep_stat *stat);

/**
 * @brief Create file (Tcreate/Rcreate)
 *
 * @param client Client instance
 * @param fid FID of parent directory
 * @param name File name
 * @param perm Permissions (use NINEP_DMDIR for directories)
 * @param mode Open mode
 * @return 0 on success, negative error code on failure
 */
int ninep_client_create(struct ninep_client *client, uint32_t fid,
                        const char *name, uint32_t perm, uint8_t mode);

/**
 * @brief Remove file (Tremove/Rremove)
 *
 * @param client Client instance
 * @param fid FID to remove
 * @return 0 on success, negative error code on failure
 */
int ninep_client_remove(struct ninep_client *client, uint32_t fid);

/**
 * @brief Clunk FID (Tclunk/Rclunk)
 *
 * @param client Client instance
 * @param fid FID to clunk (close)
 * @return 0 on success, negative error code on failure
 */
int ninep_client_clunk(struct ninep_client *client, uint32_t fid);

/**
 * @brief Allocate a new FID
 *
 * @param client Client instance
 * @param fid Output: allocated FID
 * @return 0 on success, negative error code on failure
 */
int ninep_client_alloc_fid(struct ninep_client *client, uint32_t *fid);

/**
 * @brief Free a FID
 *
 * @param client Client instance
 * @param fid FID to free
 */
void ninep_client_free_fid(struct ninep_client *client, uint32_t fid);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_CLIENT_H_ */
