# 9P over L2CAP Implementation Summary

## What Was Implemented

I've created a complete iOS L2CAP test application with proper 9P protocol support. Here's what's included:

### 1. L2CAP Transport Layer (`L2CAPManager.swift`)

**Connection Management:**
- BLE peripheral scanning and discovery
- L2CAP channel connection to configurable PSM (default 0x0009)
- Bidirectional stream I/O via CoreBluetooth L2CAP API
- Connection state tracking and error handling

**9P Message Framing:**
- State machine for proper message framing:
  - `waitingForSize`: Read 4-byte size field
  - `waitingForMessage`: Read complete message based on size
- Handles partial reads from L2CAP stream
- Validates message sizes (7 bytes minimum, 8192 maximum)
- Buffers incoming data until complete messages arrive
- Dispatches complete messages to parser

**Data Send/Receive:**
- Writes complete 9P messages to L2CAP output stream
- Logs all sent/received messages with human-readable descriptions
- Hex dump display for debugging

### 2. 9P Protocol Support (`NinePMessage.swift`)

**Message Types:**
- Enum for all 9P2000 message types (T-version, R-version, T-attach, etc.)
- Request/response type detection

**Message Parsing:**
- Parse 9P message headers (size, type, tag)
- Extract and decode payloads
- Version message parsing (msize, version string)
- Error message parsing
- Human-readable descriptions for logging

**Message Building:**
- `buildVersion()`: Create T-version messages
- `buildAttach()`: Create T-attach messages
- `buildClunk()`: Create T-clunk messages
- Proper little-endian encoding
- String length prefixes for 9P string fields

### 3. User Interface (`ContentView.swift`)

**Connection View:**
- Device scanning with RSSI display
- Device list with tap-to-connect
- Connection state indicator (color-coded)

**L2CAP Channel Controls:**
- PSM input (hex format)
- Open/close L2CAP channel
- MTU display (when available)

**Data I/O:**
- Manual hex data entry and send
- Quick-send buttons for common 9P messages:
  - **Version**: T-version with msize=8192, version="9P2000"
  - **Attach**: T-attach with fid=0, uname="user"
  - **Clunk**: T-clunk with fid=0
- Received data display

**Logging:**
- Collapsible log view
- Timestamped entries
- Color-coded by severity (info, success, warning, error)
- Auto-scroll to latest
- Clear logs button

## How It Works

### Connection Flow

```
1. User taps "Start Scanning"
   └─> CBCentralManager scans for peripherals

2. User taps on discovered device
   └─> CBCentralManager connects to peripheral
   └─> Connection state → "Connected"

3. User enters PSM (e.g., "0009") and taps "Open L2CAP Channel"
   └─> peripheral.openL2CAPChannel(0x0009)
   └─> L2CAP MTU negotiation happens automatically
   └─> Input/output streams configured
   └─> Connection state → "Ready"

4. User taps "Version" quick-send button
   └─> NinePMessageBuilder.buildVersion(...)
   └─> TX: TVERSION tag=0 size=23 msize=8192 version="9P2000"
   └─> Data written to L2CAP output stream

5. Peripheral sends R-version response
   └─> Data arrives on input stream
   └─> RX state machine:
       • Read 4 bytes → size = 23
       • Read 19 more bytes → complete message
       • Parse message → R-version
   └─> RX: RVERSION tag=0 size=23 msize=4096 version="9P2000"
   └─> Log shows negotiated msize
```

### Message Framing Example

**Sending T-version:**
```
Size calculation: 4 (size) + 1 (type) + 2 (tag) + 4 (msize) + 2 (len) + 6 ("9P2000") = 19 bytes

Bytes on wire:
13 00 00 00  ← size (19 little-endian)
64           ← type (100 = T-version)
00 00        ← tag (0)
00 20 00 00  ← msize (8192 little-endian)
06 00        ← version length (6)
39 50 32 30 30 30  ← "9P2000" UTF-8
```

**Receiving R-version:**
```
State: waitingForSize
  ← Read stream → 13 00 00 00 64 00 00 ...
  Parse: size = 19
  Transition: waitingForMessage

State: waitingForMessage (need 19 bytes total)
  Already have: 19 bytes
  Extract: bytes[0..19]
  Parse: type=101 (R-version), tag=0, msize=4096, version="9P2000"
  Dispatch: handleNinePMessage(...)
  Log: "RX: RVERSION tag=0 size=19 msize=4096 version=\"9P2000\""
  Reset: waitingForSize
```

## Design Rationale

### Why This Approach?

1. **Direct L2CAP (No GATT)**:
   - Simpler connection model
   - Lower latency
   - Higher throughput
   - Fixed PSM = no service discovery needed

2. **Stream-Based Framing**:
   - 9P already has message framing (size field)
   - No need for additional protocol layer
   - Handles MTU fragmentation automatically
   - Simple state machine

3. **iOS CoreBluetooth L2CAP**:
   - Native iOS support (iOS 11+)
   - Stream API familiar to developers
   - Automatic MTU negotiation
   - Flow control built-in

