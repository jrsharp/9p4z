# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

9P for Zephyr (9p4z) is a 9P protocol library implementation for the Zephyr RTOS. The 9P protocol is a network protocol developed for Plan 9 from Bell Labs for distributed file systems.

## Target Use Cases

- 9P over IPv4
- 9P over IPv6
- 9P over L2CAP (Bluetooth)
- 9P over Thread
- 9P over UART

## Development Environment

This is a Zephyr RTOS project. Zephyr development typically requires:
- Zephyr SDK installed
- West tool for project management
- CMake-based build system
- Device Tree configurations for hardware targets

## Project Structure

```
9p4z/
├── CMakeLists.txt              # Main build configuration
├── Kconfig                     # Configuration options
├── zephyr/
│   └── module.yml             # Zephyr module definition
├── include/zephyr/9p/         # Public API headers
│   ├── protocol.h             # 9P protocol definitions and message parsing
│   ├── fid.h                  # File ID management
│   ├── tag.h                  # Request tag management
│   ├── transport.h            # Transport abstraction layer
│   └── transport_uart.h       # UART transport
├── src/                       # Implementation
│   ├── proto.c                # Protocol message serialization
│   ├── fid.c                  # Fid table management
│   ├── tag.c                  # Tag table management
│   ├── transport_uart.c       # UART transport implementation
│   ├── server.c               # Server implementation (TODO)
│   ├── client.c               # Client implementation (TODO)
│   ├── transport_tcp.c        # TCP transport (Phase 3)
│   └── transport_l2cap.c      # L2CAP transport (Phase 4)
├── samples/
│   └── uart_echo/             # Basic UART echo sample
└── tests/                     # Unit tests (ztest)
    ├── protocol_test.c        # Protocol parsing tests
    ├── fid_test.c            # Fid management tests
    └── tag_test.c            # Tag management tests
```

## Building and Testing

### Build tests
```bash
west build -b native_posix tests/
west build -t run
```

### Build UART echo sample
```bash
west build -b <board> samples/uart_echo/
west flash
```

## Architecture

### Core Components

1. **Protocol Layer** (`protocol.h`, `proto.c`)
   - Message header parsing/serialization
   - String, qid, stat encoding/decoding
   - Little-endian byte order handling
   - Validates message sizes and types

2. **Resource Management** (`fid.h`, `tag.h`)
   - Fid table: Tracks open file handles (configurable via `CONFIG_NINEP_MAX_FIDS`)
   - Tag table: Manages pending request tags (configurable via `CONFIG_NINEP_MAX_TAGS`)
   - Fixed allocation, no dynamic memory

3. **Transport Abstraction** (`transport.h`)
   - Common interface for all transports
   - Callback-based message delivery
   - Operations: send, start, stop
   - Transport-agnostic upper layers

4. **UART Transport** (`transport_uart.c`)
   - Interrupt-driven receive
   - Polling transmit
   - Automatic message framing (reads size header first)
   - Complete message delivery to callback

## Configuration

Key Kconfig options:
- `CONFIG_NINEP` - Enable 9P support
- `CONFIG_NINEP_MAX_MESSAGE_SIZE` - Max message size (default: 8192)
- `CONFIG_NINEP_MAX_FIDS` - Max concurrent fids (default: 32)
- `CONFIG_NINEP_MAX_TAGS` - Max pending requests (default: 16)
- `CONFIG_NINEP_SERVER` - Enable server support
- `CONFIG_NINEP_CLIENT` - Enable client support
- `CONFIG_NINEP_TRANSPORT_UART` - Enable UART transport
- `CONFIG_NINEP_TRANSPORT_TCP` - Enable TCP transport (Phase 3)
- `CONFIG_NINEP_TRANSPORT_L2CAP` - Enable L2CAP transport (Phase 4)

## Implementation Status

**Phase 1 (COMPLETE):** Core protocol library
- ✅ Message parsing/serialization
- ✅ Fid and tag management
- ✅ Unit tests

**Phase 2 (COMPLETE):** Transport abstraction
- ✅ Generic transport interface
- ✅ UART implementation
- ✅ UART echo sample

**Phase 3 (TODO):** Network transports
- ⏳ TCP/IPv4 implementation
- ⏳ TCP/IPv6 implementation
- ⏳ Sample applications

**Phase 4 (TODO):** Advanced transports
- ⏳ Bluetooth L2CAP
- ⏳ Thread/802.15.4 support

**Phase 5 (TODO):** Server/Client utilities
- ⏳ Full server implementation
- ⏳ Full client implementation
- ⏳ File system abstraction