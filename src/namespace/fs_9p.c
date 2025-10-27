/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/namespace/fs_9p.h>
#include <zephyr/namespace/namespace.h>
#include <zephyr/9p/client.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(fs_9p, CONFIG_NINEP_LOG_LEVEL);

/* ========================================================================
 * FID Pool Management
 * ======================================================================== */

int ninep_fid_pool_init(struct ninep_fid_pool *pool, uint32_t base_fid,
                        uint32_t max_fids)
{
	if (!pool || max_fids > CONFIG_NINEP_MAX_FIDS) {
		return -EINVAL;
	}

	pool->base_fid = base_fid;
	pool->max_fids = max_fids;
	k_mutex_init(&pool->lock);

	/* Clear all bits */
	for (int i = 0; i < ATOMIC_BITMAP_SIZE(CONFIG_NINEP_MAX_FIDS); i++) {
		atomic_clear(&pool->bitmap[i]);
	}

	return 0;
}

int ninep_fid_pool_alloc(struct ninep_fid_pool *pool)
{
	if (!pool) {
		return -EINVAL;
	}

	k_mutex_lock(&pool->lock, K_FOREVER);

	/* Find first free FID */
	for (uint32_t i = 0; i < pool->max_fids; i++) {
		if (!atomic_test_and_set_bit(pool->bitmap, i)) {
			k_mutex_unlock(&pool->lock);
			return pool->base_fid + i;
		}
	}

	k_mutex_unlock(&pool->lock);
	return -ENOMEM;  /* No free FIDs */
}

void ninep_fid_pool_free(struct ninep_fid_pool *pool, uint32_t fid)
{
	if (!pool || fid < pool->base_fid) {
		return;
	}

	uint32_t idx = fid - pool->base_fid;
	if (idx >= pool->max_fids) {
		return;
	}

	k_mutex_lock(&pool->lock, K_FOREVER);
	atomic_clear_bit(pool->bitmap, idx);
	k_mutex_unlock(&pool->lock);
}

/* ========================================================================
 * VFS File System Operations
 * ======================================================================== */

/**
 * @brief Mount a 9P filesystem
 */
static int fs_9p_mount(struct fs_mount_t *mountp)
{
	struct ninep_mount_ctx *ctx = mountp->fs_data;
	int ret;

	if (!ctx || !ctx->client) {
		LOG_ERR("Invalid mount context");
		return -EINVAL;
	}

	/* Negotiate version */
	ret = ninep_client_version(ctx->client);
	if (ret < 0) {
		LOG_ERR("Version negotiation failed: %d", ret);
		return ret;
	}

	ctx->msize = ctx->client->msize;

	/* Attach to server root */
	ret = ninep_client_attach(ctx->client, &ctx->root_fid,
	                         NINEP_NOFID, "zephyr", ctx->aname);
	if (ret < 0) {
		LOG_ERR("Attach failed: %d", ret);
		return ret;
	}

	/* Get root QID by statting the root fid */
	struct ninep_stat stat;
	ret = ninep_client_stat(ctx->client, ctx->root_fid, &stat);
	if (ret < 0) {
		LOG_ERR("Stat root failed: %d", ret);
		ninep_client_clunk(ctx->client, ctx->root_fid);
		return ret;
	}

	ctx->root_qid = stat.qid;
	ctx->attached = true;

	LOG_INF("9P mount successful: %s (msize=%u)", mountp->mnt_point, ctx->msize);
	return 0;
}

/**
 * @brief Unmount a 9P filesystem
 */
static int fs_9p_unmount(struct fs_mount_t *mountp)
{
	struct ninep_mount_ctx *ctx = mountp->fs_data;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Clunk root FID */
	if (ctx->root_fid != NINEP_NOFID) {
		ninep_client_clunk(ctx->client, ctx->root_fid);
	}

	ctx->attached = false;
	LOG_INF("9P unmount: %s", mountp->mnt_point);

	return 0;
}

/**
 * @brief Open a file
 */
static int fs_9p_open(struct fs_file_t *filp, const char *fs_path,
                      fs_mode_t flags)
{
	struct ninep_mount_ctx *ctx = filp->mp->fs_data;
	uint32_t fid, file_fid;
	int ret;
	uint8_t mode;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Convert VFS flags to 9P mode */
	if ((flags & FS_O_RDWR) == FS_O_RDWR) {
		mode = NINEP_ORDWR;
	} else if (flags & FS_O_WRITE) {
		mode = NINEP_OWRITE;
	} else {
		mode = NINEP_OREAD;
	}

	/* Walk from root to file */
	ret = ninep_client_walk(ctx->client, ctx->root_fid, &fid, fs_path);
	if (ret < 0) {
		LOG_ERR("Walk to %s failed: %d", fs_path, ret);
		return ret;
	}

	/* Open the file */
	ret = ninep_client_open(ctx->client, fid, mode);
	if (ret < 0) {
		LOG_ERR("Open %s failed: %d", fs_path, ret);
		ninep_client_clunk(ctx->client, fid);
		return ret;
	}

	/* Store FID in file structure */
	filp->filep = (void *)(uintptr_t)fid;

	LOG_DBG("Opened %s (fid=%u)", fs_path, fid);
	return 0;
}

