# 9P LittleFS Server - Bluetooth Hard Drive

This sample demonstrates exposing a real LittleFS filesystem over 9P via Bluetooth L2CAP,
effectively creating a wireless "hard drive" accessible from mobile devices or computers.

## Overview

This sample:
- **Mounts LittleFS** on internal flash (256KB partition on nRF52840)
- **Exposes the filesystem** via 9P over Bluetooth L2CAP
- **Supports full file operations**: read, write, create, delete
- **Persists all changes** to flash storage
- **Includes 9PIS** for automatic discovery

All files created, modified, or deleted via the 9P connection are immediately
persisted to flash and survive power cycles.

## Features

### Storage
- **LittleFS**: Power-loss resilient filesystem
- **256KB partition**: Plenty of space for documents, configs, logs
- **Wear leveling**: Built into LittleFS
- **No external storage**: Everything on internal flash

### Access
- **Bluetooth L2CAP**: Wireless file access
- **9P Protocol**: Standard, well-documented protocol
- **Mobile compatible**: Works with iOS/Android
- **Auto-discovery**: 9PIS GATT service for finding devices

### Operations Supported
- ✅ Read files
- ✅ Write files
- ✅ Create files and directories
- ✅ Delete files and directories
- ✅ List directory contents
- ✅ Get file metadata (size, type, etc.)

## Requirements

- nRF52840 DK (or compatible board with >= 1MB flash)
- iOS/Android device with 9P client app
- Or: Computer with Bluetooth and 9P client

## Building

```bash
# For nRF52840 DK
west build -b nrf52840dk/nrf52840 samples/9p_server_littlefs

# Flash
west flash

# View logs
screen /dev/ttyACM0 115200
```

## Board Support

Currently configured for:
- **nRF52840 DK**: 256KB storage partition (see `boards/nrf52840dk_nrf52840.overlay`)

To add support for other boards:
1. Create a device tree overlay in `boards/<board>.overlay`
2. Define a `storage_partition` with desired size
3. Ensure partition doesn't overlap with application code

Example for a board with 512KB flash:
```dts
storage_partition: partition@60000 {
    label = "storage";
    reg = <0x00060000 0x00020000>;  /* 128KB */
};
```

## Usage

### Initial Setup

On first boot, the sample creates:
- `/welcome.txt` - Introduction message
- `/docs/` - Documentation directory
- `/docs/README.md` - Usage information

### Testing with LightBlue (Discovery)

1. Open **LightBlue** on your mobile device
2. Scan for devices - should see "9P LittleFS"
3. Connect and find service UUID `39500001-FEED-4A91-BA88-A1E0F6E4C001`
4. Read characteristics to see metadata

### Testing with 9P Client

1. **Connect via Bluetooth**
2. **Open L2CAP channel** on PSM 0x0009
3. **Send 9P messages** to browse and modify files

Example session (pseudocode):
```
# Mount filesystem
-> T-version (msize=8192, version="9P2000")
<- R-version (msize=8192, version="9P2000")

-> T-attach (fid=0, uname="user", aname="")
<- R-attach (qid=...)

# List root directory
-> T-walk (fid=0, newfid=1, nwname=0)
<- R-walk
-> T-open (fid=1, mode=OREAD)
<- R-open
-> T-read (fid=1, offset=0, count=4096)
<- R-read (data=<directory entries for welcome.txt, docs/>)

# Read welcome.txt
-> T-walk (fid=0, newfid=2, nwname=1, wname=["welcome.txt"])
<- R-walk
-> T-open (fid=2, mode=OREAD)
<- R-open
-> T-read (fid=2, offset=0, count=4096)
<- R-read (data="Welcome to 9P over LittleFS!...")

# Create a new file
-> T-walk (fid=0, newfid=3, nwname=0)
<- R-walk
-> T-create (fid=3, name="mynotes.txt", perm=0644, mode=OWRITE)
<- R-create
-> T-write (fid=3, offset=0, data="My important notes...")
<- R-write (count=20)
-> T-clunk (fid=3)
<- R-clunk

# File is now persisted to flash!
```

### Use Cases

#### Configuration Storage
Store device configuration that persists across updates:
```
/config/network.json
/config/bluetooth.json
/config/sensors.json
```

