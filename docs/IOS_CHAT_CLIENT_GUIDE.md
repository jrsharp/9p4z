# iOS 9P Chat Client Implementation Guide

## Overview

This guide covers implementing an iOS app that connects to a 9P BBS server over Bluetooth L2CAP and provides real-time chat functionality.

## Architecture

```
iOS App
  ├── CoreBluetooth (L2CAP)
  ├── 9P Protocol Client
  └── Chat UI
       ├── Room List
       ├── Message Stream
       └── Compose View
```

## Prerequisites

- iOS 14.0+ (for L2CAP support)
- Swift 5.5+ (for async/await)
- Xcode 13+

## Phase 1: Bluetooth L2CAP Connection

### 1.1 Core Bluetooth Setup

```swift
import CoreBluetooth

class BLEManager: NSObject, ObservableObject {
    private var centralManager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var l2capChannel: CBL2CAPChannel?

    // 9P L2CAP PSM (must match server CONFIG_NINEP_L2CAP_PSM)
    let ninepPSM: CBL2CAPPSM = 0x0009

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    func connect(to peripheral: CBPeripheral) {
        self.peripheral = peripheral
        peripheral.delegate = self
        centralManager.connect(peripheral)
    }
}

extension BLEManager: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            // Start scanning for 9P servers
            centralManager.scanForPeripherals(
                withServices: nil,  // Or specific service UUID
                options: nil
            )
        }
    }

    func centralManager(_ central: CBCentralManager,
                       didDiscover peripheral: CBPeripheral,
                       advertisementData: [String : Any],
                       rssi RSSI: NSNumber) {
        // Filter by name or service UUID
        if peripheral.name?.contains("9P Server") == true {
            connect(to: peripheral)
        }
    }

    func centralManager(_ central: CBCentralManager,
                       didConnect peripheral: CBPeripheral) {
        // Open L2CAP channel
        peripheral.openL2CAPChannel(ninepPSM)
    }
}

extension BLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral,
                   didOpen channel: CBL2CAPChannel?,
                   error: Error?) {
        guard let channel = channel, error == nil else {
            print("Failed to open L2CAP channel: \(error?.localizedDescription ?? "unknown")")
            return
        }

        self.l2capChannel = channel
        channel.inputStream.delegate = self
        channel.outputStream.delegate = self

        channel.inputStream.schedule(in: .main, forMode: .default)
        channel.outputStream.schedule(in: .main, forMode: .default)

        channel.inputStream.open()
        channel.outputStream.open()

        // Ready to start 9P protocol
        initiate9PConnection()
    }
}
```

### 1.2 Stream Management

```swift
extension BLEManager: StreamDelegate {
    func stream(_ aStream: Stream, handle eventCode: Stream.Event) {
        switch eventCode {
        case .hasBytesAvailable:
            if let inputStream = aStream as? InputStream {
                read9PMessage(from: inputStream)
            }

        case .hasSpaceAvailable:
            // Can send 9P messages
            processPendingWrites()

        case .errorOccurred:
            print("Stream error: \(aStream.streamError?.localizedDescription ?? "unknown")")

        case .endEncountered:
            disconnect()

        default:
            break
        }
    }
}
```

## Phase 2: 9P Protocol Implementation

### 2.1 9P Message Structure

