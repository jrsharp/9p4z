#!/bin/bash
# Generate LittleFS image for flashing

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FS_DATA_DIR="$SCRIPT_DIR/fs_data"
OUTPUT_IMAGE="$SCRIPT_DIR/lfs_image.bin"
LFS_SIZE=$((64 * 1024))  # 64KB partition size

# Check if littlefs-python is installed
if ! python3 -c "import littlefs" 2>/dev/null; then
    echo "Error: littlefs-python not installed"
    echo "Install with: pip3 install littlefs-python"
    exit 1
fi

echo "Generating LittleFS image from $FS_DATA_DIR..."

# Generate the image
python3 << EOF
from littlefs import LittleFS
import os

# Create filesystem
fs = LittleFS(block_size=4096, block_count=$LFS_SIZE // 4096)

# Helper to create directories recursively
def mkdir_recursive(fs, path):
    if not path or path == '/':
        return
    parts = path.strip('/').split('/')
    current = ''
    for part in parts:
        current += '/' + part
        try:
            fs.mkdir(current)
        except:
            pass  # Already exists

# Walk through fs_data and add all files
for root, dirs, files in os.walk('$FS_DATA_DIR'):
    for file in files:
        src_path = os.path.join(root, file)
        # Get path relative to fs_data
        rel_path = os.path.relpath(src_path, '$FS_DATA_DIR')
        dst_path = '/' + rel_path

        # Create directories if needed
        dir_path = os.path.dirname(dst_path)
        if dir_path and dir_path != '/':
            mkdir_recursive(fs, dir_path)

        # Add file
        print(f"Adding: {dst_path}")
        with open(src_path, 'rb') as f:
            data = f.read()
        with fs.open(dst_path, 'wb') as f:
            f.write(data)

# Write image
with open('$OUTPUT_IMAGE', 'wb') as f:
    f.write(fs.context.buffer)

print(f"Generated {os.path.getsize('$OUTPUT_IMAGE')} byte image")
EOF

echo "LittleFS image created: $OUTPUT_IMAGE"

# Convert to Intel HEX format with address
OUTPUT_HEX="${OUTPUT_IMAGE%.bin}.hex"
python3 << EOF
# Convert binary to Intel HEX at address 0xF0000
import struct

with open('$OUTPUT_IMAGE', 'rb') as f:
    data = f.read()

# Intel HEX format
def create_hex_record(address, record_type, data):
    byte_count = len(data)
    # High and low address bytes
    addr_high = (address >> 8) & 0xFF
    addr_low = address & 0xFF

    record = bytes([byte_count, addr_high, addr_low, record_type]) + data
    checksum = (~sum(record) + 1) & 0xFF

    return ':' + record.hex().upper() + f'{checksum:02X}'

with open('$OUTPUT_HEX', 'w') as f:
    base_addr = 0xF0000

    # Extended Linear Address Record for high 16 bits
    ext_addr = (base_addr >> 16) & 0xFFFF
    f.write(create_hex_record(0, 0x04, struct.pack('>H', ext_addr)) + '\n')

    # Write data in 16-byte chunks
    offset = 0
    while offset < len(data):
        chunk = data[offset:offset+16]
        addr = (base_addr + offset) & 0xFFFF
        f.write(create_hex_record(addr, 0x00, chunk) + '\n')
        offset += len(chunk)

    # End of File record
    f.write(':00000001FF\n')

print(f'Created HEX file: $OUTPUT_HEX')
EOF

echo ""
echo "To flash to device:"
echo "  nrfjprog --program $OUTPUT_HEX --sectorerase --verify --reset"
echo ""
echo "Or erase first then program:"
echo "  nrfjprog --erasepage 0xF0000-0x100000"
echo "  nrfjprog --program $OUTPUT_HEX --verify --reset"
