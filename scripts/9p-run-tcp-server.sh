#!/bin/bash
# Run 9P TCP Server sample in QEMU with networking
# The server listens on port 564 inside the guest (192.0.2.1)
# QEMU forwards host port 9564 to guest port 564

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Configuration
HOST_PORT="${HOST_PORT:-9564}"
GUEST_PORT="564"
GUEST_IP="192.0.2.1"
MEMORY="${MEMORY:-32M}"
KERNEL="${KERNEL:-build/zephyr/zephyr.elf}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Starting 9P TCP Server...${NC}"

# Change to workspace directory
cd "$WORKSPACE_DIR"

# Check if kernel exists
if [ ! -f "$KERNEL" ]; then
    echo -e "${RED}Error: Kernel not found at $KERNEL${NC}"
    echo "Build the TCP server first:"
    echo "  west build -b qemu_x86 9p4z/samples/9p_server_tcp"
    exit 1
fi

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    # Kill QEMU
    pkill -f "qemu-system-i386.*9p.*tcp" 2>/dev/null || true
    exit 0
}

trap cleanup INT TERM

echo "9P TCP Server Configuration:"
echo "  Guest IP:   $GUEST_IP"
echo "  Guest Port: $GUEST_PORT (9P standard port)"
echo "  Host Port:  $HOST_PORT (forwarded from guest)"
echo "  Kernel:     $KERNEL"
echo ""
echo -e "${GREEN}Connect with 9P client using:${NC}"
echo "  9p -a 'tcp!localhost!${HOST_PORT}' ls /"
echo "  9p -a 'tcp!localhost!${HOST_PORT}' read /hello.txt"
echo "  9p -a 'tcp!localhost!${HOST_PORT}' read /readme.txt"
echo ""
echo -e "${YELLOW}Starting QEMU with networking...${NC}"
echo ""

# Run QEMU with networking
# - Serial 0: Console output
# - Network: e1000 NIC with user-mode networking
# - Port forwarding: host:9564 -> guest:564
exec qemu-system-i386 \
    -m "$MEMORY" \
    -cpu qemu32,+nx,+pae \
    -machine q35 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot \
    -nographic \
    -machine acpi=off \
    -icount shift=5,align=off,sleep=off \
    -rtc clock=vm \
    -chardev stdio,id=con,mux=on \
    -serial chardev:con \
    -mon chardev=con,mode=readline \
    -netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:${GUEST_PORT} \
    -device e1000,netdev=net0,romfile="" \
    -kernel "$KERNEL"
