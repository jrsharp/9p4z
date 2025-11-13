/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/namespace/namespace.h>
#include <zephyr/9p/server.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(ns_file_ops, CONFIG_NINEP_LOG_LEVEL);

/* ========================================================================
 * File Descriptor Tracking
 * ======================================================================== */

#ifndef CONFIG_NS_MAX_OPEN_FILES
#define CONFIG_NS_MAX_OPEN_FILES 32
#endif

/**
 * @brief File descriptor entry
 */
struct ns_fd_entry {
	bool in_use;
	struct fs_file_t vfs_file;          /* Underlying VFS file */
	struct fs_dir_t vfs_dir;            /* Underlying VFS directory */
	bool is_dir;                        /* True if directory */
	struct ns_entry *ns_entry;          /* Which namespace entry */
	k_tid_t owner;                      /* Owning thread */

	/* In-process server state */
	struct ninep_fs_node *server_node;  /* Server filesystem node (if server entry) */
	uint64_t server_offset;             /* Current file offset (for servers) */
};

/* Global file descriptor table */
static struct ns_fd_entry fd_table[CONFIG_NS_MAX_OPEN_FILES];
static struct k_mutex fd_table_lock;
static bool fd_table_initialized = false;

/**
 * @brief Initialize FD table
 */
static void init_fd_table(void)
{
	if (fd_table_initialized) {
		return;
	}

	k_mutex_init(&fd_table_lock);
	memset(fd_table, 0, sizeof(fd_table));
	fd_table_initialized = true;
}

/**
 * @brief Allocate a file descriptor
 */
static int alloc_fd(void)
{
	if (!fd_table_initialized) {
		init_fd_table();
	}

	k_mutex_lock(&fd_table_lock, K_FOREVER);

	for (int i = 0; i < CONFIG_NS_MAX_OPEN_FILES; i++) {
		if (!fd_table[i].in_use) {
			fd_table[i].in_use = true;
			fd_table[i].owner = k_current_get();
			k_mutex_unlock(&fd_table_lock);
			return i;
		}
	}

	k_mutex_unlock(&fd_table_lock);
	return -ENOMEM;
}

/**
 * @brief Free a file descriptor
 */
static void free_fd(int fd)
{
	if (fd < 0 || fd >= CONFIG_NS_MAX_OPEN_FILES) {
		return;
	}

	k_mutex_lock(&fd_table_lock, K_FOREVER);
	fd_table[fd].in_use = false;
	fd_table[fd].owner = NULL;
	fd_table[fd].ns_entry = NULL;
	k_mutex_unlock(&fd_table_lock);
}

/**
 * @brief Get FD entry (with ownership check)
 */
static struct ns_fd_entry *get_fd_entry(int fd)
{
	if (fd < 0 || fd >= CONFIG_NS_MAX_OPEN_FILES) {
		return NULL;
	}

	k_mutex_lock(&fd_table_lock, K_FOREVER);

	if (!fd_table[fd].in_use) {
		k_mutex_unlock(&fd_table_lock);
		return NULL;
	}

	/* Check ownership (optional - could allow cross-thread access) */
	if (fd_table[fd].owner != k_current_get()) {
		LOG_WRN("FD %d accessed by non-owner thread", fd);
		/* Allow for now, but log warning */
	}

	k_mutex_unlock(&fd_table_lock);
	return &fd_table[fd];
}

/* ========================================================================
 * File Operations
 * ======================================================================== */

/**
 * @brief Resolve path to backend (VFS or server)
 *
 * Given a namespace path, find the matching namespace entry and
 * calculate the relative path within that entry.
 */
static int resolve_path(const char *ns_path, struct ns_entry **out_entry,
                        const char **out_rel_path)
{
	struct ns_entry *entries[CONFIG_NS_MAX_UNION_DEPTH];
	int count = ns_walk(ns_path, entries, CONFIG_NS_MAX_UNION_DEPTH);

	if (count <= 0) {
		return -ENOENT;
	}

	/* Use first matching entry (highest priority) */
	struct ns_entry *entry = entries[0];
	*out_entry = entry;

	/* Calculate path relative to mount point */
	const char *rel_path = ns_path + strlen(entry->path);
	if (*rel_path == '/') {
		rel_path++;
	}
	*out_rel_path = rel_path;

	return 0;
}

