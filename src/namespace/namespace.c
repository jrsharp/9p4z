/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/namespace/namespace.h>
#include <zephyr/namespace/fs_9p.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(namespace, CONFIG_NINEP_LOG_LEVEL);

/* ========================================================================
 * Global State
 * ======================================================================== */

/* Thread-local storage for current namespace */
#if defined(CONFIG_NS_THREAD_LOCAL_STORAGE) && defined(CONFIG_THREAD_LOCAL_STORAGE)
__thread struct thread_namespace *current_ns = NULL;
#else
/* Hash table for thread ID -> namespace mapping */
static struct {
	k_tid_t tid;
	struct thread_namespace *ns;
} ns_thread_map[CONFIG_NS_MAX_MOUNTS_PER_THREAD];
static struct k_mutex ns_thread_map_lock;
#endif

/* Global namespace initialization flag */
static bool ns_initialized = false;

/* ========================================================================
 * Path Utilities
 * ======================================================================== */

/**
 * @brief Normalize a path (remove .., ., //)
 */
static int path_normalize(const char *path, char *normalized, size_t norm_len)
{
	if (!path || !normalized || norm_len == 0) {
		return -EINVAL;
	}

	size_t i = 0, j = 0;
	bool last_was_slash = false;

	/* Start with / if input is absolute */
	if (path[0] == '/') {
		normalized[j++] = '/';
		i++;
		last_was_slash = true;
	}

	while (path[i] && j < norm_len - 1) {
		if (path[i] == '/') {
			if (!last_was_slash) {
				normalized[j++] = '/';
				last_was_slash = true;
			}
			i++;
		} else if (path[i] == '.' && path[i + 1] == '.' &&
		           (path[i + 2] == '/' || path[i + 2] == '\0')) {
			/* Handle .. */
			if (j > 1) {
				j -= 2;  /* Remove trailing / */
				while (j > 0 && normalized[j] != '/') {
					j--;
				}
				j++;
			}
			i += 2;
			if (path[i] == '/') {
				i++;
			}
			last_was_slash = false;
		} else if (path[i] == '.' && (path[i + 1] == '/' || path[i + 1] == '\0')) {
			/* Handle . */
			i++;
			if (path[i] == '/') {
				i++;
			}
		} else {
			normalized[j++] = path[i++];
			last_was_slash = false;
		}
	}

	/* Remove trailing slash unless it's the root */
	if (j > 1 && normalized[j - 1] == '/') {
		j--;
	}

	normalized[j] = '\0';

	return 0;
}

/**
 * @brief Check if path has prefix
 */
static bool path_has_prefix(const char *path, const char *prefix)
{
	size_t plen = strlen(prefix);

	/* Root prefix matches everything */
	if (plen == 1 && prefix[0] == '/') {
		return true;
	}

	/* Check prefix match */
	if (strncmp(path, prefix, plen) != 0) {
		return false;
	}

	/* Ensure it's a full path component match */
	if (path[plen] != '\0' && path[plen] != '/') {
		return false;
	}

	return true;
}

/**
 * @brief Hash function for namespace entries
 */
static uint32_t ns_hash(const char *path)
{
	uint32_t hash = 5381;
	int c;

	while ((c = *path++)) {
		hash = ((hash << 5) + hash) + c;
	}

	return hash % CONFIG_NS_HASH_SIZE;
}

/* ========================================================================
 * Namespace Initialization
 * ======================================================================== */

int ns_init(void)
{
	if (ns_initialized) {
		return 0;
	}

#if !defined(CONFIG_NS_THREAD_LOCAL_STORAGE) || !defined(CONFIG_THREAD_LOCAL_STORAGE)
	k_mutex_init(&ns_thread_map_lock);
	memset(ns_thread_map, 0, sizeof(ns_thread_map));
#endif

	/* TODO: Initialize 9P VFS driver for network mounts
	 * This would register a VFS driver that allows mounting remote
	 * 9P servers through Zephyr's VFS layer. For now, we only use
	 * in-process servers accessed via /srv, so this is not needed.
	 *
	 * int ret = fs_9p_init();
	 * if (ret < 0) {
	 *     LOG_ERR("Failed to initialize 9P VFS driver: %d", ret);
	 *     return ret;
	 * }
	 */

	ns_initialized = true;
	LOG_INF("Namespace subsystem initialized");

	return 0;
}

