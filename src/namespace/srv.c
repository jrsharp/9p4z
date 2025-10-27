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

struct srv_fs_node {
	uint8_t type;
	struct srv_entry *service;     /* For SERVICE nodes */
	struct ninep_fs_node *target;  /* Target filesystem node (for delegation) */
	uint64_t qid_path;
};

static uint64_t srv_next_qid = 2;  /* 1 is reserved for root */

/* Allocate a /srv filesystem node */
static struct srv_fs_node *srv_alloc_node(uint8_t type, struct srv_entry *service)
{
	struct srv_fs_node *node = k_malloc(sizeof(*node));
	if (!node) {
		return NULL;
	}

	node->type = type;
	node->service = service;
	node->target = NULL;
	node->qid_path = (type == SRV_NODE_ROOT) ? 1 : srv_next_qid++;

	/* For SERVICE nodes with local servers, get the target filesystem root */
	if (type == SRV_NODE_SERVICE && service &&
	    service->type == SRV_TYPE_LOCAL && service->local.server) {
		const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
		void *ctx = service->local.server->config->fs_ctx;

		if (ops && ops->get_root) {
			node->target = ops->get_root(ctx);
			LOG_INF("Service node for '%s' -> target=%p (delegates to underlying FS)",
			        service->name, node->target);
		} else {
			LOG_WRN("Service '%s' has no get_root operation!", service->name);
		}
	}

	return node;
}

/* Get root of /srv */
static struct ninep_fs_node *srv_fs_get_root(void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *node = srv_alloc_node(SRV_NODE_ROOT, NULL);
	return (struct ninep_fs_node *)node;
}

