/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_SERVER_H_
#define ZEPHYR_INCLUDE_9P_SERVER_H_

#include <zephyr/9p/protocol.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/fid.h>
#include <zephyr/9p/tag.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_server 9P Server
 * @ingroup ninep
 * @{
 */

/* Forward declarations */
struct ninep_server;
struct ninep_fs_ops;

/**
 * @brief File system node types
 */
enum ninep_node_type {
	NINEP_NODE_FILE,
	NINEP_NODE_DIR,
};

/**
 * @brief File system node
 */
struct ninep_fs_node {
	char name[64];
	enum ninep_node_type type;
	uint32_t mode;
	uint64_t length;
	void *data;  /* File content or user data */

	/* Tree structure */
	struct ninep_fs_node *parent;
	struct ninep_fs_node *children;
	struct ninep_fs_node *next_sibling;

	/* QID */
	struct ninep_qid qid;
};

/**
 * @brief File system operations
 *
 * This generic interface allows 9P to expose any subsystem as a filesystem.
 * The current implementation includes a reference RAM filesystem (ramfs),
 * but the interface is designed to be backend-agnostic.
 *
 * FUTURE ROADMAP - Additional filesystem backends:
 * ================================================
 *
 * The fs_ops interface enables "everything is a file" abstraction over
 * Zephyr subsystems, following the Plan 9 philosophy:
 *
 * - LittleFS backend (CONFIG_NINEP_FS_LITTLEFS)
 *   Expose LittleFS partitions over 9P for remote file access
 *
 * - Settings subsystem (CONFIG_NINEP_FS_SETTINGS)
 *   Read/write device settings as files:
 *   /settings/bluetooth/name, /settings/network/hostname, etc.
 *
 * - NVS backend (CONFIG_NINEP_FS_NVS)
 *   Expose Non-Volatile Storage as key-value files
 *
 * - Device drivers (CONFIG_NINEP_FS_DRIVERS)
 *   Interact with hardware as files:
 *   /dev/sensors/temp0, /dev/gpio/led0, /dev/uart/console
 *
 * - Shell execution (CONFIG_NINEP_FS_SHELL)
 *   Execute shell commands via filesystem:
 *   echo "help" > /shell/cmd; cat /shell/output
 *
 * - DFU/Firmware Update (CONFIG_NINEP_FS_DFU)
 *   Perform firmware updates over 9P:
 *   cat new_firmware.bin > /dfu/image
 *   cat /dfu/status  # Check update status
 *   echo "1" > /dfu/reboot  # Trigger reboot into new firmware
 *
 * - Debug/Coredump (CONFIG_NINEP_FS_DEBUG)
 *   Remote debugging over 9P, inspired by Plan 9's /proc:
 *   echo "halt" > /debug/ctl  # Halt execution
 *   cat /debug/threads/0/regs  # Read thread registers
 *   cat /debug/threads/0/stack  # Stack trace
 *   echo "0x20000000 256" > /debug/mem; cat /debug/mem  # Memory access
 *   cat /debug/coredump > crash.core  # Generate coredump
 *   echo "continue" > /debug/ctl  # Resume execution
 *   Enables JTAG-less debugging over UART, TCP, or Bluetooth.
 *   Compatible with acid-style debuggers and scriptable tools.
 *
 * Each backend implements this same interface, allowing seamless switching
 * or even multiplexing multiple backends under different paths.
 */
struct ninep_fs_ops {
	/**
	 * @brief Get root node
	 */
	struct ninep_fs_node *(*get_root)(void *fs_ctx);

	/**
	 * @brief Walk to path element
	 */
	struct ninep_fs_node *(*walk)(struct ninep_fs_node *parent,
	                                const char *name, uint16_t name_len,
	                                void *fs_ctx);

	/**
	 * @brief Open node
	 */
	int (*open)(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx);

	/**
	 * @brief Read from node
	 */
	int (*read)(struct ninep_fs_node *node, uint64_t offset,
	            uint8_t *buf, uint32_t count, void *fs_ctx);

	/**
	 * @brief Write to node
	 */
	int (*write)(struct ninep_fs_node *node, uint64_t offset,
	             const uint8_t *buf, uint32_t count, const char *uname,
	             void *fs_ctx);

	/**
	 * @brief Get file stat
	 */
	int (*stat)(struct ninep_fs_node *node, uint8_t *buf,
	            size_t buf_len, void *fs_ctx);

	/**
	 * @brief Create file/directory
	 */
	int (*create)(struct ninep_fs_node *parent, const char *name,
	              uint16_t name_len, uint32_t perm, uint8_t mode,
	              const char *uname, struct ninep_fs_node **new_node,
	              void *fs_ctx);

	/**
	 * @brief Remove file/directory
	 */
	int (*remove)(struct ninep_fs_node *node, void *fs_ctx);

	/**
	 * @brief Clunk (close) node
	 *
	 * Called when a fid is clunked (closed). Allows filesystem to free
	 * resources associated with the node. The node may be freed by this
	 * operation, so callers should not use it afterward.
	 *
	 * @param node Node to clunk
	 * @param fs_ctx Filesystem context
	 * @return 0 on success, negative error code on failure
	 */
	int (*clunk)(struct ninep_fs_node *node, void *fs_ctx);
};

/**
 * @brief 9P server configuration
 */
struct ninep_server_config {
	const struct ninep_fs_ops *fs_ops;
	void *fs_ctx;
	uint32_t max_message_size;
	const char *version;
};

/**
 * @brief FID entry (maps FID to filesystem node)
 */
struct ninep_server_fid {
	uint32_t fid;
	struct ninep_fs_node *node;
	bool in_use;
	uint32_t iounit;
	char uname[64];  /**< User name from Tattach (client-provided identifier) */
};

/**
 * @brief 9P server instance
 */
struct ninep_server {
	const struct ninep_server_config *config;
	struct ninep_transport *transport;

	/* FID table */
	struct ninep_server_fid fids[CONFIG_NINEP_MAX_FIDS];

	/* Request/response buffers */
	uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	uint8_t tx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	size_t rx_len;
};

/**
 * @brief Initialize 9P server
 *
 * @param server Server instance
 * @param config Server configuration
 * @param transport Transport layer
 * @return 0 on success, negative error code on failure
 */
int ninep_server_init(struct ninep_server *server,
                      const struct ninep_server_config *config,
                      struct ninep_transport *transport);

/**
 * @brief Start 9P server
 *
 * @param server Server instance
 * @return 0 on success, negative error code on failure
 */
int ninep_server_start(struct ninep_server *server);

/**
 * @brief Stop 9P server
 *
 * @param server Server instance
 * @return 0 on success, negative error code on failure
 */
int ninep_server_stop(struct ninep_server *server);

/**
 * @brief Process incoming message (called by transport)
 *
 * @param server Server instance
 * @param msg Message buffer
 * @param len Message length
 */
void ninep_server_process_message(struct ninep_server *server,
                                   const uint8_t *msg, size_t len);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_SERVER_H_ */
