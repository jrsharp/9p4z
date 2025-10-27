# Plan 9-Style Namespaces for Zephyr RTOS

**Version:** 1.0  
**Date:** October 22, 2025  
**Status:** Draft Specification

## Executive Summary

This specification defines a Plan 9-inspired namespace system for Zephyr RTOS that enables per-thread composable, hierarchical namespaces built on top of 9P protocol filesystems. The system allows threads to have customized views of the filesystem hierarchy, with union mount semantics and network-transparent access to remote resources.

**Key Innovation:** Brings Plan 9's elegant namespace composition model to resource-constrained embedded systems, enabling distributed IoT systems where sensor nodes can transparently access remote resources as local filesystem paths.

**Relationship to Inferno/Plan 9:**
This implementation captures the **core architectural insight** of Inferno and Plan 9 - the per-process namespace model with composable union mounts. While it doesn't replicate the full Inferno OS (no Limbo/Dis VM, no Inferno libraries), it brings the most powerful abstraction from that ecosystem to Zephyr. Think of it as "Plan 9 namespaces as a library" rather than a complete OS replacement - similar to how `plan9port` brings Plan 9 tools to Unix systems.

**Design Principle:**
The namespace layer is completely **filesystem-agnostic**. It contains no 9P-specific code, instead working purely with Zephyr's VFS abstraction. The same `ns_mount()` API works uniformly for 9P, FAT, LittleFS, or any future filesystem type. This clean separation makes the system simpler, more maintainable, and more extensible.

## 1. Background and Motivation

### 1.1 The Problem

Zephyr RTOS currently has:
- A VFS (Virtual Filesystem Switch) that supports multiple filesystem types at fixed mount points
- A global, system-wide filesystem namespace shared by all threads
- No mechanism for per-thread namespace customization
- No built-in support for composable union mounts

### 1.2 The Solution

Implement a namespace management layer that:
- Provides per-thread namespace isolation and customization
- Enables union mount semantics (multiple mounts at the same path)
- Leverages the existing 9p4z library for network-transparent filesystem access
- Works on top of Zephyr's VFS without modifying it
- Requires no POSIX extensions or userspace support

**Relationship to Plan 9 and Inferno:**

This design brings the core architectural insight of Plan 9 and Inferno—composable per-process namespaces with union mount semantics—to Zephyr RTOS. However, this is not a port of Inferno itself. Think of it as:

- **Plan 9/Inferno:** Complete operating systems built around the namespace abstraction
- **This system:** A namespace library that runs *on* Zephyr, similar to how plan9port brings Plan 9 concepts to Linux

You get the elegant composability and network transparency that makes Plan 9/Inferno special, but within Zephyr's existing RTOS architecture. This is "Plan 9 namespaces as a library for embedded systems" rather than a complete OS replacement.

### 1.3 Use Cases

**IoT Mesh Networks:**
```
Gateway Node Namespace:
  /local/sensors/temp       ← Local temperature sensor
  /local/sensors/humid      ← Local humidity sensor  
  /remote/node1/sensors/*   ← 9P over Thread mesh to node1
  /remote/node2/sensors/*   ← 9P over Thread mesh to node2
  /cloud/storage/*          ← 9P over TCP to cloud
```

**Distributed Device Management:**
```
Management Thread Namespace:
  /dev/local/*              ← Local device drivers
  /dev/remote1/*            ← Remote device via 9P
  /config/*                 ← Union of local + remote configs
```

**Development and Debugging:**
```
Debug Thread Namespace:
  /proc/*                   ← Synthetic filesystem exposing thread info
  /sys/*                    ← System statistics
  /logs/*                   ← Union of multiple log sources
```

## 2. Architecture Overview

### 2.1 Design Philosophy

**Filesystem Agnosticism:**
The namespace layer is intentionally designed to be **filesystem-agnostic**. It does not contain any 9P-specific code or knowledge. Instead:

- The namespace manager only understands VFS mount structures (`struct fs_mount_t`)
- 9P is registered as just another filesystem type with Zephyr's VFS
- The same `ns_mount()` function works for 9P, FAT, LittleFS, or any future filesystem
- This clean abstraction makes the code simpler, more maintainable, and more extensible

**Why This Matters:**
1. **Simplicity:** One mounting API instead of multiple filesystem-specific APIs
2. **Extensibility:** New filesystem types automatically work with namespaces
3. **Maintainability:** Namespace code doesn't need updates when 9P changes
4. **Consistency:** All filesystems are treated uniformly
5. **Testability:** Can test with mock filesystems easily

This mirrors the Unix philosophy: the namespace layer provides mechanism (composition and path resolution), while VFS provides policy (how filesystems work).

### 2.2 System Layers

```
┌─────────────────────────────────────────┐
│   Application Code                       │
│   (uses ns_open, ns_read, etc.)         │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│   Namespace API Layer                    │
│   - Per-thread namespace tables          │
│   - Path resolution and union logic      │
│   - Namespace manipulation (bind/mount)  │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│   Zephyr VFS (fs_open, fs_read, etc.)  │
│   - Multiple filesystem types            │
│   - Standard POSIX-like API              │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│   9P Filesystem Driver                   │
│   (registers as FS_TYPE_9P with VFS)    │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│   9p4z Protocol Library                  │
│   - Message parsing/serialization        │
│   - Multiple transport support           │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│   Transports (UART/TCP/BLE/Thread)      │
└─────────────────────────────────────────┘
```

### 2.2 Core Components

**Component 1: 9P VFS Driver**
- Registers with Zephyr VFS as a custom filesystem type
- Translates VFS operations to 9P protocol operations
- Manages 9P client connections

**Component 2: Namespace Manager**
- Maintains per-thread namespace tables
- Implements path resolution with union semantics
- Handles namespace inheritance for child threads

**Component 3: Namespace API**
- Provides Plan 9-style namespace manipulation functions
- Wraps standard VFS calls with namespace-aware versions
- Manages mount points and bindings

## 3. Data Structures

### 3.1 Namespace Entry

