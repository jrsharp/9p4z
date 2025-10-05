#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0

# Run QEMU with 9P client connected to 9pserve

set -e

TCP_PORT="9999"
BOARD="qemu_x86"
BUILD_DIR="build"
MEMORY=32
SERVE_DIR=""

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Run QEMU with 9P client sample connected to 9pserve

Options:
  -p PORT           TCP port for 9P server (default: 9999)
  -b BOARD          Board to run (default: qemu_x86)
  -d BUILD_DIR      Build directory (default: build)
  -m MEMORY         QEMU memory in MB (default: 32)
  --serve-dir DIR   Auto-start 9pserve on this directory
  -h                Show this help

Examples:
  $0                           # Connect to server on TCP port 9999
  $0 --serve-dir ~/9p-test     # Auto-start server and connect
  $0 -p 5555                   # Use custom TCP port
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -p) TCP_PORT="$2"; shift 2 ;;
        -b) BOARD="$2"; shift 2 ;;
        -d) BUILD_DIR="$2"; shift 2 ;;
        -m) MEMORY="$2"; shift 2 ;;
        --serve-dir) SERVE_DIR="$2"; shift 2 ;;
        -h) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# Auto-start server if requested
SOCAT_PID=""
if [ -n "$SERVE_DIR" ]; then
    if [ ! -d "$SERVE_DIR" ]; then
        echo "Error: Serve directory not found: $SERVE_DIR" >&2
        exit 1
    fi

    # Start socat TCP bridge to 9pex
    echo "Starting 9P server bridge on TCP port $TCP_PORT for $SERVE_DIR..."
    socat "TCP-LISTEN:$TCP_PORT,reuseaddr,fork" \
        "EXEC:9pex $(realpath "$SERVE_DIR")" &
    SOCAT_PID=$!

    # Wait for port to be listening
    echo "Waiting for TCP port to be ready..."
    for i in {1..20}; do
        if nc -z localhost "$TCP_PORT" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done

    echo "Server bridge started (PID: $SOCAT_PID)"
fi

# Cleanup function
cleanup() {
    if [ -n "$SOCAT_PID" ]; then
        echo "Stopping 9P server bridge..."
        kill $SOCAT_PID 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Check server is running
if ! nc -z localhost "$TCP_PORT" 2>/dev/null; then
    echo "Error: 9P server not listening on TCP port $TCP_PORT" >&2
    echo "Start server first with socat or run with --serve-dir" >&2
    exit 1
fi

# Find kernel
KERNEL="$BUILD_DIR/zephyr/zephyr.elf"
if [ ! -f "$KERNEL" ]; then
    echo "Error: Kernel not found: $KERNEL" >&2
    echo "Build first with: west build -b $BOARD 9p4z/samples/9p_client" >&2
    exit 1
fi

# Run QEMU
echo "Running QEMU with 9P client..."
echo "TCP Port: $TCP_PORT"
echo "Kernel: $KERNEL"
echo ""

qemu-system-i386 \
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
    -serial "tcp:localhost:$TCP_PORT" \
    -kernel "$KERNEL"
