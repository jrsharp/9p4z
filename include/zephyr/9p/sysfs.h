/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_SYSFS_H_
#define ZEPHYR_INCLUDE_9P_SYSFS_H_

#include <zephyr/9p/server.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Sysfs - Dynamic synthetic filesystem for Zephyr
 *
 * Sysfs provides a Plan 9-style interface to system information
 * through dynamically generated files. Content is generated on-the-fly
 * when files are read, allowing real-time access to system state.
 */

/**
 * @brief Content generator callback
 *
 * Called when a sysfs file is read. Should write content to the provided
 * buffer and return the number of bytes written.
 *
 * @param buf Buffer to write content into
 * @param buf_size Size of the buffer
 * @param offset Offset within the file (for partial reads)
 * @param ctx Optional context pointer passed during registration
 * @return Number of bytes written, or negative error code
 */
typedef int (*ninep_sysfs_generator_t)(uint8_t *buf, size_t buf_size,
                                        uint64_t offset, void *ctx);

/**
 * @brief Sysfs file entry
 */
struct ninep_sysfs_entry {
	const char *path;                  /* Full path (e.g., "/sys/uptime") */
	ninep_sysfs_generator_t generator; /* Content generator callback */
	void *ctx;                         /* Optional context for generator */
	bool is_dir;                       /* True for directories */
};

/**
 * @brief Sysfs instance
 */
struct ninep_sysfs {
	struct ninep_sysfs_entry *entries; /* Array of registered entries */
	size_t num_entries;                /* Number of registered entries */
	size_t max_entries;                /* Maximum entries (array size) */
	struct ninep_fs_node *root;        /* Root node */
	uint64_t next_qid_path;            /* Next QID path number */
};

/**
 * @brief Initialize a sysfs instance
 *
 * @param sysfs Sysfs instance to initialize
 * @param entries Array of sysfs entries (must be static/persistent)
 * @param max_entries Maximum number of entries
 * @return 0 on success, negative error code on failure
 */
int ninep_sysfs_init(struct ninep_sysfs *sysfs,
                     struct ninep_sysfs_entry *entries,
                     size_t max_entries);

/**
 * @brief Register a sysfs file
 *
 * @param sysfs Sysfs instance
 * @param path Full path to the file (e.g., "/sys/uptime")
 * @param generator Content generator callback
 * @param ctx Optional context pointer passed to generator
 * @return 0 on success, negative error code on failure
 */
int ninep_sysfs_register_file(struct ninep_sysfs *sysfs,
                               const char *path,
                               ninep_sysfs_generator_t generator,
                               void *ctx);

/**
 * @brief Register a sysfs directory
 *
 * Directories don't have content generators - they're used for
 * organizing files hierarchically.
 *
 * @param sysfs Sysfs instance
 * @param path Full path to the directory (e.g., "/sys")
 * @return 0 on success, negative error code on failure
 */
int ninep_sysfs_register_dir(struct ninep_sysfs *sysfs, const char *path);

/**
 * @brief Get filesystem operations for sysfs
 *
 * Returns the ninep_fs_ops structure that implements the sysfs
 * operations. Pass this to ninep_server_init().
 *
 * @return Pointer to filesystem operations
 */
const struct ninep_fs_ops *ninep_sysfs_get_ops(void);

#endif /* ZEPHYR_INCLUDE_9P_SYSFS_H_ */
