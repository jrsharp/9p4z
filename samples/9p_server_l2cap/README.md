# 9P L2CAP Server with Discovery (9PIS)

This sample demonstrates a discoverable 9P server running over Bluetooth L2CAP,
featuring the 9P Information Service (9PIS) GATT service for automatic discovery.

## Description

The sample creates a 9P server that listens for incoming L2CAP connections on PSM 0x0009.

### 9P Information Service (9PIS)

This sample includes the **9P Information Service (9PIS)** - a standard GATT service that
advertises the availability of 9P services on the device. This enables:

- **Automatic Discovery**: iOS/Android apps can scan and identify 9P-capable devices
- **Service Metadata**: Service description, features, and capabilities
- **Connection Details**: Transport type (L2CAP), PSM (0x0009), and MTU (4096)
- **App Discovery**: Link to download compatible client applications

When you connect with a generic BLE scanner (like LightBlue), you'll see the 9PIS
service with UUID `39500001-feed-4a91-ba88-a1e0f6e4c001` and can read all the
service information characteristics.

### 9P Beacon Discovery

**NEW!** The device broadcasts a custom **beacon service data** in its BLE advertising packets,
allowing mobile apps to discover nearby 9P+BBS hotspots **without connecting**!

#### Beacon Format

The beacon uses **Service Data** with custom UUID `0x9B50` ("9P" + "BS") and includes 8 bytes:

```
Byte 0:    Protocol version (0x01)
Byte 1:    Feature flags
           - Bit 0: BBS available (bulletin board system)
           - Bit 1: File storage available
           - Bit 2: Sysfs available
           - Bits 3-7: Reserved for future use
Bytes 2-3: Primary PSM (little-endian, typically 0x0009)
Byte 4:    Number of BBS rooms
Byte 5:    Number of registered users
Bytes 6-7: Total message count (little-endian)
```

#### Scanning for 9P Beacons

Mobile apps can scan for this service data to discover and display nearby 9P services:

**iOS (Swift):**
```swift
func centralManager(_ central: CBCentralManager,
                   didDiscover peripheral: CBPeripheral,
                   advertisementData: [String : Any], rssi RSSI: NSNumber) {
    if let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {
        let uuid = CBUUID(string: "9B50")
        if let data = serviceData[uuid] {
            let version = data[0]
            let features = data[1]
            let hasBBS = (features & 0x01) != 0
            let hasFiles = (features & 0x02) != 0
            let psm = UInt16(data[2]) | (UInt16(data[3]) << 8)
            let roomCount = data[4]
            let userCount = data[5]
            let msgCount = UInt16(data[6]) | (UInt16(data[7]) << 8)

            print("Found 9P beacon: PSM=\(psm), Rooms=\(roomCount), Users=\(userCount), Messages=\(msgCount)")
            print("  Features: BBS=\(hasBBS), Files=\(hasFiles)")
        }
    }
}
```

**Android (Kotlin):**
```kotlin
override fun onScanResult(callbackType: Int, result: ScanResult) {
    val serviceData = result.scanRecord?.serviceData
    val uuid = ParcelUuid.fromString("00009B50-0000-1000-8000-00805f9b34fb")

    serviceData?.get(uuid)?.let { data ->
        val version = data[0]
        val features = data[1]
        val hasBBS = (features.toInt() and 0x01) != 0
        val hasFiles = (features.toInt() and 0x02) != 0
        val psm = (data[2].toInt() and 0xFF) or ((data[3].toInt() and 0xFF) shl 8)
        val roomCount = data[4]
        val userCount = data[5]
        val msgCount = (data[6].toInt() and 0xFF) or ((data[7].toInt() and 0xFF) shl 8)

        Log.d("9PBeacon", "Found: PSM=$psm, Rooms=$roomCount, Users=$userCount, Messages=$msgCount")
    }
}
```

#### Use Cases

- **Proximity Detection**: Show "9P BBS nearby!" notification when user enters range
- **Service Browser**: Display list of nearby 9P services with live stats
- **Auto-Connect**: Connect to strongest signal automatically
- **Mesh Network Map**: Visualize network of 9P nodes in area
- **Community Discovery**: Find local BBS communities at events/meetups

