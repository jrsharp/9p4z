/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/passthrough_fs.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_passthrough_fs, CONFIG_NINEP_LOG_LEVEL);

/* Extended node data: stores full path */
struct node_data {
	char path[256];  /* Full path from mount point */
};

/* Helper to allocate node with path */
static struct ninep_fs_node *alloc_node(struct ninep_passthrough_fs *fs,
                                         const char *name,
                                         const char *full_path,
                                         enum ninep_node_type type,
                                         uint32_t mode,
                                         uint64_t length)
{
	struct ninep_fs_node *node = k_malloc(sizeof(struct ninep_fs_node));
	if (!node) {
		return NULL;
	}

	struct node_data *data = k_malloc(sizeof(struct node_data));
	if (!data) {
		k_free(node);
		return NULL;
	}

	memset(node, 0, sizeof(*node));
	memset(data, 0, sizeof(*data));

	strncpy(node->name, name, sizeof(node->name) - 1);
	strncpy(data->path, full_path, sizeof(data->path) - 1);

	node->type = type;
	node->mode = mode;
	node->length = length;
	node->data = data;

	node->qid.path = fs->next_qid_path++;
	node->qid.version = 0;
	node->qid.type = (type == NINEP_NODE_DIR) ? NINEP_QTDIR : NINEP_QTFILE;

	LOG_DBG("Allocated node: name='%s' path='%s' type=%d qid.path=%llu",
	        name, data->path, type, node->qid.path);

	return node;
}

/* Helper to free node */
static void free_node(struct ninep_fs_node *node)
{
	if (!node) {
		return;
	}

	if (node->data) {
		k_free(node->data);
	}
	k_free(node);
}

/* Get full path from node */
static const char *get_node_path(struct ninep_fs_node *node)
{
	if (!node || !node->data) {
		return NULL;
	}
	return ((struct node_data *)node->data)->path;
}

/* Build full path for child */
static int build_child_path(const char *parent_path, const char *child_name,
                            char *out_path, size_t out_size)
{
	/* Handle root case */
	if (strcmp(parent_path, "/") == 0) {
		snprintf(out_path, out_size, "/%s", child_name);
	} else {
		snprintf(out_path, out_size, "%s/%s", parent_path, child_name);
	}
	return 0;
}

/* Get root */
static struct ninep_fs_node *passthrough_get_root(void *fs_ctx)
{
	struct ninep_passthrough_fs *fs = fs_ctx;
	return fs->root;
}

/* Walk to child */
static struct ninep_fs_node *passthrough_walk(struct ninep_fs_node *parent,
                                                const char *name, uint16_t name_len,
                                                void *fs_ctx)
{
	struct ninep_passthrough_fs *fs = fs_ctx;

	if (!parent || parent->type != NINEP_NODE_DIR) {
		LOG_ERR("Walk failed: parent is not a directory");
		return NULL;
	}

	const char *parent_path = get_node_path(parent);
	if (!parent_path) {
		LOG_ERR("Walk failed: parent has no path");
		return NULL;
	}

	/* Build child path */
	char child_path[256];
	char child_name[name_len + 1];
	memcpy(child_name, name, name_len);
	child_name[name_len] = '\0';

	build_child_path(parent_path, child_name, child_path, sizeof(child_path));

	/* Build full filesystem path (mount_point + child_path) */
	char fs_path[256];
	snprintf(fs_path, sizeof(fs_path), "%s%s", fs->mount_point, child_path);

	LOG_DBG("Walk: looking for '%s' in '%s' -> fs_path='%s'",
	        child_name, parent_path, fs_path);

	/* Stat the path to see if it exists */
	struct fs_dirent entry;
	int ret = fs_stat(fs_path, &entry);
	if (ret < 0) {
		LOG_DBG("Walk failed: fs_stat returned %d", ret);
		return NULL;
	}

	/* Create node for this path */
	enum ninep_node_type type = (entry.type == FS_DIR_ENTRY_DIR) ?
	                             NINEP_NODE_DIR : NINEP_NODE_FILE;
	uint32_t mode = (entry.type == FS_DIR_ENTRY_DIR) ? 0755 : 0644;

	struct ninep_fs_node *node = alloc_node(fs, child_name, child_path,
	                                          type, mode, entry.size);
	if (!node) {
		LOG_ERR("Walk failed: could not allocate node");
		return NULL;
	}

	return node;
}

/* Open node */
static int passthrough_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	/* Zephyr FS doesn't require explicit open for stat/read operations */
	/* We'll open files on-demand during read/write */
	LOG_DBG("Open: node='%s' mode=%u", node->name, mode);
	return 0;
}