```c
/**
 * Represents a single mount/bind in a thread's namespace
 */
struct ns_entry {
    char path[CONFIG_NS_MAX_PATH_LEN];  /* Mount point path, e.g., "/sensors" */
    
    /* Reference to underlying VFS mount or 9P connection */
    union {
        struct fs_mount_t *vfs_mount;   /* For standard VFS mounts */
        struct ninep_client *ninep_client; /* For 9P mounts */
    };
    
    enum ns_entry_type {
        NS_ENTRY_VFS,      /* Standard VFS mount */
        NS_ENTRY_9P,       /* 9P filesystem */
        NS_ENTRY_SYNTHETIC /* In-process synthetic FS */
    } type;
    
    /* Mount flags (Plan 9 style) */
    uint32_t flags;
#define NS_FLAG_BEFORE   0x0001  /* Mount before existing mounts */
#define NS_FLAG_AFTER    0x0002  /* Mount after existing mounts */
#define NS_FLAG_CREATE   0x0004  /* Create files only in this mount */
#define NS_FLAG_REPLACE  0x0008  /* Replace existing mount */
    
    /* For union mounts: priority order */
    int priority;
    
    /* Linked list for multiple mounts at same path */
    struct ns_entry *next;
};
```

### 3.2 Thread Namespace

```c
/**
 * Per-thread namespace table
 */
struct thread_namespace {
    k_tid_t thread_id;              /* Owning thread ID */
    
    /* Hash table or linked list of namespace entries */
    struct ns_entry *entries[CONFIG_NS_HASH_SIZE];
    
    /* Parent namespace (for inheritance) */
    struct thread_namespace *parent;
    
    /* Copy-on-write flag */
    bool is_cow;
    
    /* Reference count (for sharing) */
    atomic_t refcount;
    
    /* Lock for thread-safe modifications */
    struct k_mutex lock;
};
```

### 3.3 9P Mount Context

```c
/**
 * Context for a 9P filesystem mount
 */
struct ninep_mount_ctx {
    struct ninep_client client;     /* 9P client connection */
    char aname[256];                /* Attach name */
    struct ninep_qid root_qid;      /* Root directory QID */
    uint32_t fid_pool_base;         /* Base FID for this mount */
    
    /* Transport configuration */
    struct ninep_transport *transport;
    
    /* Mount flags */
    uint32_t mount_flags;
};
```

### 3.4 File Descriptor Extension

```c
/**
 * Extension to Zephyr fs_file_t for namespace-aware operations
 */
struct ns_file {
    struct fs_file_t base;          /* Standard Zephyr file descriptor */
    
    /* Namespace context */
    struct ns_entry *ns_entry;      /* Which mount this file came from */
    
    /* For 9P files */
    struct {
        uint32_t fid;               /* 9P file identifier */
        struct ninep_qid qid;       /* 9P qid */
        uint64_t offset;            /* Current file offset */
    } ninep;
};
```

## 4. Namespace API

### 4.1 Initialization

```c
/**
 * Initialize the namespace subsystem
 * Must be called during system initialization
 */
int ns_init(void);

/**
 * Create namespace for the current thread
 * If parent is NULL, creates a fresh namespace
 * If parent is set, inherits from parent (COW)
 */
int ns_create(struct thread_namespace *parent);

/**
 * Fork current thread's namespace for a new thread
 * Creates copy-on-write namespace
 */
int ns_fork(k_tid_t child_tid);

/**
 * Destroy namespace for a thread
 * Called automatically when thread terminates
 */
int ns_destroy(k_tid_t tid);
```

### 4.2 Namespace Manipulation (Plan 9 Style)

```c
/**
 * Bind old path to new location in namespace
 * Flags control behavior:
 *   NS_FLAG_BEFORE: new appears before old
 *   NS_FLAG_AFTER: new appears after old  
 *   NS_FLAG_CREATE: create files in new
 *   NS_FLAG_REPLACE: replace old entirely
 * 
 * Example:
 *   ns_bind("/remote/node1/sensors", "/sensors", NS_FLAG_AFTER);
 *   // Now /sensors shows local sensors first, remote second
 */
int ns_bind(const char *old_path, const char *new_path, uint32_t flags);

/**
 * Mount a VFS filesystem into the namespace
 * Works with ANY filesystem type registered with VFS (9P, FAT, LittleFS, etc.)
 * 
 * @param vfs_mount  Pointer to VFS mount structure
 * @param mnt_point  Where to mount in namespace (may differ from vfs_mount->mnt_point)
 * @param flags      Namespace flags (NS_FLAG_BEFORE, NS_FLAG_AFTER, etc.)
 * 
 * The namespace layer is filesystem-agnostic. It doesn't care if vfs_mount
 * points to 9P, FAT, LittleFS, or a synthetic filesystem. This function
 * will automatically call fs_mount() if the VFS mount is not yet active.
 * 
 * Example (9P filesystem):
 *   struct ninep_mount_ctx ctx = {
 *       .client = my_client,
 *       .aname = "/sensors",
 *   };
 *   struct fs_mount_t mount = {
 *       .type = FS_TYPE_9P,
 *       .mnt_point = "/sys_9p_0",  // Internal VFS mount point
 *       .fs_data = &ctx,
 *   };
 *   ns_mount(&mount, "/remote/node1", 0);
 * 
 * Example (FAT filesystem):
 *   struct fs_mount_t fat = {
 *       .type = FS_FATFS,
 *       .mnt_point = "/sd",
 *       .fs_data = &fat_fs,
 *   };
 *   ns_mount(&fat, "/local", 0);
 */
int ns_mount(struct fs_mount_t *vfs_mount, const char *mnt_point,
             uint32_t flags);

/**
 * Unmount from namespace
 * If old_path is specified, only removes that specific binding
 */
int ns_unmount(const char *mnt_point, const char *old_path);

/**
 * Clear all mounts from namespace (reset to empty)
 */
int ns_clear(void);
```

### 4.3 File Operations (Namespace-Aware)

```c
/**
 * Open a file through the namespace
 * Resolves path through union mounts in priority order
 * 
 * @param path   Path relative to namespace root
 * @param flags  Standard open flags (FS_O_READ, FS_O_WRITE, etc.)
 * @return       File descriptor or negative error code
 */
int ns_open(const char *path, fs_mode_t flags);

/**
 * Read from namespace file
 */
ssize_t ns_read(int fd, void *buf, size_t count);

/**
 * Write to namespace file
 * If file is in union mount, writes go to mount with NS_FLAG_CREATE
 */
ssize_t ns_write(int fd, const void *buf, size_t count);

/**
 * Close namespace file
 */
int ns_close(int fd);

/**
 * Seek within file
 */
off_t ns_lseek(int fd, off_t offset, int whence);

/**
 * Get file stats
 */
int ns_stat(const char *path, struct fs_dirent *entry);

/**
 * Open directory for reading
 * For union mounts, merges directory listings
 */
int ns_opendir(const char *path);

/**
 * Read directory entry
 * Automatically deduplicates entries from union mounts
 */
int ns_readdir(int fd, struct fs_dirent *entry);

/**
 * Close directory
 */
int ns_closedir(int fd);

/**
 * Create directory
 * Created in mount with NS_FLAG_CREATE or highest priority writable mount
 */
int ns_mkdir(const char *path);

/**
 * Remove file
 * For union mounts, may create whiteout entry
 */
int ns_unlink(const char *path);

/**
 * Rename file
 */
int ns_rename(const char *old_path, const char *new_path);
```

