# Passthrough Filesystem

The passthrough filesystem backend exposes any mounted Zephyr filesystem via 9P,
enabling real file storage and persistence for your 9P servers.

## Overview

Unlike the synthetic filesystems (sysfs, ramfs), the passthrough filesystem backend
provides access to actual storage devices:

- **Internal Flash**: LittleFS partitions on MCU flash
- **External Storage**: SD cards with FAT filesystem
- **Any Zephyr FS**: Works with any filesystem Zephyr supports

This turns your embedded device into a wireless file server or "Bluetooth hard drive".

## Supported Filesystems

### LittleFS (Recommended)
- **Power-loss resilient**: Safe for embedded use
- **Wear leveling**: Protects flash memory
- **Small footprint**: ~15KB RAM, ~10KB code
- **Fast**: Optimized for NOR flash
- **Use case**: Internal flash storage

### FAT
- **Universal compatibility**: Works everywhere
- **Large files**: No file size limits
- **SD card support**: Standard filesystem for SD cards
- **Use case**: External storage, SD cards

### Others
The passthrough backend works with any Zephyr filesystem:
- **ext2**: Linux-compatible filesystem
- **FCB**: Flash Circular Buffer
- **NVS**: Non-Volatile Storage (key-value)

## Architecture

```
┌─────────────┐
│   9P Client │ (iOS, Android, Linux)
└──────┬──────┘
       │ 9P Protocol
       ▼
┌─────────────────────────────┐
│  9P Server (Zephyr)         │
│  ┌────────────────────────┐ │
│  │ Passthrough FS Backend │ │
│  └───────────┬────────────┘ │
└──────────────┼──────────────┘
               │
               ▼
       ┌───────────────┐
       │  Zephyr FS API│
       └───────┬───────┘
               │
        ┌──────┴───────┐
        ▼              ▼
   ┌─────────┐   ┌─────────┐
   │LittleFS │   │   FAT   │
   └────┬────┘   └────┬────┘
        ▼             ▼
   ┌─────────┐   ┌─────────┐
   │  Flash  │   │ SD Card │
   └─────────┘   └─────────┘
```

## API Usage

### Basic Setup

```c
#include <zephyr/9p/server.h>
#include <zephyr/9p/passthrough_fs.h>
#include <zephyr/fs/fs.h>

/* 1. Mount a filesystem (LittleFS example) */
static struct fs_mount_t lfs_mount = {
    .type = FS_LITTLEFS,
    .mnt_point = "/lfs1",
    /* ... other config ... */
};

int main(void) {
    /* Mount the filesystem */
    int ret = fs_mount(&lfs_mount);
    if (ret < 0) {
        // Handle error
    }

    /* 2. Initialize passthrough FS */
    struct ninep_passthrough_fs passthrough_fs;
    ret = ninep_passthrough_fs_init(&passthrough_fs, "/lfs1");
    if (ret < 0) {
        // Handle error
    }

    /* 3. Configure 9P server to use passthrough FS */
    struct ninep_server_config server_config = {
        .fs_ops = ninep_passthrough_fs_get_ops(),
        .fs_ctx = &passthrough_fs,
        .max_message_size = 8192,
        .version = "9P2000",
    };

    /* 4. Initialize and start server */
    struct ninep_server server;
    ninep_server_init(&server, &server_config, &transport);
    ninep_server_start(&server);
}
```

### Configuration

Enable in `prj.conf`:
```ini
# Enable passthrough filesystem
CONFIG_NINEP_FS_PASSTHROUGH=y

# Enable underlying filesystem
CONFIG_FILE_SYSTEM=y
CONFIG_FILE_SYSTEM_LITTLEFS=y  # Or FAT, etc.

# Enable flash support
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
```

## LittleFS Setup

### Device Tree Configuration

Define a storage partition in your board overlay:

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        /* Application partition */
        slot0_partition: partition@0 {
            label = "image-0";
            reg = <0x00000000 0x000C0000>;  /* 768KB */
        };

        /* Storage partition for LittleFS */
        storage_partition: partition@c0000 {
            label = "storage";
            reg = <0x000C0000 0x00040000>;  /* 256KB */
        };
    };
};

