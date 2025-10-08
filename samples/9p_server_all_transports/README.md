# 9P All Transports Server Sample

This sample demonstrates all three 9P transport types (UART, TCP, L2CAP) running simultaneously and serving the same shared filesystem.

## Description

The sample creates three separate 9P server instances, each with its own transport:

- **UART Transport**: Accessible via serial console
- **TCP Transport**: Accessible over network on port 564
- **L2CAP Transport**: Accessible via Bluetooth on PSM 0x0009

All three servers share the same synthetic filesystem:

- `/hello.txt` - Welcome message listing all transports
- `/sys/version` - System version and build info
- `/sys/uptime` - System uptime
- `/docs/` - Empty documentation directory

## Architecture

```
┌────────────────────────────────────────┐
│        Shared Filesystem (sysfs)       │
│  /hello.txt  /sys/*  /docs/            │
└─────────────┬──────────────────────────┘
              │
         ┌────┴────┬────────┬────────┐
         │         │        │        │
    ┌────▼───┐ ┌──▼───┐ ┌──▼────┐   │
    │ UART   │ │ TCP  │ │ L2CAP │   │
    │ Server │ │Server│ │Server │   │
    └────┬───┘ └──┬───┘ └──┬────┘   │
         │        │        │         │
    ┌────▼───┐ ┌──▼───┐ ┌──▼────┐   │
    │ UART   │ │ TCP  │ │ L2CAP │   │
    │Transport│ │Trans│ │Trans  │   │
    └────────┘ └──────┘ └───────┘   │
```

Each server instance operates independently but accesses the same filesystem data, allowing clients to connect via any transport and see the same content.

## Requirements

### Hardware
- Board with all features:
  - UART console (most boards)
  - Network interface (nRF7002dk, qemu_x86, etc.)
  - Bluetooth LE (nRF52/nRF53, etc.)

### Best Supported Boards
- **nRF7002dk**: WiFi (TCP) + Bluetooth + UART ✓
- **nRF5340dk**: Ethernet or Bluetooth + UART (TCP needs overlay)
- **qemu_x86**: Emulated Ethernet (QEMU) + UART (no Bluetooth)

## Building

### For nRF7002dk (all transports)
```bash
west build -b nrf7002dk/nrf5340/cpuapp samples/9p_server_all_transports
```

### For nRF5340dk (Bluetooth + UART only)
```bash
west build -b nrf5340dk/nrf5340/cpuapp samples/9p_server_all_transports
```

### For QEMU x86 (TCP + UART only)
```bash
west build -b qemu_x86 samples/9p_server_all_transports
```

## Flashing

```bash
west flash
```

## Testing

### 1. Monitor Serial Console
```bash
west attach
# or
screen /dev/ttyACM0 115200
```

You should see all transports initialize:
```
*** Booting Zephyr OS build v4.1.0 ***
9P All Transports Server
=========================================
Filesystem setup complete
Initializing transports...
UART transport initialized on uart@8000
TCP transport initialized on port 564
Bluetooth initialized
Bluetooth advertising started
L2CAP transport initialized on PSM 0x0009
=========================================
Server ready!
=========================================
UART:  Connect via serial console
TCP:   9p -a tcp!<IP>!564 ls /
L2CAP: Use iOS 9p4i app, PSM 0x0009
=========================================
```

### 2. Test UART Transport
UART transport uses the same serial console, so you'll need a second UART or to temporarily disconnect the console. Most setups use UART for logging, making this transport less practical for simultaneous testing.

### 3. Test TCP Transport
```bash
# List files
9p -a tcp!192.168.1.100!564 ls /

# Read files
9p -a tcp!192.168.1.100!564 read hello.txt
9p -a tcp!192.168.1.100!564 read sys/version
9p -a tcp!192.168.1.100!564 read sys/uptime
```

### 4. Test L2CAP Transport
Using the iOS 9p4i app:
1. Scan for "9P All Transports" device
2. Connect via BLE
3. Open L2CAP channel on PSM 0x0009
4. Send T-version, T-attach, browse filesystem
5. Verify same files as TCP transport

## Configuration

Key Kconfig options (in prj.conf):

```kconfig
# Enable all transports
CONFIG_NINEP_TRANSPORT_UART=y
CONFIG_NINEP_TRANSPORT_TCP=y
CONFIG_NINEP_TRANSPORT_L2CAP=y

# Increased resource limits for multiple servers
CONFIG_NINEP_MAX_FIDS=64    # 3 servers × ~21 fids each
CONFIG_NINEP_MAX_TAGS=32    # 3 servers × ~11 tags each
CONFIG_HEAP_MEM_POOL_SIZE=65536  # Larger heap for 3 servers

# Transport-specific settings
CONFIG_NINEP_L2CAP_PSM=0x0009
CONFIG_NINEP_L2CAP_MTU=4096
```

## Resource Usage

Each server instance requires:
- ~200 bytes server struct
- 8KB RX buffer (CONFIG_NINEP_MAX_MESSAGE_SIZE)
- 8KB TX buffer (CONFIG_NINEP_MAX_MESSAGE_SIZE)
- FID table space
- TAG table space

**Total for 3 servers**: ~48-64KB RAM

The shared filesystem (sysfs) is only allocated once and accessed by all servers.

## Troubleshooting

**TCP transport fails:**
- Check network is up: `net iface`
- Verify IP address is assigned
- Check firewall rules on client

**L2CAP transport fails:**
- Verify Bluetooth initialized successfully
- Check advertising started
- Ensure iOS device has Bluetooth enabled
- Verify PSM matches (0x0009)

**UART transport interferes with logging:**
- UART transport and console share the same UART by default
- For production, use a second UART for 9P transport
- Or disable console logging: `CONFIG_UART_CONSOLE=n`

**Out of memory:**
- Increase `CONFIG_HEAP_MEM_POOL_SIZE`
- Reduce `CONFIG_NINEP_MAX_MESSAGE_SIZE`
- Reduce `CONFIG_NINEP_MAX_FIDS` and `CONFIG_NINEP_MAX_TAGS`

## Use Cases

This sample demonstrates:

1. **Multi-transport access**: Same data available via multiple protocols
2. **Protocol comparison**: Test performance/latency across transports
3. **Failover**: If one transport fails, others remain available
4. **Flexibility**: Clients choose their preferred transport method

## Future Enhancements

- Add board-specific configurations for optimal transport selection
- Implement transport priority/fallback logic
- Add per-transport statistics in /sys/transports/
- Support dynamic transport enable/disable via filesystem

## References

- [UART Transport](../9p_server_uart/)
- [TCP Transport](../9p_server_tcp/)
- [L2CAP Transport](../9p_server_l2cap/)
- [9P Protocol Specification](http://man.cat-v.org/plan_9/5/intro)