### 4.4 Utility Functions

```c
/**
 * Get current thread's namespace
 */
struct thread_namespace *ns_get_current(void);

/**
 * Set namespace for current thread
 * Useful for switching between namespaces
 */
int ns_set_current(struct thread_namespace *ns);

/**
 * Walk through namespace to resolve a path
 * Returns list of all matching entries (for union mounts)
 * 
 * @param path           Path to resolve
 * @param entries        Output array of matching ns_entries
 * @param max_entries    Size of entries array
 * @return               Number of entries found, or negative error
 */
int ns_walk(const char *path, struct ns_entry **entries, int max_entries);

/**
 * Print current thread's namespace (for debugging)
 */
void ns_dump(void);
```

## 5. 9P VFS Driver Implementation

### 5.1 Registration

```c
/**
 * 9P filesystem type ID for Zephyr VFS
 */
#define FS_TYPE_9P (FS_TYPE_EXTERNAL_BASE + 1)

/**
 * 9P filesystem operations structure
 */
static const struct fs_file_system_t fs_9p = {
    .open = fs_9p_open,
    .read = fs_9p_read,
    .write = fs_9p_write,
    .lseek = fs_9p_lseek,
    .close = fs_9p_close,
    .opendir = fs_9p_opendir,
    .readdir = fs_9p_readdir,
    .closedir = fs_9p_closedir,
    .mount = fs_9p_mount,
    .unmount = fs_9p_unmount,
    .unlink = fs_9p_unlink,
    .rename = fs_9p_rename,
    .mkdir = fs_9p_mkdir,
    .stat = fs_9p_stat,
};

/**
 * Initialize and register 9P filesystem with VFS
 */
int fs_9p_init(void) {
    return fs_register(FS_TYPE_9P, &fs_9p);
}
```

### 5.2 VFS Operation Mappings

Each VFS operation maps to corresponding 9P protocol operations:

| VFS Operation | 9P Operations | Notes |
|---------------|---------------|-------|
| `open()` | `Twalk` + `Topen` | Walk to file, then open |
| `read()` | `Tread` | Read data with offset |
| `write()` | `Twrite` | Write data with offset |
| `close()` | `Tclunk` | Release FID |
| `opendir()` | `Twalk` + `Topen` | Walk to dir, open for reading |
| `readdir()` | `Tread` (dir mode) | Read directory entries |
| `stat()` | `Twalk` + `Tstat` | Walk and get attributes |
| `mkdir()` | `Twalk` + `Tcreate` | Walk to parent, create dir |
| `unlink()` | `Twalk` + `Tremove` | Walk and remove |
| `rename()` | `Twstat` | Modify name attribute |

### 5.3 FID Management

```c
/**
 * FID (File IDentifier) pool for 9P operations
 * Each mount gets a range of FIDs to avoid conflicts
 */
struct fid_pool {
    uint32_t base_fid;           /* Starting FID for this pool */
    uint32_t max_fids;           /* Number of FIDs available */
    ATOMIC_DEFINE(bitmap, CONFIG_NINEP_MAX_FIDS); /* Allocation bitmap */
    struct k_mutex lock;
};

/**
 * Allocate a FID from the pool
 */
uint32_t fid_alloc(struct fid_pool *pool);

/**
 * Free a FID back to the pool
 */
void fid_free(struct fid_pool *pool, uint32_t fid);
```

## 6. Path Resolution Algorithm

### 6.1 Simple Path Resolution (No Unions)

```c
/**
 * Resolve a path through the namespace
 * Returns the first matching ns_entry
 */
struct ns_entry *resolve_path(const char *path) {
    struct thread_namespace *ns = ns_get_current();
    
    // Normalize path (remove .., ., //)
    char normalized[CONFIG_NS_MAX_PATH_LEN];
    path_normalize(path, normalized);
    
    // Find longest matching prefix in namespace
    struct ns_entry *best_match = NULL;
    size_t best_len = 0;
    
    for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
        for (struct ns_entry *e = ns->entries[i]; e; e = e->next) {
            if (path_has_prefix(normalized, e->path)) {
                size_t len = strlen(e->path);
                if (len > best_len) {
                    best_match = e;
                    best_len = len;
                }
            }
        }
    }
    
    return best_match;
}
```

### 6.2 Union Mount Resolution

```c
/**
 * Resolve path through union mounts
 * Returns array of all matching ns_entries, sorted by priority
 */
int resolve_union(const char *path, struct ns_entry **entries, int max) {
    struct thread_namespace *ns = ns_get_current();
    char normalized[CONFIG_NS_MAX_PATH_LEN];
    path_normalize(path, normalized);
    
    int count = 0;
    
    // Collect all entries that match this path
    for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
        for (struct ns_entry *e = ns->entries[i]; e; e = e->next) {
            if (path_has_prefix(normalized, e->path)) {
                if (count < max) {
                    entries[count++] = e;
                }
            }
        }
    }
    
    // Sort by priority (accounting for BEFORE/AFTER flags)
    sort_entries_by_priority(entries, count);
    
    return count;
}
```

### 6.3 Union Directory Listing

For directories mounted in multiple places:

1. Collect directory entries from all union mounts
2. Deduplicate by name (first occurrence wins)
3. Merge stat information (use most recent mtime, etc.)
4. Return merged listing to caller

