# 9p4z Memory Architecture

## Design Document: Caller-Provided Memory Pools

**Status:** Proposed
**Author:** FRST Project
**Date:** 2026-02-14

## Problem Statement

The current 9p4z implementation embeds fixed-size arrays directly in client/server
structs, with sizes determined at compile time via Kconfig:

```c
struct ninep_client {
    struct ninep_client_fid fids[CONFIG_NINEP_MAX_FIDS];
    struct ninep_tag_entry tags[CONFIG_NINEP_MAX_TAGS];
    uint8_t tx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
    uint8_t resp_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
    // ...
};
```

This creates several problems:

1. **Memory location is fixed** - Arrays end up in DRAM even on platforms with
   abundant external RAM (PSRAM, SPIRAM, etc.)

2. **Size/memory tradeoff is global** - A platform with 8MB PSRAM and 300KB DRAM
   must still fit FID arrays in DRAM, limiting scalability

3. **No runtime flexibility** - Can't adjust pool sizes based on use case
   (e.g., simple keyboard client vs. full 9P workstation)

4. **Kconfig explosion** - Every size parameter requires a Kconfig option

## Design Principles

### 1. Library Never Allocates

The 9p4z library advertises "static memory" as a feature. This is valuable for:
- Predictable memory usage
- No heap fragmentation
- Works on systems without dynamic allocation
- Easier certification (automotive, medical, etc.)

**We preserve this.** The library itself never calls malloc/free/k_malloc.

### 2. Caller Provides Memory

The caller (application or platform code) provides memory pools at initialization.
The library uses what it's given. This follows Zephyr's pattern for kernel objects.

### 3. Memory Policy vs. Mechanism

- **Policy** (where memory comes from, how much) = Caller's responsibility
- **Mechanism** (how to use memory efficiently) = Library's responsibility

### 4. Platform Agnostic

The library doesn't know or care about:
- PSRAM vs DRAM vs SRAM
- Static vs heap allocation
- Memory speeds or cache behavior

It just uses the pointers it's given.

## Proposed API

### Configuration Structure

```c
/**
 * @brief Memory pool configuration for 9P client
 *
 * All memory pools are provided by the caller. The library never allocates.
 * This allows platforms to place pools in appropriate memory regions
 * (PSRAM, SRAM, heap, etc.) without library changes.
 */
struct ninep_client_pools {
    /** FID tracking pool - one entry per open file/directory */
    struct ninep_client_fid *fids;
    size_t max_fids;

    /** Tag tracking pool - one entry per concurrent request */
    struct ninep_tag_entry *tags;
    size_t max_tags;

    /** Transmit buffer - sized for max message */
    uint8_t *tx_buf;

    /** Receive/response buffer - sized for max message */
    uint8_t *rx_buf;

    /** Buffer size (same for tx and rx) */
    size_t buf_size;
};

/**
 * @brief 9P client configuration
 */
struct ninep_client_config {
    /** Memory pools - caller allocated */
    struct ninep_client_pools pools;

    /** Protocol configuration */
    const char *version;        /* "9P2000" */
    uint32_t timeout_ms;        /* Request timeout */
};
```

### Client Structure

```c
/**
 * @brief 9P client instance
 *
 * The client struct itself is small and can live in DRAM.
 * Large arrays are accessed via pointers to caller-provided pools.
 */
struct ninep_client {
    const struct ninep_client_config *config;
    struct ninep_transport *transport;

    /* Pool pointers (from config) */
    struct ninep_client_fid *fids;
    size_t max_fids;
    struct ninep_tag_entry *tags;
    size_t max_tags;
    uint8_t *tx_buf;
    uint8_t *resp_buf;
    size_t buf_size;

    /* Runtime state (small, stays in struct) */
    uint32_t msize;
    uint32_t next_fid;
    uint16_t next_tag;
    size_t resp_len;

    /* Synchronization */
    struct k_mutex lock;
    struct k_condvar resp_cv;
};
```

### Initialization

