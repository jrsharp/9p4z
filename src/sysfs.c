/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_sysfs, CONFIG_NINEP_LOG_LEVEL);

/* Node cache for dynamically created nodes */
#define SYSFS_NODE_CACHE_SIZE 32

struct sysfs_node_cache {
	struct ninep_fs_node nodes[SYSFS_NODE_CACHE_SIZE];
	bool in_use[SYSFS_NODE_CACHE_SIZE];
};

static struct sysfs_node_cache node_cache;

/* Helper: Allocate a node from cache */
static struct ninep_fs_node *alloc_node(struct ninep_sysfs *sysfs,
                                         const char *name,
                                         bool is_dir)
{
	for (int i = 0; i < SYSFS_NODE_CACHE_SIZE; i++) {
		if (!node_cache.in_use[i]) {
			struct ninep_fs_node *node = &node_cache.nodes[i];

			memset(node, 0, sizeof(*node));
			strncpy(node->name, name, sizeof(node->name) - 1);
			node->type = is_dir ? NINEP_NODE_DIR : NINEP_NODE_FILE;
			node->mode = is_dir ? (0755 | NINEP_DMDIR) : 0444;
			node->qid.path = sysfs->next_qid_path++;
			node->qid.version = 0;
			node->qid.type = is_dir ? NINEP_QTDIR : NINEP_QTFILE;
			node_cache.in_use[i] = true;

			return node;
		}
	}

	return NULL;
}

/* Helper: Find entry by path */
static struct ninep_sysfs_entry *find_entry(struct ninep_sysfs *sysfs,
                                              const char *path)
{
	for (size_t i = 0; i < sysfs->num_entries; i++) {
		if (strcmp(sysfs->entries[i].path, path) == 0) {
			return &sysfs->entries[i];
		}
	}

	return NULL;
}

/* Helper: Check if path is a child of parent_path */
static bool is_child_of(const char *path, const char *parent_path,
                         char *child_name, size_t child_name_size)
{
	size_t parent_len = strlen(parent_path);

	/* Root directory special case */
	if (strcmp(parent_path, "/") == 0) {
		parent_len = 0;
	}

	/* Check if path starts with parent */
	if (strncmp(path, parent_path, parent_len) != 0) {
		return false;
	}

	/* Skip parent path and any leading slash */
	const char *remainder = path + parent_len;
	if (*remainder == '/') {
		remainder++;
	}

	/* Find the next slash (or end of string) */
	const char *next_slash = strchr(remainder, '/');
	size_t name_len;

	if (next_slash) {
		name_len = next_slash - remainder;
	} else {
		name_len = strlen(remainder);
	}

	/* Copy child name */
	if (name_len > 0 && name_len < child_name_size) {
		strncpy(child_name, remainder, name_len);
		child_name[name_len] = '\0';
		return true;
	}

	return false;
}

/* Get root */
static struct ninep_fs_node *sysfs_get_root(void *fs_ctx)
{
	struct ninep_sysfs *sysfs = fs_ctx;

	return sysfs->root;
}

/* Walk to child */
static struct ninep_fs_node *sysfs_walk(struct ninep_fs_node *parent,
                                          const char *name, uint16_t name_len,
                                          void *fs_ctx)
{
	struct ninep_sysfs *sysfs = fs_ctx;
	char parent_path[256];
	char target_path[256];
	char child_match[64];

	/* Build parent path */
	if (strcmp(parent->name, "/") == 0) {
		parent_path[0] = '\0';
	} else {
		strncpy(parent_path, parent->name, sizeof(parent_path) - 1);
	}

	/* Build target path */
	snprintf(target_path, sizeof(target_path), "%s/%.*s",
	         parent_path, name_len, name);

	LOG_DBG("Walking: parent='%s', name='%.*s', target='%s'",
	        parent->name, name_len, name, target_path);

	/* Look for exact match first */
	struct ninep_sysfs_entry *entry = find_entry(sysfs, target_path);

	if (entry) {
		/* Found exact entry - create node */
		struct ninep_fs_node *node = alloc_node(sysfs, target_path,
		                                          entry->is_dir);
		if (!node) {
			LOG_ERR("Node cache full");
			return NULL;
		}

		return node;
	}

	/* Check if this is a directory that has children */
	for (size_t i = 0; i < sysfs->num_entries; i++) {
		if (is_child_of(sysfs->entries[i].path, target_path,
		                child_match, sizeof(child_match))) {
			/* This path would be a parent - create directory node */
			struct ninep_fs_node *node = alloc_node(sysfs, target_path, true);
			if (!node) {
				LOG_ERR("Node cache full");
				return NULL;
			}
			return node;
		}
	}

	LOG_DBG("Path not found: %s", target_path);
	return NULL;
}