### File System

The server provides a rich synthetic filesystem with:

#### Basic Files
- `/hello.txt` - Welcome message
- `/lib/9p-intro.txt` - Comprehensive 9P protocol introduction

#### System Information (`/sys/`)
- `/sys/version` - System version information
- `/sys/uptime` - Live system uptime
- `/sys/memory` - Heap statistics
- `/sys/threads` - Live thread list
- `/sys/stats` - Kernel statistics
- `/sys/firmware` - **WRITABLE** - Firmware upload demo

#### Device Control (`/dev/`)
- `/dev/led` - **WRITABLE** - LED control (write 'on'/'off')

#### Sensors (`/sensors/`)
- `/sensors/temp0` - Live (simulated) temperature readings

#### Documentation (`/docs/`)
- `/docs/readme.md` - Basic documentation

#### Bulletin Board System (`/bbs/`)
**NEW!** Plan 9-style BBS accessible as a filesystem:
- `/bbs/rooms/lobby/1` - Read messages as files (RFC822 format)
- `/bbs/rooms/lobby/2` - Second message
- `/bbs/rooms/tech/1` - Tech discussion room
- `/bbs/rooms/general/1` - General discussion
- `/bbs/etc/roomlist` - List of all rooms

The BBS demonstrates in-process 9P servers - the bulletin board runs in the same
address space but is accessed through the 9P filesystem interface. Messages are
stored in RAM and formatted as RFC822-style files. Perfect for demonstrating how
"everything is a file" works with Plan 9!

## Requirements

- Board with Bluetooth LE support (e.g., nRF52840, nRF5340, etc.)
- iOS/Android device with:
  - **LightBlue** or similar BLE scanner - For testing 9PIS service
  - **Custom 9P client app** - For full file system access
  - **9p4i app** (iOS) - Alternative 9P over L2CAP client

## Building

### Default Build (Synthetic Filesystem)

Uses sysfs for dynamic, in-memory files (system info, sensors, device control):

```bash
# For nRF52840 DK
west build -b nrf52840dk/nrf52840 samples/9p_server_l2cap

# For nRF7002 DK
west build -b nrf7002dk/nrf5340/cpuapp samples/9p_server_l2cap
```

### LittleFS Build (Persistent Storage)

Uses real filesystem on flash for persistent file storage:

```bash
# For nRF52840 DK with LittleFS
west build -b nrf52840dk/nrf52840 samples/9p_server_l2cap -- -DCONF_FILE=prj_littlefs.conf

# For nRF7002 DK with LittleFS
west build -b nrf7002dk/nrf5340/cpuapp samples/9p_server_l2cap -- -DCONF_FILE=prj_littlefs.conf
```

**LittleFS mode features:**
- ✅ 256KB persistent storage on internal flash
- ✅ All file operations (create, read, write, delete)
- ✅ Changes survive reboots
- ✅ Pre-populated with welcome files and directories
- ✅ Perfect for "Bluetooth hard drive" use case

## Flashing

```bash
west flash
```

**First time with LittleFS?** If the filesystem fails to mount, erase flash first:
```bash
west flash --erase
```

## Pre-Populated Files (LittleFS Mode)

On first boot, the LittleFS filesystem is automatically populated with:

```
/lfs1/
├── welcome.txt          # Welcome message
├── .populated           # Marker file (hidden)
├── docs/
│   └── README.md        # Documentation
├── shared/              # For shared files
└── notes/
    └── example.txt      # Example note
```

The `.populated` marker file prevents re-initialization on subsequent boots.
All files and directories persist across reboots and can be modified via 9P!

## Testing

### Testing with a 9PIS-Aware Client App

1. **Flash the firmware** to your board
2. **Open your 9P client app** on your mobile device
3. **Tap "Scan"** - App automatically discovers devices with 9PIS
4. **See "9P File Server"** in the list
5. **Tap to connect** - App reads 9PIS, opens L2CAP automatically
6. **Browse files** - Full filesystem access with UI

