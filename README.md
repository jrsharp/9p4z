9P for Zephyr
====

A clean, modern 9P protocol library implementation for Zephyr RTOS.

## Features

- ✅ **9P2000 Protocol** - Full message parsing and serialization
- ✅ **Multiple Transports** - UART, TCP/IP (coming), Bluetooth L2CAP (coming)
- ✅ **Zephyr Native** - Proper module integration with Kconfig
- ✅ **No Dynamic Allocation** - Fixed resource tables for embedded systems
- ✅ **Comprehensive Tests** - 31 test cases with 100% core coverage
- ✅ **CI/CD Ready** - GitHub Actions integration

## Transport Support

- ✅ **UART** - Serial transport with interrupt-driven RX
- ⏳ **TCP/IPv4** - Network transport (Phase 3)
- ⏳ **TCP/IPv6** - IPv6 network transport (Phase 3)
- ⏳ **Bluetooth L2CAP** - Bluetooth transport (Phase 4)
- ⏳ **Thread/802.15.4** - Mesh network transport (Phase 4)

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
- **[CLAUDE.md](CLAUDE.md)** - Development guide and architecture
- **[tests/README.md](tests/README.md)** - Testing guide

## Use Cases

- Remote filesystem access for embedded devices
- Device configuration/management via 9P
- Sensor data exposure (e.g., `/sensors/temperature`)
- Distributed IoT systems with uniform file-like interface
- Embedded Linux device development and debugging

## License

Apache-2.0
