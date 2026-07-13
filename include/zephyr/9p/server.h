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
#include <zephyr/kernel.h>
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
 * @brief Return value from read_deferred: "no data now; request parked".
 *
 * The filesystem has registered the read handle and promises to answer the
 * request later via ninep_server_read_complete(). The server sends no Rread
 * now and continues processing other requests on the session.
 */
#define NINEP_READ_DEFER (-EINPROGRESS)

/**
 * @brief Ticket for completing a parked (deferred) Tread later.
 *
 * Copy-by-value. Validity is enforced by the generation token: if the request
 * has since been flushed, clunked, or the session reset/torn down,
 * ninep_server_read_complete() returns -ESTALE and touches nothing.
 */
struct ninep_read_handle {
	struct ninep_server *server;
	uint8_t slot;   /**< Index into server->pending_reads */
	uint32_t gen;   /**< Generation token captured at parking time */
};

/**
 * @brief One parked Tread awaiting completion.
 *
 * Protected by server->tx_buf_mutex (same lock that serializes dispatch
 * and response transmission).
 */
struct ninep_pending_read {
	bool in_use;
	uint16_t tag;
	uint32_t fid;
	uint32_t count;  /**< Client's requested count, pre-clamped to msize */
	uint32_t gen;
};

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
	 *
	 * @param uname User name from session (for per-user responses, e.g., directory
	 *              listings with user-specific permissions). May be NULL if not
	 *              needed. Implementations can ignore this parameter if they don't
	 *              need per-user behavior.
	 */
	int (*read)(struct ninep_fs_node *node, uint64_t offset,
	            uint8_t *buf, uint32_t count, const char *uname, void *fs_ctx);

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

	/**
	 * @brief Read from node with deferred-response support (OPTIONAL)
	 *
	 * Like read(), with one addition: when the file is a stream (e.g. a
	 * chat hub) and the reader is caught up, the filesystem may — instead
	 * of returning 0 — register the handle @p h in its own wait registry
	 * and return NINEP_READ_DEFER. The server then sends no Rread; the
	 * filesystem answers the request later (typically when new data is
	 * written) by calling ninep_server_read_complete(*h, data, len) from
	 * any thread/context.
	 *
	 * The single call makes "check empty + register waiter" atomic with
	 * respect to writers, provided the filesystem does both under its own
	 * lock.
	 *
	 * Contract:
	 * - If @p h is NULL (server could not park the request), behave
	 *   exactly like read() — never return NINEP_READ_DEFER.
	 * - @p h points to server-owned storage only valid during this call;
	 *   copy it by value if deferring.
	 * - The registered handle is answered exactly once. A completion
	 *   attempt after the request was flushed/clunked/session-reset gets
	 *   -ESTALE from ninep_server_read_complete(); treat that as normal.
	 * - LOCK ORDER: never call ninep_server_read_complete() while holding
	 *   a filesystem/application lock. Snapshot what you need under your
	 *   lock, release it, then complete.
	 *
	 * When this op is NULL, the server uses read() for everything and
	 * behavior is identical to servers without deferred-read support.
	 */
	int (*read_deferred)(struct ninep_fs_node *node, uint64_t offset,
	                     uint8_t *buf, uint32_t count, const char *uname,
	                     const struct ninep_read_handle *h, void *fs_ctx);

	/**
	 * @brief Resolve a node to its policy-relevant path
	 *
	 * Used by the server to feed `auth_config->check_perm(identity, path,
	 * mode)` at the start of access-mutating operations (Topen for write,
	 * Tcreate, Tremove). The path string only needs to be precise enough
	 * for the application's permission policy to decide — for prefix-based
	 * policies the top-level directory is sufficient (e.g. "/admin",
	 * "/etc/boardname", "/rooms/lobby"). NUL-terminated.
	 *
	 * OPTIONAL: a filesystem may set this to NULL, in which case the
	 * server skips check_perm for that fs (permissions are then the
	 * responsibility of the filesystem's own handlers). New deployments
	 * should implement it.
	 *
	 * @param node Node to describe
	 * @param buf Output buffer
	 * @param buf_size Size of output buffer (recommend NINEP_MAX_PATH)
	 * @param fs_ctx Filesystem context
	 * @return Number of bytes written (excluding NUL) on success;
	 *         negative error on failure.
	 */
	int (*get_path)(struct ninep_fs_node *node, char *buf, size_t buf_size,
	                void *fs_ctx);
};

