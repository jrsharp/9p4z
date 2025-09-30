# 9P for Zephyr - Quick Start Guide

This guide will help you set up a clean Zephyr development workspace for 9p4z following best practices.

## Prerequisites

- **Python 3.8+** (with venv support)
- **Git**
- **CMake 3.20+**
- **Ninja or Make**
- **Device tree compiler (dtc)**

### Platform-Specific Prerequisites

**macOS:**
```bash
brew install cmake ninja dtc
```

**Ubuntu/Debian:**
```bash
sudo apt install --no-install-recommends git cmake ninja-build gperf \
  ccache dfu-util device-tree-compiler wget \
  python3-dev python3-pip python3-setuptools python3-tk python3-wheel \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
```

**Fedora:**
```bash
sudo dnf install git cmake ninja-build gperf ccache dfu-util dtc wget \
  python3-pip python3-tkinter xz file glibc-devel.i686 libstdc++-devel.i686 \
  SDL2-devel file-libs
```

## One-Command Setup

From within the 9p4z repository:

```bash
./scripts/setup-workspace.sh
```

This script will:
1. ✅ Create a clean workspace directory (outside the module)
2. ✅ Set up an isolated Python virtual environment
3. ✅ Install west and dependencies
4. ✅ Initialize west workspace (T2 topology)
5. ✅ Pull Zephyr v3.7.0 and required modules
6. ✅ Install Python requirements
7. ✅ Create an activation helper script

## Workspace Structure

After setup, your workspace will look like this:

```
9p4z-workspace/              # Workspace root
├── .venv/                   # Python virtual environment (isolated)
├── .west/                   # West workspace metadata
├── activate.sh              # Convenience activation script
├── 9p4z/                    # This module (manifest repository)
│   ├── west.yml            # West manifest
│   ├── zephyr/
│   │   └── module.yml      # Module definition
│   ├── include/            # Public headers
│   ├── src/                # Implementation
│   ├── tests/              # Test suites
│   └── samples/            # Sample applications
├── zephyr/                  # Zephyr RTOS
└── modules/                 # Zephyr dependencies
    ├── hal/
    ├── lib/
    └── ...
```

### Why This Structure?

This follows **Zephyr T2 Topology** (star with application/module as manifest):
- ✅ **Clean separation**: Your module is separate from Zephyr source
- ✅ **Version control**: Lock Zephyr version in west.yml
- ✅ **Isolated environment**: Python venv doesn't pollute system
- ✅ **Multiple projects**: Can have multiple workspaces side-by-side
- ✅ **Easy updates**: `west update` pulls latest dependencies

## Using the Workspace

### Activate Environment

```bash
cd 9p4z-workspace
source activate.sh
```

Or manually:
```bash
cd 9p4z-workspace
source .venv/bin/activate
source zephyr/zephyr-env.sh
```

### Build and Run Tests

```bash
# Build all tests for native_posix
west build -b native_posix 9p4z/tests

# Run tests
west build -t run

# Or use the test runner script
cd 9p4z
./scripts/run_tests.sh
```

### Build Sample Application

```bash
# Build UART echo sample
west build -b native_posix 9p4z/samples/uart_echo -p

# For a real board (e.g., nRF52840 DK)
west build -b nrf52840dk_nrf52840 9p4z/samples/uart_echo -p

# Flash to hardware
west flash
```

### Using Twister (Test Runner)

```bash
# Run all tests
west twister -T 9p4z/tests/

# Specific platform
west twister -p native_posix -T 9p4z/tests/

# With inline logs
west twister -p native_posix -T 9p4z/tests/ --inline-logs

# Filter by tag
west twister -T 9p4z/tests/ --tag unit

# With coverage
west twister -p native_posix -T 9p4z/tests/ --coverage
```

## Common Commands

### West Commands

