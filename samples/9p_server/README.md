# 9P Server Sample

This sample demonstrates a 9P server running on Zephyr, serving a RAM-backed filesystem over UART.

## Overview

The server exposes a simple filesystem with demo content:
- `/hello.txt` - Welcome message
- `/readme.txt` - Server documentation
- `/docs/doc1.txt` - Document 1
- `/docs/doc2.txt` - Document 2
- `/sys/version` - System version
- `/sys/board` - Board name

## Building

```bash
west build -b qemu_x86 9p4z/samples/9p_server
```

## Running

### With QEMU Script

```bash
# From workspace directory
9p4z/scripts/9p-run-server.sh
```

This will:
1. Start QEMU with the 9P server
2. Expose UART1 (9P transport) on TCP port 9998

### Connecting with 9P Client

From another terminal, use Plan 9 port tools:

```bash
# List root directory
9p -a 'tcp!localhost!9998' ls /

# Read files
9p -a 'tcp!localhost!9998' read /hello.txt
9p -a 'tcp!localhost!9998' read /readme.txt
9p -a 'tcp!localhost!9998' read /sys/version

# List subdirectory
9p -a 'tcp!localhost!9998' ls /docs
```

## Architecture

- **Server** (`src/server.c`) - 9P protocol message dispatcher
- **RAM FS** (`src/ramfs.c`) - In-memory filesystem backend
- **Transport** - UART in polling mode
- **Demo Files** - Created at startup in `setup_demo_filesystem()`

## Future Extensions

This server is designed to be extended with custom filesystems that expose:
- Zephyr Settings subsystem → `/settings/*`
- Shell commands → `/shell/exec`
- Device tree → `/dev/*`
- Logging → `/logs/*`
- System stats → `/sys/*`

See `include/zephyr/9p/server.h` for the filesystem operations interface.
