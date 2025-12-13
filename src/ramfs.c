/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/ramfs.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_ramfs, CONFIG_NINEP_LOG_LEVEL);

/* Helper to allocate node */
static struct ninep_fs_node *alloc_node(struct ninep_ramfs *ramfs,
                                         const char *name,
                                         enum ninep_node_type type)
{
	struct ninep_fs_node *node = k_malloc(sizeof(struct ninep_fs_node));

	if (!node) {
		return NULL;
	}

	memset(node, 0, sizeof(*node));
	strncpy(node->name, name, sizeof(node->name) - 1);
	node->type = type;
	node->mode = (type == NINEP_NODE_DIR) ? 0755 : 0644;
	node->qid.path = ramfs->next_qid_path++;
	node->qid.version = 0;
	node->qid.type = (type == NINEP_NODE_DIR) ? NINEP_QTDIR : NINEP_QTFILE;

	return node;
}

/* Helper to add child to parent */
static void add_child(struct ninep_fs_node *parent, struct ninep_fs_node *child)
{
	LOG_DBG("Adding child '%s' to parent '%s' (parent=%p, child=%p)",
	        child->name, parent->name, parent, child);
	child->parent = parent;
	child->next_sibling = parent->children;
	parent->children = child;
	LOG_DBG("After add_child: parent->children=%p", parent->children);
}

/* Get root */
static struct ninep_fs_node *ramfs_get_root(void *fs_ctx)
{
	struct ninep_ramfs *ramfs = fs_ctx;

	return ramfs->root;
}

/* Walk to child */
static struct ninep_fs_node *ramfs_walk(struct ninep_fs_node *parent,
                                         const char *name, uint16_t name_len,
                                         void *fs_ctx)
{
	if (!parent || parent->type != NINEP_NODE_DIR) {
		return NULL;
	}

	struct ninep_fs_node *child = parent->children;

	while (child) {
		if (strlen(child->name) == name_len &&
		    strncmp(child->name, name, name_len) == 0) {
			return child;
		}
		child = child->next_sibling;
	}

	return NULL;
}

/* Open node */
static int ramfs_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	/* RAM FS supports all open modes */
	return 0;
}

/* Read from file */
static int ramfs_read(struct ninep_fs_node *node, uint64_t offset,
                      uint8_t *buf, uint32_t count, const char *uname,
                      void *fs_ctx)
{
	if (node->type == NINEP_NODE_DIR) {
		/* Read directory entries */
		LOG_DBG("Reading directory '%s': children=%p, offset=%llu, count=%u",
		        node->name, node->children, offset, count);

		struct ninep_fs_node *child = node->children;
		size_t buf_offset = 0;
		int child_count = 0;
		uint64_t current_offset = 0;  /* Track position in directory stream */

		/* Iterate through children to find starting point and fill buffer */
		while (child) {
			/* Calculate size of this entry */
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                      2 + strlen(child->name) +
			                      2 + 0 +  /* uid */
			                      2 + 0 +  /* gid */
			                      2 + 0;   /* muid */
			uint32_t entry_size = 2 + stat_size;  /* size field + stat */

			/* Check if this entry is past the requested offset */
			if (current_offset >= offset) {
				/* This entry should be included in output */

				/* Check if we have space */
				if (buf_offset + entry_size > count) {
					break;
				}

			/* size[2] */
			buf[buf_offset++] = stat_size & 0xFF;
			buf[buf_offset++] = (stat_size >> 8) & 0xFF;

			/* type[2] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

			/* dev[4] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

			/* qid[13] */
			buf[buf_offset++] = child->qid.type;
			buf[buf_offset++] = child->qid.version & 0xFF;
			buf[buf_offset++] = (child->qid.version >> 8) & 0xFF;
			buf[buf_offset++] = (child->qid.version >> 16) & 0xFF;
			buf[buf_offset++] = (child->qid.version >> 24) & 0xFF;
			buf[buf_offset++] = child->qid.path & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 8) & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 16) & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 24) & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 32) & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 40) & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 48) & 0xFF;
			buf[buf_offset++] = (child->qid.path >> 56) & 0xFF;

			/* mode[4] */
			uint32_t mode = child->mode;

			if (child->type == NINEP_NODE_DIR) {
				mode |= NINEP_DMDIR;
			}
			buf[buf_offset++] = mode & 0xFF;
			buf[buf_offset++] = (mode >> 8) & 0xFF;
			buf[buf_offset++] = (mode >> 16) & 0xFF;
			buf[buf_offset++] = (mode >> 24) & 0xFF;

			/* atime[4] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

			/* mtime[4] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

			/* length[8] */
			uint64_t len = child->length;

			buf[buf_offset++] = len & 0xFF;
			buf[buf_offset++] = (len >> 8) & 0xFF;
			buf[buf_offset++] = (len >> 16) & 0xFF;
			buf[buf_offset++] = (len >> 24) & 0xFF;
			buf[buf_offset++] = (len >> 32) & 0xFF;
			buf[buf_offset++] = (len >> 40) & 0xFF;
			buf[buf_offset++] = (len >> 48) & 0xFF;
			buf[buf_offset++] = (len >> 56) & 0xFF;

			/* name[s] */
			uint16_t name_len = strlen(child->name);

			buf[buf_offset++] = name_len & 0xFF;
			buf[buf_offset++] = (name_len >> 8) & 0xFF;
			memcpy(&buf[buf_offset], child->name, name_len);
			buf_offset += name_len;

			/* uid[s] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

			/* gid[s] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

			/* muid[s] */
			buf[buf_offset++] = 0;
			buf[buf_offset++] = 0;

				child_count++;
			}

			/* Move to next entry and update offset */
			current_offset += entry_size;
			child = child->next_sibling;
		}

		LOG_DBG("Directory read complete: %d children, %zu bytes", child_count, buf_offset);
		return buf_offset;
	} else {
		/* Read file content */
		if (offset >= node->length) {
			return 0;
		}

		uint32_t to_read = count;

		if (offset + to_read > node->length) {
			to_read = node->length - offset;
		}

		if (node->data) {
			memcpy(buf, (uint8_t *)node->data + offset, to_read);
		}

		return to_read;
	}
}

