/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_PASSTHROUGH_FS_H_
#define ZEPHYR_INCLUDE_9P_PASSTHROUGH_FS_H_

#include <zephyr/9p/server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_passthrough_fs 9P Passthrough Filesystem
 * @ingroup ninep
 * @{
 */

/**
 * @brief Passthrough filesystem instance
 *
 * This filesystem backend exposes any mounted Zephyr filesystem via 9P.
 * Supports LittleFS, FAT, and any other Zephyr-compatible filesystem.
 */
struct ninep_passthrough_fs {
	const char *mount_point;   /* Mount point (e.g., "/lfs1", "/SD:") */
	uint64_t next_qid_path;    /* Next QID path value */
	struct ninep_fs_node *root; /* Root node */
};

/**
 * @brief Initialize passthrough filesystem
 *
 * @param fs Passthrough filesystem instance
 * @param mount_point Mount point of the Zephyr filesystem to expose
 * @return 0 on success, negative error code on failure
 */
int ninep_passthrough_fs_init(struct ninep_passthrough_fs *fs,
                               const char *mount_point);

/**
 * @brief Get filesystem operations for passthrough FS
 *
 * @return Pointer to fs_ops structure
 */
const struct ninep_fs_ops *ninep_passthrough_fs_get_ops(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_PASSTHROUGH_FS_H_ */
