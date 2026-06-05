/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Remote-mount filesystem: an fs_ops backend that transparently forwards 9P
 * operations to another 9P server over a ninep_client, re-exporting a subtree
 * of that server's namespace. See include/zephyr/9p/remote_fs.h.
 *
 * Model: each server-visible node (a pooled struct ninep_remote_node handed
 * back from walk) records its FULL upstream path and an upstream client fid.
 * A walk resolves the child by walking that full path from the live upstream
 * root (re-attaching via root_fn) -- so dynamic upstream trees work correctly:
 * walking `.../clone` allocates exactly one conversation, the returned fid stays
 * bound to it for I/O, and clunk frees it. Walking the full path from the root
 * (rather than cloning a cached parent fid) keeps the proxy clear of the 9P rule
 * that you may not Twalk from an *opened* fid -- the upstream root fid is never
 * opened, and a node's own fid is only ever used for I/O, never as a walk base.
 */

#include <zephyr/9p/remote_fs.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(ninep_remote_fs, CONFIG_NINEP_LOG_LEVEL);

#define RN(node) CONTAINER_OF(node, struct ninep_remote_node, node)

/* ---- node pool (brief rfs->lock around alloc/free/fill) ---- */

static struct ninep_remote_node *node_alloc(struct ninep_remote_fs *rfs)
{
	struct ninep_remote_node *rn = NULL;

	k_mutex_lock(&rfs->lock, K_FOREVER);
	for (size_t i = 0; i < rfs->num_nodes; i++) {
		if (!rfs->nodes[i].in_use) {
			rn = &rfs->nodes[i];
			memset(&rn->node, 0, sizeof(rn->node));
			rn->cfid = NINEP_NOFID;
			rn->path[0] = '\0';
			rn->in_use = true;
			break;
		}
	}
	k_mutex_unlock(&rfs->lock);
	return rn;
}

static void node_free(struct ninep_remote_fs *rfs, struct ninep_remote_node *rn)
{
	k_mutex_lock(&rfs->lock, K_FOREVER);
	rn->in_use = false;
	rn->cfid = NINEP_NOFID;
	k_mutex_unlock(&rfs->lock);
}

/* The upstream path a node maps to: the mount root is the base path itself. */
static const char *node_path(struct ninep_remote_fs *rfs,
			     struct ninep_fs_node *node)
{
	return (node == &rfs->root) ? rfs->base : RN(node)->path;
}

/* Walk @path on the upstream from a freshly-ensured root, with one retry: if the
 * first walk fails the upstream may have rebooted (stale root fid), so drop the
 * session (down_fn) and let root_fn re-attach. On success *cfid is the new fid. */
static int upstream_walk(struct ninep_remote_fs *rfs, const char *path,
			 uint32_t *cfid)
{
	int ret = -EIO;

	for (int attempt = 0; attempt < 2; attempt++) {
		uint32_t root;

		ret = rfs->root_fn(&root, rfs->user);
		if (ret < 0) {
			return ret;
		}
		ret = ninep_client_walk(rfs->client, root, cfid, path);
		if (ret == 0) {
			return 0;
		}
		LOG_DBG("remote_fs: walk '%s' failed: %d%s", path, ret,
			attempt == 0 ? " -- re-attaching upstream" : "");
		if (attempt == 0 && rfs->down_fn) {
			rfs->down_fn(rfs->user);
		}
	}
	return ret;
}

/* ---- fs_ops ---- */

static struct ninep_fs_node *rmount_get_root(void *ctx)
{
	struct ninep_remote_fs *rfs = ctx;

	return &rfs->root;
}

