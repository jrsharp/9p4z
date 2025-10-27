/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NAMESPACE_FS_9P_H_
#define ZEPHYR_INCLUDE_NAMESPACE_FS_9P_H_

/**
 * @file
 * @brief 9P VFS Driver
 *
 * Registers 9P as a Zephyr filesystem type, allowing 9P filesystems
 * to be mounted using the standard VFS API.
 */

#include <zephyr/fs/fs.h>
#include <zephyr/9p/client.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup fs_9p 9P VFS Driver
 * @{
 */

/**
 * @brief 9P filesystem type ID
 *
 * Use this when creating fs_mount_t structures for 9P filesystems.
 */
#define FS_TYPE_9P (FS_TYPE_EXTERNAL_BASE + 1)

/**
 * @brief 9P mount context
 *
 * Pass this as fs_data when mounting a 9P filesystem.
 */
struct ninep_mount_ctx {
	struct ninep_client *client;  /**< 9P client connection */
	char aname[256];              /**< Attach name (root path on server) */
	struct ninep_qid root_qid;    /**< Root directory QID */
	uint32_t fid_pool_base;       /**< Base FID for this mount */
	uint32_t msize;               /**< Negotiated message size */

	/* Internal state */
	uint32_t root_fid;            /**< FID for root directory */
	bool attached;                /**< Attachment complete */
};

/**
 * @brief FID pool for 9P operations
 *
 * Each mount gets a range of FIDs to avoid conflicts.
 */
struct ninep_fid_pool {
	uint32_t base_fid;           /**< Starting FID */
	uint32_t max_fids;           /**< Number of FIDs */
	ATOMIC_DEFINE(bitmap, CONFIG_NINEP_MAX_FIDS);  /**< Allocation bitmap */
	struct k_mutex lock;
};

/**
 * @brief Initialize 9P VFS driver
 *
 * Registers 9P as a filesystem type with Zephyr VFS.
 * Must be called during system initialization.
 *
 * @return 0 on success, negative error code on failure
 */
int fs_9p_init(void);

/**
 * @brief Allocate a FID from the pool
 *
 * @param pool FID pool
 * @return Allocated FID or negative error code
 */
int ninep_fid_pool_alloc(struct ninep_fid_pool *pool);

/**
 * @brief Free a FID back to the pool
 *
 * @param pool FID pool
 * @param fid FID to free
 */
void ninep_fid_pool_free(struct ninep_fid_pool *pool, uint32_t fid);

/**
 * @brief Initialize a FID pool
 *
 * @param pool FID pool to initialize
 * @param base_fid Starting FID
 * @param max_fids Number of FIDs
 * @return 0 on success, negative error code on failure
 */
int ninep_fid_pool_init(struct ninep_fid_pool *pool, uint32_t base_fid,
                        uint32_t max_fids);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NAMESPACE_FS_9P_H_ */