/**
 * @brief Allocate and initialize a new namespace
 */
static struct thread_namespace *ns_alloc(void)
{
	struct thread_namespace *ns = k_malloc(sizeof(*ns));
	if (!ns) {
		return NULL;
	}

	memset(ns, 0, sizeof(*ns));
	k_mutex_init(&ns->lock);
	atomic_set(&ns->refcount, 1);

	return ns;
}

/**
 * @brief Free a namespace
 */
static void ns_free(struct thread_namespace *ns)
{
	if (!ns) {
		return;
	}

	/* Free all namespace entries */
	for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
		struct ns_entry *entry = ns->entries[i];
		while (entry) {
			struct ns_entry *next = entry->next;
			k_free(entry);
			entry = next;
		}
	}

	k_free(ns);
}

int ns_create(struct thread_namespace *parent)
{
	struct thread_namespace *ns;

	if (parent) {
		/* Create COW namespace */
		ns = ns_alloc();
		if (!ns) {
			return -ENOMEM;
		}

		ns->parent = parent;
		ns->is_cow = true;
		atomic_inc(&parent->refcount);
	} else {
		/* Create fresh namespace */
		ns = ns_alloc();
		if (!ns) {
			return -ENOMEM;
		}
	}

	ns->thread_id = k_current_get();

	/* Set as current namespace */
#if defined(CONFIG_NS_THREAD_LOCAL_STORAGE) && defined(CONFIG_THREAD_LOCAL_STORAGE)
	current_ns = ns;
#else
	k_mutex_lock(&ns_thread_map_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NS_MAX_MOUNTS_PER_THREAD; i++) {
		if (ns_thread_map[i].tid == NULL) {
			ns_thread_map[i].tid = ns->thread_id;
			ns_thread_map[i].ns = ns;
			break;
		}
	}
	k_mutex_unlock(&ns_thread_map_lock);
#endif

	LOG_DBG("Created namespace for thread %p (parent=%p)", ns->thread_id, parent);
	return 0;
}

int ns_fork(k_tid_t child_tid)
{
	struct thread_namespace *parent = ns_get_current();
	if (!parent) {
		return -EINVAL;
	}

	struct thread_namespace *child = ns_alloc();
	if (!child) {
		return -ENOMEM;
	}

	child->thread_id = child_tid;
	child->parent = parent;
	child->is_cow = true;
	atomic_inc(&parent->refcount);

	/* Store child namespace */
#if defined(CONFIG_NS_THREAD_LOCAL_STORAGE) && defined(CONFIG_THREAD_LOCAL_STORAGE)
	/* TLS is per-thread, child will have its own */
	/* We'll set it when the child thread starts */
#else
	k_mutex_lock(&ns_thread_map_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NS_MAX_MOUNTS_PER_THREAD; i++) {
		if (ns_thread_map[i].tid == NULL) {
			ns_thread_map[i].tid = child_tid;
			ns_thread_map[i].ns = child;
			break;
		}
	}
	k_mutex_unlock(&ns_thread_map_lock);
#endif

	LOG_DBG("Forked namespace for child thread %p", child_tid);
	return 0;
}

int ns_destroy(k_tid_t tid)
{
	struct thread_namespace *ns = NULL;

#if defined(CONFIG_NS_THREAD_LOCAL_STORAGE) && defined(CONFIG_THREAD_LOCAL_STORAGE)
	if (tid == k_current_get()) {
		ns = current_ns;
		current_ns = NULL;
	}
#else
	k_mutex_lock(&ns_thread_map_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NS_MAX_MOUNTS_PER_THREAD; i++) {
		if (ns_thread_map[i].tid == tid) {
			ns = ns_thread_map[i].ns;
			ns_thread_map[i].tid = NULL;
			ns_thread_map[i].ns = NULL;
			break;
		}
	}
	k_mutex_unlock(&ns_thread_map_lock);
#endif

	if (!ns) {
		return -ENOENT;
	}

	/* Decrement refcount */
	if (atomic_dec(&ns->refcount) == 1) {
		/* Last reference, free namespace */
		if (ns->parent) {
			ns_destroy((k_tid_t)ns->parent->thread_id);
		}
		ns_free(ns);
	}

	LOG_DBG("Destroyed namespace for thread %p", tid);
	return 0;
}

