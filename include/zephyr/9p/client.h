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
 * @brief Lightweight tag tracking structure
 *
 * Tags are cheap - just tracking state, no buffers. The 9P protocol
 * uses tags to multiplex requests on a single connection. Responses
 * arrive serially on the transport, so we only need ONE shared response
 * buffer, not one per tag.
 *
 * This design allows many concurrent tags (64+) with minimal memory:
 * - 64 tags × ~16 bytes = 1KB (vs old: 64 × 300 bytes = 19KB)
 */
struct ninep_tag_entry {
	uint16_t tag;           /* Tag number */
	bool in_use;            /* Tag is allocated */
	bool complete;          /* Response received */
	int error;              /* Error code (0 = success) */
	void *user_ctx;         /* Caller-provided context for result */
};

/**
 * @brief Memory pool configuration for 9P client
 *
 * All memory pools are provided by the caller. The library never allocates.
 * This allows platforms to place pools in appropriate memory regions
 * (PSRAM, SRAM, heap, etc.) without library changes.
 *
 * If pools is NULL in ninep_client_config, the client falls back to
 * embedded arrays (backward compatibility with existing code).
 */
struct ninep_client_pools {
	/** FID tracking pool - one entry per open file/directory */
	struct ninep_client_fid *fids;
	size_t max_fids;

	/** Tag tracking pool - one entry per concurrent request */
	struct ninep_tag_entry *tags;
	size_t max_tags;

	/** Transmit buffer - sized for max message */
	uint8_t *tx_buf;

	/** Receive/response buffer - sized for max message */
	uint8_t *rx_buf;

	/** Buffer size (same for tx and rx) */
	size_t buf_size;
};

/**
 * @brief 9P client configuration
 */
struct ninep_client_config {
	uint32_t max_message_size;
	const char *version;
	uint32_t timeout_ms;  /* Request timeout in milliseconds */

	/**
	 * Optional: caller-provided memory pools.
	 * If NULL, the client uses embedded arrays (backward compatible).
	 * If non-NULL, the client uses the provided pools instead,
	 * allowing placement in PSRAM or other memory regions.
	 */
	const struct ninep_client_pools *pools;
};

/**
 * @brief 9P client instance
 *
 * Thread-safe 9P client supporting concurrent operations. Multiple threads
 * can issue 9P requests simultaneously - the client serializes TX (message
 * building and sending) but allows concurrent waits for responses.
 *
 * Memory-efficient design:
 * - Single shared response buffer (responses arrive serially on transport)
 * - Lightweight tag tracking (no per-tag buffers or semaphores)
 * - Single condvar for all waiters (broadcast on response arrival)
 *
 * This allows 64+ concurrent tags in ~2KB vs the old ~20KB.
 *
 * Pool support: If config->pools is provided, the client uses caller-provided
 * memory pools (can be in PSRAM, etc.). Otherwise falls back to embedded arrays.
 */
struct ninep_client {
	const struct ninep_client_config *config;
	struct ninep_transport *transport;

	/* Pool pointers - point to either embedded arrays or external pools */
	struct ninep_client_fid *fids;
	size_t max_fids;
	struct ninep_tag_entry *tags;
	size_t max_tags;
	uint8_t *tx_buf;
	uint8_t *resp_buf;
	size_t buf_size;

	/* Embedded arrays - used when config->pools is NULL (backward compat) */
	struct ninep_client_fid _embedded_fids[CONFIG_NINEP_MAX_FIDS];
	struct ninep_tag_entry _embedded_tags[CONFIG_NINEP_MAX_TAGS];
	uint8_t _embedded_tx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	uint8_t _embedded_resp_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

	/* Runtime state */
	size_t resp_len;
	uint32_t msize;  /* Negotiated max message size */
	uint32_t next_fid;
	uint16_t next_tag;
	uint8_t max_retries;  /* Retry count on timeout (0=no retry) */

	/* Synchronization */
	struct k_mutex lock;       /* Protects TX and tag table */
	struct k_condvar resp_cv;  /* Signaled when any response arrives */
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
 * @brief Authenticate with server (Tauth/Rauth)
 *
 * Initiates the authentication protocol by sending Tauth and receiving
 * an auth file QID. After this, the caller should read a challenge from
 * the afid, sign it, and write the response before calling attach.
 *
 * @param client Client instance
 * @param afid Output: allocated auth FID
 * @param aqid Output: auth file QID (optional, can be NULL)
 * @param uname User name (typically CGA hex string like "0ebb4aae7486")
 * @param aname Attach name (filesystem to attach)
 * @return 0 on success, negative error code on failure
 */
int ninep_client_auth(struct ninep_client *client, uint32_t *afid,
                      struct ninep_qid *aqid, const char *uname, const char *aname);

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
 * @brief Set max retries on timeout
 *
 * When a request times out, the client re-sends the same T-message
 * up to max_retries additional times.  Useful for unreliable transports
 * like LoRa.  Default is 0 (no retry).
 *
 * @param client Client instance
 * @param retries Max retry count (0 = no retry)
 */
static inline void ninep_client_set_retries(struct ninep_client *client,
					    uint8_t retries)
{
	client->max_retries = retries;
}

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
