/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/union_fs.h>
#include <zephyr/9p/server.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ninep_union_fs, CONFIG_NINEP_LOG_LEVEL);

/**
 * @brief Find mount point for a given path
 *
 * Performs longest-prefix matching to find the backend that should
 * handle operations for the given path.
 *
 * @param fs Union filesystem instance
 * @param path Path to resolve
 * @param match_len Output: length of matched prefix
 * @return Pointer to mount entry, or NULL if no match
 */
static struct ninep_union_mount *find_mount_point(struct ninep_union_fs *fs,
                                                   const char *path,
                                                   size_t *match_len)
{
	struct ninep_union_mount *best_match = NULL;
	size_t longest_match = 0;
	size_t path_len = strlen(path);

	/* Iterate through all mounts to find longest prefix match */
	for (size_t i = 0; i < fs->num_mounts; i++) {
		struct ninep_union_mount *mount = &fs->mounts[i];
		size_t mount_len = strlen(mount->path);

		/* Skip if mount path is longer than target path */
		if (mount_len > path_len) {
			continue;
		}

		/* Skip if this mount path is shorter than current best match */
		if (mount_len < longest_match) {
			continue;
		}

		/* Check if mount path is a prefix of target path */
		if (strncmp(path, mount->path, mount_len) == 0) {
			/* Verify path boundary (must be '/' or end of string) */
			if (mount_len == 1 || /* root mount "/" */
			    path[mount_len] == '/' ||
			    path[mount_len] == '\0') {
				best_match = mount;
				longest_match = mount_len;
			}
		}
	}

	if (match_len && best_match) {
		*match_len = longest_match;
	}

	return best_match;
}

/**
 * @brief Get relative path within a mount point
 *
 * Given a full path and a mount point, returns the path relative
 * to that mount point.
 *
 * Example: path="/dev/led", mount="/dev" -> returns "/led"
 *          path="/files/notes/todo.txt", mount="/files" -> returns "/notes/todo.txt"
 *
 * @param path Full path
 * @param mount_path Mount point path
 * @return Relative path (always starts with '/')
 */
static const char *get_relative_path(const char *path, const char *mount_path)
{
	size_t mount_len = strlen(mount_path);

	/* Root mount - return path as-is */
	if (mount_len == 1 && mount_path[0] == '/') {
		return path;
	}

	/* Skip past mount point prefix */
	const char *rel_path = path + mount_len;

	/* If relative path is empty, return "/" (mount point root) */
	if (*rel_path == '\0') {
		return "/";
	}

	return rel_path;
}

/* Union filesystem operations - delegate to appropriate backend */

static struct ninep_fs_node *union_walk(struct ninep_fs_node *parent,
                                         const char *name, uint16_t name_len,
                                         void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* Build full path for this walk operation */
	char full_path[256];

	/* If parent is the union root, path is just "/name" */
	if (parent == fs->root) {
		snprintf(full_path, sizeof(full_path), "/%.*s", name_len, name);
	} else {
		/* Need to reconstruct full path from node context */
		/* For now, delegate to backend if node belongs to a backend */
		/* This is simplified - real implementation needs path tracking */
		LOG_ERR("Walk on non-root union node not yet implemented");
		return NULL;
	}

	/* Find which backend should handle this path */
	size_t match_len;
	struct ninep_union_mount *mount = find_mount_point(fs, full_path, &match_len);

	if (!mount) {
		LOG_DBG("No mount point found for path: %s", full_path);
		return NULL;
	}

	/* Get relative path within this mount */
	const char *rel_path = get_relative_path(full_path, mount->path);

	/* Special case: walking to mount point itself from root */
	if (strcmp(full_path, mount->path) == 0) {
		return mount->root;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->walk) {
		return NULL;
	}

	/* Skip leading '/' in relative path */
	const char *backend_path = rel_path + 1;
	uint16_t backend_path_len = strlen(backend_path);

	return mount->fs_ops->walk(mount->root, backend_path, backend_path_len,
	                           mount->fs_ctx);
}