/* ========================================================================
 * Namespace Manipulation
 * ======================================================================== */

/**
 * @brief Ensure namespace is writable (break COW if needed)
 */
static int ns_make_writable(struct thread_namespace *ns)
{
	if (!ns->is_cow) {
		return 0;  /* Already writable */
	}

	k_mutex_lock(&ns->lock, K_FOREVER);

	/* Copy parent's entries */
	if (ns->parent) {
		for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
			struct ns_entry *e = ns->parent->entries[i];
			while (e) {
				struct ns_entry *copy = k_malloc(sizeof(*copy));
				if (!copy) {
					k_mutex_unlock(&ns->lock);
					return -ENOMEM;
				}

				memcpy(copy, e, sizeof(*copy));
				copy->next = ns->entries[i];
				ns->entries[i] = copy;

				e = e->next;
			}
		}

		/* Drop parent reference */
		if (atomic_dec(&ns->parent->refcount) == 1) {
			ns_free(ns->parent);
		}
		ns->parent = NULL;
	}

	ns->is_cow = false;
	k_mutex_unlock(&ns->lock);

	return 0;
}

int ns_mount(struct fs_mount_t *vfs_mount, const char *mnt_point, uint32_t flags)
{
	struct thread_namespace *ns = ns_get_current();
	if (!ns) {
		LOG_ERR("No namespace for current thread");
		return -EINVAL;
	}

	if (!vfs_mount || !mnt_point) {
		return -EINVAL;
	}

	/* Ensure namespace is writable */
	int ret = ns_make_writable(ns);
	if (ret < 0) {
		return ret;
	}

	/* Normalize mount point path */
	char norm_path[CONFIG_NS_MAX_PATH_LEN];
	ret = path_normalize(mnt_point, norm_path, sizeof(norm_path));
	if (ret < 0) {
		return ret;
	}

	/* Mount in VFS if not already mounted */
	if (vfs_mount->mnt_point == NULL) {
		/* Generate a mount point name */
		static int mount_counter = 0;
		static char auto_mnt_point[64];
		snprintf(auto_mnt_point, sizeof(auto_mnt_point),
		         "/vfs_%s_%d",
		         (vfs_mount->type == FS_TYPE_9P) ? "9p" : "other",
		         mount_counter++);
		vfs_mount->mnt_point = auto_mnt_point;
	}

	/* Mount in VFS */
	ret = fs_mount(vfs_mount);
	if (ret < 0 && ret != -EBUSY) {  /* EBUSY means already mounted */
		LOG_ERR("VFS mount failed: %d", ret);
		return ret;
	}

	/* Create namespace entry */
	struct ns_entry *entry = k_malloc(sizeof(*entry));
	if (!entry) {
		fs_unmount(vfs_mount);
		return -ENOMEM;
	}

	memset(entry, 0, sizeof(*entry));
	strncpy(entry->path, norm_path, sizeof(entry->path) - 1);
	entry->type = NS_ENTRY_VFS;
	entry->vfs_mount = vfs_mount;
	entry->flags = flags;

	/* Add to namespace */
	k_mutex_lock(&ns->lock, K_FOREVER);

	uint32_t hash = ns_hash(norm_path);

	if (flags & NS_FLAG_REPLACE) {
		/* Replace existing entries */
		struct ns_entry *old = ns->entries[hash];
		ns->entries[hash] = entry;
		entry->next = NULL;

		/* Free old entries */
		while (old) {
			struct ns_entry *next = old->next;
			k_free(old);
			old = next;
		}
	} else if (flags & NS_FLAG_BEFORE) {
		/* Insert at beginning */
		entry->next = ns->entries[hash];
		ns->entries[hash] = entry;
		entry->priority = 0;
	} else {  /* NS_FLAG_AFTER or default */
		/* Append to end */
		struct ns_entry **tail = &ns->entries[hash];
		int priority = 0;
		while (*tail) {
			priority = (*tail)->priority + 1;
			tail = &(*tail)->next;
		}
		*tail = entry;
		entry->priority = priority;
	}

	k_mutex_unlock(&ns->lock);

	LOG_INF("Mounted %s at %s (flags=0x%x)", vfs_mount->mnt_point, norm_path, flags);
	return 0;
}