```swift
// 9P message types
enum NinePMessageType: UInt8 {
    case Tversion = 100, Rversion = 101
    case Tauth    = 102, Rauth    = 103
    case Tattach  = 104, Rattach  = 105
    case Terror   = 106, Rerror   = 107  // Not used in 9P2000
    case Tflush   = 108, Rflush   = 109
    case Twalk    = 110, Rwalk    = 111
    case Topen    = 112, Ropen    = 113
    case Tcreate  = 114, Rcreate  = 115
    case Tread    = 116, Rread    = 117
    case Twrite   = 118, Rwrite   = 119
    case Tclunk   = 120, Rclunk   = 121
    case Tremove  = 122, Rremove  = 123
    case Tstat    = 124, Rstat    = 125
    case Twstat   = 126, Rwstat   = 127
}

// 9P message header
struct NinePHeader {
    let size: UInt32      // Total message size (including header)
    let type: NinePMessageType
    let tag: UInt16       // Request tag

    static let headerSize = 7  // 4 + 1 + 2

    func encode() -> Data {
        var data = Data()
        data.append(contentsOf: withUnsafeBytes(of: size.littleEndian) { Array($0) })
        data.append(type.rawValue)
        data.append(contentsOf: withUnsafeBytes(of: tag.littleEndian) { Array($0) })
        return data
    }

    static func decode(from data: Data) -> NinePHeader? {
        guard data.count >= headerSize else { return nil }

        let size = data.withUnsafeBytes { $0.load(as: UInt32.self) }
        let type = NinePMessageType(rawValue: data[4])
        let tag = data[5...6].withUnsafeBytes { $0.load(as: UInt16.self) }

        guard let type = type else { return nil }
        return NinePHeader(size: size, type: type, tag: tag)
    }
}
```

### 2.2 9P Client Core

```swift
class NinePClient {
    private let transport: BLEManager
    private var nextTag: UInt16 = 0
    private var pendingRequests: [UInt16: CheckedContinuation<Data, Error>] = [:]
    private var nextFid: UInt32 = 0
    private var openFids: [UInt32: String] = [:]  // fid -> path

    var msize: UInt32 = 8192  // Negotiated message size

    init(transport: BLEManager) {
        self.transport = transport
    }

    // Generate unique tag for requests
    private func allocateTag() -> UInt16 {
        defer { nextTag = nextTag &+ 1 }
        return nextTag
    }

    // Generate unique fid for file handles
    private func allocateFid() -> UInt32 {
        defer { nextFid = nextFid &+ 1 }
        return nextFid
    }

    // Send 9P message and wait for response
    func sendRequest(_ data: Data) async throws -> Data {
        let tag = allocateTag()

        return try await withCheckedThrowingContinuation { continuation in
            pendingRequests[tag] = continuation

            // Prepend header with tag
            var message = NinePHeader(
                size: UInt32(NinePHeader.headerSize + data.count),
                type: .Tversion,  // Type embedded in data
                tag: tag
            ).encode()
            message.append(data)

            // Send over L2CAP
            transport.write(message)
        }
    }

    // Handle incoming 9P response
    func handleResponse(_ data: Data) {
        guard let header = NinePHeader.decode(from: data) else {
            print("Failed to decode 9P header")
            return
        }

        let tag = header.tag
        let body = data.dropFirst(NinePHeader.headerSize)

        if let continuation = pendingRequests.removeValue(forKey: tag) {
            continuation.resume(returning: Data(body))
        }
    }
}
```

### 2.3 9P Handshake

```swift
extension NinePClient {
    func connect() async throws {
        // 1. Tversion - negotiate protocol version and message size
        var versionMsg = Data()
        versionMsg.append(contentsOf: withUnsafeBytes(of: UInt32(8192).littleEndian) { Array($0) })
        let versionStr = "9P2000"
        versionMsg.append(contentsOf: withUnsafeBytes(of: UInt16(versionStr.count).littleEndian) { Array($0) })
        versionMsg.append(versionStr.data(using: .utf8)!)

        let response = try await sendRequest(versionMsg)
        // Parse Rversion to get negotiated msize
        msize = response.withUnsafeBytes { $0.load(as: UInt32.self) }

        // 2. Tattach - attach to root filesystem
        let rootFid = allocateFid()
        var attachMsg = Data()
        attachMsg.append(contentsOf: withUnsafeBytes(of: rootFid.littleEndian) { Array($0) })
        attachMsg.append(contentsOf: withUnsafeBytes(of: UInt32.max.littleEndian) { Array($0) })  // afid = NOFID

        let username = "guest"
        attachMsg.append(contentsOf: withUnsafeBytes(of: UInt16(username.count).littleEndian) { Array($0) })
        attachMsg.append(username.data(using: .utf8)!)

        let aname = ""  // Attach to root
        attachMsg.append(contentsOf: withUnsafeBytes(of: UInt16(aname.count).littleEndian) { Array($0) })

        _ = try await sendRequest(attachMsg)
        openFids[rootFid] = "/"
    }
}
```

