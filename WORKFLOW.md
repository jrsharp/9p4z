# 9p4z Development Workflow

Quick reference for working with the 9p4z project in Claude Code sessions.

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

Current focus: UART transport causing unhandled interrupt crash on QEMU x86

Error signature:
```
[00:00:00.010,000] <inf> ninep_client: Transport initialized
[00:00:00.010,000] <err> os: EAX: 0x00000012, ...
[00:00:00.010,000] <err> os: >>> ZEPHYR FATAL ERROR 1: Unhandled interrupt
```

Need to investigate:
- UART device tree configuration for qemu_x86
- UART initialization in transport_uart.c
- Interrupt handler setup