4. **PSM 0x0009**:
   - Dynamic PSM range (0x0080-0x00FF)
   - Suitable for development/testing
   - Can be changed if needed
   - No SDP registration required

### Trade-offs

**Advantages:**
- Simple implementation
- Low overhead
- Direct 9P protocol mapping
- Easy to debug (hex logs)

**Limitations:**
- Fixed PSM (no discovery)
- No automatic reconnection
- No request timeouts (yet)
- Single client only

**Future Enhancements:**
- Advertise PSM in GATT characteristic
- Implement request timeout and retry
- Add automatic reconnection
- Multi-client support on Zephyr side

## Zephyr Implementation Guidance

Based on this iOS implementation, here's what the Zephyr side needs:

### 1. L2CAP Server Setup

```c
// Register L2CAP server channel
static struct bt_l2cap_server server = {
    .psm = 0x0009,
    .accept = l2cap_accept_cb,
};

bt_l2cap_server_register(&server);
```

### 2. RX State Machine (Similar to iOS)

```c
enum rx_state {
    RX_WAIT_SIZE,
    RX_WAIT_MSG
};

struct l2cap_9p_chan {
    struct bt_l2cap_le_chan le;
    uint8_t rx_buf[8192];
    size_t rx_len;
    uint32_t rx_expected;
    enum rx_state state;
};
```

### 3. Data Reception

```c
static int l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    struct l2cap_9p_chan *ch = CONTAINER_OF(chan, struct l2cap_9p_chan, le);

    while (buf->len > 0) {
        if (ch->state == RX_WAIT_SIZE) {
            // Copy bytes to size field
            size_t need = 4 - ch->rx_len;
            size_t copy = MIN(need, buf->len);
            memcpy(&ch->rx_buf[ch->rx_len], buf->data, copy);
            net_buf_pull(buf, copy);
            ch->rx_len += copy;

            if (ch->rx_len == 4) {
                ch->rx_expected = sys_get_le32(ch->rx_buf);
                ch->state = RX_WAIT_MSG;
            }
        } else {
            // Copy bytes to message buffer
            size_t need = ch->rx_expected - ch->rx_len;
            size_t copy = MIN(need, buf->len);
            memcpy(&ch->rx_buf[ch->rx_len], buf->data, copy);
            net_buf_pull(buf, copy);
            ch->rx_len += copy;

            if (ch->rx_len == ch->rx_expected) {
                // Complete message - dispatch to 9P handler
                ninep_handle_message(ch->rx_buf, ch->rx_len);

                // Reset for next message
                ch->rx_len = 0;
                ch->state = RX_WAIT_SIZE;
            }
        }
    }

    return 0;
}
```

### 4. Integration with 9P Server

The L2CAP transport should integrate with your existing 9p4z server:

```c
struct ninep_transport l2cap_transport = {
    .send = l2cap_transport_send,
    .recv = l2cap_transport_recv,
    .close = l2cap_transport_close,
};
```

### 5. Testing Strategy

1. **Basic Connection**: iOS connects, opens L2CAP channel
2. **Version Exchange**: iOS sends T-version, Zephyr responds with R-version
3. **Attach**: iOS sends T-attach, Zephyr responds with R-attach
4. **File Operations**: Build up from walk, open, read, clunk
5. **Error Handling**: Test disconnects, invalid messages, timeouts

## Files Overview

```
9p4i/
├── 9p4i/
│   ├── App.swift                    # App entry point
│   ├── ContentView.swift            # Main UI
│   ├── L2CAPManager.swift           # Bluetooth L2CAP + 9P framing
│   ├── NinePMessage.swift           # 9P protocol support
│   ├── Info.plist                   # Bluetooth permissions
│   └── Assets.xcassets/             # App icons, colors
├── 9p4i.xcodeproj/                  # Xcode project
├── L2CAP_TRANSPORT_DESIGN.md        # Detailed design doc
├── USAGE.md                         # User guide
└── README.md                        # Project overview
```

## Next Steps

1. **Build and Test iOS App**: Open in Xcode, run on device
2. **Implement Zephyr L2CAP Server**: Use design as reference
3. **Test Basic Connectivity**: Connect and open L2CAP channel
4. **Implement Version Exchange**: First 9P transaction
5. **Add Remaining 9P Messages**: Walk, open, read, write, etc.
6. **Stress Testing**: Large transfers, disconnects, error cases

## References

- See `L2CAP_TRANSPORT_DESIGN.md` for complete design details
- See `USAGE.md` for end-user documentation
- iOS L2CAP API: [CBL2CAPChannel](https://developer.apple.com/documentation/corebluetooth/cbl2capchannel)
- Zephyr L2CAP API: [Bluetooth L2CAP](https://docs.zephyrproject.org/latest/connectivity/bluetooth/api/l2cap.html)
- 9P Protocol: [9P2000 Spec](http://man.cat-v.org/plan_9/5/intro)