/* Read from file or directory */
static int passthrough_read(struct ninep_fs_node *node, uint64_t offset,
                             uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct ninep_passthrough_fs *fs = fs_ctx;
	const char *node_path = get_node_path(node);
	if (!node_path) {
		return -EINVAL;
	}

	/* Build full filesystem path */
	char fs_path[256];
	snprintf(fs_path, sizeof(fs_path), "%s%s", fs->mount_point, node_path);

	if (node->type == NINEP_NODE_DIR) {
		/* Read directory entries */
		LOG_DBG("Reading directory: '%s' (offset=%llu, count=%u)", fs_path, offset, count);

		struct fs_dir_t dir;
		fs_dir_t_init(&dir);

		int ret = fs_opendir(&dir, fs_path);
		if (ret < 0) {
			LOG_ERR("fs_opendir failed: %d", ret);
			return ret;
		}

		size_t buf_offset = 0;
		uint64_t current_offset = 0;
		int entry_count = 0;

		/* Iterate through directory entries */
		while (true) {
			struct fs_dirent entry;
			ret = fs_readdir(&dir, &entry);
			if (ret < 0) {
				LOG_ERR("fs_readdir failed: %d", ret);
				fs_closedir(&dir);
				return ret;
			}

			/* End of directory */
			if (entry.name[0] == '\0') {
				break;
			}

			/* Skip . and .. */
			if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
				continue;
			}

			LOG_DBG("  Entry: %s (type=%d, size=%zu)",
			        entry.name, entry.type, entry.size);

			/* Build QID for this entry */
			struct ninep_qid entry_qid = {
				.type = (entry.type == FS_DIR_ENTRY_DIR) ? NINEP_QTDIR : NINEP_QTFILE,
				.version = 0,
				.path = fs->next_qid_path++
			};

			/* Build mode */
			uint32_t mode = (entry.type == FS_DIR_ENTRY_DIR) ? 0755 : 0644;
			if (entry.type == FS_DIR_ENTRY_DIR) {
				mode |= NINEP_DMDIR;
			}

			/* Calculate stat size using ninep_write_stat's format:
			 * type[2] + dev[4] + qid[13] + mode[4] + atime[4] + mtime[4] +
			 * length[8] + name[2+len] + uid[2+6] + gid[2+6] + muid[2+6]
			 */
			uint16_t name_len = strlen(entry.name);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);

			/* Check if this entry is past the requested offset */
			if (current_offset >= offset) {
				/* Check if we have space */
				LOG_DBG("    Checking space: buf_offset=%zu + stat_size=%u > count=%u?",
				        buf_offset, stat_size, count);
				if (buf_offset + stat_size > count) {
					LOG_DBG("    NO SPACE! Breaking.");
					break;
				}

				/* Write stat structure using helper */
				size_t write_offset = 0;
				int write_ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                                  &write_offset, &entry_qid, mode,
				                                  entry.size, entry.name, name_len);

				if (write_ret < 0) {
					LOG_ERR("ninep_write_stat failed: %d", write_ret);
					break;
				}

				buf_offset += write_offset;
				current_offset += write_offset;
				entry_count++;
			} else {
				/* Skip this entry - update offset */
				current_offset += stat_size;
			}
		}

		fs_closedir(&dir);
		LOG_DBG("Directory read complete: %d entries, %zu bytes", entry_count, buf_offset);
		return buf_offset;

	} else {
		/* Read file content */
		LOG_DBG("Reading file: '%s' offset=%llu count=%u", fs_path, offset, count);

		struct fs_file_t file;
		fs_file_t_init(&file);

		int ret = fs_open(&file, fs_path, FS_O_READ);
		if (ret < 0) {
			LOG_ERR("fs_open failed: %d", ret);
			return ret;
		}

		/* Seek to offset */
		ret = fs_seek(&file, offset, FS_SEEK_SET);
		if (ret < 0) {
			LOG_ERR("fs_seek failed: %d", ret);
			fs_close(&file);
			return ret;
		}

		/* Read data */
		ssize_t bytes_read = fs_read(&file, buf, count);
		fs_close(&file);

		if (bytes_read < 0) {
			LOG_ERR("fs_read failed: %zd", bytes_read);
			return bytes_read;
		}

		LOG_DBG("Read %zd bytes from file", bytes_read);
		return bytes_read;
	}
}

/* Write to file */
static int passthrough_write(struct ninep_fs_node *node, uint64_t offset,
                              const uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct ninep_passthrough_fs *fs = fs_ctx;
	const char *node_path = get_node_path(node);
	if (!node_path) {
		return -EINVAL;
	}

	if (node->type == NINEP_NODE_DIR) {
		return -EISDIR;
	}

	/* Build full filesystem path */
	char fs_path[256];
	snprintf(fs_path, sizeof(fs_path), "%s%s", fs->mount_point, node_path);

	LOG_DBG("Writing to file: '%s' offset=%llu count=%u", fs_path, offset, count);

	struct fs_file_t file;
	fs_file_t_init(&file);

	int ret = fs_open(&file, fs_path, FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("fs_open failed: %d", ret);
		return ret;
	}

	/* Seek to offset */
	ret = fs_seek(&file, offset, FS_SEEK_SET);
	if (ret < 0) {
		LOG_ERR("fs_seek failed: %d", ret);
		fs_close(&file);
		return ret;
	}

	/* Write data */
	ssize_t bytes_written = fs_write(&file, buf, count);
	fs_close(&file);

	if (bytes_written < 0) {
		LOG_ERR("fs_write failed: %zd", bytes_written);
		return bytes_written;
	}

	/* Update node length if we extended the file */
	if (offset + bytes_written > node->length) {
		node->length = offset + bytes_written;
	}

	LOG_DBG("Wrote %zd bytes to file", bytes_written);
	return bytes_written;
}

