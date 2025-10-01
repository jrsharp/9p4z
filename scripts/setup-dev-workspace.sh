#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Simple in-place workspace setup for local development
# This is the recommended approach for working on 9p4z itself
#
# Creates workspace structure:
#   /path/to/parent-dir/
#   ├── .west/               # workspace marker
#   ├── .venv/               # Python virtual environment
#   ├── 9p4z/                # your module (you're here)
#   ├── zephyr/              # Zephyr RTOS
#   └── modules/             # dependencies

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== 9P for Zephyr - In-Place Development Setup ===${NC}\n"

# Determine locations
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
PARENT_DIR="$(dirname "$MODULE_DIR")"
MODULE_NAME="$(basename "$MODULE_DIR")"

echo -e "${BLUE}Module location:${NC}     $MODULE_DIR"
echo -e "${BLUE}Workspace root:${NC}      $PARENT_DIR"
echo ""

# Check if module is a git repo
if [ ! -d "$MODULE_DIR/.git" ]; then
    echo -e "${RED}Error: $MODULE_DIR is not a git repository${NC}"
    exit 1
fi

# Check for existing workspace
if [ -d "$PARENT_DIR/.west" ]; then
    echo -e "${YELLOW}Warning: West workspace already exists at $PARENT_DIR/.west${NC}"
    read -p "Reinitialize? This will reset west configuration. (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$PARENT_DIR/.west"
    else
        echo "Keeping existing workspace. Running update only."
        cd "$PARENT_DIR"
        source .venv/bin/activate 2>/dev/null || true
        west update
        exit 0
    fi
fi

echo -e "${YELLOW}This will set up a Zephyr workspace in:${NC}"
echo -e "  $PARENT_DIR"
echo ""
echo -e "${YELLOW}After setup, the structure will be:${NC}"
echo -e "  $PARENT_DIR/"
echo -e "  ├── .west/          # workspace metadata"
echo -e "  ├── .venv/          # Python environment"
echo -e "  ├── $MODULE_NAME/   # your module (current location)"
echo -e "  ├── zephyr/         # Zephyr RTOS"
echo -e "  └── modules/        # dependencies"
echo ""
read -p "Continue? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

cd "$PARENT_DIR"

# Create Python virtual environment
echo -e "\n${GREEN}[1/5]${NC} Creating Python virtual environment..."
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
    echo "✓ Virtual environment created"
else
    echo "✓ Virtual environment already exists"
fi

# Activate virtual environment
echo -e "${GREEN}[2/5]${NC} Activating virtual environment..."
source .venv/bin/activate

# Install/upgrade tools
echo -e "${GREEN}[3/5]${NC} Installing Python tools..."
pip install --quiet --upgrade pip
pip install --quiet west

echo "✓ Installed packages:"
pip list | grep -E "^(west|pip) "

# Initialize west workspace
echo -e "\n${GREEN}[4/5]${NC} Initializing west workspace..."
echo "Current directory: $(pwd)"
echo "Manifest repo: $MODULE_NAME"

west init -l "$MODULE_NAME"
echo "✓ West workspace initialized"

# Update all projects
echo -e "\n${GREEN}[5/5]${NC} Updating west projects (this may take a while)..."
west update

# Install Zephyr requirements
echo -e "\n${GREEN}Installing Zephyr Python requirements...${NC}"
if [ -f "zephyr/scripts/requirements.txt" ]; then
    pip install --quiet -r zephyr/scripts/requirements.txt
    echo "✓ Python requirements installed"
fi

# Source Zephyr environment
if [ -f "zephyr/zephyr-env.sh" ]; then
    source zephyr/zephyr-env.sh
    echo "✓ Zephyr environment sourced"
fi

# Create activation script
cat > activate.sh << 'EOF'
#!/bin/bash
# Activate 9p4z development environment
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/.venv/bin/activate"
source "$SCRIPT_DIR/zephyr/zephyr-env.sh"
echo "✓ 9p4z workspace activated"
echo "  Workspace: $SCRIPT_DIR"
echo "  Module: $SCRIPT_DIR/9p4z"
EOF
chmod +x activate.sh

# Summary
echo -e "\n${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║       Development Workspace Setup Complete! 🎉          ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}\n"

echo -e "${BLUE}Workspace structure:${NC}"
ls -la | grep -E "^d|^l" | awk '{print "  " $9}' | grep -v "^\.$" | grep -v "^\.\.$"
echo ""

echo -e "${BLUE}To activate in a new shell:${NC}"
echo "  cd $PARENT_DIR"
echo "  source activate.sh"
echo ""

echo -e "${BLUE}Quick test:${NC}"
echo "  cd $PARENT_DIR"
echo "  west build -b native_posix $MODULE_NAME/tests"
echo "  west build -t run"
echo ""

echo -e "${GREEN}✓ Ready for development!${NC}"
