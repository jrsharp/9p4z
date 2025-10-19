# iOS Integration Guide: 9P Information Service (9PIS)

This guide shows iOS developers how to integrate 9PIS discovery into their apps
to automatically find and connect to 9P servers over Bluetooth.

## Overview

The 9P Information Service (9PIS) is a standard GATT service that advertises
the availability of 9P services on BLE devices. By implementing 9PIS support
in your iOS app, you can:

- **Auto-discover** 9P-capable devices during BLE scanning
- **Read metadata** about available services (features, transport details)
- **Auto-connect** by parsing transport information (PSM, MTU)
- **Direct users** to download your app via App Store links

## Prerequisites

Your iOS app needs:
- `CoreBluetooth` framework
- `Info.plist` entry for `NSBluetoothAlwaysUsageDescription`
- iOS 13.0 or later (for L2CAP support)

## Quick Start

### 1. Define 9PIS UUIDs

Add these constants to your Swift project:

```swift
import CoreBluetooth

// 9P Information Service UUID
let NINEP_INFO_SERVICE_UUID = CBUUID(string: "39500001-FEED-4A91-BA88-A1E0F6E4C001")

// Characteristic UUIDs
let NINEP_SERVICE_DESC_UUID = CBUUID(string: "39500002-FEED-4A91-BA88-A1E0F6E4C001")
let NINEP_SERVICE_FEATURES_UUID = CBUUID(string: "39500003-FEED-4A91-BA88-A1E0F6E4C001")
let NINEP_TRANSPORT_INFO_UUID = CBUUID(string: "39500004-FEED-4A91-BA88-A1E0F6E4C001")
let NINEP_APP_STORE_LINK_UUID = CBUUID(string: "39500005-FEED-4A91-BA88-A1E0F6E4C001")
let NINEP_PROTOCOL_VERSION_UUID = CBUUID(string: "39500006-FEED-4A91-BA88-A1E0F6E4C001")
```

### 2. Scan for 9PIS Devices

```swift
class NinePDiscovery: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var centralManager: CBCentralManager!
    var discoveredDevices: [CBPeripheral: NinePServiceInfo] = [:]

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    func startScanning() {
        // Scan for devices with 9PIS service (optional - faster discovery)
        centralManager.scanForPeripherals(
            withServices: [NINEP_INFO_SERVICE_UUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )

        // Alternative: Scan for all devices and filter by service
        // centralManager.scanForPeripherals(withServices: nil, options: nil)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            startScanning()
        }
    }

    func centralManager(_ central: CBCentralManager,
                       didDiscover peripheral: CBPeripheral,
                       advertisementData: [String : Any],
                       rssi RSSI: NSNumber) {

        print("Discovered: \(peripheral.name ?? "Unknown")")

        // Store peripheral and connect to read 9PIS data
        discoveredDevices[peripheral] = nil
        peripheral.delegate = self
        centralManager.connect(peripheral, options: nil)
    }
}
```

### 3. Read 9PIS Characteristics

```swift
struct NinePServiceInfo {
    var serviceDescription: String = ""
    var features: [String] = []
    var transportInfo: TransportInfo?
    var appStoreLink: String = ""
    var protocolVersion: String = ""
}

struct TransportInfo {
    var type: String  // "l2cap"
    var psm: UInt16
    var mtu: Int
}

extension NinePDiscovery {
    func centralManager(_ central: CBCentralManager,
                       didConnect peripheral: CBPeripheral) {
        print("Connected to \(peripheral.name ?? "Unknown")")

        // Discover 9PIS service
        peripheral.discoverServices([NINEP_INFO_SERVICE_UUID])
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }

        for service in services {
            if service.uuid == NINEP_INFO_SERVICE_UUID {
                print("Found 9PIS service!")

                // Discover all characteristics
                peripheral.discoverCharacteristics([
                    NINEP_SERVICE_DESC_UUID,
                    NINEP_SERVICE_FEATURES_UUID,
                    NINEP_TRANSPORT_INFO_UUID,
                    NINEP_APP_STORE_LINK_UUID,
                    NINEP_PROTOCOL_VERSION_UUID
                ], for: service)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didDiscoverCharacteristicsFor service: CBService,
                   error: Error?) {
        guard let characteristics = service.characteristics else { return }

        // Read all characteristics
        for characteristic in characteristics {
            peripheral.readValue(for: characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didUpdateValueFor characteristic: CBCharacteristic,
                   error: Error?) {
        guard let data = characteristic.value,
              let stringValue = String(data: data, encoding: .utf8) else {
            return
        }

        var info = discoveredDevices[peripheral] ?? NinePServiceInfo()

        switch characteristic.uuid {
        case NINEP_SERVICE_DESC_UUID:
            info.serviceDescription = stringValue
            print("Description: \(stringValue)")

        case NINEP_SERVICE_FEATURES_UUID:
            info.features = stringValue.components(separatedBy: ",")
            print("Features: \(info.features)")

        case NINEP_TRANSPORT_INFO_UUID:
            info.transportInfo = parseTransportInfo(stringValue)
            print("Transport: \(stringValue)")

        case NINEP_APP_STORE_LINK_UUID:
            info.appStoreLink = stringValue
            print("App Link: \(stringValue)")

        case NINEP_PROTOCOL_VERSION_UUID:
            info.protocolVersion = stringValue
            print("Version: \(stringValue)")

        default:
            break
        }

        discoveredDevices[peripheral] = info

        // Once we have all the info, we can connect to 9P service
        if let transportInfo = info.transportInfo {
            connectTo9PService(peripheral: peripheral,
                              transportInfo: transportInfo)
        }
    }
}
```