```c
/**
 * Merge directory listings from multiple sources
 */
int merge_directory_listings(struct ns_entry **entries, int num_entries,
                             struct fs_dirent *output, int max_output) {
    // Hash table for deduplication
    struct {
        char name[MAX_FILE_NAME];
        bool seen;
    } seen[max_output];
    int seen_count = 0;
    
    int output_count = 0;
    
    // Read from each entry in priority order
    for (int i = 0; i < num_entries && output_count < max_output; i++) {
        struct fs_dirent entry;
        
        // Read all entries from this mount
        while (readdir_from_entry(entries[i], &entry) == 0) {
            // Check if we've seen this name before
            bool duplicate = false;
            for (int j = 0; j < seen_count; j++) {
                if (strcmp(seen[j].name, entry.name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            
            if (!duplicate && output_count < max_output) {
                output[output_count++] = entry;
                strcpy(seen[seen_count++].name, entry.name);
            }
        }
    }
    
    return output_count;
}
```

## 7. Thread Integration

### 7.1 Thread-Local Storage

Use Zephyr's TLS support to store current namespace pointer:

```c
/* Thread-local pointer to namespace */
__thread struct thread_namespace *current_ns = NULL;

/**
 * Get current thread's namespace
 * If none exists, create one (inherit from parent if available)
 */
struct thread_namespace *ns_get_current(void) {
    if (current_ns == NULL) {
        // Auto-create namespace for this thread
        current_ns = ns_create_default();
    }
    return current_ns;
}
```

### 7.2 Thread Creation Hook

```c
/**
 * Hook called when a new thread is created
 * Automatically sets up namespace inheritance
 */
void ns_thread_create_hook(k_tid_t thread_id) {
    struct thread_namespace *parent_ns = ns_get_current();
    
    // Create COW namespace for child
    struct thread_namespace *child_ns = ns_fork_internal(parent_ns);
    
    // Store in thread-local storage of new thread
    // (implementation depends on Zephyr TLS mechanisms)
    ns_set_for_thread(thread_id, child_ns);
}

/**
 * Hook called when thread terminates
 */
void ns_thread_exit_hook(k_tid_t thread_id) {
    struct thread_namespace *ns = ns_get_for_thread(thread_id);
    if (ns) {
        ns_destroy_internal(ns);
    }
}
```

### 7.3 Namespace Inheritance

```c
/**
 * Copy-on-write namespace fork
 * Child initially shares parent's namespace
 * On first modification, namespace is copied
 */
struct thread_namespace *ns_fork_internal(struct thread_namespace *parent) {
    struct thread_namespace *child = k_malloc(sizeof(*child));
    if (!child) {
        return NULL;
    }
    
    // Initially share parent's namespace
    child->parent = parent;
    child->is_cow = true;
    
    // Empty local entries
    memset(child->entries, 0, sizeof(child->entries));
    
    // Increment parent refcount
    atomic_inc(&parent->refcount);
    
    k_mutex_init(&child->lock);
    atomic_set(&child->refcount, 1);
    
    return child;
}

/**
 * Ensure namespace is writable (break COW if needed)
 */
int ns_make_writable(struct thread_namespace *ns) {
    if (!ns->is_cow) {
        return 0; // Already writable
    }
    
    k_mutex_lock(&ns->lock, K_FOREVER);
    
    // Copy parent's entries
    for (int i = 0; i < CONFIG_NS_HASH_SIZE; i++) {
        for (struct ns_entry *e = ns->parent->entries[i]; e; e = e->next) {
            struct ns_entry *copy = k_malloc(sizeof(*copy));
            if (!copy) {
                k_mutex_unlock(&ns->lock);
                return -ENOMEM;
            }
            memcpy(copy, e, sizeof(*e));
            copy->next = ns->entries[i];
            ns->entries[i] = copy;
        }
    }
    
    // Drop parent reference
    ns_release(ns->parent);
    ns->parent = NULL;
    ns->is_cow = false;
    
    k_mutex_unlock(&ns->lock);
    return 0;
}
```

## 8. Configuration Options

### 8.1 Kconfig Options

```kconfig
menuconfig NAMESPACE
    bool "Plan 9-style Namespace Support"
    depends on FILE_SYSTEM
    help
      Enable per-thread composable namespaces with union mount semantics.
      Provides Plan 9-style namespace manipulation on top of Zephyr VFS.

if NAMESPACE

config NS_MAX_PATH_LEN
    int "Maximum path length"
    default 256
    help
      Maximum length of a filesystem path in the namespace.

config NS_HASH_SIZE
    int "Namespace hash table size"
    default 32
    help
      Size of hash table for namespace entries.
      Larger values improve lookup performance but use more memory.

config NS_MAX_MOUNTS_PER_THREAD
    int "Maximum mounts per thread"
    default 16
    help
      Maximum number of mount points in a single thread's namespace.

config NS_MAX_UNION_DEPTH
    int "Maximum union mount depth"
    default 8
    help
      Maximum number of filesystems that can be unioned at a single path.

config NS_ENABLE_COW
    bool "Enable copy-on-write namespace inheritance"
    default y
    help
      When enabled, child threads inherit parent namespace via COW.
      Disable to save memory if namespace inheritance is not needed.

config NS_THREAD_LOCAL_STORAGE
    bool "Use thread-local storage for namespace"
    depends on THREAD_LOCAL_STORAGE
    default y
    help
      Store current namespace pointer in TLS for fast access.
      If disabled, uses hash table lookup (slower but works without TLS).

config NS_DEBUG
    bool "Enable namespace debugging"
    default n
    help
      Enable verbose debug logging for namespace operations.

endif # NAMESPACE
```

### 8.2 Device Tree Bindings

```dts
/ {
    namespace {
        compatible = "zephyr,namespace";
        
        /* Pre-configured namespaces */
        default-namespace {
            mounts = <&local_fs>, <&ninep_mount1>;
        };
    };
    
    /* 9P mount definition */
    ninep_mount1: ninep-mount@0 {
        compatible = "ninep,mount";
        transport = "tcp";
        address = "192.168.1.100:564";
        aname = "/sensors";
        mount-point = "/remote/node1";
    };
};
```

## 9. Example Usage Scenarios

### 9.1 Basic 9P Mount

