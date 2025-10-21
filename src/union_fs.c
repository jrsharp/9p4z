/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/union_fs.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/protocol.h>
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

/**
 * @brief Find which backend mount owns a given node
 *
 * @param fs Union filesystem instance
 * @param node Node to look up
 * @return Pointer to owning mount, or NULL if node is union root or not found
 */
/**
 * @brief Register a node as being owned by a specific mount
 */
static void unregister_node_owner(struct ninep_union_fs *fs,
                                   struct ninep_fs_node *node)
{
	/* Find and remove the node from tracking */
	for (size_t i = 0; i < fs->num_node_owners; i++) {
		if (fs->node_owners[i].node == node) {
			LOG_DBG("Unregistering node=%p name='%s' (was at index %zu)",
			        node, node->name, i);

			/* Shift remaining entries down */
			for (size_t j = i; j < fs->num_node_owners - 1; j++) {
				fs->node_owners[j] = fs->node_owners[j + 1];
			}
			fs->num_node_owners--;
			return;
		}
	}
}

static void register_node_owner(struct ninep_union_fs *fs,
                                 struct ninep_fs_node *node,
                                 struct ninep_union_mount *mount)
{
	/* Check if already registered */
	for (size_t i = 0; i < fs->num_node_owners; i++) {
		if (fs->node_owners[i].node == node) {
			/* Already registered, update mount */
			fs->node_owners[i].mount = mount;
			return;
		}
	}

	/* Add new entry if space available */
	if (fs->num_node_owners < ARRAY_SIZE(fs->node_owners)) {
		fs->node_owners[fs->num_node_owners].node = node;
		fs->node_owners[fs->num_node_owners].mount = mount;
		fs->num_node_owners++;
		LOG_DBG("Registered node=%p name='%s' -> mount '%s' (total=%zu)",
		        node, node->name, mount->path, fs->num_node_owners);
	} else {
		LOG_WRN("Node ownership table full! Cannot track node=%p", node);
	}
}

static struct ninep_union_mount *find_node_owner(struct ninep_union_fs *fs,
                                                   struct ninep_fs_node *node)
{
	LOG_DBG("find_node_owner: looking for node=%p name='%s'", node, node->name);

	/* Union root doesn't belong to any backend */
	if (node == fs->root) {
		LOG_DBG("  Node is union root, returning NULL");
		return NULL;
	}

	/* First check the ownership tracking table */
	for (size_t i = 0; i < fs->num_node_owners; i++) {
		if (fs->node_owners[i].node == node) {
			LOG_DBG("  Found in tracking table -> mount '%s'",
			        fs->node_owners[i].mount->path);
			return fs->node_owners[i].mount;
		}
	}

	/* Check each mount to see if this node is a mount root */
	for (size_t i = 0; i < fs->num_mounts; i++) {
		struct ninep_union_mount *mount = &fs->mounts[i];

		LOG_DBG("  Checking mount[%zu]: path='%s' root=%p", i, mount->path, mount->root);

		/* Check if node is the mount's root */
		if (node == mount->root) {
			LOG_DBG("  MATCH! Node is root of mount '%s'", mount->path);
			return mount;
		}
	}

	/* Not found in tracking table or mount roots */
	LOG_WRN("  No owner found for node=%p name='%s'!", node, node->name);
	return NULL;
}

/* Union filesystem operations - delegate to appropriate backend */