### 4. Parse Transport Information

```swift
extension NinePDiscovery {
    func parseTransportInfo(_ info: String) -> TransportInfo? {
        // Format: "l2cap:psm=0x0009,mtu=4096"
        let components = info.components(separatedBy: ":")
        guard components.count == 2,
              components[0] == "l2cap" else {
            return nil
        }

        let params = components[1].components(separatedBy: ",")
        var psm: UInt16 = 0
        var mtu: Int = 0

        for param in params {
            let kv = param.components(separatedBy: "=")
            guard kv.count == 2 else { continue }

            let key = kv[0].trimmingCharacters(in: .whitespaces)
            let value = kv[1].trimmingCharacters(in: .whitespaces)

            switch key {
            case "psm":
                // Parse hex value (e.g., "0x0009")
                if value.hasPrefix("0x") {
                    let hexString = String(value.dropFirst(2))
                    psm = UInt16(hexString, radix: 16) ?? 0
                } else {
                    psm = UInt16(value) ?? 0
                }

            case "mtu":
                mtu = Int(value) ?? 0

            default:
                break
            }
        }

        guard psm > 0, mtu > 0 else { return nil }

        return TransportInfo(type: "l2cap", psm: psm, mtu: mtu)
    }
}
```

### 5. Connect to 9P Service via L2CAP

```swift
extension NinePDiscovery {
    func connectTo9PService(peripheral: CBPeripheral,
                           transportInfo: TransportInfo) {
        print("Opening L2CAP channel on PSM \(transportInfo.psm)...")

        // Open L2CAP channel
        peripheral.openL2CAPChannel(CBL2CAPPSM(transportInfo.psm))
    }

    func peripheral(_ peripheral: CBPeripheral,
                   didOpen channel: CBL2CAPChannel?,
                   error: Error?) {
        guard let channel = channel else {
            print("Failed to open L2CAP channel: \(error?.localizedDescription ?? "unknown")")
            return
        }

        print("L2CAP channel opened! MTU: \(channel.outputStream.streamStatus)")

        // Now you can use the channel to send 9P messages
        // See iOS_IMPL_SUMMARY.md for 9P protocol implementation

        // Example: Send T-version
        send9PVersion(channel: channel)
    }

    func send9PVersion(channel: CBL2CAPChannel) {
        // 9P T-version message construction
        let version = "9P2000"
        let msize: UInt32 = 8192

        // Build message (simplified - see full implementation in iOS_IMPL_SUMMARY.md)
        var message = Data()

        // Size (will be filled in after building message)
        message.append(contentsOf: [0, 0, 0, 0])

        // Type: T-version (100)
        message.append(100)

        // Tag: NOTAG (0xFFFF)
        message.append(contentsOf: [0xFF, 0xFF])

        // Msize (4 bytes, little-endian)
        var msizeLE = msize.littleEndian
        message.append(Data(bytes: &msizeLE, count: 4))

        // Version string (2-byte length + string)
        let versionData = version.data(using: .utf8)!
        var versionLen = UInt16(versionData.count).littleEndian
        message.append(Data(bytes: &versionLen, count: 2))
        message.append(versionData)

        // Update size field
        var totalSize = UInt32(message.count).littleEndian
        message.replaceSubrange(0..<4, with: Data(bytes: &totalSize, count: 4))

        // Send message
        _ = channel.outputStream.write(Array(message), maxLength: message.count)

        print("Sent T-version message")
    }
}
```

## Complete Example App Structure

```
NinePClientApp/
├── Models/
│   ├── NinePServiceInfo.swift      # 9PIS service data model
│   └── TransportInfo.swift         # Transport configuration
├── Discovery/
│   ├── NinePDiscovery.swift        # BLE scanning & 9PIS reading
│   └── NinePConnection.swift       # L2CAP connection & 9P protocol
├── UI/
│   ├── DeviceListView.swift        # Show discovered devices
│   └── FileSystemView.swift        # Browse 9P filesystem
└── Info.plist
```

