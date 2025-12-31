/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/dfu.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(ninep_dfu, CONFIG_NINEP_LOG_LEVEL);

/* Progress logging interval in bytes */
#define DFU_PROGRESS_LOG_INTERVAL (50 * 1024)

/* Forward declarations for sysfs callbacks */
static int dfu_read(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx);
static int dfu_write(const uint8_t *buf, uint32_t count, uint64_t offset, void *ctx);
static int dfu_clunk(void *ctx);

/* State names for status output */
static const char *state_names[] = {
	[NINEP_DFU_IDLE] = "idle",
	[NINEP_DFU_ERASING] = "erasing",
	[NINEP_DFU_RECEIVING] = "receiving",
	[NINEP_DFU_FINALIZING] = "finalizing",
	[NINEP_DFU_COMPLETE] = "complete",
	[NINEP_DFU_ERROR] = "error",
};

/**
 * @brief Set DFU state and notify callback
 */
static void set_state(struct ninep_dfu *dfu, enum ninep_dfu_state state, int error)
{
	dfu->state = state;
	if (error) {
		dfu->last_error = error;
	}
	if (dfu->status_cb) {
		dfu->status_cb(state, dfu->bytes_written, error);
	}
}

/**
 * @brief Start a new firmware upload
 *
 * Pre-erases the secondary slot to avoid heap fragmentation from
 * progressive erase during writes.
 */
static int dfu_start_upload(struct ninep_dfu *dfu)
{
	int ret;

	if (dfu->state == NINEP_DFU_RECEIVING) {
		LOG_WRN("DFU already in progress, resetting");
	}

	set_state(dfu, NINEP_DFU_ERASING, 0);

	/* Pre-erase secondary slot to avoid heap fragmentation during writes */
	LOG_INF("DFU: erasing secondary slot (this may take a moment)...");
	ret = boot_erase_img_bank(FIXED_PARTITION_ID(slot1_partition));
	if (ret < 0) {
		LOG_ERR("Failed to erase secondary slot: %d", ret);
		set_state(dfu, NINEP_DFU_ERROR, ret);
		return ret;
	}
	LOG_INF("DFU: secondary slot erased");

	ret = flash_img_init(&dfu->flash_ctx);
	if (ret < 0) {
		LOG_ERR("Failed to init flash_img context: %d", ret);
		set_state(dfu, NINEP_DFU_ERROR, ret);
		return ret;
	}

	dfu->bytes_written = 0;
	dfu->last_progress_log = 0;
	set_state(dfu, NINEP_DFU_RECEIVING, 0);
	LOG_INF("DFU: ready to receive firmware");

	return 0;
}

/**
 * @brief Sysfs read callback - return status information
 */
static int dfu_read(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	struct ninep_dfu *dfu = ctx;
	char status[512];
	int len = 0;

	/* State */
	len += snprintf(status + len, sizeof(status) - len,
	                "state %s\n", state_names[dfu->state]);

	/* Bytes written (during upload) */
	if (dfu->state == NINEP_DFU_RECEIVING) {
		len += snprintf(status + len, sizeof(status) - len,
		                "bytes %u\n", dfu->bytes_written);
	}

	/* Error code (on error) */
	if (dfu->state == NINEP_DFU_ERROR) {
		len += snprintf(status + len, sizeof(status) - len,
		                "error %d\n", dfu->last_error);
	}

	/* Current image version (slot0) */
	struct mcuboot_img_header hdr;
	int ret = boot_read_bank_header(FIXED_PARTITION_ID(slot0_partition),
	                                &hdr, sizeof(hdr));
	if (ret == 0 && hdr.mcuboot_version == 1) {
		len += snprintf(status + len, sizeof(status) - len,
		                "current %d.%d.%d+%d\n",
		                hdr.h.v1.sem_ver.major,
		                hdr.h.v1.sem_ver.minor,
		                hdr.h.v1.sem_ver.revision,
		                hdr.h.v1.sem_ver.build_num);
	}

	/* Pending image version (slot1) */
	ret = boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition),
	                            &hdr, sizeof(hdr));
	if (ret == 0 && hdr.mcuboot_version == 1) {
		len += snprintf(status + len, sizeof(status) - len,
		                "pending %d.%d.%d+%d\n",
		                hdr.h.v1.sem_ver.major,
		                hdr.h.v1.sem_ver.minor,
		                hdr.h.v1.sem_ver.revision,
		                hdr.h.v1.sem_ver.build_num);
	}

	/* Confirmation status */
	len += snprintf(status + len, sizeof(status) - len,
	                "confirmed %s\n",
	                boot_is_img_confirmed() ? "yes" : "no");

	/* Handle offset/partial reads */
	if (offset >= (uint64_t)len) {
		return 0;
	}

	size_t remaining = len - (size_t)offset;
	size_t to_copy = MIN(remaining, buf_size);
	memcpy(buf, status + offset, to_copy);
	return to_copy;
}

/**
 * @brief Sysfs write callback - receive firmware chunks
 */