/ {
    fstab {
        compatible = "zephyr,fstab";
        lfs1: lfs1 {
            compatible = "zephyr,fstab,littlefs";
            mount-point = "/lfs1";
            partition = <&storage_partition>;
            automount;
            read-size = <16>;
            prog-size = <16>;
            cache-size = <64>;
            lookahead-size = <32>;
            block-cycles = <512>;
        };
    };
};
```

### Application Code

```c
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

/* LittleFS configuration */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);

static struct fs_mount_t lfs_mount = {
    .type = FS_LITTLEFS,
    .fs_data = &storage,
    .storage_dev = (void *)FIXED_PARTITION_ID(storage_partition),
    .mnt_point = "/lfs1",
};

/* Mount at startup */
fs_mount(&lfs_mount);
```

### Initial File Population

```c
void setup_initial_files(void)
{
    struct fs_file_t file;
    const char *welcome = "Welcome to 9P!\n";

    fs_file_t_init(&file);
    fs_open(&file, "/lfs1/welcome.txt", FS_O_CREATE | FS_O_WRITE);
    fs_write(&file, welcome, strlen(welcome));
    fs_close(&file);
}
```

## SD Card Setup

### Hardware Configuration

Configure SPI and SD card in device tree:

```dts
&spi1 {
    status = "okay";
    cs-gpios = <&gpio0 17 GPIO_ACTIVE_LOW>;

    sdhc0: sdhc@0 {
        compatible = "zephyr,sdhc-spi-slot";
        reg = <0>;
        spi-max-frequency = <8000000>;
    };
};
```

### Application Code

```c
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>

static struct fs_mount_t sd_mount = {
    .type = FS_FATFS,
    .mnt_point = "/SD:",
};

void mount_sd_card(void)
{
    /* Initialize disk */
    disk_access_init("SD");

    /* Mount FAT */
    int ret = fs_mount(&sd_mount);
    if (ret < 0) {
        printk("SD mount failed: %d\n", ret);
        return;
    }

    /* Initialize passthrough FS */
    struct ninep_passthrough_fs sd_fs;
    ninep_passthrough_fs_init(&sd_fs, "/SD:");
}
```

## File Operations

### Supported Operations

The passthrough backend supports all standard 9P operations:

#### Read Operations
- **Tstat/Rstat**: Get file metadata
- **Twalk/Rwalk**: Navigate directory tree
- **Topen/Ropen**: Open files for reading
- **Tread/Rread**: Read file contents or directory entries

#### Write Operations
- **Topen/Ropen**: Open files for writing
- **Twrite/Rwrite**: Write to files
- **Tcreate/Rcreate**: Create new files and directories
- **Tremove/Rremove**: Delete files and directories

### Operation Mapping

| 9P Operation | Zephyr FS API |
|--------------|---------------|
| Twalk        | fs_stat()     |
| Topen        | fs_open()     |
| Tread (file) | fs_read()     |
| Tread (dir)  | fs_readdir()  |
| Twrite       | fs_write()    |
| Tcreate      | fs_open(FS_O_CREATE) or fs_mkdir() |
| Tremove      | fs_unlink()   |
| Tstat        | fs_stat()     |

## Performance Considerations

### Throughput

Typical performance (nRF52840 @ 64 MHz, BLE connection):
- **Sequential read**: ~50 KB/s
- **Sequential write**: ~40 KB/s
- **Random access**: ~20-30 KB/s

Bottlenecks:
1. **Bluetooth bandwidth**: ~60 KB/s theoretical max
2. **Flash write speed**: ~40 KB/s for internal flash
3. **CPU overhead**: Message parsing, filesystem operations

### Memory Usage

RAM requirements:
- **Passthrough FS**: ~256 bytes per node (created on-demand)
- **LittleFS cache**: Configurable, typically 128-256 bytes
- **9P buffers**: 2 × message_size (default: 16 KB)

Flash requirements:
- **Passthrough code**: ~4 KB
- **LittleFS code**: ~10 KB
- **Storage partition**: User-defined (64 KB - several MB)

### Optimization Tips

#### 1. Increase Cache Size
```dts
lfs1: lfs1 {
    cache-size = <256>;  /* Larger cache = faster */
};
```

#### 2. Use Larger Transfers
```c
/* Client should request larger reads/writes */
// Good: 4KB reads
Tread (fid=1, offset=0, count=4096)

