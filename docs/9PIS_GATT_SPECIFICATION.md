# 9P Information Service (9PIS) GATT Specification

**Version:** 1.0
**Status:** Draft
**Author:** 9p4z Contributors
**Date:** 2025-01-XX

## Overview

The 9P Information Service (9PIS) is a standard Bluetooth GATT service that advertises the availability of 9P protocol services on a BLE peripheral device. This service enables automatic discovery and provides essential information for clients to connect to 9P file services.

## Use Cases

- **Mesh Networks**: Enable mobile devices to discover nearby mesh nodes offering 9P services
- **IoT Device Discovery**: Allow devices to advertise their 9P capabilities
- **Service Metadata**: Provide human-readable descriptions and connection details
- **App Discovery**: Direct users to download compatible client applications

## Service Definition

### Service UUID

```
9P Information Service
UUID: 39500001-feed-4a91-ba88-a1e0f6e4c001
```

**Rationale**: Custom 128-bit UUID starting with "9P50" (9P + ASCII 'P' = 0x50) to make it memorable and unique.

### Characteristics

#### 1. Service Description

**UUID:** `39500002-feed-4a91-ba88-a1e0f6e4c001`
**Properties:** Read
**Max Length:** 64 bytes
**Format:** UTF-8 string

Human-readable description of the 9P service.

**Examples:**
- "9P File Server"
- "IoT Device File Share"
- "Mesh Node #42"

#### 2. Service Features

**UUID:** `39500003-feed-4a91-ba88-a1e0f6e4c001`
**Properties:** Read
**Max Length:** 128 bytes
**Format:** UTF-8 string (comma-separated list)

Comma-separated list of available features and capabilities.

**Examples:**
- "file-sharing,messaging,sensor-data"
- "read-only,temperature,diagnostics"
- "read-write,firmware-update,led-control"

#### 3. Transport Information

**UUID:** `39500004-feed-4a91-ba88-a1e0f6e4c001`
**Properties:** Read
**Max Length:** 64 bytes
**Format:** UTF-8 string

Connection parameters for the 9P transport.

**Format:** `<transport>:<parameter>=<value>[,<parameter>=<value>]*`

**Examples:**
- "l2cap:psm=0x0009,mtu=4096"
- "l2cap:psm=0x0080,mtu=8192"

**Parameters:**
- `psm`: L2CAP Protocol/Service Multiplexer (hexadecimal)
- `mtu`: Maximum Transmission Unit in bytes (decimal)

#### 4. App Store Link

**UUID:** `39500005-feed-4a91-ba88-a1e0f6e4c001`
**Properties:** Read
**Max Length:** 256 bytes
**Format:** UTF-8 string (URL)

URL to download a compatible client application.

**Examples:**
- "https://apps.apple.com/app/your-9p-client/idXXXXXXXXX"
- "https://example.com/9p-client"
- "https://9p4z.org/clients"

#### 5. Protocol Version

**UUID:** `39500006-feed-4a91-ba88-a1e0f6e4c001`
**Properties:** Read
**Max Length:** 32 bytes
**Format:** UTF-8 string

9P protocol version and implementation details.

**Format:** `<protocol-version>;<implementation-name>;<implementation-version>`

**Example:**
- "9P2000;9p4z;1.0.0"
- "9P2000.L;custom-server;0.9.5"

## Security Considerations

### Authentication

The 9PIS service is designed for **public discoverability** and should:
- Not require pairing or encryption to read characteristics
- Use BT_SECURITY_L1 (no security requirements)
- Contain only non-sensitive, public information

### Privacy

Implementations should consider:
- Limiting the personal information in service descriptions
- Using generic descriptions for public deployments
- Providing user controls to disable advertising

### Data Integrity

- All characteristics are read-only from the client perspective
- Devices should validate all string data is valid UTF-8
- Maximum lengths should be enforced

## Implementation Guidelines

### Peripheral (Server) Implementation

1. **Register the GATT service** during device initialization
2. **Populate all characteristics** with appropriate values
3. **Include service in advertising data** (optional):
   ```
   BT_DATA_BYTES(BT_DATA_UUID128_SOME, 0x01, 0xc0, 0xe4, ...)
   ```
4. **Keep data static** - no dynamic updates required
5. **Validate UTF-8 encoding** for all strings

### Central (Client) Implementation

1. **Discover devices** with standard BLE scanning
2. **Check for 9PIS UUID** in advertised services (optional optimization)
3. **Connect and discover services**
4. **Read all characteristics** to get service metadata
5. **Parse transport information** to extract PSM/MTU
6. **Display service description** to user
7. **Provide "Get App" button** using App Store Link

### Example Client Flow

```
1. User opens 9P client app
2. App scans for BLE devices
3. For each device:
   a. Check if 9PIS service is present
   b. If present, read Service Description
   c. Display device in "Available Servers" list
4. User taps on "9P File Server"
5. App reads Transport Information ("l2cap:psm=0x0009,mtu=4096")
6. App reads Service Features to show capabilities
7. App opens L2CAP channel on PSM 0x0009
8. App initiates 9P handshake
```

### Example Non-Client Flow

```
1. User's smartphone discovers nearby BLE device
2. Device Settings shows "9P Server" device
3. User taps device, sees:
   - Name: "9P File Server"
   - Description: "File sharing and sensor data"
   - "Download Client App" button (uses App Store Link)
4. User taps button, opens App Store
5. User downloads 9P client app
6. Returns to device, client app now handles connection
```

## Reference Implementation

See:
- `samples/9p_server_l2cap_gatt/` - Complete Zephyr example
- `include/zephyr/9p/gatt_9pis.h` - Reusable service implementation
- `src/gatt_9pis.c` - GATT service registration

## Appendix A: UUID Allocation

The 9PIS UUID namespace is allocated as follows:

```
39500000-feed-4a91-ba88-a1e0f6e4c001  [Reserved for future use]
39500001-feed-4a91-ba88-a1e0f6e4c001  Service UUID
39500002-feed-4a91-ba88-a1e0f6e4c001  Characteristic: Service Description
39500003-feed-4a91-ba88-a1e0f6e4c001  Characteristic: Service Features
39500004-feed-4a91-ba88-a1e0f6e4c001  Characteristic: Transport Information
39500005-feed-4a91-ba88-a1e0f6e4c001  Characteristic: App Store Link
39500006-feed-4a91-ba88-a1e0f6e4c001  Characteristic: Protocol Version
39500007-39500FFF-...                [Reserved for future characteristics]
```

## Appendix B: Feature Names Registry

Standard feature names for the Service Features characteristic:

**File Operations:**
- `file-sharing` - General file sharing
- `read-only` - Read-only file access
- `read-write` - Read and write file access

**Device Control:**
- `led-control` - LED control
- `gpio-control` - General GPIO control
- `firmware-update` - Over-the-air firmware updates

**Sensors:**
- `temperature` - Temperature sensor
- `humidity` - Humidity sensor
- `pressure` - Pressure sensor
- `accelerometer` - Accelerometer data
- `sensor-data` - General sensor data

**Communication:**
- `messaging` - Message board or bulletin board
- `chat` - Real-time chat
- `notifications` - Push notifications

**Mesh:**
- `mesh-routing` - Mesh network routing
- `mesh-relay` - Message relay capabilities

**Custom Features:**
Applications may define custom features using reverse-domain notation:
- `com.example.custom-feature`

## Version History

- **1.0** (2025-01-XX): Initial specification