static int dfu_write(const uint8_t *buf, uint32_t count, uint64_t offset, void *ctx)
{
	ARG_UNUSED(offset);
	struct ninep_dfu *dfu = ctx;
	int ret;

	/* First write starts the upload */
	if (dfu->state != NINEP_DFU_RECEIVING) {
		ret = dfu_start_upload(dfu);
		if (ret < 0) {
			return ret;
		}
	}

	/* Write chunk to flash */
	ret = flash_img_buffered_write(&dfu->flash_ctx, buf, count, false);
	if (ret < 0) {
		LOG_ERR("Flash write failed: %d", ret);
		set_state(dfu, NINEP_DFU_ERROR, ret);
		return ret;
	}

	dfu->bytes_written += count;

	/* Progress logging */
	if ((dfu->bytes_written / DFU_PROGRESS_LOG_INTERVAL) >
	    (dfu->last_progress_log / DFU_PROGRESS_LOG_INTERVAL)) {
		LOG_INF("DFU: %u bytes received", dfu->bytes_written);
		dfu->last_progress_log = dfu->bytes_written;
	}

	return count;
}

/**
 * @brief Sysfs clunk callback - finalize upload when file is closed
 */
static int dfu_clunk(void *ctx)
{
	struct ninep_dfu *dfu = ctx;
	int ret;

	/* No upload in progress - nothing to finalize */
	if (dfu->state != NINEP_DFU_RECEIVING) {
		return 0;
	}

	set_state(dfu, NINEP_DFU_FINALIZING, 0);

	/* Flush remaining buffered data */
	LOG_INF("DFU: flushing buffer (%u bytes total)...", dfu->bytes_written);
	ret = flash_img_buffered_write(&dfu->flash_ctx, NULL, 0, true);
	if (ret < 0) {
		LOG_ERR("Failed to flush final data: %d", ret);
		set_state(dfu, NINEP_DFU_ERROR, ret);
		return ret;
	}

	LOG_INF("DFU: validating image...");

	/* Validate image header */
	struct mcuboot_img_header hdr;
	ret = boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition),
	                            &hdr, sizeof(hdr));
	if (ret < 0) {
		LOG_ERR("Failed to read image header: %d", ret);
		set_state(dfu, NINEP_DFU_ERROR, ret);
		return ret;
	}

	if (hdr.mcuboot_version != 1) {
		LOG_ERR("Invalid MCUboot image version: %d", hdr.mcuboot_version);
		set_state(dfu, NINEP_DFU_ERROR, -EINVAL);
		return -EINVAL;
	}

	LOG_INF("DFU: image v%d.%d.%d+%d validated",
	        hdr.h.v1.sem_ver.major,
	        hdr.h.v1.sem_ver.minor,
	        hdr.h.v1.sem_ver.revision,
	        hdr.h.v1.sem_ver.build_num);

	/* Mark for test upgrade */
	ret = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (ret < 0) {
		LOG_ERR("Failed to mark image for upgrade: %d", ret);
		set_state(dfu, NINEP_DFU_ERROR, ret);
		return ret;
	}

	set_state(dfu, NINEP_DFU_COMPLETE, 0);
	LOG_INF("DFU: complete - reboot to apply");

	return 0;
}

/* Public API */

int ninep_dfu_init(struct ninep_dfu *dfu,
                   struct ninep_sysfs *sysfs,
                   const struct ninep_dfu_config *config)
{
	if (!dfu || !sysfs) {
		return -EINVAL;
	}

	memset(dfu, 0, sizeof(*dfu));
	dfu->state = NINEP_DFU_IDLE;

	if (config && config->status_cb) {
		dfu->status_cb = config->status_cb;
	}

	const char *path = (config && config->path) ? config->path : "dev/firmware";

	int ret = ninep_sysfs_register_writable_file_ex(
		sysfs,
		path,
		dfu_read,   /* generator (for reads) */
		dfu_write,  /* writer (for writes) */
		dfu_clunk,  /* clunk (called on close) */
		dfu         /* context */
	);

	if (ret < 0) {
		LOG_ERR("Failed to register %s: %d", path, ret);
		return ret;
	}

	LOG_INF("DFU registered at /%s", path);
	return 0;
}

enum ninep_dfu_state ninep_dfu_get_state(struct ninep_dfu *dfu)
{
	return dfu ? dfu->state : NINEP_DFU_IDLE;
}

uint32_t ninep_dfu_get_bytes_written(struct ninep_dfu *dfu)
{
	return dfu ? dfu->bytes_written : 0;
}

void ninep_dfu_cancel(struct ninep_dfu *dfu)
{
	if (!dfu) {
		return;
	}

	if (dfu->state == NINEP_DFU_RECEIVING) {
		LOG_WRN("DFU cancelled at %u bytes", dfu->bytes_written);
	}

	dfu->state = NINEP_DFU_IDLE;
	dfu->bytes_written = 0;
	dfu->last_progress_log = 0;
}

int ninep_dfu_confirm(void)
{
	int ret = boot_write_img_confirmed();
	if (ret == 0) {
		LOG_INF("Image confirmed");
	} else {
		LOG_ERR("Failed to confirm image: %d", ret);
	}
	return ret;
}

bool ninep_dfu_is_confirmed(void)
{
	return boot_is_img_confirmed();
}
