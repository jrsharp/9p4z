# 9P over L2CAP Transport Design

## Overview

Design for implementing 9P protocol over Bluetooth L2CAP (Logical Link Control and Adaptation Protocol) with iOS client and Zephyr RTOS peripheral.

## Architecture

```
┌─────────────────┐                           ┌──────────────────┐
│   iOS Client    │                           │ Zephyr Peripheral│
│                 │                           │                  │
│  ┌───────────┐  │      BLE Connection       │  ┌────────────┐  │
│  │ 9P Client │  │◄─────────────────────────►│  │ 9P Server  │  │
│  └─────┬─────┘  │                           │  └──────┬─────┘  │
│        │        │                           │         │        │
│  ┌─────▼─────┐  │      L2CAP Channel        │  ┌──────▼─────┐  │
│  │  L2CAP    │  │       (PSM 0x0009)        │  │   L2CAP    │  │
│  │ Transport │  │◄═════════════════════════►│  │  Transport │  │
│  └───────────┘  │                           │  └────────────┘  │
│                 │                           │                  │
└─────────────────┘                           └──────────────────┘
```

## L2CAP Channel Setup

### PSM (Protocol/Service Multiplexer)
- **Default PSM**: `0x0009` (128 decimal)
- **Range**: 0x0080-0x00FF (dynamic PSM range for development)
- **Fixed PSM**: Use fixed PSM for simplicity (no SDP/GATT discovery needed)
- **Alternative**: Advertise PSM in GATT characteristic for discovery

### MTU (Maximum Transmission Unit)
- **Negotiated**: L2CAP handles MTU negotiation automatically
- **Typical Range**: 512 - 4096 bytes (device dependent)
- **9P msize**: Should be set to L2CAP MTU or smaller
- **Recommendation**: Use negotiated MTU - 8 bytes (safety margin)

### Connection Flow

1. **Peripheral (Zephyr)**
   - Initialize Bluetooth stack
   - Register L2CAP server on PSM 0x0009
   - Start BLE advertising
   - Accept incoming L2CAP connections

2. **Central (iOS)**
   - Scan for peripherals
   - Connect to peripheral
   - Open L2CAP channel to PSM 0x0009
   - MTU negotiation completes automatically

3. **Channel Ready**
   - Both sides have bidirectional stream
   - Proceed with 9P version negotiation

## 9P Message Framing

### Message Format
```
┌────────┬──────┬──────┬───────────┐
│ size:4 │type:1│tag:2 │ payload:N │
└────────┴──────┴──────┴───────────┘
 ◄──────────── size ──────────────►
```

- `size`: 4 bytes, little-endian, total message size including this field
- `type`: 1 byte, message type (T-version=100, R-version=101, etc.)
- `tag`: 2 bytes, little-endian, request/response identifier
- `payload`: Variable length, message-specific data

### Reading Messages from L2CAP Stream

**Algorithm:**
```c
1. Read 4 bytes → message_size
2. Validate: message_size >= 7 && message_size <= MAX_SIZE
3. Allocate buffer of message_size bytes
4. Copy size field to buffer[0..3]
5. Read (message_size - 4) more bytes → buffer[4..]
6. Parse complete 9P message
7. Dispatch to handler
```

**Handling Partial Reads:**
- L2CAP streams may return fewer bytes than requested
- Must loop until complete message received
- Keep read state (bytes_read, bytes_needed)
- Buffer partial messages between reads

### Writing Messages to L2CAP Stream

**Algorithm:**
```c
1. Format 9P response message
2. Write size field (4 bytes)
3. Write message content
4. Flush if needed
```

**Handling Partial Writes:**
- Stream may not accept all bytes at once
- Loop until entire message written
- Check stream space availability

## Implementation Details

### Zephyr Side (9p4z/transports/l2cap)

**Data Structures:**
```c
struct l2cap_transport {
    struct bt_l2cap_chan chan;          // L2CAP channel
    struct k_fifo rx_queue;             // Received message queue
    uint8_t rx_buf[MAX_MSG_SIZE];       // RX buffer
    size_t rx_len;                       // Bytes in RX buffer
    size_t rx_expected;                  // Expected message size
    enum { WAIT_SIZE, WAIT_MSG } rx_state;
};
```

**Key Functions:**
```c
// Initialize L2CAP transport
int l2cap_transport_init(uint16_t psm);

// L2CAP callbacks
void l2cap_connected(struct bt_l2cap_chan *chan);
void l2cap_disconnected(struct bt_l2cap_chan *chan);
int l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf);

// 9P transport interface
int l2cap_send(struct ninep_transport *t, struct ninep_msg *msg);
int l2cap_recv(struct ninep_transport *t, struct ninep_msg *msg);
```

