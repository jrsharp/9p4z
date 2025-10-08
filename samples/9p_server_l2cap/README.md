# 9P L2CAP Server Sample

This sample demonstrates a 9P server running over Bluetooth L2CAP.

## Description

The sample creates a 9P server that listens for incoming L2CAP connections on PSM 0x0009. It provides a synthetic filesystem with:

- `/hello.txt` - Welcome message
- `/sys/version` - System version information
- `/sys/uptime` - System uptime
- `/docs/` - Empty documentation directory

## Requirements

- Board with Bluetooth LE support (e.g., nRF52840, nRF5340, etc.)
- iOS device with 9p4i app (9P over L2CAP client)

## Building

```bash
# For nRF52840 DK
west build -b nrf52840dk/nrf52840 samples/9p_server_l2cap

# For nRF5340 DK (application core)
west build -b nrf5340dk/nrf5340/cpuapp samples/9p_server_l2cap
```

## Flashing

```bash
west flash
```

## Testing

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

## Configuration

Key Kconfig options (in prj.conf):

- `CONFIG_NINEP_L2CAP_PSM=0x0009` - L2CAP PSM (must match iOS client)
- `CONFIG_NINEP_L2CAP_MTU=4096` - Maximum L2CAP MTU
- `CONFIG_NINEP_MAX_MESSAGE_SIZE=8192` - Max 9P message size
- `CONFIG_BT_DEVICE_NAME="9P Server"` - Bluetooth device name

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

## References

- [L2CAP Transport Design](../../docs/L2CAP_TRANSPORT_DESIGN.md)
- [iOS Implementation Summary](../../docs/IOS_IMPL_SUMMARY.md)
- [9P Protocol Specification](http://man.cat-v.org/plan_9/5/intro)