/**
 * @brief Resolve path to VFS path (for backward compatibility)
 */
static int resolve_to_vfs_path(const char *ns_path, struct ns_entry **out_entry,
                               char *vfs_path, size_t vfs_path_len)
{
	const char *rel_path;
	int ret = resolve_path(ns_path, out_entry, &rel_path);
	if (ret < 0) {
		return ret;
	}

	struct ns_entry *entry = *out_entry;

	if (entry->type != NS_ENTRY_VFS) {
		/* Not a VFS mount */
		return -ENOTSUP;
	}

	/* Build VFS path: <vfs_mount_point>/<rel_path> */
	snprintf(vfs_path, vfs_path_len, "%s/%s",
	         entry->vfs_mount->mnt_point, rel_path);

	return 0;
}

/* ========================================================================
 * In-Process Server Helpers
 * ======================================================================== */

/**
 * @brief Walk through a server filesystem to find a node
 */
static struct ninep_fs_node *server_walk_path(struct ninep_server *server,
                                               const char *path)
{
	if (!server || !server->config.fs_ops) {
		return NULL;
	}

	const struct ninep_fs_ops *ops = server->config.fs_ops;
	void *fs_ctx = server->config.fs_ctx;

	/* Get root node */
	struct ninep_fs_node *node = ops->get_root(fs_ctx);
	if (!node) {
		return NULL;
	}

	/* If path is empty or "/", return root */
	if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
		return node;
	}

	/* Walk through path components */
	const char *p = path;
	if (*p == '/') {
		p++;  /* Skip leading slash */
	}

	while (*p) {
		/* Find next slash or end */
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);

		if (len == 0) {
			p++;
			continue;
		}

		/* Walk to this component */
		node = ops->walk(node, p, len, fs_ctx);
		if (!node) {
			return NULL;
		}

		p += len;
		if (*p == '/') {
			p++;
		}
	}

	return node;
}

int ns_open(const char *path, fs_mode_t flags)
{
	if (!path) {
		return -EINVAL;
	}

	struct ns_entry *entry;
	const char *rel_path;

	int ret = resolve_path(path, &entry, &rel_path);
	if (ret < 0) {
		LOG_ERR("Failed to resolve %s: %d", path, ret);
		return ret;
	}

	/* Allocate file descriptor */
	int fd = alloc_fd();
	if (fd < 0) {
		LOG_ERR("No free file descriptors");
		return fd;
	}

	struct ns_fd_entry *fd_entry = &fd_table[fd];
	fd_entry->ns_entry = entry;
	fd_entry->is_dir = false;
	fd_entry->server_offset = 0;

	if (entry->type == NS_ENTRY_VFS) {
		/* VFS mount - open through VFS */
		char vfs_path[CONFIG_NS_MAX_PATH_LEN];
		snprintf(vfs_path, sizeof(vfs_path), "%s/%s",
		         entry->vfs_mount->mnt_point, rel_path);

		ret = fs_open(&fd_entry->vfs_file, vfs_path, flags);
		if (ret < 0) {
			LOG_ERR("fs_open %s failed: %d", vfs_path, ret);
			free_fd(fd);
			return ret;
		}

		LOG_DBG("Opened VFS %s -> %s (fd=%d)", path, vfs_path, fd);

	} else if (entry->type == NS_ENTRY_SERVER) {
		/* In-process server - walk to node and open */
		struct ninep_server *server = entry->server;
		struct ninep_fs_node *node = server_walk_path(server, rel_path);
		if (!node) {
			LOG_ERR("Server walk failed for %s", rel_path);
			free_fd(fd);
			return -ENOENT;
		}

		/* Call server's open operation */
		const struct ninep_fs_ops *ops = server->config.fs_ops;

		/* Map Zephyr VFS flags to 9P mode flags
		 * FS_O_READ (0x01)  -> NINEP_OREAD
		 * FS_O_WRITE (0x02) -> NINEP_OWRITE
		 * FS_O_RDWR (0x03)  -> NINEP_OREAD | NINEP_OWRITE
		 */
		uint8_t mode = 0;
		if (flags & FS_O_READ) {
			mode |= NINEP_OREAD;
		}
		if (flags & FS_O_WRITE) {
			mode |= NINEP_OWRITE;
		}
		if (mode == 0) {
			/* Default to read-only if no mode flags specified */
			mode = NINEP_OREAD;
		}

		ret = ops->open(node, mode, server->config.fs_ctx);
		if (ret < 0) {
			LOG_ERR("Server open failed: %d", ret);
			free_fd(fd);
			return ret;
		}

		fd_entry->server_node = node;
		LOG_DBG("Opened server %s (fd=%d)", path, fd);

	} else {
		LOG_ERR("Unknown entry type: %d", entry->type);
		free_fd(fd);
		return -EINVAL;
	}

	return fd;
}

