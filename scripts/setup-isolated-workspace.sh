#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Create completely isolated workspace by COPYING module
# Use this when you want workspace completely separate from source
#
# NOTE: Changes in workspace/9p4z are NOT reflected back to source!
# This is for testing/CI, not active development.

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== 9P for Zephyr - Isolated Workspace Setup ===${NC}\n"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
MODULE_NAME="$(basename "$MODULE_DIR")"

# Use provided WORKSPACE_DIR or default
WORKSPACE_DIR="${WORKSPACE_DIR:-$HOME/zephyr-workspaces/9p4z-workspace}"

echo -e "${BLUE}Source module:${NC}       $MODULE_DIR"
echo -e "${BLUE}Workspace:${NC}           $WORKSPACE_DIR"
echo ""

if [ ! -d "$MODULE_DIR/.git" ]; then
    echo -e "${RED}Error: $MODULE_DIR is not a git repository${NC}"
    exit 1
fi

echo -e "${YELLOW}WARNING: This creates an ISOLATED copy of your module.${NC}"
echo -e "${YELLOW}Changes made in $WORKSPACE_DIR/$MODULE_NAME${NC}"
echo -e "${YELLOW}will NOT be reflected in $MODULE_DIR${NC}"
echo ""
echo -e "${BLUE}For active development, use ./scripts/setup-dev-workspace.sh instead${NC}"
echo ""
read -p "Continue with isolated workspace? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# Create workspace
echo -e "\n${GREEN}[1/6]${NC} Creating workspace directory..."
mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"

# Copy module to workspace
echo -e "${GREEN}[2/6]${NC} Copying module to workspace..."
if [ ! -d "$MODULE_NAME" ]; then
    cp -r "$MODULE_DIR" "$MODULE_NAME"
    # Remove .git to avoid confusion
    rm -rf "$MODULE_NAME/.git"
    echo "✓ Module copied (without .git)"
else
    echo "✓ Module already exists"
fi

# Create Python venv
echo -e "${GREEN}[3/6]${NC} Creating Python virtual environment..."
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
    echo "✓ Virtual environment created"
else
    echo "✓ Virtual environment exists"
fi

source .venv/bin/activate

# Install tools
echo -e "${GREEN}[4/6]${NC} Installing Python tools..."
pip install --quiet --upgrade pip
pip install --quiet west
echo "✓ Installed: $(pip list | grep -E '^(west|pip) ' | tr '\n' ' ')"

# Initialize workspace
echo -e "\n${GREEN}[5/6]${NC} Initializing west workspace..."
if [ ! -d ".west" ]; then
    west init -l "$MODULE_NAME"
    echo "✓ West workspace initialized"
else
    echo "✓ Workspace already initialized"
fi

# Update projects
echo -e "\n${GREEN}[6/6]${NC} Updating west projects..."
west update

# Install Zephyr deps
if [ -f "zephyr/scripts/requirements.txt" ]; then
    pip install --quiet -r zephyr/scripts/requirements.txt
fi

if [ -f "zephyr/zephyr-env.sh" ]; then
    source zephyr/zephyr-env.sh
fi

# Create activation script
cat > activate.sh << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/.venv/bin/activate"
source "$SCRIPT_DIR/zephyr/zephyr-env.sh"
echo "✓ Isolated workspace activated"
EOF
chmod +x activate.sh

echo -e "\n${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║      Isolated Workspace Setup Complete! 🎉              ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}\n"

echo -e "${BLUE}Workspace: $WORKSPACE_DIR${NC}"
echo -e "${BLUE}To activate:${NC} cd $WORKSPACE_DIR && source activate.sh"
echo -e "${BLUE}To test:${NC}     west build -b native_posix $MODULE_NAME/tests"
echo ""
echo -e "${YELLOW}Remember: This is a COPY. Changes here don't affect source!${NC}"