## Phase 3: Chat-Specific Operations

### 3.1 Open Chat Room

```swift
extension NinePClient {
    func openChatRoom(_ roomName: String) async throws -> UInt32 {
        // Walk from root to /chat/{roomName}
        let rootFid = openFids.first(where: { $0.value == "/" })!.key
        let chatFid = allocateFid()

        // Twalk from rootFid to chat directory
        var walkMsg = Data()
        walkMsg.append(contentsOf: withUnsafeBytes(of: rootFid.littleEndian) { Array($0) })
        walkMsg.append(contentsOf: withUnsafeBytes(of: chatFid.littleEndian) { Array($0) })
        walkMsg.append(contentsOf: withUnsafeBytes(of: UInt16(2).littleEndian) { Array($0) })  // nwname

        // Walk to "chat"
        let chatStr = "chat"
        walkMsg.append(contentsOf: withUnsafeBytes(of: UInt16(chatStr.count).littleEndian) { Array($0) })
        walkMsg.append(chatStr.data(using: .utf8)!)

        // Walk to room name
        walkMsg.append(contentsOf: withUnsafeBytes(of: UInt16(roomName.count).littleEndian) { Array($0) })
        walkMsg.append(roomName.data(using: .utf8)!)

        _ = try await sendRequest(walkMsg)

        // Topen - open for reading
        var openMsg = Data()
        openMsg.append(contentsOf: withUnsafeBytes(of: chatFid.littleEndian) { Array($0) })
        openMsg.append(0x00)  // OREAD mode

        _ = try await sendRequest(openMsg)
        openFids[chatFid] = "/chat/\(roomName)"

        return chatFid
    }
}
```

### 3.2 Read Messages (Blocking)

```swift
extension NinePClient {
    func readMessages(from fid: UInt32, maxBytes: UInt32 = 4096) async throws -> String {
        // Tread - blocking read from chat room
        var readMsg = Data()
        readMsg.append(contentsOf: withUnsafeBytes(of: fid.littleEndian) { Array($0) })
        readMsg.append(contentsOf: withUnsafeBytes(of: UInt64(0).littleEndian) { Array($0) })  // offset
        readMsg.append(contentsOf: withUnsafeBytes(of: maxBytes.littleEndian) { Array($0) })

        let response = try await sendRequest(readMsg)

        // Parse Rread
        let count = response.withUnsafeBytes { $0.load(as: UInt32.self) }
        let messageData = response.dropFirst(4).prefix(Int(count))

        return String(data: Data(messageData), encoding: .utf8) ?? ""
    }

    // Continuous message stream
    func messageStream(roomName: String) -> AsyncStream<String> {
        AsyncStream { continuation in
            Task {
                do {
                    let fid = try await openChatRoom(roomName)

                    while !Task.isCancelled {
                        let messages = try await readMessages(from: fid)
                        if !messages.isEmpty {
                            continuation.yield(messages)
                        }
                    }
                } catch {
                    continuation.finish()
                }
            }
        }
    }
}
```

### 3.3 Post Messages

```swift
extension NinePClient {
    func postMessage(_ text: String, to room: String = "lobby") async throws {
        // Open /chat/post for writing
        let postFid = try await openFile("/chat/post", mode: 0x01)  // OWRITE

        // Format: "room:message"
        let message = "\(room):\(text)\n"
        let data = message.data(using: .utf8)!

        // Twrite
        var writeMsg = Data()
        writeMsg.append(contentsOf: withUnsafeBytes(of: postFid.littleEndian) { Array($0) })
        writeMsg.append(contentsOf: withUnsafeBytes(of: UInt64(0).littleEndian) { Array($0) })  // offset
        writeMsg.append(contentsOf: withUnsafeBytes(of: UInt32(data.count).littleEndian) { Array($0) })
        writeMsg.append(data)

        _ = try await sendRequest(writeMsg)

        // Tclunk - close file
        try await clunk(postFid)
    }

    private func openFile(_ path: String, mode: UInt8) async throws -> UInt32 {
        // Walk and open in one operation
        // (Implementation similar to openChatRoom)
        // ...
        return 0  // Return fid
    }

    private func clunk(_ fid: UInt32) async throws {
        var clunkMsg = Data()
        clunkMsg.append(contentsOf: withUnsafeBytes(of: fid.littleEndian) { Array($0) })
        _ = try await sendRequest(clunkMsg)
        openFids.removeValue(forKey: fid)
    }
}
```

