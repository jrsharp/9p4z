#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0

# Start 9P file server with 9pserve

set -e

SOCKET="/tmp/9p.sock"
DIRECTORY="${1:-.}"
PORT=""
DAEMON=0

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] [DIRECTORY]

Start 9pserve with 9pex to serve a directory over 9P

Arguments:
  DIRECTORY         Directory to serve (default: current directory)

Options:
  -s SOCKET         Unix socket path (default: /tmp/9p.sock)
  -p PORT           TCP port to listen on (instead of Unix socket)
  -d                Run in background
  -h                Show this help

Examples:
  $0                    # Serve current directory on /tmp/9p.sock
  $0 ~/9p-test          # Serve ~/9p-test on /tmp/9p.sock
  $0 -p 9999 ~/data     # Serve ~/data on TCP port 9999
  $0 -d ~/9p-test       # Run in background
EOF
    exit 0
}

while getopts "s:p:dh" opt; do
    case $opt in
        s) SOCKET="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        d) DAEMON=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

shift $((OPTIND-1))
if [ -n "$1" ]; then
    DIRECTORY="$1"
fi

# Check directory exists
if [ ! -d "$DIRECTORY" ]; then
    echo "Error: Directory not found: $DIRECTORY" >&2
    exit 1
fi

# Check for required commands
if ! command -v 9pex &> /dev/null; then
    echo "Error: 9pex not found. Install plan9port (brew install plan9port)" >&2
    exit 1
fi

if ! command -v 9pserve &> /dev/null; then
    echo "Error: 9pserve not found. Install plan9port (brew install plan9port)" >&2
    exit 1
fi

# Build address
if [ -n "$PORT" ]; then
    ADDR="tcp!*!$PORT"
    echo "Starting 9P server on TCP port $PORT..."
else
    ADDR="unix!$SOCKET"
    # Remove existing socket
    rm -f "$SOCKET"
    echo "Starting 9P server on Unix socket $SOCKET..."
fi

# Build and run command
CMD="9pex $(realpath "$DIRECTORY") | 9pserve $ADDR"

echo "Serving: $DIRECTORY"
echo "Command: $CMD"

if [ "$DAEMON" -eq 1 ]; then
    eval "$CMD" &
    sleep 0.5
    echo "Server started in background (PID: $!)"
    [ -z "$PORT" ] && echo "Socket: $SOCKET"
else
    echo "Press Ctrl+C to stop server"
    eval "$CMD"
fi