/**
 * @brief Close a file
 */
static int fs_9p_close(struct fs_file_t *filp)
{
	struct ninep_mount_ctx *ctx = filp->mp->fs_data;
	uint32_t fid = (uint32_t)(uintptr_t)filp->filep;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	int ret = ninep_client_clunk(ctx->client, fid);
	if (ret < 0) {
		LOG_ERR("Clunk failed: %d", ret);
		return ret;
	}

	LOG_DBG("Closed fid=%u", fid);
	return 0;
}

/**
 * @brief Read from file
 */
static ssize_t fs_9p_read(struct fs_file_t *filp, void *buf, size_t count)
{
	struct ninep_mount_ctx *ctx = filp->mp->fs_data;
	uint32_t fid = (uint32_t)(uintptr_t)filp->filep;
	ssize_t ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	ret = ninep_client_read(ctx->client, fid, filp->offset, buf, count);
	if (ret < 0) {
		LOG_ERR("Read failed: %d", (int)ret);
		return ret;
	}

	/* Update file offset */
	filp->offset += ret;

	LOG_DBG("Read %zd bytes from fid=%u", ret, fid);
	return ret;
}

/**
 * @brief Write to file
 */
static ssize_t fs_9p_write(struct fs_file_t *filp, const void *buf, size_t count)
{
	struct ninep_mount_ctx *ctx = filp->mp->fs_data;
	uint32_t fid = (uint32_t)(uintptr_t)filp->filep;
	ssize_t ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	ret = ninep_client_write(ctx->client, fid, filp->offset, buf, count);
	if (ret < 0) {
		LOG_ERR("Write failed: %d", (int)ret);
		return ret;
	}

	/* Update file offset */
	filp->offset += ret;

	LOG_DBG("Wrote %zd bytes to fid=%u", ret, fid);
	return ret;
}

/**
 * @brief Seek in file
 */
static int fs_9p_lseek(struct fs_file_t *filp, off_t off, int whence)
{
	struct ninep_mount_ctx *ctx = filp->mp->fs_data;
	uint32_t fid = (uint32_t)(uintptr_t)filp->filep;
	off_t new_offset;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Get file size if needed */
	if (whence == FS_SEEK_END) {
		struct ninep_stat stat;
		int ret = ninep_client_stat(ctx->client, fid, &stat);
		if (ret < 0) {
			return ret;
		}
		new_offset = stat.length + off;
	} else if (whence == FS_SEEK_CUR) {
		new_offset = filp->offset + off;
	} else { /* FS_SEEK_SET */
		new_offset = off;
	}

	if (new_offset < 0) {
		return -EINVAL;
	}

	filp->offset = new_offset;
	LOG_DBG("Seek fid=%u to offset=%ld", fid, (long)new_offset);

	return new_offset;
}

/**
 * @brief Get file stats
 */
static int fs_9p_stat(struct fs_mount_t *mountp, const char *path,
                      struct fs_dirent *entry)
{
	struct ninep_mount_ctx *ctx = mountp->fs_data;
	uint32_t fid;
	struct ninep_stat stat;
	int ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Walk to file */
	ret = ninep_client_walk(ctx->client, ctx->root_fid, &fid, path);
	if (ret < 0) {
		return ret;
	}

	/* Get stat */
	ret = ninep_client_stat(ctx->client, fid, &stat);
	ninep_client_clunk(ctx->client, fid);

	if (ret < 0) {
		return ret;
	}

	/* Convert 9P stat to VFS dirent */
	strncpy(entry->name, stat.name, sizeof(entry->name) - 1);
	entry->name[sizeof(entry->name) - 1] = '\0';
	entry->type = (stat.qid.type & NINEP_QTDIR) ? FS_DIR_ENTRY_DIR : FS_DIR_ENTRY_FILE;
	entry->size = stat.length;

	return 0;
}

/**
 * @brief Open directory
 */
static int fs_9p_opendir(struct fs_dir_t *dirp, const char *fs_path)
{
	struct ninep_mount_ctx *ctx = dirp->mp->fs_data;
	uint32_t fid;
	int ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Walk to directory */
	ret = ninep_client_walk(ctx->client, ctx->root_fid, &fid, fs_path);
	if (ret < 0) {
		LOG_ERR("Walk to dir %s failed: %d", fs_path, ret);
		return ret;
	}

	/* Open directory for reading */
	ret = ninep_client_open(ctx->client, fid, NINEP_OREAD);
	if (ret < 0) {
		LOG_ERR("Open dir %s failed: %d", fs_path, ret);
		ninep_client_clunk(ctx->client, fid);
		return ret;
	}

	/* Store FID in directory structure */
	dirp->dirp = (void *)(uintptr_t)fid;

	LOG_DBG("Opened dir %s (fid=%u)", fs_path, fid);
	return 0;
}