## Best Practices

### 1. Cache Service Information

Once you've read the 9PIS characteristics, cache them:

```swift
// Save to UserDefaults or CoreData
func cacheServiceInfo(_ info: NinePServiceInfo,
                     for peripheral: CBPeripheral) {
    let key = "9pis_\(peripheral.identifier.uuidString)"
    let encoder = JSONEncoder()
    if let encoded = try? encoder.encode(info) {
        UserDefaults.standard.set(encoded, forKey: key)
    }
}
```

### 2. Handle Connection Errors Gracefully

```swift
func centralManager(_ central: CBCentralManager,
                   didFailToConnect peripheral: CBPeripheral,
                   error: Error?) {
    print("Connection failed: \(error?.localizedDescription ?? "unknown")")

    // Retry with exponential backoff
    DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
        central.connect(peripheral, options: nil)
    }
}
```

### 3. Filter by Features

Only show devices that support required features:

```swift
func filterDevices(requiring features: [String]) -> [CBPeripheral] {
    return discoveredDevices.filter { (peripheral, info) in
        guard let serviceInfo = info else { return false }
        return features.allSatisfy { serviceInfo.features.contains($0) }
    }.map { $0.key }
}

// Example: Only show devices with file-sharing
let fileServers = filterDevices(requiring: ["file-sharing"])
```

### 4. Display User-Friendly Names

Use the service description instead of the peripheral name:

```swift
func displayName(for peripheral: CBPeripheral) -> String {
    if let info = discoveredDevices[peripheral],
       !info.serviceDescription.isEmpty {
        return info.serviceDescription  // e.g. "9P File Server"
    }
    return peripheral.name ?? "Unknown Device"
}
```

## UI Integration Example

### SwiftUI Device List

```swift
struct DeviceListView: View {
    @ObservedObject var discovery: NinePDiscovery

    var body: some View {
        List {
            ForEach(Array(discovery.discoveredDevices.keys), id: \.identifier) { peripheral in
                DeviceRow(peripheral: peripheral,
                         info: discovery.discoveredDevices[peripheral])
                    .onTapGesture {
                        // Connect to device
                        discovery.connect(to: peripheral)
                    }
            }
        }
        .navigationTitle("9P Servers")
        .onAppear {
            discovery.startScanning()
        }
    }
}

struct DeviceRow: View {
    let peripheral: CBPeripheral
    let info: NinePServiceInfo?

    var body: some View {
        VStack(alignment: .leading) {
            Text(info?.serviceDescription ?? peripheral.name ?? "Unknown")
                .font(.headline)

            if let features = info?.features {
                Text(features.joined(separator: ", "))
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            if let version = info?.protocolVersion {
                Text(version)
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }
        }
        .padding(.vertical, 4)
    }
}
```

## Testing

### Test with LightBlue

1. Download **LightBlue** from the App Store
2. Scan for devices
3. Connect to your Zephyr device
4. Find service `39500001-FEED-4A91-BA88-A1E0F6E4C001`
5. Read each characteristic to verify values

### Test with Your App

1. Run your app on a real iOS device (simulator doesn't support Bluetooth)
2. Enable Bluetooth
3. Grant Bluetooth permissions when prompted
4. Start scanning - should see your device appear
5. Tap device - should read 9PIS and connect to L2CAP automatically

## Troubleshooting

**No devices found:**
- Ensure Bluetooth is enabled on both devices
- Check that `NSBluetoothAlwaysUsageDescription` is in Info.plist
- Verify device is advertising (check Zephyr logs)

**Can't read characteristics:**
- Ensure you're connected to the peripheral
- Check that service discovery completed successfully
- Verify characteristic UUIDs match exactly (including dashes)

**L2CAP connection fails:**
- PSM must match between client and server (default: 0x0009)
- Ensure iOS 13.0+ (L2CAP support)
- Check that peripheral supports L2CAP dynamic channels

## Next Steps

- Implement full 9P protocol client (see `iOS_IMPL_SUMMARY.md`)
- Add file browsing UI
- Implement file upload/download
- Add offline caching
- Support multiple simultaneous connections

## References

- [9PIS GATT Specification](9PIS_GATT_SPECIFICATION.md)
- [iOS Implementation Summary](IOS_IMPL_SUMMARY.md)
- [Apple CoreBluetooth Documentation](https://developer.apple.com/documentation/corebluetooth)
- [Apple L2CAP Documentation](https://developer.apple.com/documentation/corebluetooth/cbl2cappsm)
