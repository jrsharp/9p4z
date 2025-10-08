#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: MIT
#
# NCS Workspace Setup for All-Transports Sample
# Sets up a clean workspace for building 9p_server_all_transports on nRF7002dk

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== 9P All-Transports NCS Workspace Setup ===${NC}\n"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
MODULE_NAME="9p4z"

# Workspace configuration
WORKSPACE_ROOT="${WORKSPACE_DIR:-$HOME/zephyr-workspaces}"
WORKSPACE_NAME="9p4z-all-transports-ncs"
WORKSPACE_FULL="$WORKSPACE_ROOT/$WORKSPACE_NAME"

# NCS configuration
NCS_ROOT="/opt/nordic/ncs/v3.1.1"
NCS_TOOLCHAIN="/opt/nordic/ncs/toolchains/561dce9adf"

echo -e "${BLUE}Source repo:${NC}         $MODULE_DIR"
echo -e "${BLUE}Workspace:${NC}           $WORKSPACE_FULL"
echo -e "${BLUE}NCS location:${NC}        $NCS_ROOT"
echo -e "${BLUE}Target board:${NC}        nRF7002dk (nrf5340/cpuapp)"
echo -e "${BLUE}Sample:${NC}              9p_server_all_transports"
echo ""

# Check prerequisites
if [ ! -d "$MODULE_DIR/.git" ]; then
    echo -e "${RED}Error: $MODULE_DIR is not a git repository${NC}"
    exit 1
fi

if [ ! -d "$NCS_ROOT" ]; then
    echo -e "${RED}Error: NCS not found at $NCS_ROOT${NC}"
    echo -e "${YELLOW}Install Nordic Connect SDK v3.1.1 first${NC}"
    exit 1
fi

if [ ! -d "$NCS_TOOLCHAIN" ]; then
    echo -e "${RED}Error: NCS toolchain not found at $NCS_TOOLCHAIN${NC}"
    exit 1
fi

echo -e "${YELLOW}This will create:${NC}"
echo -e "  $WORKSPACE_FULL/"
echo -e "  ├── $MODULE_NAME/       # Git worktree (syncs with source!)"
echo -e "  └── build/              # Build output"
echo -e "      └── 9p_server_all_transports/"
echo ""
echo -e "${GREEN}This workspace uses NCS v3.1.1 (no separate west init needed)${NC}"
echo ""
read -p "Continue? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# Create workspace root
echo -e "\n${GREEN}[1/3]${NC} Creating workspace directory..."
mkdir -p "$WORKSPACE_FULL"
cd "$WORKSPACE_FULL"

# Create git worktree
echo -e "${GREEN}[2/3]${NC} Creating git worktree..."
if [ ! -d "$MODULE_NAME/.git" ]; then
    git -C "$MODULE_DIR" worktree add "$WORKSPACE_FULL/$MODULE_NAME"
    echo "✓ Git worktree created (changes sync back to source)"
else
    echo "✓ Git worktree already exists"
fi

# Create build script
echo -e "${GREEN}[3/3]${NC} Creating build script..."
cat > build.sh << 'EOFBUILD'
#!/bin/bash
# Build script for 9p_server_all_transports on NCS

set -e

WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCS_ROOT="/opt/nordic/ncs/v3.1.1"
NCS_TOOLCHAIN="/opt/nordic/ncs/toolchains/561dce9adf"

cd "$NCS_ROOT"

export PATH="$NCS_TOOLCHAIN/bin:$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin:$PATH"
export PATH="$NCS_TOOLCHAIN/nrfutil/bin:$PATH"

echo "Building 9p_server_all_transports for nRF7002dk..."

"$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin/west" build \
  -b nrf7002dk/nrf5340/cpuapp \
  -d "$WORKSPACE_DIR/build/9p_server_all_transports" \
  "$WORKSPACE_DIR/9p4z/samples/9p_server_all_transports" \
  --pristine \
  -- -DCONF_FILE=prj_nrf.conf -DZEPHYR_EXTRA_MODULES="$WORKSPACE_DIR/9p4z"