static struct ninep_fs_node *union_walk(struct ninep_fs_node *parent,
                                         const char *name, uint16_t name_len,
                                         void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* If parent is the union root, we need to figure out which backend to use */
	if (parent == fs->root) {
		/* Build full path */
		char full_path[256];
		snprintf(full_path, sizeof(full_path), "/%.*s", name_len, name);

		/* First check if this is an exact mount point match */
		for (size_t i = 0; i < fs->num_mounts; i++) {
			if (strcmp(fs->mounts[i].path, full_path) == 0) {
				/* Walking directly to a mount point - return its root */
				LOG_DBG("Walk matched mount point '%s', returning root=%p name='%s'",
				        full_path, fs->mounts[i].root,
				        fs->mounts[i].root ? fs->mounts[i].root->name : "NULL");
				/* Mount roots are implicitly owned by their mount, no need to register */
				return fs->mounts[i].root;
			}
		}

		/* Not an exact mount point - find which backend should handle it */
		size_t match_len;
		struct ninep_union_mount *mount = find_mount_point(fs, full_path, &match_len);

		if (!mount) {
			LOG_DBG("No mount point found for path: %s", full_path);
			return NULL;
		}

		/* If matched the "/" mount, delegate to it */
		if (strcmp(mount->path, "/") == 0) {
			struct ninep_fs_node *node = mount->fs_ops->walk(mount->root, name, name_len,
			                                                   mount->fs_ctx);
			if (node) {
				register_node_owner(fs, node, mount);
			}
			return node;
		}

		/* For other mounts, get relative path */
		const char *rel_path = get_relative_path(full_path, mount->path);

		/* Delegate to backend */
		if (!mount->fs_ops->walk) {
			return NULL;
		}

		/* Skip leading '/' in relative path */
		const char *backend_path = rel_path + 1;
		uint16_t backend_path_len = strlen(backend_path);

		struct ninep_fs_node *node = mount->fs_ops->walk(mount->root, backend_path,
		                                                   backend_path_len, mount->fs_ctx);
		if (node) {
			register_node_owner(fs, node, mount);
		}
		return node;
	} else {
		/* Parent is not union root - delegate to the backend that owns it */
		struct ninep_union_mount *mount = find_node_owner(fs, parent);

		if (!mount) {
			LOG_ERR("Cannot find owner for parent node");
			return NULL;
		}

		/* Delegate walk to backend */
		if (!mount->fs_ops->walk) {
			LOG_ERR("Backend does not support walk");
			return NULL;
		}

		struct ninep_fs_node *node = mount->fs_ops->walk(parent, name, name_len,
		                                                   mount->fs_ctx);
		if (node) {
			register_node_owner(fs, node, mount);
		}
		return node;
	}
}