```c
#include <zephyr/kernel.h>
#include <zephyr/namespace/namespace.h>
#include <zephyr/fs/fs_9p.h>
#include <ninep/client.h>

void example_basic_mount(void) {
    // Initialize namespace for this thread
    ns_create(NULL);
    
    // Create and configure 9P client
    struct ninep_client *client = ninep_client_create();
    ninep_connect(client, "tcp", "192.168.1.100:564");
    
    // Create mount context
    struct ninep_mount_ctx ctx = {
        .client = *client,
        .aname = "/",  // Root of remote filesystem
    };
    
    // Create VFS mount structure
    struct fs_mount_t mount = {
        .type = FS_TYPE_9P,
        .mnt_point = "/sys_9p_0",  // Internal VFS path (arbitrary)
        .fs_data = &ctx,
    };
    
    // Mount into namespace (auto-mounts in VFS if needed)
    ns_mount(&mount, "/remote/node1", 0);
    
    // Now can access remote files as /remote/node1/*
    int fd = ns_open("/remote/node1/sensors/temp", FS_O_READ);
    if (fd >= 0) {
        char buf[32];
        ssize_t n = ns_read(fd, buf, sizeof(buf));
        printk("Temperature: %.*s\n", (int)n, buf);
        ns_close(fd);
    }
}
```

### 9.2 Union Mount

```c
void example_union_mount(void) {
    ns_create(NULL);
    
    // Mount local sensors (standard VFS filesystem)
    struct fs_mount_t local = {
        .type = FS_FATFS,
        .mnt_point = "/sd",
        .fs_data = &fat_fs,
    };
    ns_mount(&local, "/sensors", 0);
    
    // Mount remote sensors (9P filesystem, union with local)
    struct ninep_client *remote = ninep_client_create();
    ninep_connect(remote, "tcp", "192.168.1.100:564");
    
    struct ninep_mount_ctx remote_ctx = {
        .client = *remote,
        .aname = "/sensors",
    };
    
    struct fs_mount_t remote_mount = {
        .type = FS_TYPE_9P,
        .mnt_point = "/sys_9p_remote",
        .fs_data = &remote_ctx,
    };
    ns_mount(&remote_mount, "/sensors", NS_FLAG_AFTER);
    
    // Reading /sensors/temp will check local first, then remote
    int fd = ns_open("/sensors/temp", FS_O_READ);
    // ...
}
```

### 9.3 Per-Thread Custom Namespace

```c
void worker_thread(void *arg1, void *arg2, void *arg3) {
    // This thread inherits parent's namespace via COW
    // Modifications don't affect parent
    
    // Add thread-specific mount
    struct ninep_client *client = ninep_client_create();
    ninep_connect(client, "uart", "/dev/ttyS1");
    
    struct ninep_mount_ctx ctx = {
        .client = *client,
        .aname = "/",
    };
    
    struct fs_mount_t debug_mount = {
        .type = FS_TYPE_9P,
        .mnt_point = "/sys_9p_debug",
        .fs_data = &ctx,
    };
    ns_mount(&debug_mount, "/debug", 0);
    
    // This thread can now access /debug/*, but parent cannot
    ns_open("/debug/stats", FS_O_READ);
}

void main(void) {
    // Setup main namespace
    ns_create(NULL);
    
    struct fs_mount_t main_fs = {
        .type = FS_FATFS,
        .mnt_point = "/sd",
        .fs_data = &fat_fs,
    };
    ns_mount(&main_fs, "/data", 0);
    
    // Spawn worker - it inherits our namespace
    k_thread_create(&worker_tid, worker_stack, STACK_SIZE,
                    worker_thread, NULL, NULL, NULL,
                    WORKER_PRIORITY, 0, K_NO_WAIT);
}
```

### 9.4 Synthetic Filesystem

```c
/**
 * Example: Expose environment variables as /env/*
 */
struct synthetic_env_fs {
    struct ns_entry base;
    struct k_hash_table env_vars;
};

int env_fs_read(struct ns_file *file, void *buf, size_t count) {
    // Look up environment variable by filename
    const char *value = env_get(file->path);
    if (value) {
        size_t len = strlen(value);
        size_t to_copy = MIN(count, len - file->offset);
        memcpy(buf, value + file->offset, to_copy);
        file->offset += to_copy;
        return to_copy;
    }
    return -ENOENT;
}

void setup_env_fs(void) {
    struct synthetic_env_fs *env = create_env_fs();
    ns_mount_synthetic(env, "/env", 0);
    
    // Now can read environment variables as files
    int fd = ns_open("/env/HOME", FS_O_READ);
    // ...
}
```

### 9.5 IoT Gateway with Mesh Network

```c
/**
 * Gateway that aggregates sensors from multiple mesh nodes
 */
void setup_sensor_gateway(void) {
    ns_create(NULL);
    
    // Mount local sensors
    struct fs_mount_t local = {
        .type = FS_FATFS,
        .mnt_point = "/sd",
        .fs_data = &fat_fs,
    };
    ns_mount(&local, "/local", 0);
    
    // Connect to mesh nodes via Thread
    for (int i = 0; i < num_nodes; i++) {
        struct ninep_client *node = ninep_client_create();
        ninep_connect(node, "thread", node_addresses[i]);
        
        struct ninep_mount_ctx ctx = {
            .client = *node,
            .aname = "/",
        };
        
        struct fs_mount_t node_mount = {
            .type = FS_TYPE_9P,
            .mnt_point = "/sys_9p_thread",  // VFS path (reused)
            .fs_data = &ctx,
        };
        
        char mnt[32];
        snprintf(mnt, sizeof(mnt), "/nodes/node%d", i);
        ns_mount(&node_mount, mnt, 0);
    }
    
    // Union all sensor directories
    ns_bind("/local/sensors", "/sensors", 0);
    for (int i = 0; i < num_nodes; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/nodes/node%d/sensors", i);
        ns_bind(path, "/sensors", NS_FLAG_AFTER);
    }
    
    // Now /sensors shows all sensors (local + all nodes)
    int dir = ns_opendir("/sensors");
    struct fs_dirent entry;
    while (ns_readdir(dir, &entry) == 0) {
        printk("Sensor: %s\n", entry.name);
    }
    ns_closedir(dir);
}
```

## 10. Implementation Phases

### Phase 1: 9P VFS Driver (Foundation)
**Goal:** Get 9P filesystems working through standard Zephyr VFS

**Deliverables:**
- `fs_9p.c` - 9P VFS driver implementation
- Registration with `FS_TYPE_9P`
- Basic operations: open, read, write, close, stat
- FID pool management
- Integration with existing 9p4z library
- Unit tests for basic file operations

**Success Criteria:**
- Can mount a 9P server using standard `fs_mount()`
- Can read/write files through VFS API
- No memory leaks
- All 9p4z transports work (UART, TCP, BLE, Thread)

### Phase 2: Namespace Manager (Core)
**Goal:** Per-thread namespace tables with basic resolution