## Phase 4: SwiftUI Chat Interface

### 4.1 Message Model

```swift
struct ChatMessage: Identifiable, Equatable {
    let id = UUID()
    let timestamp: Date
    let username: String
    let text: String

    // Parse from server format: "[HH:MM:SS] username: message"
    static func parse(_ line: String) -> ChatMessage? {
        let pattern = #"^\[(\d{2}:\d{2}:\d{2})\] (\w+): (.+)$"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)) else {
            return nil
        }

        let timeRange = Range(match.range(at: 1), in: line)!
        let userRange = Range(match.range(at: 2), in: line)!
        let textRange = Range(match.range(at: 3), in: line)!

        let timeStr = String(line[timeRange])
        let username = String(line[userRange])
        let text = String(line[textRange])

        // Parse time (today's date + HH:MM:SS)
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        let time = formatter.date(from: timeStr) ?? Date()

        return ChatMessage(timestamp: time, username: username, text: text)
    }
}
```

### 4.2 Chat View Model

```swift
@MainActor
class ChatViewModel: ObservableObject {
    @Published var messages: [ChatMessage] = []
    @Published var isConnected = false
    @Published var currentRoom = "lobby"

    private let client: NinePClient
    private var messageTask: Task<Void, Never>?

    init(client: NinePClient) {
        self.client = client
    }

    func connect() async {
        do {
            try await client.connect()
            isConnected = true
            startMessageStream()
        } catch {
            print("Connection failed: \(error)")
        }
    }

    func startMessageStream() {
        messageTask = Task {
            for await messageText in client.messageStream(roomName: currentRoom) {
                // Parse line-by-line
                let lines = messageText.components(separatedBy: "\n")
                for line in lines {
                    if let message = ChatMessage.parse(line) {
                        messages.append(message)
                    }
                }
            }
        }
    }

    func send(_ text: String) async {
        do {
            try await client.postMessage(text, to: currentRoom)
        } catch {
            print("Send failed: \(error)")
        }
    }

    func disconnect() {
        messageTask?.cancel()
        isConnected = false
    }
}
```

### 4.3 Chat View

```swift
struct ChatView: View {
    @StateObject var viewModel: ChatViewModel
    @State private var messageText = ""

    var body: some View {
        VStack {
            // Message list
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 8) {
                        ForEach(viewModel.messages) { message in
                            MessageRow(message: message)
                                .id(message.id)
                        }
                    }
                    .padding()
                }
                .onChange(of: viewModel.messages.count) { _ in
                    if let last = viewModel.messages.last {
                        withAnimation {
                            proxy.scrollTo(last.id, anchor: .bottom)
                        }
                    }
                }
            }

            // Compose bar
            HStack {
                TextField("Message", text: $messageText)
                    .textFieldStyle(RoundedBorderTextFieldStyle())

                Button(action: sendMessage) {
                    Image(systemName: "arrow.up.circle.fill")
                        .font(.title2)
                }
                .disabled(messageText.isEmpty)
            }
            .padding()
        }
        .navigationTitle(viewModel.currentRoom)
        .task {
            await viewModel.connect()
        }
        .onDisappear {
            viewModel.disconnect()
        }
    }

    private func sendMessage() {
        let text = messageText
        messageText = ""

        Task {
            await viewModel.send(text)
        }
    }
}

struct MessageRow: View {
    let message: ChatMessage

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(message.username)
                    .font(.caption)
                    .fontWeight(.bold)

                Text(message.timestamp, style: .time)
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }

            Text(message.text)
                .font(.body)
        }
        .padding(.vertical, 4)
    }
}
```

