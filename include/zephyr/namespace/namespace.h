/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NAMESPACE_NAMESPACE_H_
#define ZEPHYR_INCLUDE_NAMESPACE_NAMESPACE_H_

/**
 * @file
 * @brief Plan 9-style Namespaces for Zephyr
 *
 * This module implements Plan 9-style per-thread composable namespaces
 * with union mount semantics. It provides a layer above Zephyr's VFS
 * that allows threads to have customized views of the filesystem hierarchy.
 *
 * Key features:
 * - Per-thread namespace isolation
 * - Union mounts (multiple filesystems at same path)
 * - Network transparency via 9P
 * - Namespace inheritance (copy-on-write)
 * - Filesystem-agnostic (works with any VFS filesystem)
 */

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup namespace Plan 9 Namespaces
 * @{
 */

/* Forward declarations */
struct ns_entry;
struct thread_namespace;
struct ninep_server;

/**
 * @brief Namespace entry types
 */
enum ns_entry_type {
	NS_ENTRY_VFS,      /**< Standard VFS mount */
	NS_ENTRY_SERVER,   /**< In-process 9P server */
};

/**
 * @brief Namespace mount flags (Plan 9 style)
 */
#define NS_FLAG_BEFORE   0x0001  /**< Mount before existing mounts */
#define NS_FLAG_AFTER    0x0002  /**< Mount after existing mounts */
#define NS_FLAG_CREATE   0x0004  /**< Create files only in this mount */
#define NS_FLAG_REPLACE  0x0008  /**< Replace existing mount */

/**
 * @brief Namespace entry - represents a single mount/bind
 */
struct ns_entry {
	char path[CONFIG_NS_MAX_PATH_LEN];  /**< Mount point path */

	/* Underlying mount */
	enum ns_entry_type type;
	union {
		struct fs_mount_t *vfs_mount;     /**< VFS mount point */
		struct ninep_server *server;      /**< In-process server */
	};

	/* Mount flags */
	uint32_t flags;
	int priority;  /**< Priority for union mounts */

	/* For union mounts: linked list */
	struct ns_entry *next;
};

/**
 * @brief Per-thread namespace
 */
struct thread_namespace {
	k_tid_t thread_id;  /**< Owning thread */

	/* Namespace entries (hash table) */
	struct ns_entry *entries[CONFIG_NS_HASH_SIZE];

	/* Parent namespace (for COW inheritance) */
	struct thread_namespace *parent;
	bool is_cow;  /**< Copy-on-write flag */

	/* Reference counting */
	atomic_t refcount;

	/* Thread-safe access */
	struct k_mutex lock;
};

/**
 * @brief File descriptor extension for namespace-aware files
 */
struct ns_file {
	struct fs_file_t base;      /**< Base VFS file */
	struct ns_entry *ns_entry;  /**< Which namespace entry */

	/* For 9P files */
	struct {
		uint32_t fid;
		uint64_t offset;
	} ninep;
};

/* ========================================================================
 * Initialization
 * ======================================================================== */

/**
 * @brief Initialize the namespace subsystem
 *
 * Must be called during system initialization before any other
 * namespace functions.
 *
 * @return 0 on success, negative error code on failure
 */
int ns_init(void);

/**
 * @brief Create namespace for current thread
 *
 * If parent is NULL, creates a fresh namespace.
 * If parent is set, inherits from parent (copy-on-write).
 *
 * @param parent Parent namespace to inherit from (NULL for fresh)
 * @return 0 on success, negative error code on failure
 */
int ns_create(struct thread_namespace *parent);

/**
 * @brief Fork current thread's namespace for a new thread
 *
 * Creates a copy-on-write namespace for the child thread.
 *
 * @param child_tid Child thread ID
 * @return 0 on success, negative error code on failure
 */
int ns_fork(k_tid_t child_tid);

/**
 * @brief Destroy namespace for a thread
 *
 * Called automatically when thread terminates.
 *
 * @param tid Thread ID
 * @return 0 on success, negative error code on failure
 */
int ns_destroy(k_tid_t tid);

/* ========================================================================
 * Namespace Manipulation (Plan 9 Style)
 * ======================================================================== */

/**
 * @brief Bind old path to new location in namespace
 *
 * Flags control behavior:
 * - NS_FLAG_BEFORE: new appears before old
 * - NS_FLAG_AFTER: new appears after old
 * - NS_FLAG_CREATE: create files in new
 * - NS_FLAG_REPLACE: replace old entirely
 *
 * Example:
 *   ns_bind("/remote/node1/sensors", "/sensors", NS_FLAG_AFTER);
 *   // Now /sensors shows local sensors first, remote second
 *
 * @param old_path Source path
 * @param new_path Destination path
 * @param flags Mount flags
 * @return 0 on success, negative error code on failure
 */
int ns_bind(const char *old_path, const char *new_path, uint32_t flags);

/**
 * @brief Mount a VFS filesystem into the namespace
 *
 * Works with ANY filesystem type registered with VFS (9P, FAT, LittleFS, etc.)
 * The namespace layer is filesystem-agnostic.
 *
 * @param vfs_mount Pointer to VFS mount structure
 * @param mnt_point Where to mount in namespace
 * @param flags Namespace flags (NS_FLAG_BEFORE, NS_FLAG_AFTER, etc.)
 * @return 0 on success, negative error code on failure
 */
int ns_mount(struct fs_mount_t *vfs_mount, const char *mnt_point,
             uint32_t flags);

/**
 * @brief Mount an in-process 9P server into the namespace
 *
 * @param server Server instance to mount
 * @param mnt_point Where to mount in namespace
 * @param flags Namespace flags
 * @return 0 on success, negative error code on failure
 */
int ns_mount_server(struct ninep_server *server, const char *mnt_point,
                    uint32_t flags);

/**
 * @brief Unmount from namespace
 *
 * If old_path is specified, only removes that specific binding.
 *
 * @param mnt_point Mount point to remove
 * @param old_path Specific binding to remove (NULL for all)
 * @return 0 on success, negative error code on failure
 */
int ns_unmount(const char *mnt_point, const char *old_path);

/**
 * @brief Clear all mounts from namespace (reset to empty)
 *
 * @return 0 on success, negative error code on failure
 */
int ns_clear(void);

/* ========================================================================
 * File Operations (Namespace-Aware)
 * ======================================================================== */

/**
 * @brief Open a file through the namespace
 *
 * Resolves path through union mounts in priority order.
 *
 * @param path Path relative to namespace root
 * @param flags Standard open flags (FS_O_READ, FS_O_WRITE, etc.)
 * @return File descriptor or negative error code
 */
int ns_open(const char *path, fs_mode_t flags);

/**
 * @brief Read from namespace file
 *
 * @param fd File descriptor
 * @param buf Buffer to read into
 * @param count Bytes to read
 * @return Bytes read or negative error code
 */
ssize_t ns_read(int fd, void *buf, size_t count);

/**
 * @brief Write to namespace file
 *
 * If file is in union mount, writes go to mount with NS_FLAG_CREATE.
 *
 * @param fd File descriptor
 * @param buf Data to write
 * @param count Bytes to write
 * @return Bytes written or negative error code
 */
ssize_t ns_write(int fd, const void *buf, size_t count);

/**
 * @brief Close namespace file
 *
 * @param fd File descriptor
 * @return 0 on success, negative error code on failure
 */
int ns_close(int fd);

/**
 * @brief Seek within file
 *
 * @param fd File descriptor
 * @param offset Offset
 * @param whence Seek mode (FS_SEEK_SET, FS_SEEK_CUR, FS_SEEK_END)
 * @return New offset or negative error code
 */
off_t ns_lseek(int fd, off_t offset, int whence);

/**
 * @brief Get file stats
 *
 * @param path File path
 * @param entry Output stat structure
 * @return 0 on success, negative error code on failure
 */
int ns_stat(const char *path, struct fs_dirent *entry);

/**
 * @brief Open directory for reading
 *
 * For union mounts, merges directory listings.
 *
 * @param path Directory path
 * @return Directory descriptor or negative error code
 */
int ns_opendir(const char *path);

/**
 * @brief Read directory entry
 *
 * Automatically deduplicates entries from union mounts.
 *
 * @param fd Directory descriptor
 * @param entry Output directory entry
 * @return 0 on success, negative error code on failure
 */
int ns_readdir(int fd, struct fs_dirent *entry);

/**
 * @brief Close directory
 *
 * @param fd Directory descriptor
 * @return 0 on success, negative error code on failure
 */
int ns_closedir(int fd);

/**
 * @brief Create directory
 *
 * Created in mount with NS_FLAG_CREATE or highest priority writable mount.
 *
 * @param path Directory path
 * @return 0 on success, negative error code on failure
 */
int ns_mkdir(const char *path);

/**
 * @brief Remove file
 *
 * For union mounts, may create whiteout entry.
 *
 * @param path File path
 * @return 0 on success, negative error code on failure
 */
int ns_unlink(const char *path);

/**
 * @brief Rename file
 *
 * @param old_path Old path
 * @param new_path New path
 * @return 0 on success, negative error code on failure
 */
int ns_rename(const char *old_path, const char *new_path);

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

/**
 * @brief Get current thread's namespace
 *
 * @return Current namespace or NULL if none
 */
struct thread_namespace *ns_get_current(void);

/**
 * @brief Set namespace for current thread
 *
 * Useful for switching between namespaces.
 *
 * @param ns Namespace to set
 * @return 0 on success, negative error code on failure
 */
int ns_set_current(struct thread_namespace *ns);

/**
 * @brief Walk through namespace to resolve a path
 *
 * Returns list of all matching entries (for union mounts).
 *
 * @param path Path to resolve
 * @param entries Output array of matching ns_entries
 * @param max_entries Size of entries array
 * @return Number of entries found, or negative error
 */
int ns_walk(const char *path, struct ns_entry **entries, int max_entries);

/**
 * @brief Print current thread's namespace (for debugging)
 */
void ns_dump(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NAMESPACE_NAMESPACE_H_ */
