/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/namespace/srv.h>
#include <zephyr/namespace/namespace.h>
#include <zephyr/9p/server.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(srv, CONFIG_NINEP_LOG_LEVEL);

/* ========================================================================
 * Service Registry
 * ======================================================================== */

static struct srv_registry global_srv_registry;
static bool srv_initialized = false;

/* ========================================================================
 * Service Management
 * ======================================================================== */

int srv_post(const char *name, struct ninep_server *server)
{
	if (!name || !server) {
		return -EINVAL;
	}

	if (!srv_initialized) {
		LOG_ERR("/srv not initialized");
		return -EINVAL;
	}

	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

	/* Check if service already exists */
	struct srv_entry *e = global_srv_registry.services;
	while (e) {
		if (strcmp(e->name, name) == 0) {
			LOG_WRN("Service %s already registered", name);
			k_mutex_unlock(&global_srv_registry.lock);
			return -EEXIST;
		}
		e = e->next;
	}

	/* Check service limit */
	if (global_srv_registry.num_services >= CONFIG_SRV_MAX_SERVICES) {
		k_mutex_unlock(&global_srv_registry.lock);
		return -ENOMEM;
	}

	/* Create new service entry */
	struct srv_entry *entry = k_malloc(sizeof(*entry));
	if (!entry) {
		k_mutex_unlock(&global_srv_registry.lock);
		return -ENOMEM;
	}

	memset(entry, 0, sizeof(*entry));
	strncpy(entry->name, name, sizeof(entry->name) - 1);
	entry->type = SRV_TYPE_LOCAL;
	entry->local.server = server;
	atomic_set(&entry->refcount, 1);

	/* Add to list */
	entry->next = global_srv_registry.services;
	global_srv_registry.services = entry;
	global_srv_registry.num_services++;

	k_mutex_unlock(&global_srv_registry.lock);

	LOG_INF("Posted service: /srv/%s (in-process server)", name);
	return 0;
}

int srv_post_network(const char *name, const char *transport, const char *address)
{
	if (!name || !transport || !address) {
		return -EINVAL;
	}

	if (!srv_initialized) {
		LOG_ERR("/srv not initialized");
		return -EINVAL;
	}

	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

	/* Check if service already exists */
	struct srv_entry *e = global_srv_registry.services;
	while (e) {
		if (strcmp(e->name, name) == 0) {
			LOG_WRN("Service %s already registered", name);
			k_mutex_unlock(&global_srv_registry.lock);
			return -EEXIST;
		}
		e = e->next;
	}

	/* Check service limit */
	if (global_srv_registry.num_services >= CONFIG_SRV_MAX_SERVICES) {
		k_mutex_unlock(&global_srv_registry.lock);
		return -ENOMEM;
	}

	/* Create new service entry */
	struct srv_entry *entry = k_malloc(sizeof(*entry));
	if (!entry) {
		k_mutex_unlock(&global_srv_registry.lock);
		return -ENOMEM;
	}

	memset(entry, 0, sizeof(*entry));
	strncpy(entry->name, name, sizeof(entry->name) - 1);
	entry->type = SRV_TYPE_NETWORK;
	strncpy(entry->network.transport, transport, sizeof(entry->network.transport) - 1);
	strncpy(entry->network.address, address, sizeof(entry->network.address) - 1);
	atomic_set(&entry->refcount, 1);

	/* Add to list */
	entry->next = global_srv_registry.services;
	global_srv_registry.services = entry;
	global_srv_registry.num_services++;

	k_mutex_unlock(&global_srv_registry.lock);

	LOG_INF("Posted service: /srv/%s (%s://%s)", name, transport, address);
	return 0;
}

int srv_remove(const char *name)
{
	if (!name) {
		return -EINVAL;
	}

	if (!srv_initialized) {
		return -EINVAL;
	}

	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

	struct srv_entry **entry_ptr = &global_srv_registry.services;
	while (*entry_ptr) {
		if (strcmp((*entry_ptr)->name, name) == 0) {
			struct srv_entry *to_remove = *entry_ptr;
			*entry_ptr = to_remove->next;
			global_srv_registry.num_services--;

			/* Free entry */
			k_free(to_remove);

			k_mutex_unlock(&global_srv_registry.lock);
			LOG_INF("Removed service: /srv/%s", name);
			return 0;
		}
		entry_ptr = &(*entry_ptr)->next;
	}

	k_mutex_unlock(&global_srv_registry.lock);
	return -ENOENT;
}

void srv_foreach(void (*callback)(const struct srv_entry *, void *), void *user_data)
{
	if (!callback || !srv_initialized) {
		return;
	}

	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

	struct srv_entry *e = global_srv_registry.services;
	while (e) {
		callback(e, user_data);
		e = e->next;
	}

	k_mutex_unlock(&global_srv_registry.lock);
}