```bash
# Update all projects to manifest versions
west update

# Update only Zephyr
west update zephyr

# List all projects
west list

# Show workspace status
west status

# Build with specific board
west build -b <board> <source-dir>

# Clean build
west build -b <board> <source-dir> -p

# Build and flash
west build -b <board> <source-dir> -p && west flash
```

### Development Workflow

```bash
# 1. Activate workspace
cd 9p4z-workspace
source activate.sh

# 2. Make changes in 9p4z/

# 3. Test changes
cd 9p4z
./scripts/run_tests.sh

# 4. Build sample
cd ..  # Back to workspace root
west build -b native_posix 9p4z/samples/uart_echo -p

# 5. Commit changes
cd 9p4z
git add .
git commit -m "Your changes"
```

## Adding to Existing Zephyr Project

If you have an existing Zephyr project and want to add 9p4z as a module:

1. Add to your project's `west.yml`:

```yaml
manifest:
  projects:
    # ... your existing projects ...

    - name: 9p4z
      url: https://github.com/YOUR_USERNAME/9p4z
      revision: main
      path: modules/lib/9p4z
```

2. Update workspace:

```bash
west update
```

3. Enable in your application's `prj.conf`:

```ini
CONFIG_NINEP=y
CONFIG_NINEP_TRANSPORT_UART=y
# ... other 9p4z options ...
```

4. Use in your code:

```c
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/transport_uart.h>
// ...
```

## Troubleshooting

### "west: command not found"

Make sure virtual environment is activated:
```bash
source .venv/bin/activate
```

### "No module named 'west'"

Reinstall in venv:
```bash
source .venv/bin/activate
pip install west
```

### "Could not find Zephyr SDK"

For native_posix, Zephyr SDK is optional. For hardware targets:

1. Download Zephyr SDK: https://github.com/zephyrproject-rtos/sdk-ng/releases
2. Extract and run setup script:
```bash
tar xf zephyr-sdk-<version>-<host>.tar.gz
cd zephyr-sdk-<version>
./setup.sh
```

### Import errors when building

Make sure you've sourced the environment:
```bash
source zephyr/zephyr-env.sh
```

### "West workspace not initialized"

Run from workspace root (where .west/ directory is):
```bash
cd 9p4z-workspace  # Not 9p4z-workspace/9p4z !
```

### Build errors with module not found

Ensure west.yml is correct and run:
```bash
west update
```

### "FATAL ERROR: already initialized" during setup

If you see:
```
FATAL ERROR: already initialized in /path/to/parent, aborting.
```

You have an existing west workspace in a parent directory (e.g., `/Users/you/src/.west`).

**Solutions:**

**Option 1: Use a different workspace location** (Recommended)
```bash
# Specify workspace location outside the conflicting hierarchy
WORKSPACE_DIR=~/zephyr-workspaces/9p4z-workspace ./scripts/setup-workspace.sh
```

**Option 2: Remove the conflicting workspace** (If you don't need it)
```bash
# Find and remove the .west directory
rm -rf /Users/you/src/.west

# Then re-run setup
./scripts/setup-workspace.sh
```

**Option 3: Work within the existing workspace**
```bash
# If you want to use the existing workspace
cd /Users/you/src  # Directory with .west/

# Add 9p4z to the existing manifest
# Edit .west/config or the manifest file to include 9p4z
```

## Next Steps

- Read [tests/README.md](tests/README.md) for testing details
- Check [FEASIBILITY.md](FEASIBILITY.md) for project roadmap
- See [samples/uart_echo/README.md](samples/uart_echo/README.md) for sample usage
- Refer to [CLAUDE.md](CLAUDE.md) for development guide

## References

- [Zephyr Getting Started](https://docs.zephyrproject.org/latest/develop/getting_started/)
- [West Workspaces](https://docs.zephyrproject.org/latest/develop/west/workspaces.html)
- [Zephyr Modules](https://docs.zephyrproject.org/latest/develop/modules.html)
- [9P Protocol Spec](https://ericvh.github.io/9p-rfc/rfc9p2000.html)