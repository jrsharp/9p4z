# Feasibility & Scope Analysis: 9P for Zephyr

**Status:** FEASIBLE
**Date:** September 30, 2025
**Estimated Timeline:** 4-6 months (17-25 weeks)

## Executive Summary

Implementing 9P for Zephyr is achievable and well-scoped. The protocol's simplicity aligns well with embedded constraints, and Zephyr provides the necessary networking infrastructure.

---

## 1. Protocol Analysis

### 9P2000 Core Requirements
- **14 message types** (version, auth, attach, walk, open, read, write, clunk, remove, stat, wstat, create, error, flush)
- **Simple wire format**: 4-byte size + 1-byte type + 2-byte tag + parameters
- **Stateful protocol**: Requires fid (file ID) and tag management
- **Little-endian byte order**

### Complexity Assessment
**LOW-MEDIUM**
- Protocol is intentionally minimal and well-documented
- No complex state machines or heavy cryptography required
- Authentication can be optional for initial implementation

---

## 2. Zephyr Capabilities Assessment

### ✅ IPv4/IPv6 Support
- Full TCP/UDP stack via BSD sockets API
- Dual-stack support (IPv4 + IPv6 simultaneously)
- **Transport:** TCP recommended for reliability, UDP possible for constrained environments

### ✅ Bluetooth L2CAP Support
- Native L2CAP implementation in Bluetooth stack
- IPSP (Internet Protocol Support Profile) for IPv6 over L2CAP
- Connection-oriented channels available

### ✅ UART Support
- Three access modes: polling, interrupt-driven, asynchronous (DMA)
- Well-documented API with examples
- Suitable for point-to-point 9P connections

### ✅ Thread (IEEE 802.15.4)
- Supported via networking stack
- IPv6 over Thread mesh networking
- Good fit for IoT scenarios

---

## 3. Memory & Resource Constraints

### Estimated Footprint
- **Core 9P library:** 5-10 KB ROM (protocol handling, message parsing)
- **Transport adapters:** 2-4 KB ROM each
- **Runtime RAM:** 2-5 KB (buffers, fid/tag tables)
- **Network stack overhead:** 8-20 KB (already included in Zephyr)

### Target Hardware
- **Minimum:** 64 KB Flash, 16 KB RAM
- **Recommended:** 128+ KB Flash, 32+ KB RAM
- **Compatible with:** Most modern MCUs (ARM Cortex-M3+, ESP32, nRF52, etc.)

---

## 4. Existing Reference Implementations

### c9 library (https://sr.ht/~ft/c9/)
- ✅ Designed for low-resource MCUs
- ✅ Zero external dependencies
- ✅ No dynamic memory allocation
- ✅ Portable (no POSIX requirement)
- ⚠️ Under active development (API changes)

**Strategy:** Use c9 as reference or starting point, adapt for Zephyr conventions

---

## 5. Phased Implementation Roadmap

### Phase 1: Core Protocol Library (4-6 weeks)
- Message serialization/deserialization
- Tag and fid management
- Error handling
- Unit tests (native_posix)

### Phase 2: Transport Abstraction (2-3 weeks)
- Generic transport interface
- UART implementation (simplest)
- Integration tests

### Phase 3: Network Transports (4-6 weeks)
- TCP/IPv4 implementation
- TCP/IPv6 implementation
- Sample applications

### Phase 4: Advanced Transports (4-6 weeks)
- Bluetooth L2CAP implementation
- Thread/802.15.4 support
- Performance optimization

### Phase 5: Client & Server Utilities (3-4 weeks)
- File system abstraction layer
- VFS integration (optional)
- Documentation & examples

**Total Estimated Timeline:** 17-25 weeks (4-6 months)

---

## 6. Technical Challenges

### Medium Risk
1. **Memory management** - Fixed buffer allocation vs. dynamic messages
   - *Mitigation:* Use configurable max message size (typically 8KB)

2. **Multi-transport abstraction** - Clean API for different transports
   - *Mitigation:* Simple callback-based transport interface

### Low Risk
3. **Authentication** - May need custom implementations
   - *Mitigation:* Start without auth, add later

4. **Testing** - Need diverse hardware targets
   - *Mitigation:* Use QEMU, native_posix, and one physical board

---

## 7. Value Proposition

### Why 9P on Zephyr is Useful
- ✅ **Unified file access** across diverse embedded devices
- ✅ **Protocol flexibility** - works over any byte stream transport
- ✅ **Simpler than NFS/SMB** for embedded use cases
- ✅ **Plan 9 ecosystem integration** for distributed systems
- ✅ **Remote debugging/configuration** via file interface
- ✅ **IoT applications** - expose sensor data as files

### Use Cases
- Remote filesystem access for embedded devices
- Device configuration/management via 9P files
- Sensor data exposure (read /sensors/temperature)
- Distributed IoT systems with uniform access
- Embedded Linux device development/debugging

---

## 8. Recommendations

### PROCEED with the following approach

1. **Start minimal:** Focus on core protocol + UART transport first
2. **Leverage c9:** Study c9 implementation, possibly fork/adapt it
3. **Zephyr module structure:** Create proper Zephyr module with Kconfig
4. **Test-driven:** Build comprehensive test suite early
5. **Document as you go:** API docs and usage examples
6. **Community engagement:** Engage Zephyr community early for feedback

### Quick Wins
- UART-based 9P can be working in 2-3 weeks
- Proof-of-concept demo: Mount embedded device filesystem over UART
- TCP/IP support adds broad applicability (4-6 weeks more)

### Defer to Later
- Authentication (start with unauthenticated 9P)
- 9P2000.u or 9P2000.L extensions (stick to 9P2000 base)
- VFS integration (nice-to-have, not essential)

---

## 9. Implementation Strategy

### Initial Focus (Weeks 1-8)
1. Create basic project structure (CMakeLists.txt, Kconfig, module.yml)
2. Implement message parser (9P protocol basics)
3. Build UART echo server (simple proof of concept)
4. Iterate with basic file operations

### Key Success Metrics
- Can exchange 9P messages over UART
- Can perform basic file operations (read/write)
- Memory footprint within targets
- Unit test coverage >80%