echo ""
echo "Build complete!"
echo "Binary: $WORKSPACE_DIR/build/9p_server_all_transports/zephyr/zephyr.hex"
echo ""
echo "To flash:"
echo "  cd $WORKSPACE_DIR"
echo "  ./flash.sh"
EOFBUILD

chmod +x build.sh

# Create flash script
cat > flash.sh << 'EOFFLASH'
#!/bin/bash
# Flash script for nRF7002dk

set -e

WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCS_TOOLCHAIN="/opt/nordic/ncs/toolchains/561dce9adf"

export PATH="$NCS_TOOLCHAIN/nrfutil/bin:$PATH"

cd "$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin"

echo "Flashing nRF7002dk..."
./west flash -d "$WORKSPACE_DIR/build/9p_server_all_transports" --runner jlink

echo ""
echo "Flash complete!"
echo "Connect to serial console to see output"
EOFFLASH

chmod +x flash.sh

# Create README
cat > README.md << 'EOFREADME'
# 9P All-Transports NCS Workspace

This workspace builds the `9p_server_all_transports` sample for nRF7002dk.

## Quick Start

```bash
# Build
./build.sh

# Flash
./flash.sh

# Monitor serial console
screen /dev/ttyACM0 115200
```

## What's Built

The all-transports sample runs three 9P server instances simultaneously:

- **UART**: Serial console (conflicts with logging by default)
- **TCP**: WiFi on port 564
- **L2CAP**: Bluetooth PSM 0x0009

All three serve the same shared filesystem.

## Testing

### TCP (WiFi)
```bash
# First, connect WiFi via serial shell:
wifi connect -s "YOUR_SSID" -k 3 -p "YOUR_PASSWORD"

# Then from your computer:
9p -a tcp!<IP>!564 ls /
9p -a tcp!<IP>!564 read hello.txt
```

### L2CAP (Bluetooth)
Use the iOS 9p4i app:
1. Scan for "9P All Transports"
2. Connect
3. Open L2CAP channel on PSM 0x0009
4. Browse filesystem

## Configuration

The sample uses `prj_nrf.conf` which enables:
- WiFi (nRF70 driver)
- Bluetooth LE
- TCP networking
- L2CAP dynamic channels
- All 9P transports

## Workspace Structure

```
9p4z-all-transports-ncs/
├── 9p4z/                    # Git worktree (syncs with source)
├── build/                   # Build output
│   └── 9p_server_all_transports/
├── build.sh                 # Build script
├── flash.sh                 # Flash script
└── README.md                # This file
```

## Notes

- Changes in `9p4z/` sync back to your main repo via git
- Uses NCS v3.1.1 (no separate west workspace needed)
- Build directory is outside NCS to avoid conflicts
- Total binary size: ~700KB (all transports + WiFi + BT stacks)
EOFREADME

echo -e "\n${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║    All-Transports NCS Workspace Setup Complete! 🎉       ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}\n"

echo -e "${BLUE}Workspace location:${NC}"
echo "  $WORKSPACE_FULL"
echo ""

echo -e "${BLUE}The $MODULE_NAME/ directory is a git worktree:${NC}"
echo "  • Changes sync back to $MODULE_DIR via git"
echo "  • Commit/push/pull work normally"
echo ""

echo -e "${BLUE}To build and flash:${NC}"
echo "  cd $WORKSPACE_FULL"
echo "  ./build.sh          # Build for nRF7002dk"
echo "  ./flash.sh          # Flash via J-Link"
echo ""

echo -e "${BLUE}Next steps:${NC}"
echo "  1. Create prj_nrf.conf for all-transports sample"
echo "  2. Run ./build.sh"
echo "  3. Run ./flash.sh"
echo "  4. Connect serial console and test all transports"
echo ""

echo -e "${YELLOW}Note: You need to create prj_nrf.conf first!${NC}"
echo "  See samples/9p_server_tcp/prj_nrf.conf as reference"
echo ""

echo -e "${GREEN}✓ Ready for all-transports development on nRF7002dk!${NC}"
