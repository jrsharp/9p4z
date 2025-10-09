#!/bin/bash
# Build script for 9p_server_l2cap on NCS nrf7002dk

set -e

SAMPLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NCS_ROOT="/opt/nordic/ncs/v3.1.1"
NCS_TOOLCHAIN="/opt/nordic/ncs/toolchains/561dce9adf"

cd "$NCS_ROOT"

export PATH="$NCS_TOOLCHAIN/bin:$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin:$PATH"
export PATH="$NCS_TOOLCHAIN/nrfutil/bin:$PATH"

echo "Building 9p_server_l2cap for nRF7002dk..."

# Use the same config approach as all-transports
"$NCS_TOOLCHAIN/Cellar/python@3.12/3.12.4/Frameworks/Python.framework/Versions/3.12/bin/west" build \
  -b nrf7002dk/nrf5340/cpuapp \
  -d "$SAMPLE_DIR/build" \
  "$SAMPLE_DIR" \
  --pristine \
  -- -DCONF_FILE=prj_nrf.conf -DZEPHYR_EXTRA_MODULES="$SAMPLE_DIR/../.."

echo ""
echo "Build complete!"
echo "Binary: $SAMPLE_DIR/build/zephyr/zephyr.hex"