**RX State Machine:**
```
WAIT_SIZE:
  - Need 4 bytes for size field
  - When received: parse size, transition to WAIT_MSG

WAIT_MSG:
  - Need (size - 4) more bytes
  - Buffer incoming data
  - When complete: dispatch message, reset to WAIT_SIZE
```

**Example Code:**
```c
static int l2cap_recv_callback(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    struct l2cap_transport *t = CONTAINER_OF(chan, struct l2cap_transport, chan);

    while (buf->len > 0) {
        if (t->rx_state == WAIT_SIZE) {
            // Read size field
            size_t need = 4 - t->rx_len;
            size_t copy = MIN(need, buf->len);
            memcpy(&t->rx_buf[t->rx_len], buf->data, copy);
            net_buf_pull(buf, copy);
            t->rx_len += copy;

            if (t->rx_len == 4) {
                t->rx_expected = get_u32le(t->rx_buf);
                if (t->rx_expected < 7 || t->rx_expected > MAX_MSG_SIZE) {
                    // Error: invalid size
                    return -EINVAL;
                }
                t->rx_state = WAIT_MSG;
            }
        } else {
            // Read message body
            size_t need = t->rx_expected - t->rx_len;
            size_t copy = MIN(need, buf->len);
            memcpy(&t->rx_buf[t->rx_len], buf->data, copy);
            net_buf_pull(buf, copy);
            t->rx_len += copy;

            if (t->rx_len == t->rx_expected) {
                // Complete message received
                struct ninep_msg msg;
                ninep_parse_msg(&msg, t->rx_buf, t->rx_len);
                k_fifo_put(&t->rx_queue, &msg);

                // Reset for next message
                t->rx_len = 0;
                t->rx_state = WAIT_SIZE;
            }
        }
    }

    return 0;
}
```

### iOS Side (9p4i)

**Data Structures:**
```swift
class NinePTransport {
    private var l2capChannel: CBL2CAPChannel?
    private var rxBuffer = Data()
    private var rxState: RXState = .waitingForSize
    private var expectedSize: UInt32 = 0

    enum RXState {
        case waitingForSize
        case waitingForMessage(size: UInt32)
    }
}
```

**RX Handler:**
```swift
func stream(_ stream: Stream, handle eventCode: Stream.Event) {
    switch eventCode {
    case .hasBytesAvailable:
        guard let inputStream = stream as? InputStream else { return }

        var buffer = [UInt8](repeating: 0, count: 4096)
        let bytesRead = inputStream.read(&buffer, maxLength: 4096)

        if bytesRead > 0 {
            rxBuffer.append(Data(buffer[..<bytesRead]))
            processRxBuffer()
        }
    // ...
    }
}

func processRxBuffer() {
    while true {
        switch rxState {
        case .waitingForSize:
            guard rxBuffer.count >= 4 else { return }

            expectedSize = rxBuffer.withUnsafeBytes {
                $0.loadUnaligned(as: UInt32.self)
            }

            guard expectedSize >= 7 && expectedSize <= MAX_MSG_SIZE else {
                // Error handling
                rxBuffer.removeAll()
                return
            }

            rxState = .waitingForMessage(size: expectedSize)

        case .waitingForMessage(let size):
            guard rxBuffer.count >= size else { return }

            let msgData = rxBuffer.prefix(Int(size))
            rxBuffer.removeFirst(Int(size))

            // Parse and dispatch 9P message
            handleNinePMessage(msgData)

            rxState = .waitingForSize
        }
    }
}
```

## 9P Protocol Flow

### Initial Handshake

```
iOS                                 Zephyr
 │                                    │
 ├─── BLE Connect ──────────────────► │
 │                                    │
 ├─── Open L2CAP (PSM 0x0009)  ─────► │
 │ ◄── L2CAP Connected ──────-────────┤
 │                                    │
 ├─── T-version ────────────────────► │
 │     tag=0                          │
 │     msize=4096                     │
 │     version="9P2000"               │
 │                                    │
 │ ◄── R-version ───────────-─────────┤
 │     tag=0                          │
 │     msize=4096                     │
 │     version="9P2000"               │
 │                                    │
 ├─── T-attach ─────────────────────► │
 │     tag=1, fid=0                   │
 │     afid=~0, uname="user"          │
 │     aname=""                       │
 │                                    │
 │ ◄── R-attach ────────────-─────────┤
 │     tag=1, qid={...}               │
 │                                    │
 ├─── T-walk ───────────────────────► │
 ├─── T-open ───────────────────────► │
 ├─── T-read ───────────────────────► │
 │ ◄── R-read ──────────────-─────────┤
 │                                    │
```

