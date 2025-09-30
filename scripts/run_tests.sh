#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== 9P for Zephyr Test Runner ===${NC}\n"

# Parse command line arguments
PLATFORM="${1:-native_posix}"
VERBOSE="${2:-}"

echo "Platform: $PLATFORM"
echo "Tests directory: $PROJECT_ROOT/tests"
echo ""

# Check if west is available
if ! command -v west &> /dev/null; then
    echo -e "${RED}Error: 'west' command not found${NC}"
    echo "Please install Zephyr SDK and west tool"
    exit 1
fi

# Run twister with the specified platform
echo -e "${YELLOW}Running tests on $PLATFORM...${NC}"

cd "$PROJECT_ROOT"

TWISTER_ARGS="-p $PLATFORM -T tests/ --inline-logs"

if [ "$VERBOSE" = "-v" ] || [ "$VERBOSE" = "--verbose" ]; then
    TWISTER_ARGS="$TWISTER_ARGS -v"
fi

if west twister $TWISTER_ARGS; then
    echo -e "\n${GREEN}✓ All tests passed!${NC}"
    exit 0
else
    echo -e "\n${RED}✗ Some tests failed${NC}"
    exit 1
fi