**Deliverables:**
- `namespace.c` - Core namespace manager
- Data structures (ns_entry, thread_namespace)
- Thread-local storage integration
- Simple path resolution (no unions yet)
- `ns_create()`, `ns_destroy()`, `ns_mount()`
- `ns_open()`, `ns_read()`, `ns_write()`, `ns_close()`

**Success Criteria:**
- Each thread can have its own namespace
- Can mount multiple 9P filesystems at different paths
- Basic file operations work through namespace API
- Thread isolation works correctly

### Phase 3: Union Mounts (Advanced)
**Goal:** Multi-mount resolution with Plan 9 semantics

**Deliverables:**
- Union path resolution algorithm
- Directory merging and deduplication
- `ns_bind()` with BEFORE/AFTER/CREATE flags
- Priority-based mount ordering
- Whiteout support for deletions

**Success Criteria:**
- Multiple mounts at same path work correctly
- Directory listings merge properly
- CREATE flag directs writes to correct mount
- BEFORE/AFTER ordering is respected

### Phase 4: Namespace Inheritance (Optimization)
**Goal:** Efficient COW namespace for child threads

**Deliverables:**
- COW namespace implementation
- Thread creation/exit hooks
- `ns_fork()` with lazy copying
- Parent namespace refcounting

**Success Criteria:**
- Child threads inherit parent namespace
- COW works correctly (no extra copies until modification)
- No memory leaks on thread exit
- Reference counting is correct

### Phase 5: Driver-as-Filesystem (Critical for Plan 9 Model)
**Goal:** Enable drivers to register as in-process 9P servers

**Deliverables:**
- In-process 9P server API (`struct ninep_server_ops`)
- Driver registration mechanism (`ninep_server_register()`)
- Local transport for in-process servers
- `ns_mount_server()` for mounting in-process servers
- Example drivers:
  - `/dev/draw` (display driver)
  - `/dev/mouse` (input driver)
  - `/dev/gpio` (GPIO driver)
  - `/proc` (process/thread info)
  - `/env` (environment variables)

**Success Criteria:**
- Drivers can implement 9P interface and register as servers
- Mounted via standard `ns_mount_server()` API
- Accessed via standard `ns_open()`, `ns_read()`, `ns_write()` calls
- Minimal overhead (<20 instructions per operation)
- Can be composed with union mounts
- Can be exported over network automatically

**See:** [DRIVER_FILESYSTEM_ADDENDUM.md](./DRIVER_FILESYSTEM_ADDENDUM.md) for complete specification of this feature.

### Phase 6: Polish and Optimization
**Goal:** Production-ready quality

**Deliverables:**
- Performance optimization
- Memory usage optimization
- Comprehensive test suite
- Documentation and examples
- Shell integration (`ns` command)
- Debugging tools (`ns_dump()`)

**Success Criteria:**
- <10% overhead vs direct VFS calls
- <4KB RAM per namespace
- 100% test coverage
- Complete documentation

## 11. Testing Strategy

### 11.1 Unit Tests

```c
/**
 * Test basic namespace creation and destruction
 */
void test_ns_create_destroy(void) {
    struct thread_namespace *ns = ns_create(NULL);
    assert(ns != NULL);
    assert(ns->refcount == 1);
    
    ns_destroy(ns);
}

/**
 * Test simple mount and path resolution
 */
void test_simple_mount(void) {
    ns_create(NULL);
    
    // Mount mock 9P client
    struct ninep_client *mock = create_mock_client();
    ns_mount(mock, "/", "/remote", 0);
    
    // Resolve path
    struct ns_entry *entry = resolve_path("/remote/test");
    assert(entry != NULL);
    assert(strcmp(entry->path, "/remote") == 0);
}

/**
 * Test union mount resolution
 */
void test_union_mount(void) {
    ns_create(NULL);
    
    struct ninep_client *c1 = create_mock_client();
    struct ninep_client *c2 = create_mock_client();
    
    ns_mount(c1, "/", "/data", 0);
    ns_mount(c2, "/", "/data", NS_FLAG_AFTER);
    
    struct ns_entry *entries[10];
    int count = resolve_union("/data/file", entries, 10);
    assert(count == 2);
}

/**
 * Test COW namespace inheritance
 */
void test_cow_namespace(void) {
    // Parent namespace
    ns_create(NULL);
    struct thread_namespace *parent = ns_get_current();
    ns_mount(mock_client(), "/", "/parent", 0);
    
    // Child namespace (COW)
    struct thread_namespace *child = ns_fork_internal(parent);
    assert(child->is_cow == true);
    assert(child->parent == parent);
    
    // Child can see parent's mounts
    struct ns_entry *entry = resolve_path_in(child, "/parent/file");
    assert(entry != NULL);
    
    // Modify child namespace (should trigger copy)
    ns_mount_in(child, mock_client(), "/", "/child", 0);
    assert(child->is_cow == false);
    assert(child->parent == NULL);
}
```

### 11.2 Integration Tests

```c
/**
 * End-to-end test: mount 9P server, read file
 */
void test_e2e_9p_read(void) {
    // Setup 9P server (in separate thread or process)
    start_test_9p_server();
    
    // Mount in namespace
    ns_create(NULL);
    struct ninep_client *client = ninep_client_create();
    ninep_connect(client, "tcp", "127.0.0.1:5640");
    ns_mount(client, "/", "/remote", 0);
    
    // Read file
    int fd = ns_open("/remote/testfile", FS_O_READ);
    assert(fd >= 0);
    
    char buf[100];
    ssize_t n = ns_read(fd, buf, sizeof(buf));
    assert(n > 0);
    assert(strcmp(buf, "test content") == 0);
    
    ns_close(fd);
}

/**
 * Test multiple transports simultaneously
 */
void test_multi_transport(void) {
    ns_create(NULL);
    
    // TCP transport
    struct ninep_client *tcp = ninep_client_create();
    ninep_connect(tcp, "tcp", "192.168.1.100:564");
    ns_mount(tcp, "/", "/tcp", 0);
    
    // UART transport
    struct ninep_client *uart = ninep_client_create();
    ninep_connect(uart, "uart", "dev/ttyS1");
    ns_mount(uart, "/", "/uart", 0);
    
    // Thread transport
    struct ninep_client *thread = ninep_client_create();
    ninep_connect(thread, "thread", "fe80::1");
    ns_mount(thread, "/", "/thread", 0);
    
    // All should work simultaneously
    assert(ns_open("/tcp/file1", FS_O_READ) >= 0);
    assert(ns_open("/uart/file2", FS_O_READ) >= 0);
    assert(ns_open("/thread/file3", FS_O_READ) >= 0);
}
```

