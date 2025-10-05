# 9P Interactive Client Sample

An interactive 9P client for Zephyr that connects to a 9P server and provides shell commands to navigate the filesystem. This sample demonstrates the 9p4z protocol implementation and can be used for testing and development.

## Features

- Interactive shell with command-line editing
- Version negotiation and filesystem attachment
- Directory navigation (ls, cd, pwd)
- File operations (cat, stat)
- Error handling and reporting
- Works with any 9P2000 server

## Commands

- `help` - Show available commands
- `connect` - Connect to 9P server (version + attach)
- `pwd` - Print working directory
- `ls [path]` - List directory (currently performs walk only)
- `cd <path>` - Change directory
- `cat <file>` - Display file contents (up to 8KB)
- `stat <path>` - Show file information (type, mode, size)
- `quit` / `exit` - Exit the client

## Building

Build for native_posix (for testing with 9pserve on macOS):

```bash
cd /path/to/workspace
west build -b native_posix 9p4z/samples/9p_client
```

Build for QEMU x86:

```bash
west build -b qemu_x86 9p4z/samples/9p_client
```

## Running with 9pserve (macOS)

### Prerequisites

Install Plan 9 from User Space on macOS:

```bash
brew install plan9port
```

### Quick Start

Build and run the client in QEMU:

```bash
cd /path/to/workspace
source .venv/bin/activate

# Build the client
west build -b qemu_x86 9p4z/samples/9p_client

# Run in QEMU (boots to interactive prompt)
west build -t run
```

The client will boot and show an interactive prompt. You can type `help` to see available commands.

**Note:** Full end-to-end testing with a 9P server requires connecting QEMU's uart1 to the server. The helper scripts are a work in progress. For now, the client demonstrates successful boot with UART polling mode (no interrupt crashes!).

### Manual Setup

1. Create a test directory to serve:

```bash
mkdir -p ~/9p-test
cd ~/9p-test
echo "Hello from 9P!" > test.txt
mkdir subdir
echo "File in subdirectory" > subdir/file.txt
```

2. Start file server with 9pserve:

```bash
# Option 1: Serve directory via 9pex piped to 9pserve on Unix socket
9pex ~/9p-test | 9pserve unix!/tmp/9p.sock &

# Option 2: Serve via TCP (for remote testing)
9pex ~/9p-test | 9pserve tcp!*!9999 &

# Option 3: Use ramfs (in-memory filesystem)
ramfs | 9pserve unix!/tmp/9p.sock &
```

**Note**: `9pserve` announces a 9P service on the specified address and multiplexes clients onto the file server (like `9pex` or `ramfs`) piped to its stdin/stdout.

3. Build and run the client:

```bash
# Build for QEMU
west build -b qemu_x86 9p4z/samples/9p_client

# Run QEMU connected to the server
qemu-system-i386 \
  -m 32 \
  -cpu qemu32 \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -no-reboot \
  -nographic \
  -serial unix:/tmp/9p.sock \
  -kernel build/zephyr/zephyr.elf
```

### Using the Client

Once the client starts, you'll see an interactive prompt:

```
9P Interactive Client
Type 'help' for commands

9p> connect
Connected successfully
9p> pwd
/
9p> ls
Listing: .
Walk successful: 0 qids returned
(Full directory listing not yet implemented)
9p> cd subdir
9p> pwd
/subdir
9p> cat file.txt
File in subdirectory
9p> stat test.txt
File: test.txt
Type: file
Mode: 0x00000644
Size: 16 bytes
9p> quit
Goodbye!
```

## Architecture

The client demonstrates several key 9P concepts:

### Transport Layer
- Uses `ninep_transport_uart` for serial communication
- Async message receive via callback
- Synchronous request/response pattern using semaphores

### Protocol Layer
- Version negotiation (Tversion/Rversion)
- Filesystem attachment (Tattach/Rattach)
- Path walking (Twalk/Rwalk)
- File operations (Topen/Ropen, Tread/Rread, Tstat/Rstat)

### Resource Management
- FID table for tracking file identifiers
- Tag table for request/response matching
- Proper cleanup on errors

### State Management
- Current working directory tracking (path and FID)
- Connection state
- Response synchronization

## Configuration

Key configuration options in `prj.conf`:

```ini
# 9P protocol
CONFIG_NINEP=y
CONFIG_NINEP_MAX_MESSAGE_SIZE=8192
CONFIG_NINEP_TRANSPORT_UART=y

# Console
CONFIG_CONSOLE_SUBSYS=y
CONFIG_CONSOLE_GETCHAR=y

# Logging
CONFIG_LOG=y
CONFIG_NINEP_LOG_LEVEL_DBG=y
```

## Limitations

- Current `ls` implementation only performs the walk, doesn't read directory entries
- `cat` reads max 8KB of file data
- Single-element path walking (no multi-level paths like "foo/bar/baz")
- No write operations (read-only client)
- UART transport only (no TCP/WiFi yet)

## Future Enhancements

- Full directory listing with Topen + Tread
- Multi-element path support
- Write operations (Twrite, Tcreate, Tremove)
- WiFi transport for ESP32-S3
- Tab completion
- Command history

## Testing

The sample serves as an integration test for the 9p4z protocol stack. It exercises:

- Message building (all T-messages)
- Message parsing (R-messages and Rerror)
- Transport send/receive
- FID and tag management
- Error handling

## References

- [Plan 9 Manual - Section 5: File Protocols](http://man.cat-v.org/plan_9/5/)
- [9P2000 Protocol Specification](http://ericvh.github.io/9p-rfc/rfc9p2000.html)
- [Plan 9 from User Space](https://9fans.github.io/plan9port/)
