# 9P Server over OpenThread

This sample demonstrates a 9P filesystem server running over OpenThread mesh networking. It showcases how to use the existing TCP transport layer over Thread's IPv6 connectivity to provide filesystem access across a Thread mesh network.

## Prerequisites

**This sample requires OpenThread module support in your Zephyr workspace.**

To enable OpenThread, follow the [Zephyr OpenThread documentation](https://docs.zephyrproject.org/latest/connectivity/networking/api/openthread.html) and ensure the OpenThread module is properly integrated in your west workspace.

## Architecture

```
┌─────────────┐
│ 9P Protocol │  Application layer (9P2000)
└──────┬──────┘
       │
┌──────┴──────┐
│TCP Transport│  Uses existing ninep_tcp_transport
└──────┬──────┘
       │
┌──────┴──────┐
│    IPv6     │  Thread provides IPv6 networking
└──────┬──────┘
       │
┌──────┴──────┐
│ OpenThread  │  Thread mesh protocol
└──────┬──────┘
       │
┌──────┴──────┐
│ IEEE802.15.4│  Physical/MAC layer (2.4 GHz radio)
└─────────────┘
```

## Key Features

- ✅ **No custom transport needed** - Reuses existing TCP transport
- ✅ **Standard 9P protocol** - Compatible with any 9P client
- ✅ **Thread mesh networking** - Low-power, self-healing mesh
- ✅ **IPv6 routing** - Works with Thread border routers
- ✅ **OpenThread shell** - Runtime configuration and diagnostics

## Supported Boards

Any Zephyr board with IEEE 802.15.4 radio support:

- **nRF52840 DK** (`nrf52840dk_nrf52840`)
- **nRF5340 DK** (`nrf5340dk_nrf5340_cpuapp`)
- **Nordic Thingy:53** (`thingy53_nrf5340_cpuapp`)
- **ESP32-C6** (`esp32c6_devkitc`)
- **ESP32-H2** (`esp32h2_devkitm`)
- **Any board with 802.15.4 radio**

## Building

### For nRF52840 DK:
```bash
west build -b nrf52840dk_nrf52840 samples/9p_server_thread
west flash
```

### For nRF5340 DK:
```bash
west build -b nrf5340dk_nrf5340_cpuapp samples/9p_server_thread
west flash
```

### For ESP32-C6:
```bash
west build -b esp32c6_devkitc samples/9p_server_thread
west flash
```

## Usage

### 1. Form a Thread Network

After flashing, use the OpenThread CLI to form or join a network:

```
9p-thread:~$ ot dataset init new
Done
9p-thread:~$ ot dataset commit active
Done
9p-thread:~$ ot ifconfig up
Done
9p-thread:~$ ot thread start
Done
```

Wait a few seconds for the device to attach to the network.

### 2. Check Network Status

```
9p-thread:~$ ot state
leader
Done

9p-thread:~$ ot ipaddr
fd11:22::1
fe80::1234:5678:9abc:def0
Done
```

Note your IPv6 address (the `fd11:22::...` address).

### 3. Connect from a 9P Client

From another device on the Thread mesh or connected via a border router:

```bash
# List root directory
9p -a tcp![fd11:22::1]!564 ls /

# Read a file
9p -a tcp![fd11:22::1]!564 read /hello.txt

# Read Thread info
9p -a tcp![fd11:22::1]!564 read /thread/role

# Read system uptime
9p -a tcp![fd11:22::1]!564 read /sys/uptime
```

## Filesystem Contents

### Demo Files
- `/hello.txt` - Static greeting message
- `/readme.txt` - Information about the server
- `/mesh/node1.txt` - Mesh demo file 1
- `/mesh/node2.txt` - Mesh demo file 2

### System Information (Dynamic)
- `/sys/uptime` - System uptime
- `/sys/version` - Kernel version
- `/sys/board` - Board name

### Thread Information (Dynamic)
- `/thread/role` - Thread device role (Leader/Router/Child)
- `/thread/rloc` - Routing locator address
- `/thread/network` - Network configuration

## Multi-Device Setup

### Create a Thread Mesh with Multiple 9P Servers

1. **Device 1** (Leader):
   ```
   ot dataset init new
   ot dataset commit active
   ot ifconfig up
   ot thread start
   ```

2. **Device 2** (Router/Child):
   ```
   # Get network credentials from Device 1:
   ot dataset active -x
   # Copy the hex string, then on Device 2:
   ot dataset set active <hex_string>
   ot ifconfig up
   ot thread start
   ```

3. Both devices run 9P servers on port 564
4. Connect to either device using their respective IPv6 addresses

## Thread Border Router Integration

To connect from outside the Thread network:

1. **Setup Border Router** - Use a Thread border router (e.g., Nordic dev kit with Border Router firmware)
2. **Route IPv6** - The border router will route packets between Thread and Ethernet/WiFi
3. **Connect** - Connect from any device on your LAN:
   ```bash
   9p -a tcp![fd11:22::1]!564 ls /
   ```

## Configuration

### Thread Network Settings

Edit `prj.conf` to customize:

```kconfig
# Network name
CONFIG_OPENTHREAD_NETWORK_NAME="9P-Thread-Mesh"

# Channel (11-26 for 2.4GHz)
CONFIG_OPENTHREAD_CHANNEL=11

# PAN ID
CONFIG_OPENTHREAD_PANID=4660
```

### Device Role

- **FTD (Full Thread Device)** - Can route packets (default)
  ```kconfig
  CONFIG_OPENTHREAD_FTD=y
  CONFIG_OPENTHREAD_MTD=n
  ```

- **MTD (Minimal Thread Device)** - Low power, end device only
  ```kconfig
  CONFIG_OPENTHREAD_FTD=n
  CONFIG_OPENTHREAD_MTD=y
  CONFIG_OPENTHREAD_MTD_SED=y  # Sleepy End Device
  ```

### 9P Configuration

```kconfig
CONFIG_NINEP_MAX_MESSAGE_SIZE=8192  # Max message size
CONFIG_NINEP_MAX_FIDS=32            # Max open files
CONFIG_NINEP_MAX_TAGS=16            # Max pending requests
```

## Troubleshooting

### Device not getting IPv6 address
```
# Check interface status
ot ifconfig

# Restart Thread
ot thread stop
ot thread start
```

### Cannot connect to 9P server
```
# Verify server is running (check logs)
# Verify IPv6 address is correct
ot ipaddr

# Check if port 564 is reachable
# Try ping6 from client device
```

### Low signal strength
```
# Check TX power
ot txpower

# Set higher TX power (in dBm)
ot txpower 20
```

### Network not forming
```
# Check channel is not congested
ot scan

# Try different channel
ot channel 15
ot ifconfig up
ot thread start
```

## Performance Considerations

### Thread Constraints
- **MTU**: ~1280 bytes (IPv6 over 802.15.4)
- **Bandwidth**: ~250 kbps (802.15.4)
- **Latency**: Higher than WiFi/Ethernet due to mesh routing
- **Range**: Typically 10-30m indoors, better with multiple routers

### Optimization Tips
1. **Keep messages small** - Thread works best with small messages
2. **Use multiple routers** - Improves mesh reliability
3. **Reduce message size** - Consider `CONFIG_NINEP_MAX_MESSAGE_SIZE=4096`
4. **Connection pooling** - Reuse TCP connections when possible

## Example Use Cases

### IoT Sensor Mesh
```
Sensor Nodes → Thread Mesh → Border Router → Cloud
     ↑
  9P Server (exposes sensor data as files)
```

### Home Automation
```
Smart Devices → Thread Network → Home Hub
     ↑
  9P Server (configuration and status files)
```

### Embedded Device Management
```
Embedded Nodes → Thread Mesh → Gateway
     ↑
  9P Server (logs, config, firmware updates)
```

## References

- [OpenThread Documentation](https://openthread.io/)
- [Thread Specification](https://www.threadgroup.org/)
- [Zephyr Thread Guide](https://docs.zephyrproject.org/latest/connectivity/networking/api/openthread.html)
- [9P Protocol](http://9p.cat-v.org/)
- [Plan 9 Documentation](https://9p.io/plan9/)

## License

MIT License - See project root LICENSE file
