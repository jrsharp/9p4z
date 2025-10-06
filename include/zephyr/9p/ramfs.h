/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_RAMFS_H_
#define ZEPHYR_INCLUDE_9P_RAMFS_H_

#include <zephyr/9p/server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_ramfs RAM Filesystem for 9P
 * @ingroup ninep_server
 * @{
 */

/**
 * @brief RAM filesystem context
 */
struct ninep_ramfs {
	struct ninep_fs_node *root;
	uint64_t next_qid_path;
};

/**
 * @brief Initialize RAM filesystem
 *
 * @param ramfs RAM filesystem context
 * @return 0 on success, negative error code on failure
 */
int ninep_ramfs_init(struct ninep_ramfs *ramfs);

/**
 * @brief Create a file node
 *
 * @param ramfs RAM filesystem context
 * @param parent Parent directory
 * @param name File name
 * @param content File content (can be NULL)
 * @param length Content length
 * @return Pointer to new node, or NULL on failure
 */
struct ninep_fs_node *ninep_ramfs_create_file(struct ninep_ramfs *ramfs,
                                                struct ninep_fs_node *parent,
                                                const char *name,
                                                const void *content,
                                                size_t length);

/**
 * @brief Create a directory node
 *
 * @param ramfs RAM filesystem context
 * @param parent Parent directory
 * @param name Directory name
 * @return Pointer to new node, or NULL on failure
 */
struct ninep_fs_node *ninep_ramfs_create_dir(struct ninep_ramfs *ramfs,
                                               struct ninep_fs_node *parent,
                                               const char *name);

/**
 * @brief Get filesystem operations
 *
 * @return Pointer to filesystem operations structure
 */
const struct ninep_fs_ops *ninep_ramfs_get_ops(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_RAMFS_H_ */
