#!/bin/bash
# Setup clean NCS workspace for 9p4z L2CAP development
# This creates a minimal workspace that properly integrates with Nordic NCS

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}=== 9p4z NCS Workspace Setup ===${NC}\n"

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
WORKSPACE_DIR="${HOME}/zephyr-workspaces/9p4z-l2cap-ncs"
NCS_ROOT="/opt/nordic/ncs/v3.1.1"
NCS_TOOLCHAIN="/opt/nordic/ncs/toolchains/561dce9adf"

echo -e "${BLUE}Configuration:${NC}"
echo "  Source module: $MODULE_DIR"
echo "  Workspace:     $WORKSPACE_DIR"
echo "  NCS version:   v3.1.1"
echo ""

# Check prerequisites
if [ ! -d "$NCS_ROOT" ]; then
    echo -e "${RED}Error: NCS not found at $NCS_ROOT${NC}"
    exit 1
fi

if [ ! -d "$NCS_TOOLCHAIN" ]; then
    echo -e "${RED}Error: NCS toolchain not found at $NCS_TOOLCHAIN${NC}"
    exit 1
fi

# Remove old workspace if it exists
if [ -d "$WORKSPACE_DIR" ]; then
    echo -e "${YELLOW}Removing old workspace...${NC}"
    rm -rf "$WORKSPACE_DIR"
fi

# Create workspace
echo -e "${GREEN}[1/5]${NC} Creating workspace directory..."
mkdir -p "$WORKSPACE_DIR"

# Create manifest directory
echo -e "${GREEN}[2/5]${NC} Setting up west manifest..."
mkdir -p "$WORKSPACE_DIR/manifest"

# Create west.yml manifest
cat > "$WORKSPACE_DIR/manifest/west.yml" << 'EOF'
manifest:
  version: "0.13"

  self:
    path: manifest

  projects:
    # Use local 9p4z module
    - name: 9p4z
      path: 9p4z
      url: file:///Users/jrsharp/src/9p4z
      revision: HEAD

  # All NCS modules are symlinked, not fetched
EOF

echo "✓ Created west.yml"

# Initialize west workspace
echo -e "${GREEN}[3/5]${NC} Initializing west workspace..."
cd "$WORKSPACE_DIR"
export PATH="$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin:$PATH"

# Initialize west pointing to manifest directory
"$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin/west" init -l manifest/

echo "✓ West initialized"

# Create symlinks to NCS installation (avoid downloading gigabytes)
echo -e "${GREEN}[4/5]${NC} Creating symlinks to NCS installation..."

ln -s "$NCS_ROOT/zephyr" "$WORKSPACE_DIR/zephyr"
ln -s "$NCS_ROOT/nrf" "$WORKSPACE_DIR/nrf"
ln -s "$NCS_ROOT/nrfxlib" "$WORKSPACE_DIR/nrfxlib"
ln -s "$NCS_ROOT/modules" "$WORKSPACE_DIR/modules"
ln -s "$NCS_ROOT/bootloader" "$WORKSPACE_DIR/bootloader"
ln -s "$NCS_ROOT/tools" "$WORKSPACE_DIR/tools"
ln -s "$NCS_ROOT/test" "$WORKSPACE_DIR/test"

echo "✓ Symlinks created"

# Update west to fetch 9p4z
echo -e "${GREEN}[5/5]${NC} Updating west projects..."
cd "$WORKSPACE_DIR"
"$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin/west" update

echo "✓ Projects updated"

# Create activation script
echo -e "${GREEN}Creating activation script...${NC}"
cat > "$WORKSPACE_DIR/activate.sh" << 'ACTIVATE_EOF'
#!/bin/bash
# Activate 9p4z NCS development environment

export PATH="/opt/nordic/ncs/toolchains/561dce9adf/bin:$PATH"
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin:$PATH"
export PATH="/opt/nordic/ncs/toolchains/561dce9adf/nrfutil/bin:$PATH"

export ZEPHYR_BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/zephyr" && pwd)"

echo "✓ NCS environment activated"
echo "  ZEPHYR_BASE: $ZEPHYR_BASE"
echo "  Workspace:   $(pwd)"
echo ""
echo "Build L2CAP sample:"
echo "  cd 9p4z/samples/9p_server_l2cap"
echo "  ./build-ncs.sh"
ACTIVATE_EOF

chmod +x "$WORKSPACE_DIR/activate.sh"

# Summary
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║          Workspace Setup Complete! 🎉                   ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${BLUE}Workspace structure:${NC}"
echo "  $WORKSPACE_DIR/"
echo "  ├── manifest/           # West configuration"
echo "  ├── 9p4z/              # Your module (from source)"
echo "  ├── zephyr/            # NCS Zephyr (symlink)"
echo "  ├── nrf/               # Nordic SDK (symlink)"
echo "  └── modules/           # Dependencies (symlink)"
echo ""
echo -e "${BLUE}To use this workspace:${NC}"
echo "  cd $WORKSPACE_DIR"
echo "  source activate.sh"
echo "  cd 9p4z/samples/9p_server_l2cap"
echo "  ./build-ncs.sh"
echo ""
echo -e "${YELLOW}Note: Changes in /Users/jrsharp/src/9p4z will be reflected${NC}"
echo -e "${YELLOW}      in the workspace via the symlink!${NC}"
echo ""