static int union_read(struct ninep_fs_node *node, uint64_t offset,
                       uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* If reading the union root directory */
	if (node == fs->root) {
		/* Check if there's a backend mounted at "/" */
		struct ninep_union_mount *root_mount = NULL;
		int root_mount_idx = -1;
		for (size_t i = 0; i < fs->num_mounts; i++) {
			if (strcmp(fs->mounts[i].path, "/") == 0) {
				root_mount = &fs->mounts[i];
				root_mount_idx = i;
				break;
			}
		}

		/* Count non-root mounts */
		size_t num_other_mounts = 0;
		for (size_t i = 0; i < fs->num_mounts; i++) {
			if (strcmp(fs->mounts[i].path, "/") != 0) {
				num_other_mounts++;
			}
		}

		/* If there's a root mount AND no other mounts, just delegate */
		if (root_mount && num_other_mounts == 0 && root_mount->fs_ops->read) {
			return root_mount->fs_ops->read(root_mount->root, offset,
			                                 buf, count, root_mount->fs_ctx);
		}

		/* If there's a root mount AND other mounts, we need to merge listings */
		if (root_mount && num_other_mounts > 0) {
			/* First, get the "/" mount's entries */
			int ret = root_mount->fs_ops->read(root_mount->root, offset,
			                                    buf, count, root_mount->fs_ctx);

			if (ret < 0) {
				return ret;
			}

			/* Only append mount points on the FIRST read (offset == 0) or
			 * when sysfs still has data. This prevents infinite loops where
			 * we keep appending mount points on every paginated read. */
			if (offset == 0 && ret > 0) {
				size_t buf_offset = ret;  /* Bytes used by "/" mount's entries */

				/* Now append stat entries for mount points */
				for (size_t i = 0; i < fs->num_mounts; i++) {
					struct ninep_union_mount *mount = &fs->mounts[i];

					/* Skip root mount */
					if (strcmp(mount->path, "/") == 0) {
						continue;
					}

					/* Extract mount point name (skip leading '/') */
					const char *name = mount->path + 1;
					uint16_t name_len = strlen(name);

					/* Create a synthetic QID for this mount point */
					struct ninep_qid mount_qid = {
						.type = NINEP_QTDIR,
						.version = 0,
						.path = (uint64_t)mount  /* Use mount address as unique path */
					};

					/* Write stat structure for mount point directory */
					size_t write_offset = 0;
					int write_ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
					                                  &write_offset, &mount_qid,
					                                  0755 | NINEP_DMDIR,  /* Directory mode */
					                                  0,  /* length */
					                                  name, name_len);

					if (write_ret < 0) {
						/* No more space - return what we have */
						break;
					}

					buf_offset += write_offset;
				}

				return buf_offset;
			}

			/* For subsequent reads (offset > 0), just return what sysfs gives us */
			return ret;
		}

		/* No root mount - synthesize directory with mount points only */
		uint32_t pos = 0;

		/* Skip entries based on offset (simplified) */
		size_t start_idx = offset / 128; /* rough estimate */

		for (size_t i = start_idx; i < fs->num_mounts && pos < count; i++) {
			struct ninep_union_mount *mount = &fs->mounts[i];

			/* Skip root mount (already handled above) */
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

	/* Find which backend owns this node and delegate */
	struct ninep_union_mount *mount = find_node_owner(fs, node);

	if (!mount) {
		LOG_ERR("Cannot find owner for node '%s' (qid.path=%llu)",
		        node->name, node->qid.path);
		/* Debug: print all mount roots */
		for (size_t i = 0; i < fs->num_mounts; i++) {
			LOG_ERR("  Mount[%zu]: path='%s' root=%p root->name='%s'",
			        i, fs->mounts[i].path, fs->mounts[i].root,
			        fs->mounts[i].root ? fs->mounts[i].root->name : "NULL");
		}
		LOG_ERR("  Node we're looking for: %p", node);
		return -ENOENT;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->read) {
		LOG_ERR("Backend does not support read");
		return -ENOTSUP;
	}

	LOG_DBG("Delegating read to mount '%s' for node '%s'",
	        mount->path, node->name);
	return mount->fs_ops->read(node, offset, buf, count, mount->fs_ctx);
}

static int union_stat(struct ninep_fs_node *node, uint8_t *buf,
                       size_t buf_len, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* If stat on union root */
	if (node == fs->root) {
		/* Check if there's a backend mounted at "/" */
		struct ninep_union_mount *root_mount = NULL;
		for (size_t i = 0; i < fs->num_mounts; i++) {
			if (strcmp(fs->mounts[i].path, "/") == 0) {
				root_mount = &fs->mounts[i];
				break;
			}
		}

		/* If there's a root mount, delegate directly to it */
		if (root_mount && root_mount->fs_ops->stat) {
			return root_mount->fs_ops->stat(root_mount->root, buf,
			                                 buf_len, root_mount->fs_ctx);
		}

		/* Otherwise, return synthetic directory stat */
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

	/* Find which backend owns this node and delegate */
	struct ninep_union_mount *mount = find_node_owner(fs, node);

	if (!mount) {
		LOG_ERR("Cannot find owner for node");
		return -ENOENT;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->stat) {
		LOG_ERR("Backend does not support stat");
		return -ENOTSUP;
	}

	return mount->fs_ops->stat(node, buf, buf_len, mount->fs_ctx);
}

/* Get union filesystem root */
static struct ninep_fs_node *union_get_root(void *ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)ctx;
	return fs->root;
}

static int union_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* If opening the union root directory */
	if (node == fs->root) {
		/* Check if there's a backend mounted at "/" */
		struct ninep_union_mount *root_mount = NULL;
		for (size_t i = 0; i < fs->num_mounts; i++) {
			if (strcmp(fs->mounts[i].path, "/") == 0) {
				root_mount = &fs->mounts[i];
				break;
			}
		}

		/* If there's a root mount, delegate directly to it */
		if (root_mount && root_mount->fs_ops->open) {
			return root_mount->fs_ops->open(root_mount->root, mode,
			                                 root_mount->fs_ctx);
		}

		/* Otherwise, allow opening synthetic root directory */
		return 0;
	}

	/* Find which backend owns this node */
	struct ninep_union_mount *mount = find_node_owner(fs, node);

	if (!mount) {
		LOG_ERR("Cannot find owner for node");
		return -ENOENT;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->open) {
		LOG_ERR("Backend does not support open");
		return -ENOTSUP;
	}

	return mount->fs_ops->open(node, mode, mount->fs_ctx);
}