### 11.3 Stress Tests

```c
/**
 * Stress test: many threads with namespaces
 */
void test_many_threads(void) {
    #define NUM_THREADS 50
    
    k_tid_t threads[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        threads[i] = k_thread_create(...);
    }
    
    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        k_thread_join(threads[i], K_FOREVER);
    }
    
    // Verify no memory leaks
    assert(get_namespace_count() == 1); // Only main thread
}

/**
 * Stress test: many mounts in one namespace
 */
void test_many_mounts(void) {
    ns_create(NULL);
    
    for (int i = 0; i < 100; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/mount%d", i);
        ns_mount(create_mock_client(), "/", path, 0);
    }
    
    // Verify all are accessible
    for (int i = 0; i < 100; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/mount%d/file", i);
        assert(resolve_path(path) != NULL);
    }
}
```

## 12. Performance Considerations

### 12.1 Optimization Targets

| Operation | Target | Notes |
|-----------|--------|-------|
| Path resolution | <100 μs | For non-union paths |
| Union resolution | <500 μs | For up to 8 unions |
| Thread namespace lookup | <10 μs | Via TLS |
| File open (cached) | <1 ms | Excluding network latency |
| File open (uncached) | <10 ms | With network round-trip |

### 12.2 Memory Usage

| Component | RAM Usage | Notes |
|-----------|-----------|-------|
| Per-thread namespace | ~200 bytes | Base structure |
| Per mount entry | ~128 bytes | Depends on path length |
| Per open file | ~64 bytes | ns_file structure |
| FID pool | ~512 bytes | Bitmap for 4096 FIDs |

**Example:** System with 10 threads, 5 mounts each, 3 open files per thread:
- Namespaces: 10 × 200 = 2 KB
- Mounts: 50 × 128 = 6.4 KB
- Files: 30 × 64 = 1.9 KB
- **Total: ~10 KB**

### 12.3 Optimization Strategies

**Path Resolution:**
- Cache frequently-accessed paths
- Use hash table instead of linear search
- Optimize string operations

**Union Mounts:**
- Limit maximum union depth (CONFIG_NS_MAX_UNION_DEPTH)
- Cache merged directory listings
- Early exit when file found in first mount

**Memory:**
- Use memory pools for fixed-size allocations
- Implement slab allocator for ns_entry structures
- Share read-only namespace data between threads

**Network:**
- Pipeline 9P operations where possible
- Use 9P read-ahead for sequential access
- Implement client-side caching

## 13. Error Handling

### 13.1 Error Codes

Standard Zephyr error codes are used:

| Error | Meaning | When |
|-------|---------|------|
| `-ENOENT` | No such file or directory | Path not found in namespace |
| `-ENOTDIR` | Not a directory | Tried to opendir on file |
| `-EISDIR` | Is a directory | Tried to open dir as file |
| `-ENOMEM` | Out of memory | Allocation failed |
| `-EINVAL` | Invalid argument | Bad path, flags, etc. |
| `-ENOSPC` | No space left | Cannot create file |
| `-EEXIST` | File exists | CREATE with existing file |
| `-EACCES` | Permission denied | 9P server denied access |
| `-ETIMEDOUT` | Timeout | Network operation timeout |
| `-ECONNREFUSED` | Connection refused | Cannot connect to 9P server |

### 13.2 Error Recovery

**Network Failures:**
- Retry with exponential backoff
- Mark mount as unavailable
- Fall through to next union mount
- Optionally unmount after repeated failures

**Resource Exhaustion:**
- Return error to caller
- Clean up partial allocations
- Log warning for debugging

**Protocol Errors:**
- Close connection
- Attempt reconnection
- If persistent, disable mount

## 14. Security Considerations

### 14.1 Thread Isolation

- Each thread's namespace is isolated by default
- COW prevents accidental cross-thread modification
- Explicit sharing requires intentional API usage

### 14.2 9P Authentication

- Support 9P authentication mechanisms
- Store credentials per-mount
- Do not leak credentials across threads

### 14.3 Path Validation

- Prevent path traversal attacks (`../../../etc/passwd`)
- Validate all user-provided paths
- Enforce maximum path length
- Reject paths with null bytes

### 14.4 Resource Limits

- Limit number of mounts per thread
- Limit number of open files per thread
- Limit namespace depth (prevent infinite recursion)
- Implement quotas if needed

## 15. Future Enhancements

### 15.1 Advanced Features

**Dynamic Namespace Updates:**
- Hot-plug/unplug of filesystems
- Automatic remount on network reconnection
- Namespace change notifications

**Extended Attributes:**
- 9P xattr support
- Namespace-specific metadata
- Per-file caching policies

**Performance:**
- Asynchronous I/O support
- Multi-level caching (memory, flash)
- Prefetching and read-ahead

**Security:**
- Mandatory access control (MAC)
- Namespace capabilities/permissions
- Encrypted 9P connections (TLS transport)

### 15.2 Integration Points

**Shell Integration:**
```
uart:~$ ns mount /sd /local fatfs
uart:~$ ns mount tcp://192.168.1.100:564 /remote
uart:~$ ns list
/remote -> 9P (tcp://192.168.1.100:564)
/local -> FATFS (/sd)
uart:~$ ns bind /remote/sensors /sensors after
uart:~$ ls /sensors
temp  humid  pressure
```

**Debugging:**
```
uart:~$ ns dump
Thread 0x20001000:
  /remote -> 9P (tcp://192.168.1.100:564)
  /local -> VFS (fatfs)
  /sensors -> UNION:
    [0] /local/sensors (CREATE)
    [1] /remote/sensors
```

**Profiling:**
```
uart:~$ ns stats
Path resolutions: 1543
Cache hits: 1291 (83.7%)
9P operations: 252
  Twalk: 89
  Topen: 63
  Tread: 78
  Twrite: 22
```

## 16. API Reference Summary

### 16.1 Namespace Management

| Function | Purpose |
|----------|---------|
| `ns_init()` | Initialize namespace subsystem |
| `ns_create()` | Create namespace for current thread |
| `ns_fork()` | Fork namespace for child thread |
| `ns_destroy()` | Destroy thread's namespace |
| `ns_get_current()` | Get current namespace |
| `ns_set_current()` | Switch namespace |