/* Walk to a service or deeper into service filesystem */
static struct ninep_fs_node *srv_fs_walk(struct ninep_fs_node *dir, const char *name,
                                          uint16_t name_len, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *dir_node = (struct srv_fs_node *)dir;

	LOG_DBG("srv_fs_walk: type=%d, name='%.*s'", dir_node->type, name_len, name);

	if (dir_node->type == SRV_NODE_ROOT) {
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

		/* Create service proxy node (srv_alloc_node will set target) */
		struct srv_fs_node *node = srv_alloc_node(SRV_NODE_SERVICE, entry);
		LOG_DBG("Created service node for '%s', target=%p", service_name, node ? node->target : NULL);
		return (struct ninep_fs_node *)node;

	} else if (dir_node->type == SRV_NODE_SERVICE && dir_node->target) {
		/* Delegate walk to target filesystem */
		struct srv_entry *service = dir_node->service;
		if (service->type == SRV_TYPE_LOCAL && service->local.server) {
			const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
			void *ctx = service->local.server->config->fs_ctx;

			if (ops && ops->walk) {
				struct ninep_fs_node *child = ops->walk(dir_node->target, name, name_len, ctx);
				LOG_DBG("Delegated walk in '%s' -> child=%p", service->name, child);
				return child;
			}
		}
	}

	/* Otherwise, this might be a raw delegated node (e.g., BBS room node).
	 * Try to find which service might own it. */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config->fs_ops;
			void *ctx = entry->local.server->config->fs_ctx;

			if (ops && ops->walk) {
				/* Try this service - it will return NULL if it doesn't own this node */
				struct ninep_fs_node *child = ops->walk(dir, name, name_len, ctx);
				if (child) {
					/* This service handled it successfully */
					k_mutex_unlock(&global_srv_registry.lock);
					LOG_DBG("Delegated walk to service '%s' via raw node -> child=%p",
					        entry->name, child);
					return child;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	return NULL;
}

/* Stat a node */
static int srv_fs_stat(struct ninep_fs_node *node, uint8_t *buf,
                       size_t buf_len, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *n = (struct srv_fs_node *)node;

	/* For SERVICE nodes with targets, delegate to the target filesystem */
	if (n->type == SRV_NODE_SERVICE && n->target && n->service) {
		struct srv_entry *service = n->service;
		if (service->type == SRV_TYPE_LOCAL && service->local.server) {
			const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
			void *ctx = service->local.server->config->fs_ctx;

			if (ops && ops->stat) {
				LOG_INF("Delegating stat to service '%s' target=%p", service->name, n->target);
				int result = ops->stat(n->target, buf, buf_len, ctx);
				LOG_INF("Stat delegation returned %d bytes", result);
				return result;
			}
		}
	}

	/* Default: stat the srv node itself */
	bool is_dir = (n->type == SRV_NODE_ROOT) ||
	              (n->type == SRV_NODE_SERVICE && n->target != NULL);

	struct ninep_qid qid = {
		.path = n->qid_path,
		.version = 0,
		.type = is_dir ? NINEP_QTDIR : NINEP_QTFILE
	};

	uint32_t mode = is_dir ? (0555 | NINEP_DMDIR) : 0444;
	const char *name = (n->type == SRV_NODE_ROOT) ? "" : n->service->name;
	uint16_t name_len = strlen(name);

	size_t offset = 0;
	int ret = ninep_write_stat(buf, buf_len, &offset, &qid, mode, 0,
	                            name, name_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

/* Open a node */
static int srv_fs_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *n = (struct srv_fs_node *)node;

	/* Delegate to target filesystem if this is a service with a target */
	if (n->type == SRV_NODE_SERVICE && n->target && n->service) {
		struct srv_entry *service = n->service;
		if (service->type == SRV_TYPE_LOCAL && service->local.server) {
			const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
			void *ctx = service->local.server->config->fs_ctx;

			if (ops && ops->open) {
				LOG_DBG("Delegating open to service '%s'", service->name);
				return ops->open(n->target, mode, ctx);
			}
		}
	}

	/* Default: just allow opening */
	return 0;
}

/* Read directory or service info */
static int srv_fs_read(struct ninep_fs_node *node, uint64_t offset,
                       uint8_t *buf, uint32_t count, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	LOG_DBG("srv_fs_read: node=%p, offset=%llu, count=%u", node, offset, count);

	struct srv_fs_node *n = (struct srv_fs_node *)node;

	/* Delegate to target filesystem if this is a service with a target */
	if (n->type == SRV_NODE_SERVICE && n->target && n->service) {
		struct srv_entry *service = n->service;
		if (service->type == SRV_TYPE_LOCAL && service->local.server) {
			const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
			void *ctx = service->local.server->config->fs_ctx;

			if (ops && ops->read) {
				LOG_DBG("Delegating read to service '%s' via srv node", service->name);
				return ops->read(n->target, offset, buf, count, ctx);
			}
		}
	}

	/* Otherwise, this might be a raw delegated node (e.g., BBS room node).
	 * Try to find which service might own it. */
	if (n->type != SRV_NODE_ROOT) {
		k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
		struct srv_entry *entry = global_srv_registry.services;
		while (entry) {
			if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
				const struct ninep_fs_ops *ops = entry->local.server->config->fs_ops;
				void *ctx = entry->local.server->config->fs_ctx;

				if (ops && ops->read) {
					/* Try this service - it will return -EINVAL if it doesn't own this node */
					int ret = ops->read(node, offset, buf, count, ctx);
					if (ret >= 0 || ret != -EINVAL) {
						/* This service handled it (success or legitimate error) */
						k_mutex_unlock(&global_srv_registry.lock);
						if (ret > 0) {
							LOG_DBG("Delegated read to service '%s' via raw node (%d bytes)",
							        entry->name, ret);
						}
						return ret;
					}
				}
			}
			entry = entry->next;
		}
		k_mutex_unlock(&global_srv_registry.lock);
	}

	if (n->type == SRV_NODE_ROOT) {
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
	} else {
		/* Reading service file - return service info */
		const char *info = "9P service\n";
		uint32_t info_len = strlen(info);

		if (offset >= info_len) {
			return 0;
		}

		uint32_t to_read = info_len - offset;
		if (to_read > count) {
			to_read = count;
		}

		memcpy(buf, info + offset, to_read);
		return to_read;
	}
}

/* Clunk (close) a node */
static int srv_fs_clunk(struct ninep_fs_node *node, void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *n = (struct srv_fs_node *)node;

	LOG_DBG("srv_fs_clunk: node=%p", node);

	/* Only free nodes we allocated (ROOT and SERVICE types)
	 * Don't free nodes from other filesystems (returned by walk to service root)
	 */
	if (n->type == SRV_NODE_ROOT || n->type == SRV_NODE_SERVICE) {
		LOG_DBG("srv_fs_clunk: freeing srv node type=%d", n->type);
		k_free(node);
	} else {
		LOG_DBG("srv_fs_clunk: not freeing - not an srv node");
	}

	return 0;
}

/* Write - delegate to underlying service */
static int srv_fs_write(struct ninep_fs_node *node, uint64_t offset,
                        const uint8_t *buf, uint32_t count, const char *uname,
                        void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *srv_node = (struct srv_fs_node *)node;

	/* If this is an srv service node, delegate through the target */
	if (srv_node->type == SRV_NODE_SERVICE && srv_node->target && srv_node->service) {
		struct srv_entry *service = srv_node->service;
		if (service->type == SRV_TYPE_LOCAL && service->local.server) {
			const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
			void *ctx = service->local.server->config->fs_ctx;

			if (ops && ops->write) {
				LOG_DBG("Delegating write to service '%s' via srv node", service->name);
				return ops->write(srv_node->target, offset, buf, count, uname, ctx);
			}
		}
	}

	/* Otherwise, this might be a raw delegated node (e.g., BBS message node).
	 * Try to find which service might own it. */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config->fs_ops;
			void *ctx = entry->local.server->config->fs_ctx;

			if (ops && ops->write) {
				/* Try this service - it will return -EINVAL if it doesn't own this node */
				int ret = ops->write(node, offset, buf, count, uname, ctx);
				if (ret >= 0 || ret != -EINVAL) {
					/* This service handled it (success or legitimate error) */
					k_mutex_unlock(&global_srv_registry.lock);
					if (ret > 0) {
						LOG_DBG("Delegated write to service '%s' via raw node", entry->name);
					}
					return ret;
				}
			}
		}
		entry = entry->next;
	}
	k_mutex_unlock(&global_srv_registry.lock);

	return -EROFS;
}

