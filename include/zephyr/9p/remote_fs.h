/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9P_REMOTE_FS_H_
#define ZEPHYR_INCLUDE_9P_REMOTE_FS_H_

#include <zephyr/9p/server.h>
#include <zephyr/9p/client.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup ninep_remote_fs 9P Remote-mount filesystem (transparent proxy)
 * @ingroup ninep
 * @{
 *
 * A backend `fs_ops` that forwards 9P operations to *another* 9P server reached
 * over a `struct ninep_client`, re-exporting a subtree of that server's
 * namespace into this server's tree. Walk/open/read/write/clunk on a node are
 * proxied verbatim to the upstream, with a 1:1 mapping between a local
 * (server-side) fid and an upstream (client-side) fid.
 *
 * Unlike a curated per-file proxy, this is fully general: it transparently
 * carries *dynamic* trees (e.g. a Plan 9 /net-style clone/conversation
 * interface, where walking `clone` allocates state upstream) because each local
 * fid keeps a persistent upstream fid for its whole lifetime -- the proxy never
 * re-walks a path, so it never double-allocates.
 *
 * Mount it under a union_fs at the path where the upstream subtree should
 * appear:
 *   ninep_union_fs_mount(&u, "/net/aether",
 *                        ninep_remote_fs_get_ops(), &rfs);
 *
 * Session/reattach policy stays with the caller: @ref ninep_remote_root_fn is
 * asked for the live upstream root fid before walking the base path, and @ref
 * ninep_remote_down_fn is invoked when a walk from the (cached) base fid fails
 * with a stale-fid error so the caller can drop and re-establish its session.
 */

/**
 * @brief One proxied node: a server-visible fs node + its upstream client fid.
 *
 * Pointer-stable for the node's lifetime (the union_fs tracks nodes by pointer),
 * so these live in a caller-provided pool, one per concurrently-walked fid in
 * the re-exported subtree.
 */
struct ninep_remote_node {
	struct ninep_fs_node node;   /* server-visible node (handed back from walk) */
	uint32_t cfid;               /* upstream client fid (NINEP_NOFID if none) */
	char path[48];               /* full upstream path, e.g. "net/aether/3/data" */
	bool in_use;
};

/**
 * @brief (Re)establish the upstream session and return its live root fid.
 *
 * Called before the proxy walks its base path. Implementations typically wrap a
 * lazy Tversion+Tattach (the upstream may boot later / reboot). Return 0 and
 * set *root on success, or a negative errno.
 */
typedef int (*ninep_remote_root_fn)(uint32_t *root, void *user);

/**
 * @brief Notify the caller that the upstream session looks stale.
 *
 * Invoked when a walk from the cached base fid fails (e.g. the upstream rebooted
 * and answers "unknown fid"). The caller should drop its cached session so the
 * next ninep_remote_root_fn() re-attaches. Optional (may be NULL).
 */
typedef void (*ninep_remote_down_fn)(void *user);

/**
 * @brief Remote-mount instance (one per re-exported upstream subtree).
 */
struct ninep_remote_fs {
	struct ninep_client *client;       /* upstream link */
	const char *base;                  /* upstream base path, e.g. "net/aether" */
	ninep_remote_root_fn root_fn;
	ninep_remote_down_fn down_fn;
	void *user;

	struct ninep_remote_node *nodes;   /* caller-provided node pool */
	size_t num_nodes;

	struct ninep_fs_node root;         /* the mount-root node (== base path) */

	struct k_mutex lock;               /* guards the node pool */
};

/**
 * @brief Initialize a remote-mount instance.
 *
 * @param rfs       Instance to initialize
 * @param client    Upstream 9P client (already init'd; attach is lazy via root_fn)
 * @param base      Upstream base path to re-export (e.g. "net/aether"); the
 *                  string must remain valid for the life of @p rfs
 * @param nodes     Caller-provided node pool
 * @param num_nodes Pool size (>= max concurrent fids in the subtree)
 * @param root_fn   Hook returning the live upstream root fid (required)
 * @param down_fn   Hook to drop the upstream session on stale fid (optional)
 * @param user      Opaque pointer passed to root_fn/down_fn
 * @return 0 on success, negative errno on failure
 */
int ninep_remote_fs_init(struct ninep_remote_fs *rfs,
			 struct ninep_client *client, const char *base,
			 struct ninep_remote_node *nodes, size_t num_nodes,
			 ninep_remote_root_fn root_fn,
			 ninep_remote_down_fn down_fn, void *user);

/** @brief fs_ops table for remote-mount backends (shared; ctx = the instance). */
const struct ninep_fs_ops *ninep_remote_fs_get_ops(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9P_REMOTE_FS_H_ */
