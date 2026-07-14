# 9p4z migration notes

## Per-tag client buffers (9P tag multiplexing fix)

**What changed:** the 9P *client* now gives every tag its own TX and RX buffer
instead of a single shared response buffer. The old shared buffer silently
**cross-delivered responses** when more than one request was outstanding on a
session at once (e.g. a multi-threaded client, or 9P-over-9P where one session
carries a `Twrite` and a `Tread` concurrently). The client always advertised
concurrent-tag support; it now actually delivers it.

This is a **per-tag-by-default** change. There is no shared-buffer mode anymore.

### Do I have to change anything?

| Your client uses... | Migration |
|---|---|
| **Embedded arrays** (`config.pools == NULL`) | **Recompile only.** The embedded arrays are now per-tag. Your client struct grows by `CONFIG_NINEP_MAX_TAGS * CONFIG_NINEP_MAX_MESSAGE_SIZE * 2` (minus the old single buffer). Make sure it still fits; if tight, switch that client to PSRAM-backed pools (below). |
| **Caller pools** (`config.pools` set) | **Resize two buffers.** `pools.tx_buf` and `pools.rx_buf` must now each be `max_tags * buf_size` bytes (one buffer per tag) instead of `buf_size`. Index is `buf + tag_slot * buf_size`; the library does this for you — you just provide the bigger region. |

### Removed

- `ninep_client_config.tag_tx_buf` / `tag_rx_buf` (a short-lived opt-in that
  never shipped) — folded into the embedded/pools paths.
- The shared `resp_buf` / `resp_len` / `tx_buf` client fields (internal).

### Scaling `max_tags`

`CONFIG_NINEP_MAX_TAGS` sizes the **embedded** arrays, so raising it grows every
embedded client (and any pool-client still carrying the embedded fallback). To
run many concurrent tags cheaply, keep `CONFIG_NINEP_MAX_TAGS` modest and give
the high-concurrency client **PSRAM-backed pools** with a larger per-client
`pools.max_tags`. Cost is `max_tags * msize * 2` per client — trivial in PSRAM.

### Memory

Per-tag buffers are plain data — safe in PSRAM. (Thread *stacks* and kernel
objects are the things that must NOT go in PSRAM on ESP32-S3; buffers are fine.)