/* Other operations not supported */
static int srv_fs_create(struct ninep_fs_node *dir, const char *name,
                         uint16_t name_len, uint32_t perm, uint8_t mode,
                         const char *uname, struct ninep_fs_node **child,
                         void *fs_ctx)
{
	ARG_UNUSED(fs_ctx);

	struct srv_fs_node *dir_node = (struct srv_fs_node *)dir;

	/* If this is an srv service node, delegate through the target */
	if (dir_node->type == SRV_NODE_SERVICE && dir_node->target && dir_node->service) {
		struct srv_entry *service = dir_node->service;
		if (service->type == SRV_TYPE_LOCAL && service->local.server) {
			const struct ninep_fs_ops *ops = service->local.server->config->fs_ops;
			void *ctx = service->local.server->config->fs_ctx;

			if (ops && ops->create) {
				LOG_DBG("Delegating create to service '%s' via srv node", service->name);
				return ops->create(dir_node->target, name, name_len, perm,
				                   mode, uname, child, ctx);
			}
		}
	}

	/* Otherwise, this might be a raw delegated node (e.g., BBS node inside /srv/bbs).
	 * Try to find which service might own it by checking all services. */
	k_mutex_lock(&global_srv_registry.lock, K_FOREVER);
	struct srv_entry *entry = global_srv_registry.services;
	while (entry) {
		if (entry->type == SRV_TYPE_LOCAL && entry->local.server) {
			const struct ninep_fs_ops *ops = entry->local.server->config->fs_ops;
			void *ctx = entry->local.server->config->fs_ctx;

			if (ops && ops->create) {
				/* Try this service - it will return -EINVAL if it doesn't own this node */
				int ret = ops->create(dir, name, name_len, perm,
				                      mode, uname, child, ctx);
				if (ret != -EINVAL) {
					/* This service handled it (success or legitimate error) */
					k_mutex_unlock(&global_srv_registry.lock);
					if (ret == 0) {
						LOG_DBG("Delegated create to service '%s' via raw node", entry->name);
					}
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
