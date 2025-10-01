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

# Do NOT cd into workspace yet - we need to init from parent

# Create Python virtual environment IN workspace
echo -e "${GREEN}[2/6]${NC} Creating Python virtual environment..."
if [ ! -d "$WORKSPACE_DIR/.venv" ]; then
    python3 -m venv "$WORKSPACE_DIR/.venv"
    echo "✓ Virtual environment created"
else
    echo "✓ Virtual environment already exists"
fi

# Activate virtual environment
echo -e "${GREEN}[3/6]${NC} Activating virtual environment..."
source "$WORKSPACE_DIR/.venv/bin/activate"

# Upgrade pip
echo -e "${GREEN}[4/6]${NC} Installing/upgrading Python tools..."
pip install --quiet --upgrade pip
pip install --quiet west

# Check Python package versions
echo "✓ Installed packages:"
pip list | grep -E "^(west|pip) "

# Initialize west workspace
echo -e "\n${GREEN}[5/6]${NC} Initializing west workspace..."

# Check for conflicting west workspaces in parent directories
CONFLICT_CHECK="$WORKSPACE_DIR"
while [ "$CONFLICT_CHECK" != "/" ]; do
    CONFLICT_CHECK="$(dirname "$CONFLICT_CHECK")"
    if [ -d "$CONFLICT_CHECK/.west" ]; then
        echo -e "${RED}Error: Found existing west workspace at: $CONFLICT_CHECK/.west${NC}"
        echo ""
        echo "You have a west workspace in a parent directory that conflicts."
        echo "This will prevent creating an isolated workspace here."
        echo ""
        echo "Options:"
        echo "  1. Use a different workspace location (set WORKSPACE_DIR env var)"
        echo "     Example: WORKSPACE_DIR=~/zephyr-workspaces/9p4z-workspace ./scripts/setup-workspace.sh"
        echo "  2. Remove the conflicting workspace:"
        echo "     rm -rf $CONFLICT_CHECK/.west"
        echo ""
        exit 1
    fi
done

if [ ! -d "$WORKSPACE_DIR/.west" ]; then
    # Use local path if module is already cloned
    if [ -d "$MODULE_DIR/.git" ]; then
        echo "Using local module at: $MODULE_DIR"
        echo "Workspace directory: $WORKSPACE_DIR"

        # Create symlink to module inside workspace
        # This follows the pattern: workspace/module-name/ -> actual module location
        MODULE_LINK="$WORKSPACE_DIR/$(basename "$MODULE_DIR")"
        if [ ! -e "$MODULE_LINK" ]; then
            ln -s "$MODULE_DIR" "$MODULE_LINK"
            echo "✓ Created symlink: $MODULE_LINK -> $MODULE_DIR"
        fi

        # Now run west init FROM workspace directory WITH -l pointing to the linked module
        # This creates .west/ in WORKSPACE_DIR, not in MODULE_DIR
        cd "$WORKSPACE_DIR"
        west init -l "$(basename "$MODULE_DIR")"
    else
        echo -e "${YELLOW}Note: For remote setup, use:${NC}"
        echo "  west init -m https://github.com/YOUR_USERNAME/9p4z --mr main"
        exit 1
    fi
    echo "✓ West workspace initialized"
else
    echo "✓ West workspace already initialized"
    cd "$WORKSPACE_DIR"
fi

# Update all projects (pull Zephyr and dependencies)
echo -e "\n${GREEN}[6/6]${NC} Updating west projects (this may take a while)..."
cd "$WORKSPACE_DIR"
west update

# Export Zephyr environment variables
echo -e "\n${GREEN}Setting up Zephyr environment...${NC}"
cd "$WORKSPACE_DIR"
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