/* Write (not implemented) */
static int ramfs_write(struct ninep_fs_node *node, uint64_t offset,
                       const uint8_t *buf, uint32_t count, const char *uname,
                       void *fs_ctx)
{
	ARG_UNUSED(uname);
	return -ENOTSUP;
}

/* Get stat */
static int ramfs_stat(struct ninep_fs_node *node, uint8_t *buf,
                      size_t buf_len, void *fs_ctx)
{
	if (!node || !buf) {
		return -EINVAL;
	}

	size_t offset = 0;
	uint16_t name_len = strlen(node->name);

	int ret = ninep_write_stat(buf, buf_len, &offset, &node->qid,
	                            node->mode, node->length,
	                            node->name, name_len,
	                            NULL, NULL, NULL);  /* uid/gid/muid default to "zephyr" */
	if (ret < 0) {
		return ret;
	}

	return offset;
}

/* Create (not implemented) */
static int ramfs_create(struct ninep_fs_node *parent, const char *name,
                        uint16_t name_len, uint32_t perm, uint8_t mode,
                        const char *uname, struct ninep_fs_node **new_node,
                        void *fs_ctx)
{
	ARG_UNUSED(uname);
	return -ENOTSUP;
}

/* Remove (not implemented) */
static int ramfs_remove(struct ninep_fs_node *node, void *fs_ctx)
{
	return -ENOTSUP;
}

static const struct ninep_fs_ops ramfs_ops = {
	.get_root = ramfs_get_root,
	.walk = ramfs_walk,
	.open = ramfs_open,
	.read = ramfs_read,
	.write = ramfs_write,
	.stat = ramfs_stat,
	.create = ramfs_create,
	.remove = ramfs_remove,
};

const struct ninep_fs_ops *ninep_ramfs_get_ops(void)
{
	return &ramfs_ops;
}

int ninep_ramfs_init(struct ninep_ramfs *ramfs)
{
	if (!ramfs) {
		return -EINVAL;
	}

	memset(ramfs, 0, sizeof(*ramfs));
	ramfs->next_qid_path = 1;

	/* Create root directory */
	ramfs->root = alloc_node(ramfs, "/", NINEP_NODE_DIR);
	if (!ramfs->root) {
		return -ENOMEM;
	}

	LOG_INF("RAM filesystem initialized");
	return 0;
}

struct ninep_fs_node *ninep_ramfs_create_file(struct ninep_ramfs *ramfs,
                                                struct ninep_fs_node *parent,
                                                const char *name,
                                                const void *content,
                                                size_t length)
{
	if (!ramfs || !parent || !name) {
		return NULL;
	}

	struct ninep_fs_node *file = alloc_node(ramfs, name, NINEP_NODE_FILE);

	if (!file) {
		return NULL;
	}

	if (content && length > 0) {
		file->data = k_malloc(length);
		if (!file->data) {
			k_free(file);
			return NULL;
		}
		memcpy(file->data, content, length);
		file->length = length;
	}

	add_child(parent, file);
	return file;
}

struct ninep_fs_node *ninep_ramfs_create_dir(struct ninep_ramfs *ramfs,
                                               struct ninep_fs_node *parent,
                                               const char *name)
{
	if (!ramfs || !parent || !name) {
		return NULL;
	}

	struct ninep_fs_node *dir = alloc_node(ramfs, name, NINEP_NODE_DIR);

	if (!dir) {
		return NULL;
	}

	add_child(parent, dir);
	return dir;
}