#### Data Logging
Log sensor data or events to files:
```
/logs/2025-01-15.log
/logs/2025-01-16.log
```

#### File Sharing
Share files between devices wirelessly:
```
/shared/document.txt
/shared/image.jpg
```

#### Remote Debugging
Store debug info accessible wirelessly:
```
/debug/crash.log
/debug/trace.bin
```

## Storage Capacity

Default configuration (nRF52840):
- **Total partition**: 256 KB
- **Usable space**: ~240 KB (after filesystem overhead)
- **Typical file count**: 50-100 files (depending on size)

LittleFS overhead:
- ~16 KB for metadata and wear leveling
- Small files (~1KB) have minimal overhead
- Large files are more space-efficient

## Performance

Typical performance over Bluetooth L2CAP (nRF52840 @ 64 MHz):
- **Sequential read**: ~50 KB/s
- **Sequential write**: ~40 KB/s
- **Random read (4KB)**: ~30 KB/s
- **Random write (4KB)**: ~20 KB/s
- **Directory listing**: ~100 entries/s

Bottlenecks:
1. Bluetooth throughput (limited to ~60 KB/s in practice)
2. Flash write speed
3. CPU overhead for 9P message processing

## Configuration

### Storage Size

Edit `boards/<board>.overlay` to adjust partition size:
```dts
storage_partition: partition@c0000 {
    reg = <0x000C0000 0x00080000>;  /* 512 KB */
};
```

### LittleFS Parameters

Adjust in the overlay's `fstab` section:
- `read-size`: Minimum read size (keep at 16 for flash)
- `prog-size`: Minimum program size (keep at 16 for flash)
- `cache-size`: Larger = faster but uses more RAM (default: 64)
- `lookahead-size`: Affects allocation speed (default: 32)
- `block-cycles`: Wear leveling cycles (default: 512)

### Memory Usage

RAM usage (approximate):
- **LittleFS cache**: cache-size * 2 (default: 128 bytes)
- **9P server**: ~4 KB for buffers and state
- **L2CAP**: ~8 KB for buffers
- **Total**: ~15 KB + application code

## Troubleshooting

**Mount fails with -EIO:**
- Flash partition may be corrupted
- Erase flash: `west flash --erase`
- Reflash firmware

**Out of space errors:**
- Check available space via logs
- LittleFS reserves space for wear leveling
- Increase partition size in device tree

**Slow writes:**
- Normal for flash - writes are slower than reads
- Use larger write sizes when possible
- Consider write caching in your application

**Files corrupted after power loss:**
- LittleFS is power-loss resilient by design
- Corruption should not occur
- If it does, file a bug report!

## Extending

### Adding More Initial Files

Edit `setup_initial_files()` in `src/main.c`:
```c
const char *myfile_path = LITTLEFS_MOUNT_POINT "/myfile.txt";
// ... create file
```

### Mounting Multiple Filesystems

You can mount multiple filesystems and expose them via 9P:
1. Define multiple mount points
2. Create multiple passthrough_fs instances
3. Use a multiplexer to route requests to the right filesystem

### SD Card Support

To use an SD card instead of internal flash:
1. Enable FAT filesystem: `CONFIG_FILE_SYSTEM_FAT=y`
2. Configure SPI and SD card in device tree
3. Change mount point to `/SD:`
4. Update `ninep_passthrough_fs_init()` call

See the `9p_server_sdcard` sample for a complete example.

## Security Considerations

**⚠️ No Authentication**: This sample has no authentication. Anyone who
connects via Bluetooth can access all files.

**⚠️ No Encryption**: Files are stored in plain text on flash.

**⚠️ No Access Control**: All files are readable and writable.

For production use, consider:
- Bluetooth pairing/bonding
- 9P authentication mechanisms
- Flash encryption (if supported by hardware)
- File-level permissions (requires extended 9P implementation)

## References

- [9PIS GATT Specification](../../docs/9PIS_GATT_SPECIFICATION.md)
- [Passthrough FS API](../../include/zephyr/9p/passthrough_fs.h)
- [LittleFS Documentation](https://github.com/littlefs-project/littlefs)
- [Zephyr File Systems](https://docs.zephyrproject.org/latest/services/file_system/index.html)