int ns_mount_server(struct ninep_server *server, const char *mnt_point,
                    uint32_t flags)
{
	struct thread_namespace *ns = ns_get_current();
	if (!ns) {
		return -EINVAL;
	}

	if (!server || !mnt_point) {
		return -EINVAL;
	}

	/* Ensure namespace is writable */
	int ret = ns_make_writable(ns);
	if (ret < 0) {
		return ret;
	}

	/* Normalize mount point path */
	char norm_path[CONFIG_NS_MAX_PATH_LEN];
	ret = path_normalize(mnt_point, norm_path, sizeof(norm_path));
	if (ret < 0) {
		return ret;
	}

	/* Create namespace entry */
	struct ns_entry *entry = k_malloc(sizeof(*entry));
	if (!entry) {
		return -ENOMEM;
	}

	memset(entry, 0, sizeof(*entry));
	strncpy(entry->path, norm_path, sizeof(entry->path) - 1);
	entry->type = NS_ENTRY_SERVER;
	entry->server = server;
	entry->flags = flags;

	/* Add to namespace (same logic as ns_mount) */
	k_mutex_lock(&ns->lock, K_FOREVER);

	uint32_t hash = ns_hash(norm_path);

	if (flags & NS_FLAG_REPLACE) {
		struct ns_entry *old = ns->entries[hash];
		ns->entries[hash] = entry;
		entry->next = NULL;

		while (old) {
			struct ns_entry *next = old->next;
			k_free(old);
			old = next;
		}
	} else if (flags & NS_FLAG_BEFORE) {
		entry->next = ns->entries[hash];
		ns->entries[hash] = entry;
		entry->priority = 0;
	} else {
		struct ns_entry **tail = &ns->entries[hash];
		int priority = 0;
		while (*tail) {
			priority = (*tail)->priority + 1;
			tail = &(*tail)->next;
		}
		*tail = entry;
		entry->priority = priority;
	}

	k_mutex_unlock(&ns->lock);

	LOG_INF("Mounted in-process server at %s", norm_path);
	return 0;
}

int ns_bind(const char *old_path, const char *new_path, uint32_t flags)
{
	/* TODO: Implement bind operation */
	/* This is more complex as it needs to create an alias */
	LOG_ERR("ns_bind not yet implemented");
	return -ENOTSUP;
}

int ns_unmount(const char *mnt_point, const char *old_path)
{
	struct thread_namespace *ns = ns_get_current();
	if (!ns || !mnt_point) {
		return -EINVAL;
	}

	/* Ensure namespace is writable */
	int ret = ns_make_writable(ns);
	if (ret < 0) {
		return ret;
	}

	char norm_path[CONFIG_NS_MAX_PATH_LEN];
	ret = path_normalize(mnt_point, norm_path, sizeof(norm_path));
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&ns->lock, K_FOREVER);

	uint32_t hash = ns_hash(norm_path);
	struct ns_entry **entry_ptr = &ns->entries[hash];

	while (*entry_ptr) {
		if (strcmp((*entry_ptr)->path, norm_path) == 0) {
			struct ns_entry *to_remove = *entry_ptr;
			*entry_ptr = to_remove->next;

			/* Unmount from VFS if needed */
			if (to_remove->type == NS_ENTRY_VFS) {
				fs_unmount(to_remove->vfs_mount);
			}

			k_free(to_remove);
			k_mutex_unlock(&ns->lock);
			LOG_INF("Unmounted %s", norm_path);
			return 0;
		}
		entry_ptr = &(*entry_ptr)->next;
	}

	k_mutex_unlock(&ns->lock);
	return -ENOENT;
}

int ns_clear(void)
{
	struct thread_namespace *ns = ns_get_current();
	if (!ns) {
		return -EINVAL;
	}

	/* Ensure namespace is writable */
	int ret = ns_make_writable(ns);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&ns->lock, K_FOREVER);

	/* Unmount and free all entries */
	for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
		struct ns_entry *entry = ns->entries[i];
		while (entry) {
			struct ns_entry *next = entry->next;

			if (entry->type == NS_ENTRY_VFS) {
				fs_unmount(entry->vfs_mount);
			}

			k_free(entry);
			entry = next;
		}
		ns->entries[i] = NULL;
	}

	k_mutex_unlock(&ns->lock);

	LOG_INF("Cleared namespace");
	return 0;
}

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

