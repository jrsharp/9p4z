#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Clean workspace setup script for 9p4z Zephyr module development
#
# This script follows Zephyr best practices for T2 topology:
# - Creates a workspace directory OUTSIDE the module
# - Sets up isolated Python virtual environment
# - Installs west and dependencies
# - Initializes west workspace with 9p4z as manifest repo
# - Pulls Zephyr and required dependencies

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== 9P for Zephyr - Clean Workspace Setup ===${NC}\n"

# Determine script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
MODULE_NAME="$(basename "$MODULE_DIR")"

# Default workspace location (sibling to module)
WORKSPACE_DIR="${WORKSPACE_DIR:-$(dirname "$MODULE_DIR")/${MODULE_NAME}-workspace}"

echo -e "${BLUE}Module directory:${NC}    $MODULE_DIR"
echo -e "${BLUE}Workspace directory:${NC} $WORKSPACE_DIR"
echo ""

# Check if module is a git repo
if [ ! -d "$MODULE_DIR/.git" ]; then
    echo -e "${RED}Error: $MODULE_DIR is not a git repository${NC}"
    echo "Please run this script from within the 9p4z git repository"
    exit 1
fi

# Ask for confirmation
echo -e "${YELLOW}This will create a new Zephyr workspace at:${NC}"
echo -e "  ${WORKSPACE_DIR}"
echo ""
read -p "Continue? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# Create workspace directory
echo -e "\n${GREEN}[1/6]${NC} Creating workspace directory..."
mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"

# Create Python virtual environment
echo -e "${GREEN}[2/6]${NC} Creating Python virtual environment..."
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
    echo "✓ Virtual environment created"
else
    echo "✓ Virtual environment already exists"
fi

# Activate virtual environment
echo -e "${GREEN}[3/6]${NC} Activating virtual environment..."
source .venv/bin/activate

# Upgrade pip
echo -e "${GREEN}[4/6]${NC} Installing/upgrading Python tools..."
pip install --quiet --upgrade pip
pip install --quiet west

# Check Python package versions
echo "✓ Installed packages:"
pip list | grep -E "^(west|pip) "

# Initialize west workspace
echo -e "\n${GREEN}[5/6]${NC} Initializing west workspace..."
if [ ! -d ".west" ]; then
    # Use local path if module is already cloned, otherwise use git URL
    if [ -d "$MODULE_DIR/.git" ]; then
        echo "Using local module at: $MODULE_DIR"
        west init -l "$MODULE_DIR"
    else
        echo -e "${YELLOW}Note: For remote setup, use:${NC}"
        echo "  west init -m https://github.com/YOUR_USERNAME/9p4z --mr main"
        exit 1
    fi
    echo "✓ West workspace initialized"
else
    echo "✓ West workspace already initialized"
fi

# Update all projects (pull Zephyr and dependencies)
echo -e "\n${GREEN}[6/6]${NC} Updating west projects (this may take a while)..."
west update

# Export Zephyr environment variables
echo -e "\n${GREEN}Setting up Zephyr environment...${NC}"
if [ -f "zephyr/zephyr-env.sh" ]; then
    source zephyr/zephyr-env.sh
    echo "✓ Zephyr environment sourced"
fi

# Install Python dependencies
echo -e "\n${GREEN}Installing Zephyr Python requirements...${NC}"
if [ -f "zephyr/scripts/requirements.txt" ]; then
    pip install --quiet -r zephyr/scripts/requirements.txt
    echo "✓ Python requirements installed"
fi

# Summary
echo -e "\n${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║          Workspace Setup Complete! 🎉                   ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}\n"

echo -e "${BLUE}Workspace structure:${NC}"
echo "  $WORKSPACE_DIR/"
echo "  ├── .venv/              # Python virtual environment"
echo "  ├── .west/              # West workspace metadata"
echo "  ├── 9p4z/               # Your module (manifest repo)"
echo "  ├── zephyr/             # Zephyr RTOS"
echo "  └── modules/            # Zephyr dependencies"
echo ""

echo -e "${BLUE}To activate this workspace in a new shell:${NC}"
echo "  cd $WORKSPACE_DIR"
echo "  source .venv/bin/activate"
echo "  source zephyr/zephyr-env.sh"
echo ""

echo -e "${BLUE}Quick test:${NC}"
echo "  cd $WORKSPACE_DIR"
echo "  west build -b native_posix 9p4z/tests"
echo "  west build -t run"
echo ""

echo -e "${BLUE}Run sample:${NC}"
echo "  west build -b native_posix 9p4z/samples/uart_echo"
echo ""

# Create convenience activation script
echo -e "${GREEN}Creating activation helper script...${NC}"
cat > activate.sh << 'EOF'
#!/bin/bash
# Activate 9p4z development environment
source .venv/bin/activate
source zephyr/zephyr-env.sh
echo "✓ 9p4z workspace activated"
echo "Working directory: $(pwd)"
EOF
chmod +x activate.sh
echo "✓ Created activate.sh"

echo -e "\n${YELLOW}Next time, simply run:${NC} source $WORKSPACE_DIR/activate.sh"
echo ""