/**
 * @brief Authentication configuration
 *
 * Optional public-key authentication using 9P Tauth/Rauth protocol.
 * If provided, server will handle Tauth by generating a challenge.
 * The application callback verifies the identity claim and signature.
 */
struct ninep_auth_config {
	/**
	 * @brief Verify authentication response
	 *
	 * Called when client writes signature+pubkey to auth fid.
	 * Application is responsible for ALL verification:
	 * - Verify identity matches pubkey (e.g., CGA = SHA256(pubkey)[:20])
	 * - Verify signature over challenge
	 * - Check if identity is authorized (not banned, etc.)
	 *
	 * @param identity The claimed identity string from Tauth uname
	 * @param pubkey Public key bytes from auth response
	 * @param pubkey_len Length of public key
	 * @param signature Signature bytes from auth response
	 * @param sig_len Length of signature
	 * @param challenge The challenge that was signed
	 * @param challenge_len Length of challenge
	 * @param auth_ctx Application-provided auth context
	 * @return 0 on success (verified and authorized), negative error code on failure
	 */
	int (*verify_auth)(const char *identity,
	                   const uint8_t *pubkey, size_t pubkey_len,
	                   const uint8_t *signature, size_t sig_len,
	                   const uint8_t *challenge, size_t challenge_len,
	                   void *auth_ctx);

	/**
	 * @brief Check if identity has permission for operation
	 *
	 * Called by filesystem operations to check authorization.
	 * Not used by 9p4z directly - provided for application use.
	 *
	 * @param identity The authenticated identity (or NULL for anonymous)
	 * @param path Path being accessed
	 * @param mode Access mode (NINEP_OREAD, NINEP_OWRITE, etc.)
	 * @param auth_ctx Application-provided auth context
	 * @return 0 if permitted, -EPERM if denied
	 */
	int (*check_perm)(const char *identity, const char *path,
	                  uint8_t mode, void *auth_ctx);

	/** Application-provided context passed to callbacks */
	void *auth_ctx;

	/** If true, require auth for all connections. If false, anonymous allowed. */
	bool required;
};

/**
 * @brief 9P server configuration
 */
struct ninep_server_config {
	const struct ninep_fs_ops *fs_ops;
	void *fs_ctx;
	uint32_t max_message_size;
	const char *version;

	/** Optional authentication config. NULL = no auth required */
	const struct ninep_auth_config *auth_config;
};

/** Challenge size for auth (32 bytes) */
#define NINEP_AUTH_CHALLENGE_SIZE 32

/** Maximum identity string length (e.g., CGA = 40 hex + null = 41) */
#define NINEP_AUTH_IDENTITY_MAX 64

/**
 * @brief Auth FID state (for authentication in progress)
 */
struct ninep_auth_state {
	uint8_t challenge[NINEP_AUTH_CHALLENGE_SIZE];  /**< Random challenge */
	char claimed_identity[NINEP_AUTH_IDENTITY_MAX]; /**< Identity from Tauth uname */
	uint64_t challenge_time;                        /**< Timestamp for expiry */
	bool challenge_issued;                          /**< Challenge has been read */
	bool authenticated;                             /**< Auth completed successfully */
};

/** Invalid index for pools */
#define NINEP_POOL_NONE 0xFF

/** Max concurrently parked (deferred) reads per server/session */
#ifndef CONFIG_NINEP_SERVER_MAX_PENDING_READS
#define CONFIG_NINEP_SERVER_MAX_PENDING_READS 4
#endif

/**
 * @brief Lightweight FID entry (maps FID to filesystem node)
 *
 * Memory-efficient design using pooled auth state and interned usernames:
 * - Old: 220 bytes per FID (embedded auth + uname)
 * - New: ~20 bytes per FID (indices into shared pools)
 *
 * 32 FIDs: 640 bytes (vs old: 7KB)
 */