/* Get stat */
static int passthrough_stat(struct ninep_fs_node *node, uint8_t *buf,
                             size_t buf_len, void *fs_ctx)
{
	if (!node || !buf) {
		return -EINVAL;
	}

	size_t offset = 0;
	uint16_t name_len = strlen(node->name);

	int ret = ninep_write_stat(buf, buf_len, &offset, &node->qid,
	                            node->mode, node->length,
	                            node->name, name_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

/* Create file or directory */
static int passthrough_create(struct ninep_fs_node *parent, const char *name,
                               uint16_t name_len, uint32_t perm, uint8_t mode,
                               struct ninep_fs_node **new_node, void *fs_ctx)
{
	struct ninep_passthrough_fs *fs = fs_ctx;
	const char *parent_path = get_node_path(parent);
	if (!parent_path) {
		return -EINVAL;
	}

	/* Build child path */
	char child_path[256];
	char child_name[name_len + 1];
	memcpy(child_name, name, name_len);
	child_name[name_len] = '\0';

	build_child_path(parent_path, child_name, child_path, sizeof(child_path));

	/* Build full filesystem path */
	char fs_path[256];
	snprintf(fs_path, sizeof(fs_path), "%s%s", fs->mount_point, child_path);

	LOG_DBG("Create: path='%s' perm=0x%x mode=%u", fs_path, perm, mode);

	int ret;
	enum ninep_node_type node_type;
	uint32_t node_mode;

	if (perm & NINEP_DMDIR) {
		/* Create directory */
		ret = fs_mkdir(fs_path);
		if (ret < 0 && ret != -EEXIST) {
			LOG_ERR("fs_mkdir failed: %d", ret);
			return ret;
		}
		node_type = NINEP_NODE_DIR;
		node_mode = 0755;
	} else {
		/* Create file */
		struct fs_file_t file;
		fs_file_t_init(&file);

		ret = fs_open(&file, fs_path, FS_O_CREATE | FS_O_WRITE);
		if (ret < 0) {
			LOG_ERR("fs_open(create) failed: %d", ret);
			return ret;
		}
		fs_close(&file);

		node_type = NINEP_NODE_FILE;
		node_mode = 0644;
	}

	/* Create node */
	struct ninep_fs_node *node = alloc_node(fs, child_name, child_path,
	                                          node_type, node_mode, 0);
	if (!node) {
		return -ENOMEM;
	}

	*new_node = node;
	LOG_DBG("Created: %s", fs_path);
	return 0;
}

/* Remove file or directory */
static int passthrough_remove(struct ninep_fs_node *node, void *fs_ctx)
{
	struct ninep_passthrough_fs *fs = fs_ctx;
	const char *node_path = get_node_path(node);
	if (!node_path) {
		return -EINVAL;
	}

	/* Build full filesystem path */
	char fs_path[256];
	snprintf(fs_path, sizeof(fs_path), "%s%s", fs->mount_point, node_path);

	LOG_DBG("Remove: path='%s'", fs_path);

	int ret = fs_unlink(fs_path);
	if (ret < 0) {
		LOG_ERR("fs_unlink failed: %d", ret);
		return ret;
	}

	/* Free the node */
	free_node(node);

	LOG_DBG("Removed: %s", fs_path);
	return 0;
}

static const struct ninep_fs_ops passthrough_fs_ops = {
	.get_root = passthrough_get_root,
	.walk = passthrough_walk,
	.open = passthrough_open,
	.read = passthrough_read,
	.write = passthrough_write,
	.stat = passthrough_stat,
	.create = passthrough_create,
	.remove = passthrough_remove,
};

const struct ninep_fs_ops *ninep_passthrough_fs_get_ops(void)
{
	return &passthrough_fs_ops;
}

int ninep_passthrough_fs_init(struct ninep_passthrough_fs *fs,
                               const char *mount_point)
{
	if (!fs || !mount_point) {
		return -EINVAL;
	}

	memset(fs, 0, sizeof(*fs));
	fs->mount_point = mount_point;
	fs->next_qid_path = 1;

	/* Create root node */
	fs->root = alloc_node(fs, "/", "/", NINEP_NODE_DIR, 0755, 0);
	if (!fs->root) {
		return -ENOMEM;
	}

	LOG_INF("Passthrough filesystem initialized (mount_point: %s)", mount_point);
	return 0;
}