static struct ninep_fs_node *rmount_walk(struct ninep_fs_node *parent,
					 const char *name, uint16_t name_len,
					 void *ctx)
{
	struct ninep_remote_fs *rfs = ctx;
	char elem[32];
	char cpath[sizeof(((struct ninep_remote_node *)0)->path)];

	if (name_len >= sizeof(elem)) {
		return NULL;
	}
	memcpy(elem, name, name_len);
	elem[name_len] = '\0';

	const char *ppath = node_path(rfs, parent);

	if (snprintf(cpath, sizeof(cpath), "%s/%s", ppath, elem) >= (int)sizeof(cpath)) {
		LOG_WRN("remote_fs: path too long: %s/%s", ppath, elem);
		return NULL;
	}

	struct ninep_remote_node *rn = node_alloc(rfs);

	if (!rn) {
		LOG_ERR("remote_fs: node pool full (%zu)", rfs->num_nodes);
		return NULL;
	}

	uint32_t cfid;
	int ret = upstream_walk(rfs, cpath, &cfid);

	if (ret < 0) {
		node_free(rfs, rn);
		return NULL;
	}

	/* Populate the server-visible node from the upstream walk result. */
	struct ninep_qid q;

	memset(&q, 0, sizeof(q));
	(void)ninep_client_get_qid(rfs->client, cfid, &q);

	k_mutex_lock(&rfs->lock, K_FOREVER);
	rn->cfid = cfid;
	strncpy(rn->path, cpath, sizeof(rn->path) - 1);
	strncpy(rn->node.name, elem, sizeof(rn->node.name) - 1);
	rn->node.qid = q;
	rn->node.type = (q.type & NINEP_QTDIR) ? NINEP_NODE_DIR : NINEP_NODE_FILE;
	rn->node.mode = (rn->node.type == NINEP_NODE_DIR) ? (0755 | NINEP_DMDIR)
							  : 0666;
	k_mutex_unlock(&rfs->lock);
	return &rn->node;
}

static int rmount_open(struct ninep_fs_node *node, uint8_t mode, void *ctx)
{
	struct ninep_remote_fs *rfs = ctx;

	/* The mount root is a directory whose listing is served transiently in
	 * read() (a fresh clone, so the persistent walk path is never opened),
	 * so open is a no-op here. */
	if (node == &rfs->root) {
		return 0;
	}

	struct ninep_remote_node *rn = RN(node);
	uint32_t fid;

	k_mutex_lock(&rfs->lock, K_FOREVER);
	fid = rn->in_use ? rn->cfid : NINEP_NOFID;
	k_mutex_unlock(&rfs->lock);
	if (fid == NINEP_NOFID) {
		return -ESTALE;
	}
	return ninep_client_open(rfs->client, fid, mode);
}

static int rmount_read(struct ninep_fs_node *node, uint64_t offset, uint8_t *buf,
		       uint32_t count, const char *uname, void *ctx)
{
	ARG_UNUSED(uname);
	struct ninep_remote_fs *rfs = ctx;

	/* Mount-root directory listing: walk -> open -> read -> clunk a transient
	 * upstream fid. The read is offset-addressed and the upstream dir read is
	 * restartable, so paginated Treads each re-derive from offset. */
	if (node == &rfs->root) {
		uint32_t tmp;
		int ret = upstream_walk(rfs, rfs->base, &tmp);

		if (ret < 0) {
			return ret;
		}
		ret = ninep_client_open(rfs->client, tmp, NINEP_OREAD);
		if (ret < 0) {
			(void)ninep_client_clunk(rfs->client, tmp);
			return ret;
		}
		ret = ninep_client_read(rfs->client, tmp, offset, buf, count);
		(void)ninep_client_clunk(rfs->client, tmp);
		return ret;
	}

	struct ninep_remote_node *rn = RN(node);
	uint32_t fid;

	k_mutex_lock(&rfs->lock, K_FOREVER);
	fid = rn->in_use ? rn->cfid : NINEP_NOFID;
	k_mutex_unlock(&rfs->lock);
	if (fid == NINEP_NOFID) {
		return -ESTALE;
	}
	/* No rfs->lock held across the read: an upstream data read may block (a
	 * Plan 9 /net data read blocks until a datagram arrives), and the
	 * ninep_client is per-tag concurrency-safe, so other proxied ops flow. */
	return ninep_client_read(rfs->client, fid, offset, buf, count);
}

