# SMP DFU over Bluetooth GATT

The **traditional** firmware-update path — MCUmgr/SMP over a GATT service — built
as a deliberate, controlled twin of [`../9p_dfu_l2cap`](../9p_dfu_l2cap).

Same board. Same MCUboot (`sysbuild/mcuboot.conf` is byte-identical). Same slot
layout. Same signed image. The **only** variable is the update mechanism above
the BLE link:

| | `9p_dfu_l2cap` | `smp_dfu_ble` |
|---|---|---|
| Protocol | 9P2000 | SMP (CBOR) |
| BLE transport | L2CAP CoC, PSM `0x0080` | GATT service `8D53DC1D-…` |
| Flow control | L2CAP credits (transport) | request/response per chunk (application) |
| Segmentation | controller segments the SDU | MCUmgr reassembles ATT writes |
| Upload | `write /dev/firmware` | `img upload` |
| Confirm | `write /dev/confirm` | `img confirm <hash>` |
| Reboot | `write /dev/reboot` | `os reset` |
| Client needs to know | "it's a file" | 2 command groups + CBOR schemas |

This exists so that "GATT or L2CAP?" can be answered with a measurement instead
of an opinion. See **`dectmesh/doc/DFU_COMPARISON.md`** for the method, the
results, and the honest scoring in both directions.

## Fairness

This is a **tuned** mcumgr build, lifted from Zephyr's own
`samples/subsys/mgmt/mcumgr/smp_svr/overlay-bt.conf`: packet reassembly on,
2475-byte netbuf, 498-byte ATT MTU, connection-parameter control. An untuned
mcumgr configuration would make the comparison a strawman.

One deliberate deviation, documented in `prj.conf`: `MCUMGR_TRANSPORT_BT_PERM`
defaults to `RW_AUTHEN` when `BT_SMP=y`, i.e. SMP demands pairing **and**
authentication before a byte moves, while the L2CAP twin runs at sec_level L1.
We force `RW` so both paths are measured over the same link security — otherwise
we would be timing a pairing dance. That default is a point **in SMP's favour**
and is scored as one.

## Building

```bash
# Thingy:91 X (nRF5340 app core) — the reference bench target.
#
# SB_CONF_FILE replaces sysbuild.conf, adding the net-core HCI controller image
# (the nRF5340's Bluetooth host needs a controller on the network core).
#
# ZEPHYR_EXTRA_MODULES is required when building from an NCS workspace that
# does not already list 9p4z as a manifest module — which is the normal case if
# you build out of /opt/nordic/ncs/<version>. This sample uses no 9P code, but
# the 9P twin does, and keeping the flag identical keeps the twins comparable.
P9Z=/path/to/9p4z
cd /opt/nordic/ncs/v3.3.0          # west resolves its workspace from the cwd
west build -b thingy91x/nrf5340/cpuapp --sysbuild "$P9Z/samples/smp_dfu_ble" \
  -- -DZEPHYR_EXTRA_MODULES="$P9Z" \
     -DSB_CONF_FILE="$P9Z/samples/smp_dfu_ble/sysbuild_thingy91x.conf"

# nRF52840 DK
west build -b nrf52840dk/nrf52840 --sysbuild "$P9Z/samples/smp_dfu_ble" \
  -- -DZEPHYR_EXTRA_MODULES="$P9Z"
```

Build the 9P twin the same way, swapping the sample name, and the two are
directly comparable. `dectmesh/tools/dfu_bench/footprint.sh twins` does both and
reports the delta.

> Omitting `ZEPHYR_EXTRA_MODULES` fails the 9P twin with
> `attempt to assign the value 'y' to the undefined symbol NINEP` — 9p4z's
> Kconfig is simply not in the tree.

## Usage

Any SMP client works. On Nordic silicon the most reliable route is **nRF Connect
Device Manager** (iOS/Android), which also reports upload throughput directly.
From a host, `mcumgr` (Go) or `smpmgr` (Python) — check your tool's `--help`, the
CLIs differ.

```
image upload <zephyr.signed.bin>   # transfer
image confirm <hash>               # make permanent
os reset                           # apply
```

## Instrumentation

The board times its own transfer and prints one line to the console:

```
BENCH mech=smp-gatt event=start
BENCH mech=smp-gatt event=complete bytes=305152 ms=41230 Bps=7401
```

`9p_dfu_l2cap` emits the identical format with `mech=9p-l2cap`. The clock starts
when the first upload byte is accepted (which is also when the secondary-slot
erase begins — `IMG_ERASE_PROGRESSIVELY=n` in both) and stops at completion.

Timing **on the device** is deliberate: it removes host scan/connect/pair
overhead and, more importantly, makes the result independent of which host tool
you managed to get working — the largest source of doubt in published DFU
benchmarks.

On Thingy:91 X the console is RTT (the board default; the 5340's `uart0` is the
inter-chip link to the nRF9151, so it must not be used for console).
