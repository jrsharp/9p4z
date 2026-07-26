9P for Zephyr
====

A clean, modern 9P protocol library implementation for Zephyr RTOS.

## Why 9P? Why Zephyr?

9P makes everything a file -- sensors, configuration, control -- behind one
tiny protocol: a handful of fixed-layout messages, no JSON, no schemas, no
code generation. Its embedded pedigree is real, not retrofitted: Bell Labs'
Inferno OS used 9P (as Styx) as its universal system interface on set-top
boxes and network appliances with under a megabyte of memory. The protocol
fits those constraints naturally -- messages parse in place with fixed-size
resource tables and zero dynamic allocation, and the same wire format works
over any byte stream.  Use existing host tools on Linux, macOS, Plan 9, etc.
(`mount -t 9p`, `socat`, plan9port, etc.) -- `cat /sensors/temperature`
-- no custom client, no SDK, no app -- it's files all the way down!

Zephyr is the natural host:  its transport diversity (UART, TCP, Bluetooth
L2CAP, 802.15.4/Thread) pairs perfectly with 9P's transport-agnostic
design, so one filesystem tree serves identically over a serial cable or a
mesh network.  And Zephyr's Kconfig-everything philosophy matches 9P's
minimalism — you compile in exactly the transports and table sizes you
need, and nothing else.

Not to mention:

- **Multiplexing built in** -- tagged messages mean one connection carries
  any number of concurrent operations on any number of files, so a single
  L2CAP channel replaces a whole zoo of GATT characteristics (and streams
  real data, not attribute-sized nibbles).
- **Scripting and testing made easy** -- `echo 1 > /leds/status` works from
  any shell, so run your device's filesystem on `native_sim`, mount it from
  your dev box with the stock Linux client, and drive integration tests
  with plain shell commands, no hardware in the loop.
- **Namespaces that compose** -- a gateway can mount fifty devices and
  re-export one tree.
- **Alive and well** -- 9P is in the mainline Linux kernel, QEMU, and WSL2.
- **Debuggable on the wire** -- a fixed little-endian layout you can read
  from a hexdump.

Thirty years ago, Bell Labs engineers imagined computing as a grid: CPU
servers, file servers, and terminals scattered across a network, stitched
into one seamless namespace by 9P. That idea quietly shaped much of what
followed. Swap in sensors, actuators, and gateways, and it describes
exactly what an embedded fleet wants to be -- this library is a small
attempt to carry their vision to the edge.

## Features

- ✅ **9P2000 Protocol** - Full message parsing and serialization
- ✅ **Multiple Transports** - UART, TCP/IPv4, TCP/IPv6, Bluetooth L2CAP, Thread
- ✅ **Zephyr Native** - Proper module integration with Kconfig
- ✅ **Comprehensive Tests** - 31 test cases with 100% core coverage
- ✅ **CI/CD Ready** - GitHub Actions integration

## Transport Support

- ✅ **UART** - Serial transport with interrupt-driven RX
- ✅ **TCP/IPv4** - Network transport with server and client support
- ✅ **TCP/IPv6** - IPv6 network transport
- ✅ **Bluetooth L2CAP** - Bluetooth transport with connection-oriented channels
- ✅ **Thread/OpenThread** - Mesh network transport over 802.15.4

## Quick Start

```bash
# Clone the repository
git clone https://github.com/YOUR_USERNAME/9p4z.git
cd 9p4z

# Run setup script (creates clean workspace)
./scripts/setup-workspace.sh

# Activate environment
cd ../9p4z-workspace
source activate.sh

# Run tests (macOS: use qemu_x86, Linux: use native_posix)
west build -b qemu_x86 9p4z/tests
west build -t run

# Build sample
west build -b qemu_x86 9p4z/samples/uart_echo
```

See [QUICKSTART.md](QUICKSTART.md) for detailed setup instructions.

## Documentation

- **[QUICKSTART.md](QUICKSTART.md)** - Setup and getting started
- **[FEASIBILITY.md](FEASIBILITY.md)** - Project analysis and roadmap
- **[tests/README.md](tests/README.md)** - Testing guide

## Use Cases

- Remote filesystem access for embedded devices
- Device configuration/management via 9P
- Sensor data exposure (e.g., `/sensors/temperature`)
- Distributed IoT systems with uniform file-like interface
- Embedded Linux device development and debugging

## License

MIT License - see [LICENSE](LICENSE) file for details.  Third-party
attribution and authorship notes in [NOTICE](NOTICE).

---

Copyright (c) 2025-2026 Jon Sharp.

Built in substantial collaboration with Claude (Anthropic) — see the
"Development and authorship" section of [NOTICE](NOTICE).