static int rmount_write(struct ninep_fs_node *node, uint64_t offset,
			const uint8_t *buf, uint32_t count, const char *uname,
			void *ctx)
{
	ARG_UNUSED(uname);
	struct ninep_remote_fs *rfs = ctx;

	if (node == &rfs->root) {
		return -EISDIR;
	}

	struct ninep_remote_node *rn = RN(node);
	uint32_t fid;

	k_mutex_lock(&rfs->lock, K_FOREVER);
	fid = rn->in_use ? rn->cfid : NINEP_NOFID;
	k_mutex_unlock(&rfs->lock);
	if (fid == NINEP_NOFID) {
		return -ESTALE;
	}
	return ninep_client_write(rfs->client, fid, offset, buf, count);
}

/* Synthesize the Rstat from the node's own cached qid/name (set at walk),
 * avoiding an extra upstream round-trip and a dangling upstream name pointer. */
static int rmount_stat(struct ninep_fs_node *node, uint8_t *buf, size_t buf_len,
		       void *ctx)
{
	ARG_UNUSED(ctx);
	size_t off = 0;
	int ret = ninep_write_stat(buf, buf_len, &off, &node->qid, node->mode,
				   node->length, node->name, strlen(node->name),
				   NULL, NULL, NULL);

	return ret < 0 ? ret : (int)off;
}

/* The mount-root dir listing is a quick transient walk+read; any child read
 * forwards to the upstream over the client and may block (e.g. a datagram data
 * read), so let the server dispatch those to a worker thread. */
static int rmount_read_will_block(struct ninep_fs_node *node, void *ctx)
{
	struct ninep_remote_fs *rfs = ctx;

	return node == &rfs->root ? 0 : 1;
}

static int rmount_clunk(struct ninep_fs_node *node, void *ctx)
{
	struct ninep_remote_fs *rfs = ctx;

	/* The mount root persists (union_fs handles its clunks and never
	 * delegates them here), and it holds no persistent upstream fid. */
	if (node == &rfs->root) {
		return 0;
	}

	struct ninep_remote_node *rn = RN(node);
	uint32_t fid;

	k_mutex_lock(&rfs->lock, K_FOREVER);
	fid = rn->cfid;
	rn->in_use = false;
	rn->cfid = NINEP_NOFID;
	k_mutex_unlock(&rfs->lock);

	if (fid != NINEP_NOFID) {
		(void)ninep_client_clunk(rfs->client, fid);
	}
	return 0;
}

static const struct ninep_fs_ops remote_fs_ops = {
	.get_root = rmount_get_root,
	.walk = rmount_walk,
	.open = rmount_open,
	.read = rmount_read,
	.write = rmount_write,
	.stat = rmount_stat,
	.clunk = rmount_clunk,
	.read_will_block = rmount_read_will_block,
};

const struct ninep_fs_ops *ninep_remote_fs_get_ops(void)
{
	return &remote_fs_ops;
}

int ninep_remote_fs_init(struct ninep_remote_fs *rfs,
			 struct ninep_client *client, const char *base,
			 struct ninep_remote_node *nodes, size_t num_nodes,
			 ninep_remote_root_fn root_fn,
			 ninep_remote_down_fn down_fn, void *user)
{
	if (!rfs || !client || !base || !nodes || num_nodes == 0 || !root_fn) {
		return -EINVAL;
	}

	memset(rfs, 0, sizeof(*rfs));
	rfs->client = client;
	rfs->base = base;
	rfs->nodes = nodes;
	rfs->num_nodes = num_nodes;
	rfs->root_fn = root_fn;
	rfs->down_fn = down_fn;
	rfs->user = user;
	k_mutex_init(&rfs->lock);

	for (size_t i = 0; i < num_nodes; i++) {
		nodes[i].in_use = false;
		nodes[i].cfid = NINEP_NOFID;
		nodes[i].path[0] = '\0';
	}

	/* Root node: a directory named after the last component of `base`. */
	const char *slash = strrchr(base, '/');
	const char *rname = slash ? slash + 1 : base;

	strncpy(rfs->root.name, rname[0] ? rname : "/", sizeof(rfs->root.name) - 1);
	rfs->root.type = NINEP_NODE_DIR;
	rfs->root.mode = 0755 | NINEP_DMDIR;
	rfs->root.qid.type = NINEP_QTDIR;
	rfs->root.qid.path = 0;

	LOG_INF("remote_fs: re-export upstream '%s' (%zu node pool)", base, num_nodes);
	return 0;
}