### 16.2 Mount Operations

| Function | Purpose |
|----------|---------|
| `ns_mount()` | Mount any VFS filesystem (9P, FAT, LittleFS, etc.) |
| `ns_bind()` | Bind path to another location |
| `ns_unmount()` | Remove mount or binding |
| `ns_clear()` | Clear all mounts |

### 16.3 File Operations

| Function | Purpose |
|----------|---------|
| `ns_open()` | Open file |
| `ns_read()` | Read from file |
| `ns_write()` | Write to file |
| `ns_close()` | Close file |
| `ns_lseek()` | Seek in file |
| `ns_stat()` | Get file attributes |

### 16.4 Directory Operations

| Function | Purpose |
|----------|---------|
| `ns_opendir()` | Open directory |
| `ns_readdir()` | Read directory entry |
| `ns_closedir()` | Close directory |
| `ns_mkdir()` | Create directory |

### 16.5 Utility Functions

| Function | Purpose |
|----------|---------|
| `ns_walk()` | Resolve path to entries |
| `ns_dump()` | Print namespace (debug) |

## 17. Acceptance Criteria

The implementation is considered complete when:

1. ✅ All Phase 1-4 deliverables are implemented
2. ✅ All unit tests pass
3. ✅ All integration tests pass
4. ✅ Memory usage meets targets (<4KB per namespace)
5. ✅ Performance meets targets (<100μs path resolution)
6. ✅ No memory leaks detected
7. ✅ Works with all 9p4z transports (UART, TCP, BLE, Thread)
8. ✅ Documentation is complete
9. ✅ Example applications run successfully
10. ✅ Code review approved

## 18. References

### 18.1 Plan 9 Documentation
- Plan 9 intro(3) - Introduction to Plan 9 namespaces
- Plan 9 bind(1) - Bind and mount command
- Plan 9 ns(1) - Display namespace

### 18.2 Zephyr Documentation
- Zephyr File Systems documentation
- Zephyr Thread APIs
- Zephyr VFS implementation

### 18.3 Related Projects
- 9p4z: https://github.com/jrsharp/9p4z
- Plan 9 from User Space (plan9port)
- v9fs (Linux 9P filesystem)

## Appendix A: Code Organization

```
namespace/
├── include/
│   └── zephyr/
│       └── namespace/
│           ├── namespace.h         # Public API
│           ├── fs_9p.h            # 9P VFS driver API
│           └── synthetic.h        # Synthetic FS API
├── src/
│   ├── namespace.c                # Core namespace manager
│   ├── resolve.c                  # Path resolution
│   ├── union.c                    # Union mount logic
│   ├── fs_9p.c                    # 9P VFS driver
│   ├── fid_pool.c                 # FID management
│   ├── thread_hooks.c             # Thread integration
│   └── synthetic_fs.c             # Synthetic filesystem support
├── tests/
│   ├── unit/
│   │   ├── test_namespace.c
│   │   ├── test_resolve.c
│   │   ├── test_union.c
│   │   └── test_fs_9p.c
│   └── integration/
│       ├── test_e2e.c
│       ├── test_multi_transport.c
│       └── test_stress.c
├── samples/
│   ├── basic_mount/
│   ├── union_mount/
│   ├── mesh_gateway/
│   └── synthetic_fs/
├── Kconfig                        # Configuration options
└── CMakeLists.txt
```

## Appendix B: Implementation Checklist

### Phase 1: 9P VFS Driver
- [ ] Create `fs_9p.c` skeleton
- [ ] Implement `fs_9p_mount()`
- [ ] Implement `fs_9p_open()` (Twalk + Topen)
- [ ] Implement `fs_9p_read()` (Tread)
- [ ] Implement `fs_9p_write()` (Twrite)
- [ ] Implement `fs_9p_close()` (Tclunk)
- [ ] Implement `fs_9p_stat()` (Twalk + Tstat)
- [ ] Implement `fs_9p_opendir()`
- [ ] Implement `fs_9p_readdir()`
- [ ] Implement `fs_9p_mkdir()` (Twalk + Tcreate)
- [ ] Implement `fs_9p_unlink()` (Twalk + Tremove)
- [ ] Implement FID pool management
- [ ] Register with VFS using `fs_register()`
- [ ] Write unit tests
- [ ] Test with 9p4z transports

### Phase 2: Namespace Manager
- [ ] Define data structures (ns_entry, thread_namespace)
- [ ] Implement `ns_init()`
- [ ] Implement `ns_create()`
- [ ] Implement `ns_destroy()`
- [ ] Implement TLS integration
- [ ] Implement simple path resolution
- [ ] Implement `ns_mount()` (filesystem-agnostic, VFS-based)
- [ ] Add auto-mount logic (call fs_mount if not already mounted)
- [ ] Implement `ns_open()`
- [ ] Implement `ns_read()`
- [ ] Implement `ns_write()`
- [ ] Implement `ns_close()`
- [ ] Write unit tests
- [ ] Test per-thread isolation
- [ ] Test with multiple filesystem types (9P, FAT, etc.)

### Phase 3: Union Mounts
- [ ] Implement union path resolution
- [ ] Implement `ns_bind()` with flags
- [ ] Implement priority-based ordering
- [ ] Implement directory merging
- [ ] Implement deduplication
- [ ] Handle CREATE flag
- [ ] Handle BEFORE/AFTER flags
- [ ] Write unit tests
- [ ] Test complex union scenarios

### Phase 4: Namespace Inheritance
- [ ] Implement COW namespace
- [ ] Implement `ns_fork()`
- [ ] Implement reference counting
- [ ] Add thread creation hooks
- [ ] Add thread exit hooks
- [ ] Implement lazy copying
- [ ] Write unit tests
- [ ] Test parent-child scenarios

### Phase 5: Polish
- [ ] Add Kconfig options
- [ ] Add shell commands
- [ ] Implement `ns_dump()`
- [ ] Write documentation
- [ ] Create example applications
- [ ] Performance profiling
- [ ] Memory optimization
- [ ] Comprehensive testing

---

**End of Specification**

This specification provides a complete blueprint for implementing Plan 9-style namespaces on Zephyr RTOS. The implementation should follow the phases in order, with each phase building on the previous one. The key insight is that Zephyr's VFS provides the filesystem abstraction layer, while the namespace manager sits above it to provide per-thread composition and union semantics.
