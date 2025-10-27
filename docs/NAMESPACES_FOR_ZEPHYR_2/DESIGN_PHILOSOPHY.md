# Design Philosophy: Plan 9 Namespaces for Zephyr

## How Close to Inferno?

### What You Get ✅

This implementation captures the **core architectural insight** of Plan 9 and Inferno:

1. **Per-process (thread) namespaces** - Each thread can have its own view of the filesystem hierarchy
2. **Union mounts** - Multiple filesystems can be overlaid at the same path with Plan 9 semantics (BEFORE/AFTER/CREATE)
3. **Network transparency** - Remote resources accessed via 9P appear as local files
4. **Everything-is-a-file** - Uniform interface for all resources
5. **Composable namespaces** - Build complex hierarchies by combining simple mounts
6. **Dynamic construction** - Namespaces assembled at runtime, not compile-time

### What You Don't Get ❌

1. **Limbo/Dis** - No VM or bytecode system (as noted)
2. **Full Inferno OS** - This runs *on* Zephyr, not as a complete OS replacement
3. **Inferno's process model** - Uses Zephyr threads, not Inferno processes
4. **Built-in distributed GC** - Would need separate implementation
5. **Styx protocol** - Uses standard 9P2000, not Inferno's Styx variant (though 9p4z could be extended)
6. **Prefab/Tk graphics** - Not relevant for embedded RTOS

### The Bottom Line

**You're not transforming Zephyr into Inferno.** You're bringing Inferno's most powerful abstraction—composable namespaces—to the Zephyr ecosystem.

Think of it like this:
- **Inferno:** A complete OS built around namespaces (the house)
- **This:** A namespace library for Zephyr (a really nice room in a different house)

It's analogous to:
- `plan9port` - Brings Plan 9 tools to Linux
- `libv9fs` - Brings 9P to Linux kernel
- **This** - Brings Plan 9 namespaces to Zephyr RTOS

You get the elegant composability and network transparency that makes Plan 9/Inferno special, but within Zephyr's existing RTOS architecture rather than replacing it.

## Why Only One Mount Function?

### The Original Question

Should we have:
- `ns_mount(struct ninep_client *, ...)` - 9P-specific mounting
- `ns_mount_vfs(struct fs_mount_t *, ...)` - Generic VFS mounting

Or just:
- `ns_mount(struct fs_mount_t *, ...)` - One function for all filesystems

### The Answer: One Function (Filesystem-Agnostic)

**We chose the single, filesystem-agnostic API** for these reasons:

### 1. Clean Abstraction

The namespace layer should not know about 9P (or FAT, or LittleFS, or anything else). It should only understand:
- VFS mount structures
- Path resolution
- Union semantics

This follows the Unix philosophy: **mechanism, not policy**.

```
NAMESPACE LAYER = Path composition mechanism
VFS LAYER       = Filesystem policy
```

### 2. Consistency

One API means consistent usage patterns:

```c
// 9P filesystem
struct fs_mount_t ninep_mount = {
    .type = FS_TYPE_9P,
    .fs_data = &ninep_ctx,
    // ...
};
ns_mount(&ninep_mount, "/remote", 0);

// FAT filesystem  
struct fs_mount_t fat_mount = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
    // ...
};
ns_mount(&fat_mount, "/local", 0);

// Same function, same pattern!
```

### 3. Extensibility

When someone adds a new filesystem type to Zephyr (e.g., a custom synthetic FS), it automatically works with namespaces. No changes needed to namespace code.

```c
// Future filesystem - works immediately
struct fs_mount_t custom_mount = {
    .type = FS_TYPE_CUSTOM,
    .fs_data = &custom_ctx,
};
ns_mount(&custom_mount, "/custom", 0);  // Just works!
```

### 4. Simplicity

Compare the implementation complexity:

**Two Functions (9P-aware):**
```c
int ns_mount(struct ninep_client *client, ...) {
    // Create VFS mount internally
    // Special handling for 9P
    // Add to namespace
}

int ns_mount_vfs(struct fs_mount_t *mount, ...) {
    // Add to namespace
}

// Namespace code now knows about 9P!
// More code paths to maintain and test
```

**One Function (9P-agnostic):**
```c
int ns_mount(struct fs_mount_t *mount, ...) {
    // Just add to namespace
    // Doesn't care what filesystem type it is
}

// Namespace code is simpler
// Single code path
// Easier to maintain and test
```

### 5. Flexibility

Users can configure their mounts however they want before adding to namespace:

```c
struct fs_mount_t mount = {
    .type = FS_TYPE_9P,
    .mnt_point = "/sys_9p_0",
    .fs_data = &ctx,
};

// Maybe set special flags
mount.flags = SOME_SPECIAL_FLAG;

// Maybe register custom callbacks
mount.on_error = my_error_handler;

// Now add to namespace
ns_mount(&mount, "/remote", 0);
```

### The Trade-off

**Yes**, it's slightly more verbose for 9P mounts:

```c
// Before (if we had 9P-specific API):
struct ninep_client *client = ninep_client_create();
ninep_connect(client, "tcp", "192.168.1.100:564");
ns_mount(client, "/", "/remote", 0);

// After (filesystem-agnostic API):
struct ninep_client *client = ninep_client_create();
ninep_connect(client, "tcp", "192.168.1.100:564");

struct ninep_mount_ctx ctx = { .client = *client, .aname = "/" };
struct fs_mount_t mount = {
    .type = FS_TYPE_9P,
    .mnt_point = "/sys_9p_0",
    .fs_data = &ctx,
};
ns_mount(&mount, "/remote", 0);
```

But you gain:
- ✅ Cleaner architecture
- ✅ Simpler implementation  
- ✅ Better extensibility
- ✅ More maintainable code
- ✅ Consistent API across all filesystem types

### Could We Add a Helper?

If you *really* want convenience, you could add a helper function in user code:

```c
// Helper in your application (not in namespace library)
static inline int ns_mount_9p_helper(struct ninep_client *client,
                                      const char *aname,
                                      const char *mnt_point,
                                      uint32_t flags) {
    struct ninep_mount_ctx ctx = { .client = *client, .aname = aname };
    struct fs_mount_t mount = {
        .type = FS_TYPE_9P,
        .mnt_point = mnt_point,  // or some generated name
        .fs_data = &ctx,
    };
    return ns_mount(&mount, mnt_point, flags);
}
```

But we don't include this in the core namespace library to keep it filesystem-agnostic.

## The Plan 9 Spirit

This design stays true to the Plan 9 philosophy:

> "The key feature of Plan 9 is that it does not distinguish between local and remote resources."

In our implementation:
- The **namespace layer** doesn't distinguish between filesystem types
- A mount is a mount, whether it's 9P over a mesh network or FAT on an SD card
- Union mounts work identically regardless of the underlying filesystem
- Path resolution is uniform

This is **more** Plan 9-like than having special cases for 9P!

## Summary

**Relationship to Inferno:**
- Captures the namespace abstraction that makes Plan 9/Inferno elegant
- Not a complete OS replacement, but a powerful library for Zephyr
- "Plan 9 namespaces for embedded systems"

**API Design Choice:**
- Single `ns_mount()` function for all filesystem types
- Namespace layer is filesystem-agnostic (doesn't know about 9P)
- Simpler, cleaner, more extensible
- Slightly more verbose, but gains architectural clarity

**The Big Picture:**

You're bringing one of operating systems' most elegant ideas—composable per-process namespaces—to the resource-constrained embedded world. That's exciting! And by keeping the abstraction clean (namespace doesn't know about 9P), you're staying true to the Plan 9 philosophy of simple, composable mechanisms.