See `docs/IOS_9PIS_INTEGRATION.md` for implementing 9PIS discovery in your app.

### Testing Discovery with LightBlue

To verify the 9PIS GATT service is working:

1. **Open LightBlue** or similar BLE scanner
2. **Scan for devices** - should see "9P Server"
3. **Connect** to the device
4. **Find service** with UUID `39500001-feed-4a91-ba88-a1e0f6e4c001`
5. **Read characteristics**:
   - Service Description: "9P File Server"
   - Service Features: "file-sharing,sensor-data,led-control,firmware-update"
   - Transport Info: "l2cap:psm=0x0009,mtu=4096"
   - App Store Link: "https://9p4z.org/clients"
   - Protocol Version: "9P2000;9p4z;1.0.0"

### Testing with 9p4i (Manual L2CAP)

1. **Flash the firmware** to your board
2. **Open serial console** to view logs
3. **Open 9p4i app** on your iOS device
4. **Scan for devices** - should see "9P Server"
5. **Connect** to the device
6. **Open L2CAP channel** on PSM 0x0009
7. **Send T-version** message to initiate 9P handshake
8. **Send T-attach** to attach to the root filesystem
9. **Send T-walk**, **T-open**, **T-read** to browse files

## Example Session

```
iOS -> Zephyr: T-version (msize=4096, version="9P2000")
Zephyr -> iOS: R-version (msize=4096, version="9P2000")

iOS -> Zephyr: T-attach (fid=0, uname="user", aname="")
Zephyr -> iOS: R-attach (qid=...)

iOS -> Zephyr: T-walk (fid=0, newfid=1, nwname=1, wname=["hello.txt"])
Zephyr -> iOS: R-walk (nwqid=1, wqid=[...])

iOS -> Zephyr: T-open (fid=1, mode=OREAD)
Zephyr -> iOS: R-open (qid=..., iounit=0)

iOS -> Zephyr: T-read (fid=1, offset=0, count=4096)
Zephyr -> iOS: R-read (data="Hello from 9P over L2CAP!")
```

### Reading BBS Messages

From your 9P client, you can read bulletin board messages like regular files:

```bash
# List available rooms
cat /bbs/etc/roomlist

# Read first message in lobby
cat /bbs/rooms/lobby/1
```

Output:
```
From: alice
To: lobby
Date: 1234567890
X-Date-N: 1234567890

Welcome to 9bbs over Bluetooth L2CAP!

This is a Plan 9-style bulletin board system accessible
as a filesystem. Read and post messages by navigating to
/bbs/rooms/<room>/<message_number>

alice
```

The BBS demonstrates **in-process 9P servers** - no network serialization happens
when accessing BBS files locally, yet the same filesystem can be exported over
Bluetooth and accessed from your phone!

## Configuration

Key Kconfig options (in prj.conf):

- `CONFIG_NINEP_GATT_9PIS=y` - **Enable 9PIS discovery service**
- `CONFIG_NINEP_9BBS=y` - **Enable bulletin board system**
- `CONFIG_NINEP_L2CAP_PSM=0x0009` - L2CAP PSM (must match client)
- `CONFIG_NINEP_L2CAP_MTU=4096` - Maximum L2CAP MTU
- `CONFIG_NINEP_MAX_MESSAGE_SIZE=8192` - Max 9P message size
- `CONFIG_BT_DEVICE_NAME="9P Server"` - Bluetooth device name

### Filesystem Backends

The sample supports multiple filesystem backends that can be composed into a unified namespace:

**Synthetic Filesystem (sysfs):**
- Dynamic, generated content
- Files exist only in RAM
- Perfect for system info, live sensors, device control
- Always available for system files

**Persistent Storage (passthrough_fs with LittleFS):**
- Real filesystem on flash memory
- All changes persist across reboots
- Configured in `prj_littlefs.conf`
- Requires `CONFIG_NINEP_FS_PASSTHROUGH=y`
- Storage partition defined in device tree overlay

**Bulletin Board System (9bbs):**
- In-process 9P server demonstrating driver-as-filesystem
- Messages stored in RAM (could be backed by LittleFS)
- RFC822-style message format
- Multiple rooms (lobby, tech, general)
- Requires `CONFIG_NINEP_9BBS=y`