ssize_t ns_read(int fd, void *buf, size_t count)
{
	if (!buf) {
		return -EINVAL;
	}

	struct ns_fd_entry *entry = get_fd_entry(fd);
	if (!entry) {
		LOG_ERR("Invalid file descriptor %d", fd);
		return -EBADF;
	}

	if (entry->is_dir) {
		LOG_ERR("Cannot read from directory");
		return -EISDIR;
	}

	ssize_t ret;

	if (entry->ns_entry->type == NS_ENTRY_VFS) {
		/* Read through VFS */
		ret = fs_read(&entry->vfs_file, buf, count);
		if (ret < 0) {
			LOG_ERR("fs_read failed: %d", (int)ret);
			return ret;
		}

	} else if (entry->ns_entry->type == NS_ENTRY_SERVER) {
		/* Read from in-process server */
		struct ninep_server *server = entry->ns_entry->server;
		const struct ninep_fs_ops *ops = server->config.fs_ops;

		ret = ops->read(entry->server_node, entry->server_offset,
		                buf, count, server->config.fs_ctx);
		if (ret < 0) {
			LOG_ERR("Server read failed: %d", (int)ret);
			return ret;
		}

		/* Update offset */
		entry->server_offset += ret;

	} else {
		return -EINVAL;
	}

	LOG_DBG("Read %zd bytes from fd=%d", ret, fd);
	return ret;
}

ssize_t ns_write(int fd, const void *buf, size_t count)
{
	if (!buf) {
		return -EINVAL;
	}

	struct ns_fd_entry *entry = get_fd_entry(fd);
	if (!entry) {
		LOG_ERR("Invalid file descriptor %d", fd);
		return -EBADF;
	}

	if (entry->is_dir) {
		LOG_ERR("Cannot write to directory");
		return -EISDIR;
	}

	ssize_t ret;

	if (entry->ns_entry->type == NS_ENTRY_VFS) {
		/* Write through VFS */
		ret = fs_write(&entry->vfs_file, buf, count);
		if (ret < 0) {
			LOG_ERR("fs_write failed: %d", (int)ret);
			return ret;
		}

	} else if (entry->ns_entry->type == NS_ENTRY_SERVER) {
		/* Write to in-process server */
		struct ninep_server *server = entry->ns_entry->server;
		const struct ninep_fs_ops *ops = server->config.fs_ops;

		/* Use "local" as uname for namespace operations (not remote 9P) */
		ret = ops->write(entry->server_node, entry->server_offset,
		                 buf, count, "local", server->config.fs_ctx);
		if (ret < 0) {
			LOG_ERR("Server write failed: %d", (int)ret);
			return ret;
		}

		/* Update offset */
		entry->server_offset += ret;

	} else {
		return -EINVAL;
	}

	LOG_DBG("Wrote %zd bytes to fd=%d", ret, fd);
	return ret;
}

