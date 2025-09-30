# 9P UART Echo Sample

This sample demonstrates basic 9P protocol functionality over UART transport.

## Overview

The sample:
- Initializes a 9P UART transport
- Receives 9P messages over UART
- Echoes received messages back
- Logs message headers for debugging

## Building and Running

```bash
west build -b <board> samples/uart_echo
west flash
```

## Testing

You can test this sample by sending 9P messages to the UART port. A simple
test message (Tversion):

```
13 00 00 00  # size = 19 bytes
64           # type = Tversion (100)
01 00        # tag = 1
00 20 00 00  # msize = 8192
06 00        # version string length = 6
39 50 32 30 30 30  # "9P2000"
```

## Requirements

- Board with UART support
- UART interrupt-driven API support