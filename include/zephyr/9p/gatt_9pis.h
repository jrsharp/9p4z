/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_GATT_9PIS_H_
#define ZEPHYR_INCLUDE_9P_GATT_9PIS_H_

#include <zephyr/bluetooth/gatt.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_gatt_9pis 9P Information Service (9PIS)
 * @ingroup ninep
 * @{
 */

/**
 * @brief 9P Information Service configuration
 *
 * Contains all the metadata that will be exposed via the GATT service.
 * All strings should be UTF-8 encoded and null-terminated.
 */
struct ninep_9pis_config {
	/** Service description (max 64 bytes, e.g., "9P File Server") */
	const char *service_description;

	/** Service features (max 128 bytes, e.g., "file-sharing,messaging") */
	const char *service_features;

	/** Transport information (max 64 bytes, e.g., "l2cap:psm=0x0009,mtu=4096") */
	const char *transport_info;

	/** App store link (max 256 bytes, URL) */
	const char *app_store_link;

	/** Protocol version (max 32 bytes, e.g., "9P2000;9p4z;1.0.0") */
	const char *protocol_version;
};

/**
 * @brief Initialize and register the 9P Information Service
 *
 * This function registers the 9PIS GATT service with the Bluetooth stack.
 * It should be called after bt_enable() and before starting advertising.
 *
 * @param config Configuration containing service metadata
 * @return 0 on success, negative error code on failure
 */
int ninep_9pis_init(const struct ninep_9pis_config *config);

/**
 * @brief Get the 9PIS service UUID for advertising
 *
 * Returns a pointer to the 9PIS service UUID that can be used in
 * advertising data to indicate the service is available.
 *
 * @return Pointer to 128-bit service UUID
 */
const struct bt_uuid *ninep_9pis_get_uuid(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_GATT_9PIS_H_ */
