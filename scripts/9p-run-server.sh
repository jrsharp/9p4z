#!/bin/bash
# Run 9P Server sample in QEMU and expose it for client connections

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Configuration
TCP_PORT="${TCP_PORT:-9998}"
MEMORY="${MEMORY:-32M}"
KERNEL="${KERNEL:-build/zephyr/zephyr.elf}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Starting 9P Server on TCP port ${TCP_PORT}...${NC}"

# Change to workspace directory
cd "$WORKSPACE_DIR"

# Check if kernel exists
if [ ! -f "$KERNEL" ]; then
    echo -e "${RED}Error: Kernel not found at $KERNEL${NC}"
    echo "Build the server first:"
    echo "  west build -b qemu_x86 9p4z/samples/9p_server"
    exit 1
fi

# Cleanup function
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    # Kill QEMU
    pkill -f "qemu-system-i386.*9p.*server" 2>/dev/null || true
    exit 0
}

trap cleanup INT TERM

echo "9P Server Configuration:"
echo "  TCP Port: $TCP_PORT"
echo "  Kernel: $KERNEL"
echo ""
echo -e "${GREEN}Connect with 9P client using:${NC}"
echo "  9p -a 'tcp!localhost!${TCP_PORT}' ls /"
echo "  9p -a 'tcp!localhost!${TCP_PORT}' read /hello.txt"
echo "  9p -a 'tcp!localhost!${TCP_PORT}' read /readme.txt"
echo ""
echo -e "${YELLOW}Starting QEMU...${NC}"
echo ""

# Run QEMU with 9P server
# - Serial 0: Console output (chardev for console + QEMU monitor)
# - Serial 1: 9P transport (exposed via TCP)
exec qemu-system-i386 \
    -m "$MEMORY" \
    -cpu qemu32,+nx,+pae \
    -machine q35 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -no-reboot \
    -nographic \
    -machine acpi=off \
    -net none \
    -icount shift=5,align=off,sleep=off \
    -rtc clock=vm \
    -chardev stdio,id=con,mux=on \
    -serial chardev:con \
    -mon chardev=con,mode=readline \
    -serial "tcp:localhost:${TCP_PORT},server,nowait" \
    -kernel "$KERNEL"