**Namespace Composition (union_fs):**
- Plan 9-style namespace composition
- Mounts multiple backends into a unified namespace
- Longest-prefix-match routing to backends
- **Always available** when using CONFIG_NINEP_SERVER

The namespace is always composed using union_fs. When building with LittleFS (`prj_littlefs.conf`), it looks like:
```
/                    -> union_fs (namespace multiplexer)
├── dev/             -> sysfs (device control)
├── sys/             -> sysfs (system info)
├── sensors/         -> sysfs (live sensor data)
├── lib/             -> sysfs (reference material)
├── docs/            -> sysfs (documentation)
├── bbs/             -> 9bbs (in-process 9P server)
│   ├── rooms/
│   │   ├── lobby/
│   │   ├── tech/
│   │   └── general/
│   └── etc/
│       └── roomlist
└── files/           -> passthrough_fs -> LittleFS (persistent storage)
    ├── welcome.txt
    ├── docs/
    ├── notes/
    └── shared/
```

When building without LittleFS (default `prj.conf`), union_fs still provides a single namespace:
```
/                    -> union_fs (namespace multiplexer)
├── dev/             -> sysfs (device control)
├── sys/             -> sysfs (system info)
├── bbs/             -> 9bbs (in-process 9P server)
├── sensors/         -> sysfs (live sensor data)
├── lib/             -> sysfs (reference material)
└── docs/            -> sysfs (documentation)
```

The union filesystem performs path-based routing:
- Accessing `/dev/led` routes to sysfs
- Accessing `/files/notes/todo.txt` routes to passthrough_fs → LittleFS
- This mirrors Plan 9's approach to namespace construction
- Even with one backend, union_fs provides a clean, extensible architecture

### Customizing 9PIS Metadata

You can customize the 9PIS service metadata in `src/main.c`:

```c
struct ninep_9pis_config gatt_config = {
	.service_description = "Your Custom Name",
	.service_features = "file-sharing,custom-feature",
	.transport_info = "l2cap:psm=0x0009,mtu=4096",
	.app_store_link = "https://your-app.com/download",
	.protocol_version = "9P2000;9p4z;1.0.0",
};
```

## Troubleshooting

**Device not visible in iOS scan:**
- Check that advertising started successfully in logs
- Ensure Bluetooth is enabled on iOS device
- Try restarting Bluetooth on both devices

**L2CAP connection fails:**
- Verify PSM matches between server (0x0009) and iOS client
- Check L2CAP logs for error messages
- Ensure MTU negotiation succeeded

**9P messages fail:**
- Enable debug logging: `CONFIG_NINEP_LOG_LEVEL_DBG=y`
- Check that RX buffer is large enough for messages
- Verify message framing (size field must be little-endian)

## Customizing LittleFS Content

To add your own pre-populated files, edit `populate_littlefs()` in `src/main.c`:

```c
/* Add your custom file */
const char *myfile_path = LITTLEFS_MOUNT_POINT "/myfile.txt";
const char *myfile_content = "My custom content\n";

fs_file_t_init(&file);
ret = fs_open(&file, myfile_path, FS_O_CREATE | FS_O_WRITE);
if (ret >= 0) {
    fs_write(&file, myfile_content, strlen(myfile_content));
    fs_close(&file);
}
```

The population only runs once (checked via `.populated` marker file).
To re-populate, either:
1. Delete the `.populated` file via 9P
2. Erase flash: `west flash --erase`

## References

- [9PIS GATT Specification](../../docs/9PIS_GATT_SPECIFICATION.md) - 9P Information Service
- [Passthrough FS Documentation](../../docs/PASSTHROUGH_FS.md) - LittleFS backend
- [L2CAP Transport Design](../../docs/L2CAP_TRANSPORT_DESIGN.md)
- [iOS Implementation Summary](../../docs/IOS_IMPL_SUMMARY.md)
- [9P Protocol Specification](http://man.cat-v.org/plan_9/5/intro)