```c
/**
 * @brief Initialize 9P client with caller-provided memory pools
 *
 * @param client    Client instance (caller provides storage)
 * @param config    Configuration including memory pools
 * @param transport Transport layer instance
 * @return 0 on success, negative errno on failure
 */
int ninep_client_init(struct ninep_client *client,
                      const struct ninep_client_config *config,
                      struct ninep_transport *transport);
```

## Platform Usage Examples

### Example 1: Small MCU (nRF52, 256KB SRAM)

Static allocation in SRAM, minimal sizes:

```c
/* Static pools in SRAM */
static struct ninep_client_fid my_fids[8];
static struct ninep_tag_entry my_tags[4];
static uint8_t my_tx_buf[256];
static uint8_t my_rx_buf[256];

static const struct ninep_client_config my_config = {
    .pools = {
        .fids = my_fids,
        .max_fids = ARRAY_SIZE(my_fids),
        .tags = my_tags,
        .max_tags = ARRAY_SIZE(my_tags),
        .tx_buf = my_tx_buf,
        .rx_buf = my_rx_buf,
        .buf_size = 256,
    },
    .version = "9P2000",
    .timeout_ms = 30000,
};

static struct ninep_client my_client;

int init(void) {
    return ninep_client_init(&my_client, &my_config, &my_transport);
}
```

### Example 2: ESP32-S3 (300KB DRAM, 8MB PSRAM)

Large pools in PSRAM for full workstation capability:

```c
/* Large pools in PSRAM - 64 open files, 32 concurrent ops */
static struct ninep_client_fid my_fids[64]
    __attribute__((section(".ext_ram.bss")));
static struct ninep_tag_entry my_tags[32]
    __attribute__((section(".ext_ram.bss")));
static uint8_t my_tx_buf[4096]
    __attribute__((section(".ext_ram.bss")));
static uint8_t my_rx_buf[4096]
    __attribute__((section(".ext_ram.bss")));

static const struct ninep_client_config my_config = {
    .pools = {
        .fids = my_fids,
        .max_fids = ARRAY_SIZE(my_fids),
        .tags = my_tags,
        .max_tags = ARRAY_SIZE(my_tags),
        .tx_buf = my_tx_buf,
        .rx_buf = my_rx_buf,
        .buf_size = 4096,
    },
    .version = "9P2000",
    .timeout_ms = 35000,
};
```

### Example 3: Dynamic Allocation (Linux port, testing)

Runtime allocation from heap:

```c
struct ninep_client_config *create_config(size_t max_fids, size_t max_tags) {
    struct ninep_client_config *cfg = malloc(sizeof(*cfg));

    cfg->pools.fids = calloc(max_fids, sizeof(struct ninep_client_fid));
    cfg->pools.max_fids = max_fids;
    cfg->pools.tags = calloc(max_tags, sizeof(struct ninep_tag_entry));
    cfg->pools.max_tags = max_tags;
    cfg->pools.tx_buf = malloc(8192);
    cfg->pools.rx_buf = malloc(8192);
    cfg->pools.buf_size = 8192;
    cfg->version = "9P2000";
    cfg->timeout_ms = 30000;

    return cfg;
}
```

### Example 4: Zephyr with Runtime PSRAM Allocation

Using Zephyr's shared_multi_heap for PSRAM:

```c
#include <zephyr/multi_heap/shared_multi_heap.h>

int init_9p_client_psram(struct ninep_client *client,
                         size_t max_fids, size_t max_tags) {
    static struct ninep_client_config cfg;

    /* Allocate pools from PSRAM */
    cfg.pools.fids = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL,
                         max_fids * sizeof(struct ninep_client_fid));
    cfg.pools.max_fids = max_fids;

    cfg.pools.tags = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL,
                         max_tags * sizeof(struct ninep_tag_entry));
    cfg.pools.max_tags = max_tags;

    cfg.pools.tx_buf = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, 4096);
    cfg.pools.rx_buf = shared_multi_heap_alloc(SMH_REG_ATTR_EXTERNAL, 4096);
    cfg.pools.buf_size = 4096;

    cfg.version = "9P2000";
    cfg.timeout_ms = 35000;

    return ninep_client_init(client, &cfg, transport);
}
```

