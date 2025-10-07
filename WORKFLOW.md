# 9p4z Development Workflow

Quick reference for working with the 9p4z project in Claude Code sessions.

## Hardware Target: nRF7002dk (Nordic NCS)

**⭐ RECOMMENDED: Use this for real hardware testing with WiFi**

See **[docs/NCS-BUILD-SETUP.md](docs/NCS-BUILD-SETUP.md)** for complete setup.

Quick commands:
```bash
# Build
/Users/jrsharp/zephyr-workspaces/build-ncs.sh

# Flash
cd /opt/nordic/ncs/v3.1.1
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/bin:/opt/nordic/ncs/toolchains/561dce9adf/nrfutil/bin:$PATH"
west flash -d /Users/jrsharp/zephyr-workspaces/9p4z-ncs-workspace/build/9p_server_tcp/9p_server_tcp --runner jlink
```

**Key Points:**
- Source: `/Users/jrsharp/src/9p4z/` (changes take effect immediately)
- No west.yml modification needed
- Uses `ZEPHYR_EXTRA_MODULES` cmake argument
- 686KB binary with full WiFi + 9P TCP server

## QEMU Testing (x86)

**Use this for quick local testing without hardware**

## Project Structure

```
/Users/jrsharp/src/9p4z/                    # Git repository (source of truth)
/Users/jrsharp/zephyr-workspaces/9p4z-workspace/
├── .venv/                                  # Python venv with west, zephyr deps
├── 9p4z/                                   # Module (sync from src/9p4z)
├── zephyr/                                 # Zephyr RTOS v3.7.0
├── modules/                                # Dependencies
└── build/                                  # Build output
```

## Environment Setup

Always use the workspace venv:
```bash
cd /Users/jrsharp/zephyr-workspaces/9p4z-workspace
source .venv/bin/activate
```

## Build Commands

```bash
# Build 9p_client sample
west build -b qemu_x86 9p4z/samples/9p_client

# Clean build
west build -t pristine
west build -b qemu_x86 9p4z/samples/9p_client

# Run in QEMU
west build -t run

# Run tests
west build -b qemu_x86 9p4z/tests
west build -t run
```

## 9P Server + Client Testing

```bash
# Option 1: All-in-one (auto-starts server)
9p4z/scripts/9p-run.sh --serve-dir ~/9p-test

# Option 2: Manual (two terminals)
# Terminal 1:
9p4z/scripts/9p-serve.sh ~/9p-test

# Terminal 2:
9p4z/scripts/9p-run.sh
```

## Syncing Changes

Changes in `/Users/jrsharp/src/9p4z` are the source of truth.
To sync to workspace:

```bash
# Only sync 9p4z module files (NOT the entire workspace)
rsync -av /Users/jrsharp/src/9p4z/ \
  /Users/jrsharp/zephyr-workspaces/9p4z-workspace/9p4z/ \
  --exclude build --exclude .git --exclude '.DS_Store' --exclude tests
```

## Key Files

- `src/transport_uart.c` - UART transport implementation
- `src/message.c` - 9P message builders
- `samples/9p_client/src/main.c` - Interactive client
- `include/zephyr/9p/*.h` - Public API headers
- `scripts/9p-run.sh` - QEMU runner script
- `scripts/9p-serve.sh` - 9P server script

## Common Issues

### Workspace Corrupted
```bash
cd /Users/jrsharp/zephyr-workspaces/9p4z-workspace
rm -rf * .west .venv
python3 -m venv .venv
source .venv/bin/activate
pip install west pyyaml
west init -l 9p4z
west update
pip install -r zephyr/scripts/requirements.txt
```

### Build Fails
```bash
west build -t pristine
west build -b qemu_x86 9p4z/samples/9p_client
```

## Active Development

**Status: TCP transport working on nRF7002dk! ✅**

Successfully implemented:
- ✅ 9P protocol core (message parsing, fid/tag management)
- ✅ UART transport (Phase 2)
- ✅ TCP transport (Phase 3)
- ✅ 9P server sample with WiFi on nRF7002dk
- ✅ NCS build integration (see docs/NCS-BUILD-SETUP.md)

Next steps:
- Test 9P server over WiFi/TCP with real clients
- Implement file system backends
- Add additional transports (L2CAP, Thread)