struct ninep_server_fid {
	uint32_t fid;
	struct ninep_fs_node *node;
	uint32_t iounit;
	uint8_t uname_idx;    /**< Index into uname pool, or NINEP_POOL_NONE */
	uint8_t auth_idx;     /**< Index into auth pool, or NINEP_POOL_NONE */
	bool in_use;
	bool is_auth_fid;     /**< True if this is an auth fid from Tauth */
	bool authenticated;   /**< True if Tattach used a verified afid (uname
	                       *   is then a CGA validated via P-256 over a
	                       *   challenge); false if uname was merely claimed */
	bool is_open;         /**< True after a successful Topen. Tread/Twrite
	                       *   require it, so they cannot bypass the
	                       *   open-time permission check. */
	uint8_t open_mode;    /**< The mode Topen succeeded with (low 2 bits give
	                       *   the access direction). */
};

/**
 * @brief 9P server instance
 *
 * Memory-efficient design:
 * - Lightweight FID table (~20 bytes per FID)
 * - Pooled auth state (only allocated for auth fids in progress)
 * - Interned usernames (shared across FIDs)
 * - Dynamic RX/TX buffers (can use PSRAM on ESP32)
 *
 * Total overhead for 32 FIDs: ~2KB (vs old: ~7KB)
 */
struct ninep_server {
	struct ninep_server_config config;  /* Store by value, not pointer */
	struct ninep_transport *transport;
	uint32_t msize;  /* Negotiated max message size from Tversion */

	/* Lightweight FID table */
	struct ninep_server_fid fids[CONFIG_NINEP_SERVER_MAX_FIDS];

	/* Auth state pool - only a few concurrent auths needed */
	struct ninep_auth_state auth_pool[CONFIG_NINEP_SERVER_AUTH_POOL];
	bool auth_pool_used[CONFIG_NINEP_SERVER_AUTH_POOL];

	/* Interned username pool - most sessions share usernames */
	char uname_pool[CONFIG_NINEP_SERVER_UNAME_POOL][64];
	uint8_t uname_refcount[CONFIG_NINEP_SERVER_UNAME_POOL];

	/* Request/response buffers (dynamically allocated, may use PSRAM) */
	uint8_t *rx_buf;
	uint8_t *tx_buf;
	size_t rx_buf_size;  /* Allocated size for validation */
	size_t tx_buf_size;  /* Allocated size for validation */
	size_t rx_len;

	struct k_mutex tx_buf_mutex;

	/* Deferred-read support (see read_deferred / ninep_server_read_complete).
	 *
	 * pending_reads[] is protected by tx_buf_mutex. The remaining fields
	 * coordinate completion vs. server teardown and are protected by
	 * pending_lock, which is never held while acquiring tx_buf_mutex or
	 * during dispatch, so it cannot participate in a lock cycle. */
	struct ninep_pending_read pending_reads[CONFIG_NINEP_SERVER_MAX_PENDING_READS];
	uint32_t pending_gen;           /**< Monotonic generation counter */
	struct k_mutex pending_lock;
	struct k_condvar pending_cv;
	bool dying;                     /**< Set by cleanup; refuses new completions */
	uint32_t completions_active;    /**< Completions currently touching this server */
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
 * @brief Clean up 9P server - clunk all open fids
 *
 * Call this before destroying a server instance to properly release
 * filesystem resources. This clunks all open fids.
 *
 * @param server Server instance
 */
void ninep_server_cleanup(struct ninep_server *server);

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
 * @brief Complete a parked (deferred) read
 *
 * Answers the Tread previously parked via the read_deferred fs op by
 * building and sending its Rread. Thread-safe; callable from any
 * thread/context (another connection's dispatch, a shell thread, etc.).
 *
 * @param h Handle copied at parking time
 * @param data Payload (may be NULL if len == 0)
 * @param len Payload length; clamped to the request's count. 0 is legal
 *            and answers the read with zero bytes.
 * @return 0 on success;
 *         -ESTALE if the request no longer exists (flushed, clunked,
 *          session reset, or server shutting down) — a normal outcome,
 *          not an error to escalate;
 *         other negative errno on build/transport failure (the request
 *          is freed regardless).
 */
int ninep_server_read_complete(struct ninep_read_handle h,
                               const uint8_t *data, size_t len);

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
