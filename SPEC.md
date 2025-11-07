# 9BBS Specification v1.0

**9P-Based Bulletin Board System**

This document specifies the filesystem layout, message format, and identity system for 9BBS - a bulletin board system served over the 9P protocol.

---

## Table of Contents

1. [Filesystem Structure](#filesystem-structure)
2. [Message Format](#message-format)
3. [Cryptographic Identity](#cryptographic-identity)
4. [BBS Metadata](#bbs-metadata)
5. [Moderation System](#moderation-system)
6. [FSXNet Integration](#fsxnet-integration)
7. [Client Implementation Notes](#client-implementation-notes)

---

## 1. Filesystem Structure

### 1.1 Root Layout

The BBS filesystem is rooted at `/srv/bbs/` and organized as follows:

```
/srv/bbs/
  ├── etc/                    # Board metadata (read-only)
  │   ├── boardname           # BBS name
  │   ├── sysop               # Sysop name/contact
  │   ├── motd                # Message of the day
  │   ├── location            # Physical location (optional)
  │   ├── description         # Board description
  │   ├── version             # BBS software version
  │   └── nets/               # Carried networks
  │       ├── fsxnet          # FSXNet areas list
  │       ├── aethernet       # Aether mesh network
  │       └── ...
  ├── rooms/                  # Message rooms
  │   ├── lobby/
  │   │   ├── <timestamp>-<msgid>
  │   │   └── ...
  │   ├── general/
  │   ├── tech/
  │   └── ...
  ├── moderation/             # Moderation messages
  │   ├── <timestamp>
  │   └── ...
  └── .sync/                  # Sync metadata (hidden from clients)
      ├── log                 # Change log
      ├── sequence            # Current sequence number
      └── peers/              # Per-peer sync state
```

### 1.2 Directory Permissions

| Path | Permissions | Description |
|------|-------------|-------------|
| `/srv/bbs/etc/` | Read-only | Board metadata |
| `/srv/bbs/rooms/` | Read/Write | Message storage |
| `/srv/bbs/moderation/` | Read/Write | Moderation actions |
| `/srv/bbs/.sync/` | Hidden | Sync infrastructure |

### 1.3 Message File Naming

Message files are named: `<timestamp>-<msgid>`

- `<timestamp>`: Unix timestamp in milliseconds (13 digits)
- `<msgid>`: First 16 characters of Message-ID (alphanumeric)

Example: `1738005432123-abc123def456ghi7`

---

## 2. Message Format

### 2.1 RFC-822 Style Headers

Messages use RFC-822 style headers with a blank line separating headers from body:

```
Header-Name: value
Another-Header: value

Message body text...
```

### 2.2 Required Headers

| Header | Description | Example |
|--------|-------------|---------|
| `From` | Author handle | `alice` |
| `Date` | Unix timestamp (milliseconds) | `1738005432123` |
| `Subject` | Message subject | `Welcome to the BBS` |
| `Room` | Destination room | `lobby` |
| `Message-ID` | Unique message identifier | `<abc123def456@node1>` |
| `Origin` | Originating device/node ID | `device-uuid-1234` |
| `X-Client` | Client software identifier | `TheAether-iOS/1.0` |

### 2.3 Optional Headers

#### Identity/Crypto Headers

| Header | Description | Example |
|--------|-------------|---------|
| `CGA-Address` | Cryptographically Generated Address | `fe80::a1b2:c3d4:e5f6:7890` |
| `Pubkey` | Base64-encoded Ed25519 public key | `U29tZVB1YmxpY0tleURhdGE=` |
| `Signature` | Base64-encoded message signature | `U2lnbmF0dXJlRGF0YQ==` |
| `Identity-Claim` | Compact identity claim (handle binding) | `alice:1738005432:sig...` |

#### Threading Headers

| Header | Description | Example |
|--------|-------------|---------|
| `In-Reply-To` | Message-ID of parent message | `<parent-msg-id@node>` |
| `References` | Space-separated list of Message-IDs | `<msg1> <msg2>` |

#### Federation Headers

| Header | Description | Example |
|--------|-------------|---------|
| `Path` | Message propagation path | `node1!node2!node3` |
| `SEEN-BY` | Nodes that have seen this message | `1/100 2/200` |

### 2.4 Example Message

```
From: alice
Date: 1738005432123
Subject: Hello World
Room: lobby
Message-ID: <1738005432123-abc123@node1>
Origin: device-uuid-1234
X-Client: TheAether-iOS/1.0
CGA-Address: fe80::a1b2:c3d4:e5f6:7890
Pubkey: SGVsbG8gV29ybGQgUHVibGljS2V5RGF0YQ==
Signature: VGhpc0lzQVNpZ25hdHVyZQ==
Identity-Claim: alice:1738005432:compactsig

This is the message body. It can span
multiple lines and contain any text.
```

---

## 3. Cryptographic Identity

### 3.1 Overview

9BBS uses **CGA (Cryptographically Generated Addresses)** for identity. Each identity consists of:

- **Private Key**: Ed25519 signing key (stored in device keychain)
- **Public Key**: Ed25519 public key (32 bytes)
- **CGA Address**: IPv6 address derived from public key using CGA RFC 3972

### 3.2 CGA Address Generation

```
1. Generate Ed25519 key pair
2. Compute SHA-256 hash of public key
3. Format as IPv6 link-local address:
   fe80::<48-bit-prefix>:<hash[0:16]>:<hash[16:32]>
4. Set Universal/Local bit (U/L) to indicate CGA
```

Example:
- Public Key: `4a7e...` (32 bytes)
- SHA-256: `a1b2c3d4e5f67890...`
- CGA Address: `fe80::a1b2:c3d4:e5f6:7890`

### 3.3 Message Signing

Messages are signed using the private key:

```
1. Construct canonical message text:
   - All headers in sorted order (excluding Signature header)
   - Blank line
   - Message body
2. Sign canonical text with Ed25519 private key
3. Encode signature as Base64
4. Add Signature header to message
```

### 3.4 Signature Verification

Recipients verify signatures:

```
1. Extract Pubkey and Signature headers
2. Reconstruct canonical message (without Signature header)
3. Verify Ed25519 signature using public key
4. Derive CGA address from public key
5. Compare derived CGA with CGA-Address header
```

### 3.5 Identity Claims

Identity claims bind a human-readable handle to a CGA address:

**Compact Format**: `<handle>:<timestamp>:<signature>`

- `<handle>`: User's chosen handle (e.g., "alice")
- `<timestamp>`: Unix timestamp (milliseconds)
- `<signature>`: Base64-encoded signature of `handle:timestamp:cga`

**Verification**:
1. Parse compact claim
2. Verify signature using public key
3. Check timestamp is recent (within validity period, e.g., 30 days)
4. Confirm CGA address matches

---

## 4. BBS Metadata

### 4.1 Board Information Files

All metadata files in `/srv/bbs/etc/` are **plain text** and **read-only** to clients.

#### `/srv/bbs/etc/boardname`
Single line containing the BBS name.
```
The Aether Node #42
```

#### `/srv/bbs/etc/sysop`
Sysop name and optional contact info.
```
SysOp: Alice
Contact: alice@example.com
```

#### `/srv/bbs/etc/motd`
Message of the day (can be multiple lines).
```
Welcome to The Aether!

Today's system maintenance: 2:00 AM UTC
Please report bugs to the sysop.

Enjoy your stay!
```

#### `/srv/bbs/etc/location`
Physical location (optional).
```
San Francisco, CA, USA
```

#### `/srv/bbs/etc/description`
Board description (can be multiple lines).
```
A local mesh BBS serving the SF Bay Area.
Topics: Tech, local events, buy/sell/trade.
```

#### `/srv/bbs/etc/version`
BBS software and version.
```
9BBS/Zephyr 1.0.0
```

### 4.2 Network Information

Directory: `/srv/bbs/etc/nets/`

Each file represents a network carried by this BBS.

#### `/srv/bbs/etc/nets/fsxnet`
FSXNet areas carried by this board (one per line).
```
FSX_GEN
FSX_BBS
FSX_HAM
FSX_RETRO
```

#### `/srv/bbs/etc/nets/aethernet`
Local Aether mesh network info.
```
mesh: local-sf-bay
coverage: 10km radius
nodes: 23
```

### 4.3 Client Display

Clients SHOULD display board info when connecting:

```
┌─────────────────────────────────────┐
│  The Aether Node #42                │
│  SysOp: Alice                       │
│  San Francisco, CA, USA             │
├─────────────────────────────────────┤
│  MOTD:                              │
│  Welcome to The Aether!             │
│                                     │
│  Networks: FSXNet, AetherNet        │
└─────────────────────────────────────┘
```

---

## 5. Moderation System

### 5.1 Overview

9BBS uses a **Slashdot-style moderation system** with moderation points earned through participation.

### 5.2 Moderation Message Format

Moderation actions are stored in `/srv/bbs/moderation/` as RFC-822 style messages:

```
From: moderator-handle
Date: 1738005432123
Subject: Moderation
Room: moderation
Message-ID: <1738005432123-mod-device1>
Origin: device-uuid-1234
X-Client: TheAether-iOS/1.0
Mod-Target: <target-message-id>
Mod-Value: +1
Mod-Reason: Insightful
Mod-Points-Earned: 42
Mod-Points-Spent: 1

(Moderation action)
```

### 5.3 Moderation Headers

| Header | Description | Values |
|--------|-------------|--------|
| `Mod-Target` | Message-ID being moderated | `<msgid>` |
| `Mod-Value` | Moderation value | `+1` or `-1` |
| `Mod-Reason` | Reason for moderation | See below |
| `Mod-Points-Earned` | Moderator's lifetime points | Integer |
| `Mod-Points-Spent` | Points spent on this action | Usually `1` |

### 5.4 Moderation Reasons

**Positive** (+1):
- `Insightful`
- `Informative`
- `Funny`
- `Interesting`

**Negative** (-1):
- `Offtopic`
- `Troll`
- `Flamebait`

### 5.5 Point Management

Points are tracked per device/user:

- `/srv/bbs/moderation/points/<device-id>` - Current available points
- `/srv/bbs/moderation/earned/<device-id>` - Lifetime earned points

**Earning Points**:
- Running a relay node
- Active participation
- Good moderation history

**Spending Points**:
- Each moderation action costs 1 point

---

## 6. FSXNet Integration

### 6.1 Overview

9BBS can bridge to **FSXNet** (FidoNet-style network) for wider message distribution.

### 6.2 FSXNet Room Mapping

FSXNet echoareas map to rooms:

| FSXNet Area | 9BBS Room | Description |
|-------------|-----------|-------------|
| `FSX_GEN` | `/srv/bbs/rooms/FSX_GEN/` | General discussion |
| `FSX_BBS` | `/srv/bbs/rooms/FSX_BBS/` | BBS topics |
| `FSX_HAM` | `/srv/bbs/rooms/FSX_HAM/` | Ham radio |

### 6.3 Message Translation

**9BBS → FSXNet**:
1. Parse 9BBS message
2. Strip crypto headers (FSXNet doesn't support)
3. Convert to FTS-0001 packet format
4. Store crypto info in kludge line for round-trip
5. Send to FSXNet uplink

**FSXNet → 9BBS**:
1. Parse FTS-0001 packet
2. Convert to 9BBS RFC-822 format
3. Store in appropriate room
4. No signature/CGA (FSXNet messages are unsigned)

### 6.4 Carried Networks File

`/srv/bbs/etc/nets/fsxnet` lists areas:

```
FSX_GEN
FSX_BBS
FSX_HAM
FSX_RETRO
```

Clients can read this to discover available FSXNet areas.

---

## 7. Client Implementation Notes

### 7.1 Reading Messages

1. Connect to BBS via 9P
2. Read `/srv/bbs/etc/` files for board info
3. List rooms: `ls /srv/bbs/rooms/`
4. List messages in room: `ls /srv/bbs/rooms/lobby/`
5. Read message file
6. Parse RFC-822 headers
7. Verify signature if present
8. Display message

### 7.2 Posting Messages

1. Construct message with required headers
2. Sign message if identity exists
3. Generate Message-ID: `<timestamp>-<random>@<origin>`
4. Write to `/srv/bbs/rooms/<room>/<timestamp>-<msgid>`

### 7.3 Moderation

1. Check available points: read `/srv/bbs/moderation/points/<device-id>`
2. If points > 0, construct moderation message
3. Write to `/srv/bbs/moderation/<timestamp>`
4. Server deducts point

### 7.4 Signature Verification

**Valid Signature**:
- ✅ Green badge: `checkmark.seal.fill`
- Message is authentic

**Invalid/Missing Signature**:
- ❌ Red badge: `xmark.seal.fill`
- ⚠️ Warning if no signature

**Verified Identity Claim**:
- 🛡️ Blue badge: `person.badge.shield.checkmark`
- Handle is cryptographically bound to CGA

---

## 8. Implementation Checklist

### Server (Zephyr) Implementation

- [ ] Filesystem structure: `/srv/bbs/etc/`, `/srv/bbs/rooms/`, etc.
- [ ] Board metadata files (boardname, sysop, motd, etc.)
- [ ] Message storage: timestamp-msgid naming
- [ ] RFC-822 message parsing
- [ ] Signature verification (optional but recommended)
- [ ] Moderation point tracking
- [ ] FSXNet bridge (optional)
- [ ] Replica log sync (optional, for mesh)

### Client (iOS) Implementation

- [x] 9P client
- [x] Read board metadata
- [x] List rooms and messages
- [x] Parse RFC-822 messages
- [x] CGA identity generation
- [x] Message signing
- [x] Signature verification
- [x] Identity claim generation
- [x] Moderation UI
- [ ] Sync with updated `/srv/bbs/rooms/` path

---

## 9. Example Session

```
# Connect to BBS
$ 9p connect tcp!bbs.local:564

# Read board info
$ cat /srv/bbs/etc/boardname
The Aether Node #42

$ cat /srv/bbs/etc/motd
Welcome to The Aether!
Enjoy your stay!

$ cat /srv/bbs/etc/nets/fsxnet
FSX_GEN
FSX_BBS

# List rooms
$ ls /srv/bbs/rooms/
lobby/
general/
tech/
FSX_GEN/

# Read messages in lobby
$ ls /srv/bbs/rooms/lobby/
1738005432123-abc123def456
1738005433456-def789ghi012

$ cat /srv/bbs/rooms/lobby/1738005432123-abc123def456
From: alice
Date: 1738005432123
Subject: Hello World
Room: lobby
Message-ID: <1738005432123-abc123@node1>
Origin: device-uuid-1234
X-Client: TheAether-iOS/1.0
CGA-Address: fe80::a1b2:c3d4:e5f6:7890
Pubkey: SGVsbG8gV29ybGQgUHVibGljS2V5RGF0YQ==
Signature: VGhpc0lzQVNpZ25hdHVyZQ==

This is my first message!

# Post a message
$ cat > /srv/bbs/rooms/lobby/1738005500000-newmsg123
From: bob
Date: 1738005500000
Subject: Re: Hello World
Room: lobby
Message-ID: <1738005500000-newmsg123@node2>
Origin: device-uuid-5678
X-Client: TheAether-iOS/1.0
In-Reply-To: <1738005432123-abc123@node1>

Welcome Alice!
^D

# Moderate a message
$ cat > /srv/bbs/moderation/1738005600000
From: bob
Date: 1738005600000
Subject: Moderation
Room: moderation
Message-ID: <1738005600000-mod-bob>
Origin: device-uuid-5678
Mod-Target: <1738005432123-abc123@node1>
Mod-Value: +1
Mod-Reason: Insightful
Mod-Points-Earned: 15
Mod-Points-Spent: 1

(Moderation action)
^D
```

---

## 10. Version History

- **v1.0** (2025-01-27): Initial specification
  - RFC-822 message format
  - CGA-based identity
  - Filesystem structure with `/srv/bbs/etc/` and `/srv/bbs/rooms/`
  - BBS metadata specification
  - Moderation system
  - FSXNet integration

---

## 11. References

- RFC 822: Standard for ARPA Internet Text Messages
- RFC 3972: Cryptographically Generated Addresses (CGA)
- FTS-0001: FidoNet packet format
- 9P Protocol: Plan 9 filesystem protocol
- Slashdot Moderation: Community-driven content curation

---

**Document Status**: Draft
**Last Updated**: 2025-01-27
**Authors**: The Aether Project
**License**: Public Domain
