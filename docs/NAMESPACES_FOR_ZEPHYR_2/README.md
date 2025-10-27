# Plan 9 Namespaces for Zephyr - Complete Package

## Document Overview

This package contains a complete specification for implementing Plan 9-style composable namespaces on Zephyr RTOS.

### Core Documents

1. **[NAMESPACE_SPEC.md](./NAMESPACE_SPEC.md)** - Complete technical specification
   - Architecture and data structures
   - Full API reference (30+ functions)
   - Implementation phases (6 phases)
   - Testing strategy
   - Performance targets
   - Example code

2. **[DRIVER_FILESYSTEM_ADDENDUM.md](./DRIVER_FILESYSTEM_ADDENDUM.md)** - Driver-as-filesystem model
   - How drivers register as 9P servers
   - In-process 9P server implementation
   - Examples: display, mouse, GPIO, network drivers
   - The missing piece that makes it truly Plan 9-like!

3. **[SRV_REGISTRY_ADDENDUM.md](./SRV_REGISTRY_ADDENDUM.md)** - Service registry (/srv)
   - Service discovery and rendezvous mechanism
   - Dynamic service binding (services come and go)
   - Network service integration (mDNS, CoAP-RD)
   - **Completes the Plan 9 model!**

4. **[SYSFS_REFACTORING_SPECIFIC.md](./SYSFS_REFACTORING_SPECIFIC.md)** - Migrate existing 9p4z sysfs
   - **Precise refactoring guide for YOUR actual code**
   - Wrap existing sysfs as 9P server
   - Zero driver changes needed
   - Get network transparency + service discovery

5. **[DESIGN_PHILOSOPHY.md](./DESIGN_PHILOSOPHY.md)** - Philosophy and rationale
   - Relationship to Plan 9 and Inferno
   - Why filesystem-agnostic API
   - Architectural decisions explained

## Quick Start for Implementers

### What You're Building

A namespace system that brings Plan 9's elegant composability to Zephyr:

```c
// Per-thread custom namespaces
ns_create(NULL);

// Mount local filesystem
struct fs_mount_t local = { .type = FS_FATFS, ... };
ns_mount(&local, "/local", 0);

// Mount remote 9P filesystem (over TCP, BLE, Thread mesh, etc.)
struct fs_mount_t remote = { .type = FS_TYPE_9P, ... };
ns_mount(&remote, "/remote", 0);

// Union mount - local appears first, remote as fallback
ns_bind("/remote/sensors", "/sensors", NS_FLAG_AFTER);

// Access through standard file I/O
int fd = ns_open("/sensors/temp", FS_O_READ);
char buf[32];
ns_read(fd, buf, sizeof(buf));
ns_close(fd);

// Drivers can register as filesystems
struct ninep_server *display = ninep_server_register("draw", &draw_ops, drv);
srv_post("display", display);  // Post to /srv for discovery

// Now applications discover and use it
srv_mount("display", "/dev/draw", 0);

// Display is accessible via file I/O
int ctl = ns_open("/dev/draw/ctl", FS_O_WRITE);
ns_write(ctl, "rect 10 10 100 100 ff0000", 25);
ns_close(ctl);

// Service discovery - what's available?
int dir = ns_opendir("/srv");
struct fs_dirent entry;
while (ns_readdir(dir, &entry) == 0) {
    printk("Available: /srv/%s\n", entry.name);
}
ns_closedir(dir);
```

### Implementation Order

**Phase 1: 9P VFS Driver** (2-3 weeks)
- Register 9P as a Zephyr filesystem type
- Map VFS operations to 9P protocol operations
- Integrate with existing 9p4z library
- Test with all transports (UART, TCP, BLE, Thread)

**Phase 2: Namespace Manager** (2-3 weeks)
- Per-thread namespace tables (using TLS)
- Simple path resolution
- Basic mount/unmount operations
- File operations (open, read, write, close)

**Phase 3: Union Mounts** (2-3 weeks)
- Multi-mount path resolution
- Directory merging and deduplication
- BEFORE/AFTER/CREATE flag semantics
- Whiteout support