struct thread_namespace *ns_get_current(void)
{
#if defined(CONFIG_NS_THREAD_LOCAL_STORAGE) && defined(CONFIG_THREAD_LOCAL_STORAGE)
	return current_ns;
#else
	k_tid_t tid = k_current_get();

	k_mutex_lock(&ns_thread_map_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NS_MAX_MOUNTS_PER_THREAD; i++) {
		if (ns_thread_map[i].tid == tid) {
			struct thread_namespace *ns = ns_thread_map[i].ns;
			k_mutex_unlock(&ns_thread_map_lock);
			return ns;
		}
	}
	k_mutex_unlock(&ns_thread_map_lock);

	return NULL;
#endif
}

int ns_set_current(struct thread_namespace *ns)
{
#if defined(CONFIG_NS_THREAD_LOCAL_STORAGE) && defined(CONFIG_THREAD_LOCAL_STORAGE)
	current_ns = ns;
	return 0;
#else
	k_tid_t tid = k_current_get();

	k_mutex_lock(&ns_thread_map_lock, K_FOREVER);
	for (int i = 0; i < CONFIG_NS_MAX_MOUNTS_PER_THREAD; i++) {
		if (ns_thread_map[i].tid == tid) {
			ns_thread_map[i].ns = ns;
			k_mutex_unlock(&ns_thread_map_lock);
			return 0;
		}
	}
	k_mutex_unlock(&ns_thread_map_lock);

	return -ENOENT;
#endif
}

int ns_walk(const char *path, struct ns_entry **entries, int max_entries)
{
	struct thread_namespace *ns = ns_get_current();
	if (!ns || !path || !entries) {
		return -EINVAL;
	}

	char norm_path[CONFIG_NS_MAX_PATH_LEN];
	int ret = path_normalize(path, norm_path, sizeof(norm_path));
	if (ret < 0) {
		return ret;
	}

	int count = 0;

	k_mutex_lock(&ns->lock, K_FOREVER);

	/* Check COW parent first if needed */
	struct thread_namespace *search_ns = ns;
	while (search_ns) {
		/* Search all hash buckets for matching entries */
		for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
			struct ns_entry *e = search_ns->entries[i];
			while (e) {
				if (path_has_prefix(norm_path, e->path)) {
					if (count < max_entries) {
						entries[count++] = e;
					}
				}
				e = e->next;
			}
		}

		/* Check parent namespace if COW */
		search_ns = search_ns->is_cow ? search_ns->parent : NULL;
	}

	k_mutex_unlock(&ns->lock);

	/* Sort by priority (longest prefix first, then by priority) */
	/* Simple bubble sort for small arrays */
	for (int i = 0; i < count - 1; i++) {
		for (int j = 0; j < count - i - 1; j++) {
			size_t len_j = strlen(entries[j]->path);
			size_t len_jp1 = strlen(entries[j + 1]->path);

			if (len_jp1 > len_j ||
			    (len_jp1 == len_j && entries[j + 1]->priority < entries[j]->priority)) {
				struct ns_entry *temp = entries[j];
				entries[j] = entries[j + 1];
				entries[j + 1] = temp;
			}
		}
	}

	return count;
}

void ns_dump(void)
{
	struct thread_namespace *ns = ns_get_current();
	if (!ns) {
		printk("No namespace for current thread\n");
		return;
	}

	printk("Namespace for thread %p:\n", ns->thread_id);

	k_mutex_lock(&ns->lock, K_FOREVER);

	for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
		struct ns_entry *e = ns->entries[i];
		while (e) {
			const char *type = (e->type == NS_ENTRY_VFS) ? "VFS" : "SERVER";
			printk("  %s -> %s (priority=%d, flags=0x%x)\n",
			       e->path, type, e->priority, e->flags);
			e = e->next;
		}
	}

	if (ns->is_cow && ns->parent) {
		printk("  (COW parent namespace exists)\n");
	}

	k_mutex_unlock(&ns->lock);
}
