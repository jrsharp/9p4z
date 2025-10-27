/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_NAMESPACE_SRV_H_
#define ZEPHYR_INCLUDE_NAMESPACE_SRV_H_

/**
 * @file
 * @brief /srv Service Registry
 *
 * Implements the Plan 9 /srv service registry, where 9P servers
 * post themselves for discovery and connection by clients.
 */

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup srv Service Registry
 * @{
 */

/* Forward declarations */
struct ninep_server;
struct srv_entry;

/**
 * @brief Service types
 */
enum srv_type {
	SRV_TYPE_LOCAL,      /**< In-process 9P server */
	SRV_TYPE_NETWORK,    /**< Network connection (TCP/BLE/etc.) */
};

/**
 * @brief Service entry in /srv
 */
struct srv_entry {
	char name[CONFIG_SRV_MAX_NAME_LEN];  /**< Service name */

	/* Connection info */
	enum srv_type type;
	union {
		struct {
			struct ninep_server *server;  /**< In-process server */
		} local;

		struct {
			char transport[32];   /**< Transport type */
			char address[128];    /**< Address string */
		} network;
	};

	/* Metadata */
	uint32_t flags;

	/* Reference counting */
	atomic_t refcount;

	/* List linkage */
	struct srv_entry *next;
};

/**
 * @brief Global service registry
 */
struct srv_registry {
	struct k_mutex lock;
	struct srv_entry *services;  /**< Linked list of services */
	int num_services;
};

/* ========================================================================
 * Initialization
 * ======================================================================== */

/**
 * @brief Initialize /srv filesystem
 *
 * Creates the /srv service registry and mounts it in the default namespace.
 *
 * @return 0 on success, negative error code on failure
 */
int srv_init(void);

/* ========================================================================
 * Server-Side: Posting Services
 * ======================================================================== */

/**
 * @brief Post an in-process 9P server to /srv
 *
 * @param name Service name (will appear as /srv/{name})
 * @param server In-process 9P server
 * @return 0 on success, negative error code on failure
 *
 * Example:
 *   struct ninep_server *display = ninep_server_register("draw", &draw_ops, drv);
 *   srv_post("display", display);
 *   // Now visible as /srv/display
 */
int srv_post(const char *name, struct ninep_server *server);

/**
 * @brief Post a network service to /srv
 *
 * Creates a service entry that clients can use to connect.
 *
 * @param name Service name
 * @param transport Transport type ("tcp", "ble", "uart", "thread")
 * @param address Transport-specific address
 * @return 0 on success, negative error code on failure
 *
 * Example:
 *   srv_post_network("remote_sensor", "tcp", "192.168.1.100:564");
 *   // Now clients can: srv_mount("remote_sensor", "/remote");
 */
int srv_post_network(const char *name, const char *transport,
                     const char *address);

/**
 * @brief Remove a service from /srv
 *
 * @param name Service name to remove
 * @return 0 on success, negative error code on failure
 */
int srv_remove(const char *name);

/**
 * @brief List all services in /srv
 *
 * @param callback Called for each service
 * @param user_data User data passed to callback
 */
void srv_foreach(void (*callback)(const struct srv_entry *, void *),
                 void *user_data);

/* ========================================================================
 * Client-Side: Using Services
 * ======================================================================== */

/**
 * @brief Mount a service from /srv into the namespace
 *
 * Convenience function that opens a service and mounts it.
 *
 * @param srv_name Service name (without /srv/ prefix)
 * @param mnt_point Where to mount in namespace
 * @param flags Namespace flags
 * @return 0 on success, negative error code on failure
 *
 * Example:
 *   srv_mount("sensors", "/remote/sensors", 0);
 */
int srv_mount(const char *srv_name, const char *mnt_point, uint32_t flags);

/**
 * @brief Lookup a service by name
 *
 * @param name Service name
 * @return Service entry or NULL if not found
 */
struct srv_entry *srv_lookup(const char *name);

/* ========================================================================
 * Filesystem Operations
 * ======================================================================== */

/**
 * @brief Get the /srv filesystem operations
 *
 * Returns the 9P operations for the /srv synthetic filesystem.
 * This can be mounted to expose /srv as a browsable directory.
 *
 * @return 9P filesystem operations for /srv
 */
const struct ninep_fs_ops *srv_get_fs_ops(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NAMESPACE_SRV_H_ */
