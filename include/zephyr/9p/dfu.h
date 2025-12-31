/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_DFU_H_
#define ZEPHYR_INCLUDE_9P_DFU_H_

#include <zephyr/9p/sysfs.h>

#ifndef CONFIG_IMG_MANAGER
#error "NINEP_DFU requires CONFIG_IMG_MANAGER=y"
#endif

#include <zephyr/dfu/flash_img.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 9P DFU - Firmware update via 9P filesystem
 *
 * Provides a Plan 9-style interface for device firmware updates.
 * Exposes /dev/firmware (or custom path) that accepts MCUboot images.
 *
 * Usage:
 *   1. Read /dev/firmware for status (current version, pending upgrades)
 *   2. Write firmware image bytes to /dev/firmware
 *   3. Close file - automatically finalizes and marks for upgrade
 *   4. Reboot to apply
 *
 * The module handles:
 *   - Pre-erasing secondary slot on first write (avoids heap fragmentation)
 *   - Buffered streaming writes to flash
 *   - Image validation on close
 *   - Marking image for test upgrade
 */

/**
 * @brief DFU state
 */
enum ninep_dfu_state {
	NINEP_DFU_IDLE,        /**< Ready for upload */
	NINEP_DFU_ERASING,     /**< Erasing secondary slot */
	NINEP_DFU_RECEIVING,   /**< Receiving firmware data */
	NINEP_DFU_FINALIZING,  /**< Flushing and validating */
	NINEP_DFU_COMPLETE,    /**< Success - reboot to apply */
	NINEP_DFU_ERROR,       /**< Error occurred */
};

/**
 * @brief DFU status callback (optional)
 *
 * Called when DFU state changes. Useful for LED feedback, logging, etc.
 *
 * @param state Current DFU state
 * @param bytes_written Bytes written so far (valid during RECEIVING)
 * @param error_code Error code (valid during ERROR state)
 */
typedef void (*ninep_dfu_status_cb_t)(enum ninep_dfu_state state,
                                       uint32_t bytes_written,
                                       int error_code);

/**
 * @brief DFU configuration
 */
struct ninep_dfu_config {
	const char *path;                /**< Sysfs path, default "dev/firmware" */
	ninep_dfu_status_cb_t status_cb; /**< Optional status callback */
};

/**
 * @brief DFU instance
 *
 * Allocate one instance per DFU endpoint. Typically only one is needed.
 */
struct ninep_dfu {
	enum ninep_dfu_state state;      /**< Current state */
	uint32_t bytes_written;          /**< Bytes written in current upload */
	int last_error;                  /**< Last error code */
	ninep_dfu_status_cb_t status_cb; /**< Status callback */
	uint32_t last_progress_log;      /**< For progress logging */
	struct flash_img_context flash_ctx; /**< Flash image context */
};

/**
 * @brief Initialize DFU and register with sysfs
 *
 * Registers /dev/firmware (or custom path) with the sysfs instance.
 * The file supports:
 *   - Read: Returns status (state, bytes written, version info)
 *   - Write: Streams firmware data to secondary slot
 *   - Close: Finalizes upload, validates image, marks for upgrade
 *
 * @param dfu DFU instance to initialize
 * @param sysfs Sysfs instance to register with
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, negative error code on failure
 */
int ninep_dfu_init(struct ninep_dfu *dfu,
                   struct ninep_sysfs *sysfs,
                   const struct ninep_dfu_config *config);

/**
 * @brief Get current DFU state
 *
 * @param dfu DFU instance
 * @return Current state
 */
enum ninep_dfu_state ninep_dfu_get_state(struct ninep_dfu *dfu);

/**
 * @brief Get bytes written in current upload
 *
 * @param dfu DFU instance
 * @return Bytes written (0 if no upload in progress)
 */
uint32_t ninep_dfu_get_bytes_written(struct ninep_dfu *dfu);

/**
 * @brief Cancel any in-progress upload
 *
 * Resets state to IDLE. Does not erase any partial data in secondary slot.
 *
 * @param dfu DFU instance
 */
void ninep_dfu_cancel(struct ninep_dfu *dfu);

/**
 * @brief Confirm current image after successful boot
 *
 * Call this after verifying the new firmware works correctly.
 * If not called, MCUboot will revert to previous image on next reboot.
 *
 * @return 0 on success, negative error code on failure
 */
int ninep_dfu_confirm(void);

/**
 * @brief Check if current image is confirmed
 *
 * @return true if image is confirmed, false if pending confirmation
 */
bool ninep_dfu_is_confirmed(void);

#endif /* ZEPHYR_INCLUDE_9P_DFU_H_ */