struct srv_entry *srv_lookup(const char *name)
{
	if (!name || !srv_initialized) {
		return NULL;
	}

	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

	struct srv_entry *e = global_srv_registry.services;
	while (e) {
		if (strcmp(e->name, name) == 0) {
			k_mutex_unlock(&global_srv_registry.lock);
			return e;
		}
		e = e->next;
	}

	k_mutex_unlock(&global_srv_registry.lock);
	return NULL;
}

/* ========================================================================
 * Client-Side Operations
 * ======================================================================== */

int srv_mount(const char *srv_name, const char *mnt_point, uint32_t flags)
{
	if (!srv_name || !mnt_point) {
		return -EINVAL;
	}

	/* Look up service */
	struct srv_entry *entry = srv_lookup(srv_name);
	if (!entry) {
		LOG_ERR("Service not found: /srv/%s", srv_name);
		return -ENOENT;
	}

	int ret = 0;

	switch (entry->type) {
	case SRV_TYPE_LOCAL:
		/* Mount in-process server */
		ret = ns_mount_server(entry->local.server, mnt_point, flags);
		if (ret < 0) {
			LOG_ERR("Failed to mount local service %s: %d", srv_name, ret);
			return ret;
		}
		LOG_INF("Mounted /srv/%s -> %s (local)", srv_name, mnt_point);
		break;

	case SRV_TYPE_NETWORK:
		/* TODO: Create 9P client connection and mount */
		LOG_ERR("Network service mounting not yet implemented");
		ret = -ENOTSUP;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* ========================================================================
 * /srv Synthetic Filesystem
 * ======================================================================== */

/* Node types for /srv filesystem */
#define SRV_NODE_ROOT 0
#define SRV_NODE_SERVICE 1

/* srv only allocates one node: the /srv root directory.
 * All other nodes are from underlying filesystems (BBS, sysfs, etc.) */
static struct ninep_fs_node *srv_root_node = NULL;

static uint64_t srv_next_qid = 2;  /* 1 is reserved for root */

/* Create the /srv root directory node (only called once) */
static struct ninep_fs_node *srv_alloc_root_node(void)
{
	struct ninep_fs_node *node = k_malloc(sizeof(*node));
	if (!node) {
		return NULL;
	}

	memset(node, 0, sizeof(*node));
	strncpy(node->name, "", sizeof(node->name) - 1);  /* Empty name for root */
	node->type = NINEP_NODE_DIR;
	node->mode = 0555 | NINEP_DMDIR;
	node->qid.type = NINEP_QTDIR;
	node->qid.version = 0;
	node->qid.path = 1;

	return node;
}

/* Get root of /srv */
static struct ninep_fs_node *srv_fs_get_root(void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	/* Allocate root node on first call */
	if (!srv_root_node) {
		srv_root_node = srv_alloc_root_node();
	}
	return srv_root_node;
}

/* Walk to a service or deeper into service filesystem */
static struct ninep_fs_node *srv_fs_walk(struct ninep_fs_node *dir, const char *name,
                                          uint16_t name_len, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	LOG_DBG("srv_fs_walk: name='%.*s'", name_len, name);

	/* Check if walking from /srv root */
	if (dir == srv_root_node) {
		/* Walking from /srv root to a service */
		char service_name[256];
		if (name_len >= sizeof(service_name)) {
			return NULL;
		}
		memcpy(service_name, name, name_len);
		service_name[name_len] = '\0';

		struct srv_entry *entry = srv_lookup(service_name);
		if (!entry) {
			return NULL;
		}

		/* For local services, return the service's root node directly.
		 * This avoids invalid casts and ensures union_fs tracks the actual
		 * filesystem nodes that have proper ninep_fs_node structure. */
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->get_root) {
				struct ninep_fs_node *target = ops->get_root(ctx);
				LOG_DBG("Returning service root for '%s': target=%p", service_name, target);
				return target;
			}
		}

		/* For network services (not yet implemented), we'd need different handling */
		LOG_WRN("Service '%s' has no accessible root", service_name);
		return NULL;

	}

	/* If dir is not srv_root_node, we need to delegate to the service that owns this node.
	 * Walk through all services to find which one owns this node. */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			/* Check if this service's root matches our dir, or if dir is a descendant.
			 * For simplicity, we check if the service root matches the dir pointer exactly.
			 * For deeper nodes, we rely on the fact that BBS (and other services) will
			 * return nodes that can be checked. */
			if (ops && ops->get_root) {
				struct ninep_fs_node *service_root = ops->get_root(ctx);
				if (service_root == dir) {
					/* Found the service that owns this node - delegate the walk */
					k_mutex_unlock(&global_srv_registry.lock);
					if (ops->walk) {
						LOG_DBG("Delegating walk to service '%s' filesystem", entry->name);
						return ops->walk(dir, name, name_len, ctx);
					}
					LOG_WRN("Service '%s' has no walk function", entry->name);
					return NULL;
				}
			}
		}
		entry = entry->next;
	}

	k_mutex_unlock(&global_srv_registry.lock);

	/* If we couldn't find a service root match, this might be a deeper node.
	 * Try delegating to each service - the service will return NULL if it
	 * doesn't own the node. */
	LOG_DBG("Node %p (%s) not a service root, trying all services", dir, dir->name);

	/* We need to iterate without holding the lock during the walk call.
	 * Collect service info first. */
	struct {
		const struct ninep_fs_ops *ops;
		void *ctx;
		char name[32];
	} services[32];
	int num_services = 0;

	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	entry = global_srv_registry.services;
	while (entry && num_services < 32) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			services[num_services].ops = entry->local.server->config.fs_ops;
			services[num_services].ctx = entry->local.server->config.fs_ctx;
			strncpy(services[num_services].name, entry->name, sizeof(services[num_services].name) - 1);
			services[num_services].name[sizeof(services[num_services].name) - 1] = '\0';
			num_services++;
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	/* Now try each service */
	for (int i = 0; i < num_services; i++) {
		if (services[i].ops && services[i].ops->walk) {
			LOG_DBG("Trying service '%s' for node %p", services[i].name, dir);
			struct ninep_fs_node *result = services[i].ops->walk(dir, name, name_len, services[i].ctx);
			if (result) {
				LOG_DBG("Service '%s' handled the walk", services[i].name);
				return result;
			}
		}
	}

	/* Walk failed - normal "file not found" case, no logging needed */
	return NULL;
}

