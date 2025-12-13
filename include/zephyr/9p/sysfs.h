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
 * @brief Content writer callback
 *
 * Called when a sysfs file is written. Should process the written data.
 *
 * @param buf Buffer containing data to write
 * @param count Number of bytes to write
 * @param offset Offset within the file (for partial writes)
 * @param ctx Optional context pointer passed during registration
 * @return Number of bytes written, or negative error code
 */
typedef int (*ninep_sysfs_writer_t)(const uint8_t *buf, uint32_t count,
                                     uint64_t offset, void *ctx);

/**
 * @brief Clunk (close) callback
 *
 * Called when a sysfs file is closed (clunked). Useful for finalizing
 * operations like DFU writes that need to know when the file is closed.
 *
 * @param ctx Optional context pointer passed during registration
 * @return 0 on success, negative error code on failure
 */
typedef int (*ninep_sysfs_clunk_t)(void *ctx);

/**
 * @brief Sysfs file entry
 */
struct ninep_sysfs_entry {
	const char *path;                  /* Full path (e.g., "/sys/uptime") */
	ninep_sysfs_generator_t generator; /* Content generator callback */
	ninep_sysfs_writer_t writer;       /* Content writer callback (NULL for read-only) */
	ninep_sysfs_clunk_t clunk;         /* Clunk (close) callback (NULL if not needed) */
	void *ctx;                         /* Optional context for callbacks */
	bool is_dir;                       /* True for directories */
	bool writable;                     /* True if file is writable */
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
 * @brief Register a writable sysfs file
 *
 * @param sysfs Sysfs instance
 * @param path Full path to the file (e.g., "/dev/led")
 * @param generator Content generator callback (for reads)
 * @param writer Content writer callback (for writes)
 * @param ctx Optional context pointer passed to generator/writer
 * @return 0 on success, negative error code on failure
 */
int ninep_sysfs_register_writable_file(struct ninep_sysfs *sysfs,
                                        const char *path,
                                        ninep_sysfs_generator_t generator,
                                        ninep_sysfs_writer_t writer,
                                        void *ctx);

/**
 * @brief Register a writable sysfs file with clunk callback
 *
 * Like ninep_sysfs_register_writable_file() but with an additional clunk
 * callback that is called when the file is closed. Useful for operations
 * that need to finalize when writing is complete (e.g., DFU).
 *
 * @param sysfs Sysfs instance
 * @param path Full path to the file (e.g., "/dev/firmware")
 * @param generator Content generator callback (for reads)
 * @param writer Content writer callback (for writes)
 * @param clunk Clunk callback (called when file is closed)
 * @param ctx Optional context pointer passed to all callbacks
 * @return 0 on success, negative error code on failure
 */
int ninep_sysfs_register_writable_file_ex(struct ninep_sysfs *sysfs,
                                           const char *path,
                                           ninep_sysfs_generator_t generator,
                                           ninep_sysfs_writer_t writer,
                                           ninep_sysfs_clunk_t clunk,
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
