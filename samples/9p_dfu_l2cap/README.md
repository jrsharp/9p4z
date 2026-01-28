# 9P DFU over Bluetooth L2CAP

Minimal firmware update server using 9P filesystem over Bluetooth.
Suitable for ZMK keyboards and other BLE devices.

## Features

- Firmware update via 9P filesystem (`/dev/firmware`)
- MCUboot integration with image swap/revert
- Remote reboot (`/dev/reboot`)
- Image confirmation (`/dev/confirm`)
- 9PIS GATT service for client discovery
- Minimal footprint (~30KB code)

## Building

### For nRF52840 DK

```bash
west build -b nrf52840dk/nrf52840 samples/9p_dfu_l2cap --sysbuild
west flash
```

### For nice!nano (ZMK keyboard controller)

```bash
west build -b nice_nano_v2 samples/9p_dfu_l2cap --sysbuild
# Double-tap reset to enter UF2 bootloader, then:
cp build/9p_dfu_l2cap/zephyr/zephyr.uf2 /Volumes/NICENANO/
```

## Usage

### From macOS/iOS (with 9P client)

```bash
# Connect via Bluetooth, then:

# Check DFU status
9p -a 'bt!9P DFU!0080' read /dev/firmware

# Upload new firmware
cat app_update.bin | 9p -a 'bt!9P DFU!0080' write /dev/firmware

# Confirm the new image works
echo 1 | 9p -a 'bt!9P DFU!0080' write /dev/confirm

# Reboot to apply
echo 1 | 9p -a 'bt!9P DFU!0080' write /dev/reboot
```

### DFU Workflow

1. **Upload**: Write firmware binary to `/dev/firmware`
2. **Reboot**: Write to `/dev/reboot` to restart
3. **Test**: Device boots new image in "test" mode
4. **Confirm**: Write to `/dev/confirm` to make permanent
5. If not confirmed, next reboot reverts to previous image

## Files Exposed

| Path | Mode | Description |
|------|------|-------------|
| `/dev/firmware` | RW | Write: upload firmware. Read: status |
| `/dev/reboot` | W | Write anything to reboot |
| `/dev/confirm` | W | Write to confirm running image |
| `/name` | R | Device name |

## Status Format

Reading `/dev/firmware` returns:

```
state idle
current 1.2.3+0
confirmed yes
```

During upload:
```
state receiving
bytes 153600
current 1.2.3+0
confirmed yes
```

## ZMK Integration

To add 9P DFU to an existing ZMK keyboard:

### 1. Add 9p4z as a module

In your `west.yml`:

```yaml
manifest:
  projects:
    - name: 9p4z
      url: https://github.com/jrsharp/9p4z
      revision: main
      path: modules/9p4z
```

### 2. Add to your board's Kconfig

```kconfig
# In your keyboard's Kconfig.defconfig
config NINEP_DFU
    default y

config NINEP_TRANSPORT_L2CAP
    default y
```

### 3. Add to your prj.conf

```conf
# 9P DFU support
CONFIG_NINEP=y
CONFIG_NINEP_SERVER=y
CONFIG_NINEP_TRANSPORT_L2CAP=y
CONFIG_NINEP_DFU=y

# MCUboot (if not already enabled)
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_IMG_MANAGER=y
```

### 4. Initialize in your code

```c
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/dfu.h>

static struct ninep_dfu dfu;

void init_dfu(struct ninep_sysfs *sysfs) {
    ninep_dfu_init(&dfu, sysfs, NULL);
}
```

## Signing Firmware Images

MCUboot requires signed images. Generate keys and sign:

```bash
# Generate signing key (once)
imgtool keygen -k my_key.pem -t ecdsa-p256

# Sign firmware for upload
imgtool sign --key my_key.pem \
    --header-size 0x200 \
    --slot-size 0x6e000 \
    --version 1.2.3 \
    zephyr.bin app_update.bin
```

## Memory Usage (nRF52840)

| Component | Flash | RAM |
|-----------|-------|-----|
| MCUboot | ~24KB | ~8KB |
| 9P Server | ~12KB | ~2KB |
| L2CAP Transport | ~4KB | ~1KB |
| DFU Module | ~3KB | ~1KB |
| Sysfs | ~2KB | <1KB |
| **Total 9P** | **~21KB** | **~4KB** |

Plus application code and Bluetooth stack (~50KB flash, ~20KB RAM).

## Troubleshooting

### "Image not confirmed" warning on boot

The device booted a new image but it hasn't been confirmed yet.
Write to `/dev/confirm` or call `ninep_dfu_confirm()` in code.

### DFU upload fails

- Check image is properly signed
- Verify slot sizes match your partition layout
- Ensure enough heap for buffered writes (16KB minimum)

### Cannot connect via Bluetooth

- Device may be bonded to another host
- Try unpairing and re-pairing
- Check BT_MAX_CONN allows another connection

## License

MIT