// Bad: 64-byte reads
Tread (fid=1, offset=0, count=64)
```

#### 3. Minimize Walk Operations
```c
/* Cache FIDs on the client side */
/* Walk once, then reuse FID for multiple reads */
```

#### 4. Batch Directory Reads
```c
/* Read entire directory in one operation */
Tread (fid=dir, offset=0, count=8192)
```

## Error Handling

### Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| -ENOENT | File not found | Check path, walk from root |
| -ENOSPC | Out of space | Free files or increase partition |
| -EINVAL | Invalid operation | Check file is opened with correct mode |
| -EISDIR | Is a directory | Cannot read/write directories |
| -ENOTDIR | Not a directory | Cannot walk into files |
| -EEXIST | File exists | Use different name or remove first |

### Mount Failures

```c
int ret = fs_mount(&lfs_mount);
if (ret == -EIO) {
    /* Filesystem corrupted, need to format */
    printk("Formatting filesystem...\n");
    fs_mkfs(FS_LITTLEFS, (uintptr_t)FIXED_PARTITION_ID(storage_partition),
            &storage, FS_MOUNT_FLAG_NO_FORMAT);
    ret = fs_mount(&lfs_mount);
}
```

## Security

### Access Control

**⚠️ Important**: The passthrough filesystem has **no access control**. All
files are accessible to anyone who connects via 9P.

For production use, implement:
1. **Bluetooth pairing**: Require bonding before file access
2. **9P authentication**: Implement user/group permissions
3. **Read-only mode**: Only allow Tread operations
4. **Path restrictions**: Filter which directories are accessible

### Encryption

Files are stored in plain text. For sensitive data:
1. **Flash encryption**: Use hardware encryption if available (e.g., nRF52840)
2. **File-level encryption**: Encrypt files before writing
3. **Bluetooth encryption**: Use BT pairing/encryption

### Example: Read-Only Mode

```c
/* Disable write operations in passthrough_fs.c */
static int passthrough_write(...)
{
    return -EROFS;  /* Read-only filesystem */
}

static int passthrough_create(...)
{
    return -EROFS;
}

static int passthrough_remove(...)
{
    return -EROFS;
}
```

## Debugging

### Enable Logging

```ini
CONFIG_NINEP_LOG_LEVEL_DBG=y
CONFIG_LOG=y
CONFIG_FS_LOG_LEVEL_DBG=y
```

### Common Issues

**"Walk failed: fs_stat returned -2"**
- File doesn't exist at that path
- Check mount point matches: `/lfs1/file.txt` not `/file.txt`

**"fs_open failed: -28"**
- Out of space (ENOSPC)
- Check available space: `fs_statvfs()`

**"Directory read returns 0 entries"**
- Directory is empty
- Or offset is past end of directory

**"Writes don't persist"**
- Check fs_close() is called
- LittleFS requires explicit sync
- Power loss during write can lose data

## Advanced Usage

### Multiple Mount Points

Expose multiple filesystems under different paths:

```c
/* Mount LittleFS at /lfs1 */
struct ninep_passthrough_fs lfs_backend;
ninep_passthrough_fs_init(&lfs_backend, "/lfs1");

/* Mount SD card at /SD: */
struct ninep_passthrough_fs sd_backend;
ninep_passthrough_fs_init(&sd_backend, "/SD:");

/* Use a union filesystem to combine them */
/* (Requires custom implementation) */
```

### Custom Filesystem Backend

You can create custom backends for any data source:

```c
static const struct ninep_fs_ops my_custom_ops = {
    .get_root = my_get_root,
    .walk = my_walk,
    .read = my_read,
    /* ... implement all operations ... */
};

struct ninep_server_config config = {
    .fs_ops = &my_custom_ops,
    .fs_ctx = &my_custom_context,
};
```

Examples:
- **Database backend**: Expose SQL tables as files
- **Network backend**: Proxy to remote filesystem
- **Sensor backend**: Generate files from live sensor data
- **Log backend**: Stream logs as files

## Examples

Complete samples available:
- **samples/9p_server_littlefs**: LittleFS on internal flash
- **samples/9p_server_sdcard**: FAT on SD card (future)

## References

- [Zephyr File System API](https://docs.zephyrproject.org/latest/services/file_system/index.html)
- [LittleFS Documentation](https://github.com/littlefs-project/littlefs)
- [9P Protocol Specification](http://man.cat-v.org/plan_9/5/intro)
- [Passthrough FS API](../include/zephyr/9p/passthrough_fs.h)