/* Open node */
static int sysfs_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	/* Sysfs files are read-only */
	if (mode != NINEP_OREAD && mode != NINEP_OEXEC) {
		return -EACCES;
	}

	return 0;
}

/* Read from file */
static int sysfs_read(struct ninep_fs_node *node, uint64_t offset,
                      uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct ninep_sysfs *sysfs = fs_ctx;

	if (node->type == NINEP_NODE_DIR) {
		/* Read directory - list children */
		LOG_DBG("Reading directory: %s, offset=%llu", node->name, offset);

		char parent_path[256];
		strncpy(parent_path, node->name, sizeof(parent_path) - 1);
		parent_path[sizeof(parent_path) - 1] = '\0';

		char child_names[32][64];  /* Track unique child names */
		int num_children = 0;

		/* Find all unique immediate children */
		for (size_t i = 0; i < sysfs->num_entries; i++) {
			char child_name[64];

			if (is_child_of(sysfs->entries[i].path, parent_path,
			                child_name, sizeof(child_name))) {
				/* Check if we've seen this child */
				bool found = false;
				for (int j = 0; j < num_children; j++) {
					if (strcmp(child_names[j], child_name) == 0) {
						found = true;
						break;
					}
				}

				if (!found && num_children < 32) {
					strncpy(child_names[num_children], child_name, 64);
					num_children++;
				}
			}
		}

		/* Handle offset - skip entries we've already sent */
		uint64_t current_offset = 0;
		size_t buf_offset = 0;

		/* Generate directory entries */
		for (int i = 0; i < num_children; i++) {
			char child_path[256];
			snprintf(child_path, sizeof(child_path), "%s/%s",
			         parent_path, child_names[i]);

			struct ninep_sysfs_entry *child_entry = find_entry(sysfs, child_path);
			bool is_dir = child_entry ? child_entry->is_dir : true;

			/* Build qid for this child */
			struct ninep_qid child_qid;
			child_qid.type = is_dir ? NINEP_QTDIR : NINEP_QTFILE;
			child_qid.version = 0;
			/* Generate unique qid.path based on path string hash */
			child_qid.path = 0;
			for (const char *p = child_path; *p; p++) {
				child_qid.path = child_qid.path * 31 + *p;
			}

			/* Calculate stat entry size:
			 * size[2] + type[2] + dev[4] + qid[13] + mode[4] + atime[4] +
			 * mtime[4] + length[8] + name[2+len] + uid[2+6] + gid[2+6] + muid[2+6]
			 */
			uint16_t name_len = strlen(child_names[i]);
			size_t stat_size = 2 + 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                   (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);

			/* Skip if this entry is before our requested offset */
			if (current_offset + stat_size <= offset) {
				current_offset += stat_size;
				continue;
			}

			/* Check if we have space in the buffer */
			if (buf_offset + stat_size > count) {
				/* No more space */
				break;
			}

			/* Write the stat structure */
			size_t write_offset = 0;
			int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
			                           &write_offset, &child_qid,
			                           is_dir ? (0755 | NINEP_DMDIR) : 0444,
			                           0,  /* length */
			                           child_names[i], name_len);

			if (ret < 0) {
				/* Error writing stat */
				break;
			}

			buf_offset += write_offset;
			current_offset += write_offset;
		}

		LOG_DBG("Directory read: %d children, %zu bytes", num_children, buf_offset);
		return buf_offset;
	} else {
		/* Read file - call generator */
		struct ninep_sysfs_entry *entry = find_entry(sysfs, node->name);

		if (!entry || !entry->generator) {
			return -EIO;
		}

		int ret = entry->generator(buf, count, offset, entry->ctx);
		LOG_DBG("File read: %s, offset=%llu, count=%u, ret=%d",
		        node->name, offset, count, ret);
		return ret;
	}
}