static int union_write(struct ninep_fs_node *node, uint64_t offset,
                        const uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* Find which backend owns this node */
	struct ninep_union_mount *mount = find_node_owner(fs, node);

	if (!mount) {
		LOG_ERR("Cannot write to union root directory");
		return -EISDIR;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->write) {
		LOG_ERR("Backend does not support write");
		return -ENOTSUP;
	}

	return mount->fs_ops->write(node, offset, buf, count, mount->fs_ctx);
}

static int union_create(struct ninep_fs_node *parent, const char *name,
                         uint16_t name_len, uint32_t perm, uint8_t mode,
                         struct ninep_fs_node **new_node, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* Find which backend owns the parent node */
	struct ninep_union_mount *mount = find_node_owner(fs, parent);

	if (!mount) {
		LOG_ERR("Cannot create in union root directory");
		return -EPERM;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->create) {
		LOG_ERR("Backend does not support create");
		return -ENOTSUP;
	}

	int ret = mount->fs_ops->create(parent, name, name_len, perm, mode,
	                                  new_node, mount->fs_ctx);

	/* Register the newly created node so subsequent operations can find its owner */
	if (ret == 0 && new_node && *new_node) {
		register_node_owner(fs, *new_node, mount);
	}

	return ret;
}

static int union_remove(struct ninep_fs_node *node, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* Find which backend owns this node */
	struct ninep_union_mount *mount = find_node_owner(fs, node);

	if (!mount) {
		LOG_ERR("Cannot remove union root directory");
		return -EPERM;
	}

	/* Delegate to backend */
	if (!mount->fs_ops->remove) {
		LOG_ERR("Backend does not support remove");
		return -ENOTSUP;
	}

	return mount->fs_ops->remove(node, mount->fs_ctx);
}

static int union_clunk(struct ninep_fs_node *node, void *fs_ctx)
{
	struct ninep_union_fs *fs = (struct ninep_union_fs *)fs_ctx;

	/* Don't clunk the union root itself */
	if (node == fs->root) {
		return 0;
	}

	/* Find which backend owns this node */
	struct ninep_union_mount *mount = find_node_owner(fs, node);

	if (!mount) {
		LOG_WRN("Clunking node with no owner: %p", node);
		return -EINVAL;
	}

	/* Unregister from tracking BEFORE delegating (backend may free the node) */
	unregister_node_owner(fs, node);

	/* Delegate to backend if it has a clunk handler */
	if (mount->fs_ops->clunk) {
		return mount->fs_ops->clunk(node, mount->fs_ctx);
	}

	return 0;
}

/* Union filesystem operations table */
static const struct ninep_fs_ops union_fs_ops = {
	.get_root = union_get_root,
	.walk = union_walk,
	.open = union_open,
	.read = union_read,
	.write = union_write,
	.stat = union_stat,
	.create = union_create,
	.remove = union_remove,
	.clunk = union_clunk,
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