int ns_close(int fd)
{
	struct ns_fd_entry *entry = get_fd_entry(fd);
	if (!entry) {
		LOG_ERR("Invalid file descriptor %d", fd);
		return -EBADF;
	}

	int ret = 0;

	if (entry->ns_entry->type == NS_ENTRY_VFS) {
		/* Close through VFS */
		if (entry->is_dir) {
			ret = fs_closedir(&entry->vfs_dir);
		} else {
			ret = fs_close(&entry->vfs_file);
		}

		if (ret < 0) {
			LOG_ERR("VFS close failed: %d", ret);
		}

	} else if (entry->ns_entry->type == NS_ENTRY_SERVER) {
		/* Clunk on in-process server */
		struct ninep_server *server = entry->ns_entry->server;
		const struct ninep_fs_ops *ops = server->config.fs_ops;

		if (ops->clunk) {
			ret = ops->clunk(entry->server_node, server->config.fs_ctx);
			if (ret < 0) {
				LOG_ERR("Server clunk failed: %d", ret);
			}
		}
	}

	/* Free the file descriptor */
	free_fd(fd);

	LOG_DBG("Closed fd=%d", fd);
	return ret;
}

off_t ns_lseek(int fd, off_t offset, int whence)
{
	struct ns_fd_entry *entry = get_fd_entry(fd);
	if (!entry) {
		LOG_ERR("Invalid file descriptor %d", fd);
		return -EBADF;
	}

	if (entry->is_dir) {
		LOG_ERR("Cannot seek in directory");
		return -EISDIR;
	}

	off_t ret;

	if (entry->ns_entry->type == NS_ENTRY_VFS) {
		/* Seek through VFS */
		ret = fs_seek(&entry->vfs_file, offset, whence);
		if (ret < 0) {
			LOG_ERR("fs_seek failed: %d", (int)ret);
			return ret;
		}

	} else if (entry->ns_entry->type == NS_ENTRY_SERVER) {
		/* Server - just update offset */
		switch (whence) {
		case FS_SEEK_SET:
			entry->server_offset = offset;
			break;
		case FS_SEEK_CUR:
			entry->server_offset += offset;
			break;
		case FS_SEEK_END:
			/* Would need node size - not supported yet */
			return -ENOTSUP;
		default:
			return -EINVAL;
		}
		ret = entry->server_offset;

	} else {
		return -EINVAL;
	}

	LOG_DBG("Seeked fd=%d to offset=%ld", fd, (long)ret);
	return ret;
}

int ns_stat(const char *path, struct fs_dirent *entry)
{
	if (!path || !entry) {
		return -EINVAL;
	}

	struct ns_entry *ns_entry;
	char vfs_path[CONFIG_NS_MAX_PATH_LEN];

	int ret = resolve_to_vfs_path(path, &ns_entry, vfs_path, sizeof(vfs_path));
	if (ret < 0) {
		return ret;
	}

	/* Get stat through VFS */
	ret = fs_stat(vfs_path, entry);
	if (ret < 0) {
		LOG_ERR("fs_stat %s failed: %d", vfs_path, ret);
		return ret;
	}

	LOG_DBG("Stat %s -> %s: size=%zu", path, vfs_path, entry->size);
	return 0;
}

int ns_opendir(const char *path)
{
	if (!path) {
		return -EINVAL;
	}

	struct ns_entry *entry;
	char vfs_path[CONFIG_NS_MAX_PATH_LEN];

	int ret = resolve_to_vfs_path(path, &entry, vfs_path, sizeof(vfs_path));
	if (ret < 0) {
		LOG_ERR("Failed to resolve %s: %d", path, ret);
		return ret;
	}

	/* Allocate file descriptor */
	int fd = alloc_fd();
	if (fd < 0) {
		LOG_ERR("No free file descriptors");
		return fd;
	}

	struct ns_fd_entry *fd_entry = &fd_table[fd];

	/* Open directory through VFS */
	ret = fs_opendir(&fd_entry->vfs_dir, vfs_path);
	if (ret < 0) {
		LOG_ERR("fs_opendir %s failed: %d", vfs_path, ret);
		free_fd(fd);
		return ret;
	}

	/* Store namespace entry reference */
	fd_entry->ns_entry = entry;
	fd_entry->is_dir = true;

	LOG_DBG("Opened directory %s -> %s (fd=%d)", path, vfs_path, fd);
	return fd;
}