static int union_read(struct ninep_fs_node *node, uint64_t offset,
                       uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* If reading the union root directory, synthesize directory listing */
	if (node == fs->root) {
		/* Generate synthetic directory with mount points */
		uint32_t pos = 0;

		/* Skip entries based on offset (simplified) */
		size_t start_idx = offset / 128; /* rough estimate */

		for (size_t i = start_idx; i < fs->num_mounts && pos < count; i++) {
			struct ninep_union_mount *mount = &fs->mounts[i];

			/* Skip root mount (it's synthetic) */
			if (strcmp(mount->path, "/") == 0) {
				continue;
			}

			/* Extract mount point name (skip leading '/') */
			const char *name = mount->path + 1;

			/* Simple name output (real implementation would use stat format) */
			size_t name_len = strlen(name);
			if (pos + name_len + 1 > count) {
				break;
			}

			memcpy(buf + pos, name, name_len);
			pos += name_len;
			buf[pos++] = '\n';
		}

		return pos;
	}

	/* Find which backend owns this node */
	/* For now, this is simplified - need to track backend per node */
	LOG_ERR("Read on union backend node not yet implemented");
	return -ENOTSUP;
}

static int union_stat(struct ninep_fs_node *node, uint8_t *buf,
                       size_t buf_len, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* If stat on union root, return synthetic directory */
	if (node == fs->root) {
		/* Build a stat structure (simplified encoding) */
		/* Real implementation should use proper 9P stat encoding */
		struct ninep_stat stat;
		memset(&stat, 0, sizeof(stat));
		stat.qid = node->qid;
		stat.mode = NINEP_DMDIR | 0755;
		stat.name = "/";
		stat.uid = "root";
		stat.gid = "root";
		stat.muid = "root";

		/* Encode into buffer - for now, return basic info */
		/* This is a placeholder - real encoding should use protocol.h functions */
		if (buf_len < sizeof(stat)) {
			return -ENOSPC;
		}

		memcpy(buf, &stat, sizeof(stat));
		return sizeof(stat);
	}

	/* Delegate to backend */
	LOG_ERR("Stat on union backend node not yet implemented");
	return -ENOTSUP;
}

/* Get union filesystem root */
static struct ninep_fs_node *union_get_root(void *ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)ctx;
	return fs->root;
}

/* Union filesystem operations table */
static const struct ninep_fs_ops union_fs_ops = {
	.get_root = union_get_root,
	.walk = union_walk,
	.read = union_read,
	.stat = union_stat,
	/* Other operations would be delegated similarly */
};

/* Public API */

int ninep_union_fs_init(struct ninep_union_fs *fs,
                         struct ninep_union_mount *mounts,
                         size_t max_mounts)
{
	if (!fs || !mounts) {
		return -EINVAL;
	}

	memset(fs, 0, sizeof(*fs));
	fs->mounts = mounts;
	fs->max_mounts = max_mounts;
	fs->num_mounts = 0;
	fs->next_qid_path = 1;

	/* Create synthetic root node */
	fs->root = k_malloc(sizeof(struct ninep_fs_node));
	if (!fs->root) {
		return -ENOMEM;
	}

	memset(fs->root, 0, sizeof(*fs->root));
	fs->root->qid.type = NINEP_QTDIR;
	fs->root->qid.version = 0;
	fs->root->qid.path = fs->next_qid_path++;

	LOG_INF("Union filesystem initialized (max mounts: %zu)", max_mounts);
	return 0;
}

int ninep_union_fs_mount(struct ninep_union_fs *fs,
                          const char *path,
                          const struct ninep_fs_ops *fs_ops,
                          void *fs_ctx)
{
	if (!fs || !path || !fs_ops) {
		return -EINVAL;
	}

	if (fs->num_mounts >= fs->max_mounts) {
		LOG_ERR("Maximum mounts reached (%zu)", fs->max_mounts);
		return -ENOSPC;
	}

	/* Check for duplicate mount point */
	for (size_t i = 0; i < fs->num_mounts; i++) {
		if (strcmp(fs->mounts[i].path, path) == 0) {
			LOG_ERR("Mount point already exists: %s", path);
			return -EEXIST;
		}
	}

	/* Add new mount */
	struct ninep_union_mount *mount = &fs->mounts[fs->num_mounts];
	mount->path = path;
	mount->fs_ops = fs_ops;
	mount->fs_ctx = fs_ctx;

	/* Get root node from backend */
	if (!fs_ops->get_root) {
		LOG_ERR("Backend does not provide get_root operation");
		return -ENOTSUP;
	}

	mount->root = fs_ops->get_root(fs_ctx);
	if (!mount->root) {
		LOG_ERR("Backend get_root returned NULL");
		return -EINVAL;
	}

	fs->num_mounts++;

	LOG_INF("Mounted backend at '%s' (%zu/%zu mounts)",
	        path, fs->num_mounts, fs->max_mounts);

	return 0;
}

const struct ninep_fs_ops *ninep_union_fs_get_ops(void)
{
	return &union_fs_ops;
}
