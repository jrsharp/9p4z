#!/bin/bash
# Copyright (c) 2025 9p4z Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Clean workspace setup using git worktree
# This creates a separate git worktree in a clean location
# Changes sync back to your main repo via git

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== 9P for Zephyr - Clean Workspace Setup ===${NC}\n"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
MODULE_NAME="$(basename "$MODULE_DIR")"

# Default to ~/zephyr-workspaces/9p4z
WORKSPACE_ROOT="${WORKSPACE_DIR:-$HOME/zephyr-workspaces}"
WORKSPACE_NAME="${WORKSPACE_NAME:-9p4z}"
WORKSPACE_FULL="$WORKSPACE_ROOT/$WORKSPACE_NAME"

echo -e "${BLUE}Source repo:${NC}         $MODULE_DIR"
echo -e "${BLUE}Workspace:${NC}           $WORKSPACE_FULL"
echo ""

if [ ! -d "$MODULE_DIR/.git" ]; then
    echo -e "${RED}Error: $MODULE_DIR is not a git repository${NC}"
    exit 1
fi

echo -e "${YELLOW}This will create:${NC}"
echo -e "  $WORKSPACE_FULL/"
echo -e "  ├── .venv/              # Python environment"
echo -e "  ├── .west/              # West workspace"
echo -e "  ├── $MODULE_NAME/       # Git worktree (syncs with source!)"
echo -e "  ├── zephyr/             # Zephyr RTOS"
echo -e "  └── modules/            # Dependencies"
echo ""
echo -e "${GREEN}Changes in $WORKSPACE_FULL/$MODULE_NAME sync via git!${NC}"
echo ""
read -p "Continue? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# Create workspace root
echo -e "\n${GREEN}[1/6]${NC} Creating workspace directory..."
mkdir -p "$WORKSPACE_FULL"
cd "$WORKSPACE_FULL"

# Create git worktree (this is the magic!)
echo -e "${GREEN}[2/6]${NC} Creating git worktree..."
if [ ! -d "$MODULE_NAME/.git" ]; then
    git -C "$MODULE_DIR" worktree add "$WORKSPACE_FULL/$MODULE_NAME"
    echo "✓ Git worktree created (changes sync back to source)"
else
    echo "✓ Git worktree already exists"
fi

# Create Python venv
echo -e "${GREEN}[3/6]${NC} Creating Python virtual environment..."
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
    echo "✓ Virtual environment created"
fi

source .venv/bin/activate

# Install tools
echo -e "${GREEN}[4/6]${NC} Installing Python tools..."
pip install --quiet --upgrade pip
pip install --quiet west
echo "✓ west $(west --version)"

# Initialize workspace
echo -e "\n${GREEN}[5/6]${NC} Initializing west workspace..."
if [ ! -d ".west" ]; then
    west init -l "$MODULE_NAME"
    echo "✓ West workspace initialized"
fi

# Update projects
echo -e "\n${GREEN}[6/6]${NC} Updating west projects (this takes a while)..."
west update

# Install Zephyr requirements
if [ -f "zephyr/scripts/requirements.txt" ]; then
    pip install --quiet -r zephyr/scripts/requirements.txt
fi

source zephyr/zephyr-env.sh

# Create activation script
cat > activate.sh << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/.venv/bin/activate"
source "$SCRIPT_DIR/zephyr/zephyr-env.sh" 2>/dev/null || true
echo "✓ 9p4z workspace activated"
echo "  Location: $SCRIPT_DIR"
EOF
chmod +x activate.sh

echo -e "\n${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║        Clean Workspace Setup Complete! 🎉                ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}\n"

echo -e "${BLUE}Workspace location:${NC}"
echo "  $WORKSPACE_FULL"
echo ""

echo -e "${BLUE}The $MODULE_NAME/ directory is a git worktree:${NC}"
echo "  • Changes sync back to $MODULE_DIR via git"
echo "  • Commit/push/pull work normally"
echo "  • Completely separate from ~/src"
echo ""

echo -e "${BLUE}To use this workspace:${NC}"
echo "  cd $WORKSPACE_FULL"
echo "  source activate.sh"
echo "  west build -b native_posix $MODULE_NAME/tests"
echo ""

echo -e "${BLUE}To remove this workspace later:${NC}"
echo "  git -C $MODULE_DIR worktree remove $WORKSPACE_FULL/$MODULE_NAME"
echo "  rm -rf $WORKSPACE_FULL"
echo ""

echo -e "${GREEN}✓ Ready for clean, isolated development!${NC}"