### Message Size Negotiation

1. **iOS sends T-version**:
   - `msize` = L2CAP MTU - 8 (e.g., 4088 if MTU is 4096)
   - This is the max message size iOS can handle

2. **Zephyr responds R-version**:
   - `msize` = MIN(requested_msize, server_max_size)
   - Both sides now know the maximum message size

3. **All subsequent messages** must be ≤ negotiated msize

## Error Handling

### Connection Errors
- **Disconnect**: Clean up resources, notify 9P layer
- **Reconnect**: iOS should allow manual reconnect
- **Timeout**: Implement request timeout (e.g., 30 seconds)

### Protocol Errors
- **Invalid size**: Disconnect and log error
- **Parse error**: Send R-error response (if possible)
- **Unknown message type**: Send R-error
- **Tag mismatch**: Log warning, may indicate lost message

### Flow Control
- **TX buffer full**: Block/queue until space available
- **RX buffer overflow**: Disconnect (indicates bug)
- **Slow peer**: Rely on L2CAP flow control

## Configuration

### Zephyr Kconfig Options
```kconfig
config NINEP_TRANSPORT_L2CAP
    bool "L2CAP transport for 9P"
    depends on BT_L2CAP_DYNAMIC_CHANNEL

config NINEP_L2CAP_PSM
    hex "L2CAP PSM for 9P service"
    default 0x0009

config NINEP_L2CAP_MTU
    int "Maximum L2CAP MTU"
    default 4096

config NINEP_L2CAP_RX_BUF_SIZE
    int "RX buffer size"
    default 4096
```

### iOS Configuration
```swift
struct L2CAPConfig {
    static let psm: UInt16 = 0x0009
    static let maxMessageSize: UInt32 = 8192
    static let requestTimeout: TimeInterval = 30.0
}
```

## Testing Strategy

### Unit Tests
- Message framing/parsing
- Partial read handling
- Size validation
- Buffer management

### Integration Tests
1. **Basic connectivity**: Connect, disconnect, reconnect
2. **Version negotiation**: Verify msize negotiation
3. **Small messages**: T-version, T-attach
4. **Large messages**: T-read with large data
5. **Fragmentation**: Messages larger than single L2CAP packet
6. **Error cases**: Invalid size, disconnect during transfer

### Test Utilities
- **iOS quick-send buttons**: Pre-configured test messages
- **Hex dump logging**: See exact bytes sent/received
- **Zephyr logging**: Enable detailed L2CAP and 9P logs

## Performance Considerations

### Latency
- **BLE connection interval**: 7.5ms - 4s (affects latency)
- **Request-response**: ~10-100ms typical
- **Optimization**: Use shorter connection intervals if supported

### Throughput
- **L2CAP MTU**: Larger MTU = fewer packets for large transfers
- **9P msize**: Match MTU for optimal performance
- **BLE 4.2+**: Up to ~1 Mbps throughput possible

### Power
- **Peripheral sleep**: Design for peripheral to sleep between requests
- **Connection parameters**: Longer intervals = lower power
- **Trade-off**: Latency vs power consumption

## Future Enhancements

### Discovery
- Advertise 9P service UUID in BLE advertisement
- GATT characteristic for PSM discovery
- Service name, version info

### Security
- BLE pairing/bonding
- Encrypted L2CAP channels
- 9P authentication (T-auth)

### Reliability
- Request retry on timeout
- Automatic reconnection
- Transaction IDs for deduplication

### Multi-client
- Support multiple simultaneous iOS clients
- Per-client 9P sessions
- Resource management

## Reference Implementation

See:
- `9p4i/L2CAPManager.swift` - iOS L2CAP handling
- `9p4z/transports/l2cap/` - Zephyr L2CAP transport (to be implemented)
- `9p4z/samples/l2cap_server/` - Example peripheral (to be implemented)

## References

- [9P2000 Protocol Specification](http://man.cat-v.org/plan_9/5/intro)
- [Bluetooth L2CAP Spec](https://www.bluetooth.com/specifications/specs/core-specification/)
- [iOS CoreBluetooth L2CAP](https://developer.apple.com/documentation/corebluetooth/cbl2cappsm)
- [Zephyr Bluetooth L2CAP](https://docs.zephyrproject.org/latest/connectivity/bluetooth/api/l2cap.html)