**Phase 4: Namespace Inheritance** (1-2 weeks)
- Copy-on-write namespace forking
- Thread creation/exit hooks
- Reference counting

**Phase 5: Driver-as-Filesystem** (2-3 weeks) ⭐ **Critical for Plan 9 model**
- In-process 9P server API
- Driver registration mechanism
- Example drivers (display, GPIO, /proc)

**Phase 6: Polish** (1-2 weeks)
- Performance optimization
- Comprehensive testing
- Documentation and examples

**Total: ~12-16 weeks** for complete implementation

## Key Design Decisions

### 1. Filesystem-Agnostic Namespace Layer

The namespace layer doesn't know about 9P (or any other specific filesystem):

```c
// Single API for ALL filesystem types
int ns_mount(struct fs_mount_t *vfs_mount, const char *mnt_point, uint32_t flags);

// Works with 9P
ns_mount(&ninep_mount, "/remote", 0);

// Works with FAT
ns_mount(&fat_mount, "/local", 0);

// Works with in-process driver servers
ns_mount_server(display_server, "/dev/draw", 0);
```

**Why?**
- Simpler implementation (one code path)
- More extensible (new filesystems automatically work)
- More maintainable (namespace doesn't change when 9P changes)
- Consistent API across all filesystem types

### 2. VFS as the Abstraction Layer

Build on top of Zephyr's existing VFS rather than bypassing it:

```
Namespace Layer (composition)
     ↓
Zephyr VFS (filesystem abstraction)
     ↓
9P Driver / FAT / LittleFS / etc.
```

**Why?**
- Leverages existing, tested VFS code
- Interoperates with standard Zephyr filesystems
- Namespace becomes a pure composition layer

### 3. No POSIX or Userspace Required

Works with native Zephyr threads in supervisor mode:

```c
// No POSIX needed
CONFIG_POSIX_API=n

// No userspace needed
CONFIG_USERSPACE=n

// Just threads + optional TLS
CONFIG_THREAD_LOCAL_STORAGE=y
```

**Why?**
- Simpler for embedded systems
- Lower overhead
- Works on any Zephyr-supported hardware
- Userspace is optional (can add later)

### 4. In-Process 9P Servers for Drivers

Drivers implement lightweight 9P interface and run in the same address space:

```c
// Driver implements 9P ops
static const struct ninep_server_ops draw_ops = {
    .open = draw_open,
    .read = draw_read,
    .write = draw_write,
    // ...
};

// Register and mount
struct ninep_server *srv = ninep_server_register("draw", &draw_ops, drv);
ns_mount_server(srv, "/dev/draw", 0);
```

**Why?**
- Minimal overhead (direct function calls, no serialization)
- Network transparency (can export over TCP/BLE/etc. automatically)
- Uniform interface (everything is a file)
- Composable (drivers can be unioned, bound, etc.)

## What Makes This Plan 9-like?

### You Get ✅

1. **Per-process (thread) namespaces** - Custom view of filesystem hierarchy
2. **Union mounts** - Multiple filesystems at same path with Plan 9 semantics
3. **Network transparency** - Remote resources appear as local files via 9P
4. **Everything-is-a-file** - Drivers expose themselves as filesystems
5. **Composability** - Build complex hierarchies from simple mounts
6. **Dynamic construction** - Assemble namespaces at runtime

### This Is NOT ❌

- A complete Inferno port (no Limbo/Dis VM)
- A complete Plan 9 port (missing many userland tools)
- A replacement for Zephyr (it runs *on* Zephyr)

### It IS ✅

**"Plan 9 namespaces as a library for embedded systems"**

You get the core architectural insight—composable namespaces with uniform resource access—that makes Plan 9 elegant, but within Zephyr's existing RTOS architecture.

## Use Cases

### IoT Mesh Networks

```
Gateway Node:
  /local/sensors/temp       ← Local temperature sensor
  /nodes/node1/sensors/*    ← 9P over Thread mesh to node1
  /nodes/node2/sensors/*    ← 9P over Thread mesh to node2
  /cloud/storage/*          ← 9P over TCP to cloud server
  /sensors -> UNION         ← Union mount showing all sensors
```

Gateway can transparently read from sensors across the entire mesh as if they were local files.

### Distributed Device Management

```
Manager Thread:
  /dev/local/gpio/*         ← Local GPIO
  /dev/remote1/gpio/*       ← GPIO on remote device (via 9P)
  /dev -> UNION             ← Union of all device trees
  
# Write to any GPIO pin (local or remote) the same way:
echo 1 > /dev/gpio/pin42/value
```

### Development and Debugging

```
Debug Thread:
  /proc/*                   ← Thread/process info (synthetic FS)
  /sys/*                    ← System statistics (synthetic FS)
  /logs/local/*             ← Local logs
  /logs/remote/*            ← Logs from remote devices
  /logs -> UNION            ← Union of all log sources
```

### Dynamic Resource Discovery

```c
// Device appears on network
void device_discovered(const char *addr) {
    struct ninep_client *client = ninep_client_create();
    ninep_connect(client, "tcp", addr);
    
    char mnt[64];
    snprintf(mnt, sizeof(mnt), "/devices/%s", addr);
    
    struct fs_mount_t mount = { .type = FS_TYPE_9P, ... };
    ns_mount(&mount, mnt, 0);
    
    // Device's resources now accessible as files!
}
```

## Memory and Performance

### Memory Usage

- **Per namespace:** ~200 bytes base + ~128 bytes per mount
- **Per open file:** ~64 bytes
- **Example system:** 10 threads, 5 mounts each, 3 files per thread = ~11 KB total

### Performance Targets

- **Path resolution (simple):** <100 μs
- **Path resolution (union):** <500 μs  
- **File operation overhead:** <20 instructions vs direct VFS
- **Network round-trip:** Depends on transport (1-100ms typical)

### Optimization Strategies

- Path caching for frequently-accessed files
- Hash table for namespace lookup
- Memory pools for fixed-size allocations
- Pipeline 9P operations where possible

## Testing Strategy

### Unit Tests
- Namespace creation/destruction
- Path resolution (simple and union)
- Mount operations
- COW namespace inheritance
- 30+ test cases

### Integration Tests
- End-to-end file operations over 9P
- Multiple transports simultaneously
- Driver-as-filesystem
- Complex namespace compositions

### Stress Tests
- Many threads (50+)
- Many mounts (100+)
- Long-running stability
- Memory leak detection

## Getting Started

### For Implementers

1. Read [NAMESPACE_SPEC.md](./NAMESPACE_SPEC.md) for complete technical details
2. Read [DRIVER_FILESYSTEM_ADDENDUM.md](./DRIVER_FILESYSTEM_ADDENDUM.md) for driver model
3. Start with Phase 1 (9P VFS Driver)
4. Implement phases in order
5. Refer to examples and test cases

### For Users (Once Implemented)

```c
#include <zephyr/kernel.h>
#include <zephyr/namespace/namespace.h>

void main(void) {
    // Initialize namespace for main thread
    ns_create(NULL);
    
    // Mount local storage
    struct fs_mount_t local = { .type = FS_FATFS, ... };
    ns_mount(&local, "/local", 0);
    
    // Mount remote 9P server
    struct ninep_client *client = ninep_client_create();
    ninep_connect(client, "tcp", "192.168.1.100:564");
    struct fs_mount_t remote = { .type = FS_TYPE_9P, ... };
    ns_mount(&remote, "/remote", 0);
    
    // Use standard file I/O
    int fd = ns_open("/remote/sensors/temp", FS_O_READ);
    // ...
}
```

## Questions or Clarifications?

This specification package is designed to be handed to another Claude Sonnet 4.5 (or human implementer) for execution. It contains:

- ✅ Complete technical specification
- ✅ Full API reference
- ✅ Implementation phases with success criteria
- ✅ Test strategy
- ✅ Example code
- ✅ Performance targets
- ✅ Design rationale

Everything needed to implement Plan 9-style namespaces on Zephyr RTOS!

---

**Version:** 1.0  
**Date:** October 22, 2025  
**Status:** Ready for Implementation
