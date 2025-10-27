# 9bbs Demo - Plan 9-style BBS with Namespaces

This sample demonstrates:
- Creating a 9bbs (bulletin board system) instance
- Registering it as an in-process 9P server
- Mounting it via Plan 9-style namespaces
- Posting and reading messages through the filesystem interface

## Overview

9bbs is a filesystem-oriented BBS inspired by Citadel and the Plan 9 9bbs implementation. It exposes the bulletin board as a 9P filesystem:

```
/bbs/
  rooms/
    lobby/
      1          # Message #1
      2          # Message #2
    tech/
      1
  etc/
    roomlist     # List of rooms
```

## Building

```bash
west build -p -b native_posix samples/9bbs_demo/
west build -t run
```

## What It Does

1. Initializes namespace support
2. Creates a BBS instance with "lobby" room
3. Creates some demo users
4. Registers BBS as a 9P server
5. Mounts BBS at `/bbs` via namespaces
6. Posts demo messages
7. Reads messages back through filesystem operations
8. Demonstrates the filesystem interface

## Expected Output

```
[00:00:00.000,000] <inf> bbs: BBS initialized with lobby
[00:00:00.000,000] <inf> bbs: Created user: alice
[00:00:00.000,000] <inf> bbs: Created user: bob
[00:00:00.000,000] <inf> bbs: Registered BBS as 9P server
[00:00:00.000,000] <inf> namespace: Mounted in-process server at /bbs
[00:00:00.000,000] <inf> bbs: Posted message 1 to lobby by alice
[00:00:00.000,000] <inf> bbs: Posted message 2 to lobby by bob

Reading /bbs/rooms/lobby/1:
From: alice
To: lobby
Date: 0
X-Date-N: 0

Hello, this is the first message!

alice

Reading /bbs/etc/roomlist:
lobby
```

## Network Transparency

The same BBS server can be exported over the network:

```c
// Export via /srv for local discovery
srv_post("bbs", bbs_server);

// Other threads/processes can mount it:
srv_mount("bbs", "/bbs", 0);

// Or export over L2CAP (Bluetooth)
struct ninep_transport l2cap_transport = { ... };
ninep_server_start(bbs_server, &l2cap_transport);
// Now accessible from iOS/Mac 9P clients!
```

## Next Steps

- Add UART/Telnet command interface (like the original 9bbs)
- Add persistence (store messages in LittleFS)
- Add authentication
- Export over network transports (L2CAP, TCP, CoAP)