/**
 * @brief Read directory entry
 */
static int fs_9p_readdir(struct fs_dir_t *dirp, struct fs_dirent *entry)
{
	struct ninep_mount_ctx *ctx = dirp->mp->fs_data;
	uint32_t fid = (uint32_t)(uintptr_t)dirp->dirp;
	uint8_t buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	int ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Read directory data */
	ret = ninep_client_read(ctx->client, fid, dirp->offset, buf, sizeof(buf));
	if (ret < 0) {
		return ret;
	}

	if (ret == 0) {
		/* End of directory */
		entry->name[0] = '\0';
		return 0;
	}

	/* Parse stat structure from directory data */
	struct ninep_stat stat;
	uint16_t stat_size = buf[0] | (buf[1] << 8);

	/* TODO: Proper stat parsing */
	/* For now, just return end of directory */
	entry->name[0] = '\0';

	dirp->offset += stat_size + 2;

	return 0;
}

/**
 * @brief Close directory
 */
static int fs_9p_closedir(struct fs_dir_t *dirp)
{
	struct ninep_mount_ctx *ctx = dirp->mp->fs_data;
	uint32_t fid = (uint32_t)(uintptr_t)dirp->dirp;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	int ret = ninep_client_clunk(ctx->client, fid);
	if (ret < 0) {
		LOG_ERR("Clunk dir failed: %d", ret);
		return ret;
	}

	LOG_DBG("Closed dir fid=%u", fid);
	return 0;
}

/**
 * @brief Create directory
 */
static int fs_9p_mkdir(struct fs_mount_t *mountp, const char *path)
{
	struct ninep_mount_ctx *ctx = mountp->fs_data;
	uint32_t parent_fid;
	char *parent_path, *name;
	char path_copy[CONFIG_NS_MAX_PATH_LEN];
	int ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Split path into parent and name */
	strncpy(path_copy, path, sizeof(path_copy) - 1);
	path_copy[sizeof(path_copy) - 1] = '\0';

	name = strrchr(path_copy, '/');
	if (!name) {
		return -EINVAL;
	}

	*name++ = '\0';
	parent_path = path_copy;

	/* Walk to parent */
	ret = ninep_client_walk(ctx->client, ctx->root_fid, &parent_fid, parent_path);
	if (ret < 0) {
		return ret;
	}

	/* Create directory */
	ret = ninep_client_create(ctx->client, parent_fid, name,
	                         NINEP_DMDIR | 0755, NINEP_OREAD);
	ninep_client_clunk(ctx->client, parent_fid);

	if (ret < 0) {
		LOG_ERR("Create dir %s failed: %d", path, ret);
		return ret;
	}

	LOG_DBG("Created dir %s", path);
	return 0;
}

/**
 * @brief Remove file
 */
static int fs_9p_unlink(struct fs_mount_t *mountp, const char *path)
{
	struct ninep_mount_ctx *ctx = mountp->fs_data;
	uint32_t fid;
	int ret;

	if (!ctx || !ctx->attached) {
		return -EINVAL;
	}

	/* Walk to file */
	ret = ninep_client_walk(ctx->client, ctx->root_fid, &fid, path);
	if (ret < 0) {
		return ret;
	}

	/* Remove file */
	ret = ninep_client_remove(ctx->client, fid);
	if (ret < 0) {
		LOG_ERR("Remove %s failed: %d", path, ret);
		return ret;
	}

	LOG_DBG("Removed %s", path);
	return 0;
}

/**
 * @brief Rename file
 */
static int fs_9p_rename(struct fs_mount_t *mountp, const char *from,
                        const char *to)
{
	/* 9P rename is done via Twstat, which is not yet implemented */
	LOG_ERR("Rename not yet implemented");
	return -ENOTSUP;
}

/* ========================================================================
 * File System Registration
 * ======================================================================== */

static struct fs_file_system_t fs_9p = {
	.open = fs_9p_open,
	.close = fs_9p_close,
	.read = fs_9p_read,
	.write = fs_9p_write,
	.lseek = fs_9p_lseek,
	.stat = fs_9p_stat,
	.opendir = fs_9p_opendir,
	.readdir = fs_9p_readdir,
	.closedir = fs_9p_closedir,
	.mount = fs_9p_mount,
	.unmount = fs_9p_unmount,
	.mkdir = fs_9p_mkdir,
	.unlink = fs_9p_unlink,
	.rename = fs_9p_rename,
};

int fs_9p_init(void)
{
	int ret = fs_register(FS_TYPE_9P, &fs_9p);
	if (ret < 0) {
		LOG_ERR("Failed to register 9P filesystem: %d", ret);
		return ret;
	}

	LOG_INF("9P VFS driver registered");
	return 0;
}