## Phase 5: Advanced Features

### 5.1 Multiple Rooms

```swift
struct RoomListView: View {
    @State private var rooms: [String] = []
    let client: NinePClient

    var body: some View {
        List(rooms, id: \.self) { room in
            NavigationLink(destination: ChatView(viewModel: ChatViewModel(client: client))) {
                Text(room)
            }
        }
        .navigationTitle("Chat Rooms")
        .task {
            rooms = await loadRoomList()
        }
    }

    private func loadRoomList() async -> [String] {
        do {
            // Read /chat directory
            let fid = try await client.openFile("/chat", mode: 0x00)  // OREAD
            let dirData = try await client.readMessages(from: fid)

            // Parse directory entries (stat structures)
            // ... parse each entry's name field ...

            return ["lobby", "tech", "random"]  // Parsed from dirData
        } catch {
            return []
        }
    }
}
```

### 5.2 User Presence

```swift
extension ChatViewModel {
    @Published var activeUsers: [String] = []

    func updateUserList() async {
        do {
            let fid = try await client.openFile("/chat/users", mode: 0x00)
            let userData = try await client.readMessages(from: fid)

            activeUsers = userData.components(separatedBy: "\n")
                .filter { !$0.isEmpty }
        } catch {
            print("Failed to get user list: \(error)")
        }
    }
}
```

### 5.3 Push Notifications

```swift
import UserNotifications

extension ChatViewModel {
    func handleBackgroundMessage(_ message: ChatMessage) {
        // Show notification if app is in background
        if UIApplication.shared.applicationState != .active {
            let content = UNMutableNotificationContent()
            content.title = message.username
            content.body = message.text
            content.sound = .default

            let request = UNNotificationRequest(
                identifier: message.id.uuidString,
                content: content,
                trigger: nil
            )

            UNUserNotificationCenter.current().add(request)
        }
    }
}
```

## Testing

### Mock Server

For testing without hardware:

```swift
class Mock9PServer: NinePClient {
    override func sendRequest(_ data: Data) async throws -> Data {
        // Simulate server responses
        switch messageType(data) {
        case .Tversion:
            return mockRversion()
        case .Tattach:
            return mockRattach()
        case .Tread:
            return mockRread(withMessages: [
                "[12:34:56] alice: Hello!",
                "[12:35:01] bob: Hi alice!"
            ])
        default:
            return Data()
        }
    }
}
```

## Optimization Tips

1. **Connection Pooling**: Keep L2CAP connection alive
2. **Message Batching**: Group reads to reduce protocol overhead
3. **Offline Queue**: Buffer writes when disconnected
4. **Background Refresh**: Use BGTaskScheduler for periodic sync
5. **Lazy Loading**: Only load visible messages initially

## Security Considerations

1. **Authentication**: Extend Tattach to include password
2. **TLS**: Consider adding encryption layer (future)
3. **Rate Limiting**: Throttle message sends
4. **Input Validation**: Sanitize message text

## Reference Implementation

See full iOS client code at:
- GitHub: [9p4z-ios-client](https://github.com/9p4z/ios-client) (future)
- Sample app in `samples/ios_chat/`

## Protocol Reference

- 9P2000 spec: http://9p.cat-v.org/
- 9p4z API docs: `/docs/API.md`
- BBS filesystem: `/docs/9BBS.md`

## Troubleshooting

### Connection Issues

1. Check L2CAP PSM matches server config (0x0009)
2. Verify iOS 14.0+ for L2CAP support
3. Enable Bluetooth permissions in Info.plist

### Protocol Errors

1. Use Wireshark with BTLE plugin to debug
2. Check message size doesn't exceed negotiated msize
3. Verify little-endian byte order in all integers

### Performance

1. Reduce poll interval if battery drain is high
2. Use incremental reads instead of full history
3. Implement message pagination

## Next Steps

1. Implement full 9P client library in Swift
2. Add support for file browsing beyond chat
3. Integrate with iOS Shortcuts
4. Add widget for quick message view
5. Support iCloud sync for offline messages