## Convenience Macros

For simple cases, provide macros that embed pools in a wrapper struct:

```c
/**
 * @brief Define a 9P client with embedded static pools
 *
 * Creates a static client with embedded memory pools. Use for simple cases
 * where caller-provided pools aren't needed.
 *
 * @param name      Client variable name
 * @param max_fids  Maximum open files
 * @param max_tags  Maximum concurrent requests
 * @param buf_size  TX/RX buffer size
 */
#define NINEP_CLIENT_DEFINE_STATIC(name, max_fids_, max_tags_, buf_size_) \
    static struct { \
        struct ninep_client client; \
        struct ninep_client_fid fids[max_fids_]; \
        struct ninep_tag_entry tags[max_tags_]; \
        uint8_t tx_buf[buf_size_]; \
        uint8_t rx_buf[buf_size_]; \
        struct ninep_client_config config; \
    } name##_storage = { \
        .config = { \
            .pools = { \
                .fids = name##_storage.fids, \
                .max_fids = max_fids_, \
                .tags = name##_storage.tags, \
                .max_tags = max_tags_, \
                .tx_buf = name##_storage.tx_buf, \
                .rx_buf = name##_storage.rx_buf, \
                .buf_size = buf_size_, \
            }, \
            .version = "9P2000", \
            .timeout_ms = 30000, \
        }, \
    }; \
    static struct ninep_client *name = &name##_storage.client

/* Usage */
NINEP_CLIENT_DEFINE_STATIC(my_client, 8, 4, 512);

int init(void) {
    return ninep_client_init(my_client, &my_client_storage.config, transport);
}
```

## Server Side

The same pattern applies to `struct ninep_server`:

```c
struct ninep_server_pools {
    struct ninep_server_fid *fids;
    size_t max_fids;
    uint8_t *tx_buf;
    uint8_t *rx_buf;
    size_t buf_size;
    /* Additional server-specific pools */
    char *uname_pool;           /* Username string pool */
    size_t uname_pool_size;
    struct ninep_auth_state *auth_pool;
    size_t max_auth;
};
```

## Migration Path

### Phase 1: Add Pool Support (non-breaking)

1. Add `struct ninep_client_pools` to config
2. If pools are NULL, fall back to embedded arrays (current behavior)
3. If pools are provided, use them instead
4. Deprecation warning for embedded array usage

### Phase 2: Default to External Pools

1. Embedded arrays become opt-in via Kconfig
2. Default expectation is caller provides pools
3. Update all examples and documentation

### Phase 3: Remove Embedded Arrays

1. Remove CONFIG_NINEP_MAX_FIDS etc. from client struct sizing
2. Keep Kconfig only for convenience macro defaults
3. Embedded arrays only via NINEP_CLIENT_DEFINE_STATIC macro

## Memory Sizing Guidelines

| Use Case | FIDs | Tags | Buffer | Notes |
|----------|------|------|--------|-------|
| Simple peripheral (keyboard) | 4-8 | 2-4 | 256-512 | Single connection |
| Terminal client | 16-32 | 8-16 | 512-1024 | Multiple open files |
| Full workstation | 64-128 | 32-64 | 2048-8192 | Many mounts, concurrent I/O |
| File server | 256+ | 64+ | 8192+ | Many clients |

## Implementation Notes

### Thread Safety

The mutex and condvar remain in the client struct (small, must be in DRAM for
kernel object requirements). Only data arrays move to external pools.

### Validation

`ninep_client_init()` should validate:
- All pool pointers are non-NULL
- Sizes are reasonable minimums (e.g., max_fids >= 1)
- Buffers are large enough for minimum 9P message

### Cache Considerations

On platforms with cached PSRAM, callers may need to ensure cache coherency
for buffers used in DMA operations. This is outside the library's scope -
the caller knows their memory architecture.

## Conclusion

This design maintains 9p4z's "static memory" property while enabling platforms
to scale memory usage appropriately. The library remains simple and portable;
complexity of memory management stays with the platform code where it belongs.
