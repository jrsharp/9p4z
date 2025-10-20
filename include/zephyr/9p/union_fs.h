/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_UNION_FS_H_
#define ZEPHYR_INCLUDE_9P_UNION_FS_H_

#include <zephyr/9p/server.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_union_fs 9P Union Filesystem (Namespace Composition)
 * @ingroup ninep
 * @{
 */

/**
 * @brief Union filesystem mount entry
 *
 * Represents a filesystem backend mounted at a specific path in the namespace.
 * This is the Plan 9 style namespace composition - each backend serves a subtree.
 */
struct ninep_union_mount {
	const char *path;                    /* Mount point (e.g., "/dev", "/files") */
	const struct ninep_fs_ops *fs_ops;   /* Backend filesystem operations */
	void *fs_ctx;                        /* Backend context */
	struct ninep_fs_node *root;          /* Backend's root node */
};

/**
 * @brief Node ownership tracking entry
 */
struct ninep_node_owner {
	struct ninep_fs_node *node;
	struct ninep_union_mount *mount;
};

/**
 * @brief Union filesystem instance
 *
 * Provides Plan 9-style namespace composition by routing operations to
 * multiple filesystem backends based on path prefixes.
 *
 * Example namespace:
 *   /dev/led      -> sysfs backend
 *   /sys/version  -> sysfs backend
 *   /files/notes/ -> LittleFS passthrough backend
 *   /shared/      -> LittleFS passthrough backend
 */
struct ninep_union_fs {
	struct ninep_union_mount *mounts;   /* Array of mount points */
	size_t num_mounts;                  /* Number of mounts */
	size_t max_mounts;                  /* Maximum mounts */
	struct ninep_fs_node *root;         /* Synthetic root directory */
	uint64_t next_qid_path;             /* Next QID for synthetic nodes */

	/* Node ownership tracking (for non-root nodes) */
	struct ninep_node_owner node_owners[128];  /* Track up to 128 nodes */
	size_t num_node_owners;
};

/**
 * @brief Initialize union filesystem
 *
 * @param fs Union filesystem instance
 * @param mounts Array of mount entries (must remain valid)
 * @param max_mounts Maximum number of mounts
 * @return 0 on success, negative error code on failure
 */
int ninep_union_fs_init(struct ninep_union_fs *fs,
                         struct ninep_union_mount *mounts,
                         size_t max_mounts);

/**
 * @brief Mount a filesystem backend at a path
 *
 * Registers a filesystem backend to serve a subtree of the namespace.
 * The backend will handle all operations under the specified mount point.
 *
 * @param fs Union filesystem instance
 * @param path Mount point (e.g., "/dev", "/files")
 * @param fs_ops Backend filesystem operations
 * @param fs_ctx Backend context
 * @return 0 on success, negative error code on failure
 *
 * Example:
 *   ninep_union_fs_mount(&union_fs, "/dev", sysfs_ops, &sysfs_dev);
 *   ninep_union_fs_mount(&union_fs, "/files", passthrough_ops, &lfs);
 */
int ninep_union_fs_mount(struct ninep_union_fs *fs,
                          const char *path,
                          const struct ninep_fs_ops *fs_ops,
                          void *fs_ctx);

/**
 * @brief Get filesystem operations for union FS
 *
 * @return Pointer to fs_ops structure
 */
const struct ninep_fs_ops *ninep_union_fs_get_ops(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_UNION_FS_H_ */