/* Stat a node */
static int srv_fs_stat(struct ninep_fs_node *node, uint8_t *buf,
                       size_t buf_len, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	/* Check if this is the srv root node */
	if (node == srv_root_node) {
		/* Stat the /srv directory itself */
		size_t offset = 0;
		int ret = ninep_write_stat(buf, buf_len, &offset, &node->qid,
		                            node->mode, 0, node->name, strlen(node->name));
		return (ret < 0) ? ret : offset;
	}

	/* Otherwise, delegate to underlying services.
	 * Since we return target nodes directly from walk, this should normally
	 * be handled by the service's own stat handler via union_fs delegation. */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->stat) {
				int ret = ops->stat(node, buf, buf_len, ctx);
				if (ret >= 0 || ret != -EINVAL) {
					k_mutex_unlock(&global_srv_registry.lock);
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	return -ENOENT;
}

/* Open a node */
static int srv_fs_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	/* Check if this is the srv root node */
	if (node == srv_root_node) {
		/* Allow opening /srv directory */
		return 0;
	}

	/* Delegate to underlying services */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->open) {
				int ret = ops->open(node, mode, ctx);
				if (ret == 0 || ret != -EINVAL) {
					k_mutex_unlock(&global_srv_registry.lock);
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	/* No service claimed it, but allow opening anyway */
	return 0;
}

/* Read directory or service info */
static int srv_fs_read(struct ninep_fs_node *node, uint64_t offset,
                       uint8_t *buf, uint32_t count, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	LOG_DBG("srv_fs_read: node=%p, offset=%llu, count=%u", node, offset, count);

	/* Check if this is the srv root node */
	if (node == srv_root_node) {
		/* Reading directory - list services */
		uint32_t entry_index = 0;
		uint32_t buf_offset = 0;
		uint64_t current_offset = 0;

		k_mutex_lock(&global_srv_registry.lock, K_FOREVER);

		LOG_DBG("/srv read: offset=%llu, count=%u, num_services=%d",
		        offset, count, global_srv_registry.num_services);

		struct srv_entry *entry = global_srv_registry.services;
		while (entry) {
			LOG_DBG("  Entry: %s", entry->name);

			uint16_t name_len = strlen(entry->name);

			/* Calculate stat size (ninep_write_stat includes size[2] field) */
			const char *uid = "zephyr";
			const char *gid = "zephyr";
			const char *muid = "zephyr";
			uint16_t uid_len = strlen(uid);
			uint16_t gid_len = strlen(gid);
			uint16_t muid_len = strlen(muid);

			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                      (2 + name_len) + (2 + uid_len) +
			                      (2 + gid_len) + (2 + muid_len);
			uint32_t entry_size = 2 + stat_size;  /* size[2] + stat data */

			/* Check if this entry is past the requested offset */
			if (current_offset >= offset) {
				/* Check if we have space */
				if (buf_offset + entry_size > count) {
					break;
				}

				/* Write stat using ninep_write_stat (it writes size[2] + stat) */
				size_t stat_offset = buf_offset;

				/* Local services with servers appear as directories */
				bool is_dir = (entry->type == SRV_TYPE_LOCAL && entry->local.server != NULL);

				struct ninep_qid qid = {
					.type = is_dir ? NINEP_QTDIR : NINEP_QTFILE,
					.path = 2 + entry_index,
					.version = 0
				};

				uint32_t mode = is_dir ? (0555 | NINEP_DMDIR) : 0444;
				int ret = ninep_write_stat(buf, count, &stat_offset, &qid, mode, 0,
				                            entry->name, name_len);
				if (ret < 0) {
					k_mutex_unlock(&global_srv_registry.lock);
					return ret;
				}

				buf_offset = stat_offset;
			}

			current_offset += entry_size;
			entry_index++;
			entry = entry->next;
		}

		k_mutex_unlock(&global_srv_registry.lock);
		return buf_offset;
	}

	/* Not the srv root - delegate to underlying services */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->read) {
				int ret = ops->read(node, offset, buf, count, ctx);
				if (ret >= 0 || ret != -EINVAL) {
					k_mutex_unlock(&global_srv_registry.lock);
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	return -ENOENT;
}

/* Clunk (close) a node */
static int srv_fs_clunk(struct ninep_fs_node *node, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	LOG_DBG("srv_fs_clunk: node=%p name='%s'", node, node->name);

	/* Check if this is the srv root - DON'T free it, it's reused */
	if (node == srv_root_node) {
		LOG_DBG("srv_fs_clunk: srv root clunked (not freeing, will be reused)");
		return 0;
	}

	/* For all other nodes, they belong to underlying services.
	 * Delegate clunk to the owning service. */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->clunk) {
				/* Try this service's clunk - it will return error if it doesn't own the node */
				int ret = ops->clunk(node, ctx);
				if (ret == 0 || ret != -EINVAL) {
					/* This service handled it */
					k_mutex_unlock(&global_srv_registry.lock);
					LOG_DBG("Delegated clunk to service '%s'", entry->name);
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	/* No service claimed this node - it might be a node without clunk handler */
	LOG_DBG("srv_fs_clunk: no service claimed node '%s'", node->name);
	return 0;
}

/* Write - delegate to underlying service */
static int srv_fs_write(struct ninep_fs_node *node, uint64_t offset,
                        const uint8_t *buf, uint32_t count, const char *uname,
                        void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	/* Delegate to underlying services */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->write) {
				int ret = ops->write(node, offset, buf, count, uname, ctx);
				if (ret >= 0 || ret != -EINVAL) {
					k_mutex_unlock(&global_srv_registry.lock);
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	return -EROFS;
}

/* Create operation - delegate to underlying service */
static int srv_fs_create(struct ninep_fs_node *dir, const char *name,
                         uint16_t name_len, uint32_t perm, uint8_t mode,
                         const char *uname, struct ninep_fs_node **child,
                         void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	/* Delegate to underlying services */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config.fs_ops;
			void *ctx = entry->local.server->config.fs_ctx;

			if (ops && ops->create) {
				int ret = ops->create(dir, name, name_len, perm,
				                      mode, uname, child, ctx);
				if (ret != -EINVAL) {
					k_mutex_unlock(&global_srv_registry.lock);
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	/* Can't create directly in /srv root */
	return -EROFS;
}

static int srv_fs_remove(struct ninep_fs_node *node, void *fs_ctx)
{
	ARG_UNUSED(node);
	ARG_UNUSED(fs_ctx);

	return -EROFS;
}

static const struct ninep_fs_ops srv_fs_ops = {
	.get_root = srv_fs_get_root,
	.walk = srv_fs_walk,
	.open = srv_fs_open,
	.read = srv_fs_read,
	.write = srv_fs_write,
	.stat = srv_fs_stat,
	.create = srv_fs_create,
	.remove = srv_fs_remove,
	.clunk = srv_fs_clunk,
};

const struct ninep_fs_ops *srv_get_fs_ops(void)
{
	return &srv_fs_ops;
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

int srv_init(void)
{
	if (srv_initialized) {
		return 0;
	}

	memset(&global_srv_registry, 0, sizeof(global_srv_registry));
	k_mutex_init(&global_srv_registry.lock);

	srv_initialized = true;
	LOG_INF("/srv service registry initialized");

	return 0;
}