int ns_readdir(int fd, struct fs_dirent *entry)
{
	if (!entry) {
		return -EINVAL;
	}

	struct ns_fd_entry *fd_entry = get_fd_entry(fd);
	if (!fd_entry) {
		LOG_ERR("Invalid file descriptor %d", fd);
		return -EBADF;
	}

	if (!fd_entry->is_dir) {
		LOG_ERR("Not a directory");
		return -ENOTDIR;
	}

	/* Read directory entry through VFS */
	int ret = fs_readdir(&fd_entry->vfs_dir, entry);
	if (ret < 0) {
		LOG_ERR("fs_readdir failed: %d", ret);
		return ret;
	}

	/* TODO: For union mounts, merge and deduplicate entries */

	if (entry->name[0] != '\0') {
		LOG_DBG("Read dir entry: %s (type=%d)", entry->name, entry->type);
	}

	return ret;
}

int ns_closedir(int fd)
{
	struct ns_fd_entry *entry = get_fd_entry(fd);
	if (!entry) {
		LOG_ERR("Invalid file descriptor %d", fd);
		return -EBADF;
	}

	if (!entry->is_dir) {
		LOG_ERR("Not a directory");
		return -ENOTDIR;
	}

	/* Close through VFS */
	int ret = fs_closedir(&entry->vfs_dir);
	if (ret < 0) {
		LOG_ERR("fs_closedir failed: %d", ret);
	}

	/* Free the file descriptor */
	free_fd(fd);

	LOG_DBG("Closed directory fd=%d", fd);
	return ret;
}

int ns_mkdir(const char *path)
{
	if (!path) {
		return -EINVAL;
	}

	struct ns_entry *entry;
	char vfs_path[CONFIG_NS_MAX_PATH_LEN];

	/* Find writable mount (NS_FLAG_CREATE or highest priority) */
	int ret = resolve_to_vfs_path(path, &entry, vfs_path, sizeof(vfs_path));
	if (ret < 0) {
		return ret;
	}

	/* Create directory through VFS */
	ret = fs_mkdir(vfs_path);
	if (ret < 0) {
		LOG_ERR("fs_mkdir %s failed: %d", vfs_path, ret);
		return ret;
	}

	LOG_DBG("Created directory %s -> %s", path, vfs_path);
	return 0;
}

int ns_unlink(const char *path)
{
	if (!path) {
		return -EINVAL;
	}

	struct ns_entry *entry;
	char vfs_path[CONFIG_NS_MAX_PATH_LEN];

	int ret = resolve_to_vfs_path(path, &entry, vfs_path, sizeof(vfs_path));
	if (ret < 0) {
		return ret;
	}

	/* Remove file through VFS */
	ret = fs_unlink(vfs_path);
	if (ret < 0) {
		LOG_ERR("fs_unlink %s failed: %d", vfs_path, ret);
		return ret;
	}

	LOG_DBG("Removed %s -> %s", path, vfs_path);
	return 0;
}

int ns_rename(const char *old_path, const char *new_path)
{
	if (!old_path || !new_path) {
		return -EINVAL;
	}

	struct ns_entry *old_entry, *new_entry;
	char old_vfs_path[CONFIG_NS_MAX_PATH_LEN];
	char new_vfs_path[CONFIG_NS_MAX_PATH_LEN];

	int ret = resolve_to_vfs_path(old_path, &old_entry,
	                              old_vfs_path, sizeof(old_vfs_path));
	if (ret < 0) {
		return ret;
	}

	ret = resolve_to_vfs_path(new_path, &new_entry,
	                         new_vfs_path, sizeof(new_vfs_path));
	if (ret < 0) {
		return ret;
	}

	/* Verify both paths are in same mount */
	if (old_entry != new_entry) {
		LOG_ERR("Cannot rename across different mounts");
		return -EXDEV;
	}

	/* Rename through VFS */
	ret = fs_rename(old_vfs_path, new_vfs_path);
	if (ret < 0) {
		LOG_ERR("fs_rename %s -> %s failed: %d",
		        old_vfs_path, new_vfs_path, ret);
		return ret;
	}

	LOG_DBG("Renamed %s -> %s", old_path, new_path);
	return 0;
}