/* Get stat */
static int sysfs_stat(struct ninep_fs_node *node, uint8_t *buf,
                      size_t buf_len, void *fs_ctx)
{
	size_t offset = 0;
	uint16_t name_len = strlen(node->name);

	LOG_DBG("sysfs_stat: name='%s', len=%u, mode=0x%x",
	        node->name, name_len, node->mode);

	int ret = ninep_write_stat(buf, buf_len, &offset, &node->qid,
	                           node->mode, node->length,
	                           node->name, name_len);
	if (ret < 0) {
		LOG_ERR("ninep_write_stat failed: %d", ret);
		return ret;
	}

	LOG_DBG("sysfs_stat returning %zu bytes", offset);
	return offset;  /* Return number of bytes written */
}

/* Filesystem operations */
static const struct ninep_fs_ops sysfs_ops = {
	.get_root = sysfs_get_root,
	.walk = sysfs_walk,
	.open = sysfs_open,
	.read = sysfs_read,
	.write = NULL,  /* Read-only */
	.stat = sysfs_stat,
	.create = NULL,
	.remove = NULL,
};

/* Public API */

int ninep_sysfs_init(struct ninep_sysfs *sysfs,
                     struct ninep_sysfs_entry *entries,
                     size_t max_entries)
{
	if (!sysfs || !entries) {
		return -EINVAL;
	}

	memset(sysfs, 0, sizeof(*sysfs));
	memset(&node_cache, 0, sizeof(node_cache));

	sysfs->entries = entries;
	sysfs->num_entries = 0;
	sysfs->max_entries = max_entries;
	sysfs->next_qid_path = 1;

	/* Create root node */
	sysfs->root = alloc_node(sysfs, "/", true);
	if (!sysfs->root) {
		return -ENOMEM;
	}

	LOG_INF("Sysfs initialized (max_entries=%zu)", max_entries);
	return 0;
}

int ninep_sysfs_register_file(struct ninep_sysfs *sysfs,
                               const char *path,
                               ninep_sysfs_generator_t generator,
                               void *ctx)
{
	if (!sysfs || !path || !generator) {
		return -EINVAL;
	}

	if (sysfs->num_entries >= sysfs->max_entries) {
		return -ENOMEM;
	}

	struct ninep_sysfs_entry *entry = &sysfs->entries[sysfs->num_entries];

	entry->path = path;
	entry->generator = generator;
	entry->ctx = ctx;
	entry->is_dir = false;

	sysfs->num_entries++;

	LOG_DBG("Registered file: %s", path);
	return 0;
}

int ninep_sysfs_register_dir(struct ninep_sysfs *sysfs, const char *path)
{
	if (!sysfs || !path) {
		return -EINVAL;
	}

	if (sysfs->num_entries >= sysfs->max_entries) {
		return -ENOMEM;
	}

	struct ninep_sysfs_entry *entry = &sysfs->entries[sysfs->num_entries];

	entry->path = path;
	entry->generator = NULL;
	entry->ctx = NULL;
	entry->is_dir = true;

	sysfs->num_entries++;

	LOG_DBG("Registered directory: %s", path);
	return 0;
}

const struct ninep_fs_ops *ninep_sysfs_get_ops(void)
{
	return &sysfs_ops;
}
