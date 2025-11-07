# 9BBS Implementation Guide for Zephyr

**Quick reference for implementing the 9BBS specification on Zephyr firmware**

See [SPEC.md](./SPEC.md) for the complete specification.

---

## Minimum Viable Implementation

To get a basic BBS working, implement these **required** components:

### 1. Filesystem Structure

```
/srv/bbs/
  ├── etc/
  │   ├── boardname          # "My Zephyr BBS"
  │   ├── sysop              # "SysOp: admin"
  │   ├── motd               # "Welcome!"
  │   └── version            # "9BBS/Zephyr 1.0"
  └── rooms/
      ├── lobby/
      │   ├── 1738005432123-abc123def456
      │   └── ...
      └── general/
          └── ...
```

**Priority**: `/srv/bbs/etc/` files are **read-only**. `/srv/bbs/rooms/` is **read-write**.

### 2. Message File Format

Each message is a plain text file with RFC-822 headers:

```
From: alice
Date: 1738005432123
Subject: Hello World
Room: lobby
Message-ID: <1738005432123-abc123@node1>
Origin: device-uuid-1234
X-Client: TheAether-iOS/1.0

This is the message body.
```

**Minimum Required Headers**:
- `From`: Author handle
- `Date`: Unix timestamp (milliseconds)
- `Subject`: Message subject
- `Room`: Room name
- `Message-ID`: Unique ID
- `Origin`: Device/node ID
- `X-Client`: Client identifier

### 3. Message Filename Convention

```
<timestamp>-<msgid>
```

Example: `1738005432123-abc123def456`

- **`<timestamp>`**: 13-digit Unix milliseconds
- **`<msgid>`**: First 16 chars of Message-ID (alphanumeric only)

### 4. Client Operations

**Read messages**:
1. Client lists `/srv/bbs/rooms/lobby/`
2. Client reads each file
3. Client parses RFC-822 headers

**Post message**:
1. Client creates message with headers + body
2. Client writes to `/srv/bbs/rooms/<room>/<timestamp>-<msgid>`

---

## Optional: Cryptographic Identity Support

If implementing signature verification:

### CGA Address Verification

```c
bool verify_cga_address(const uint8_t *pubkey, const char *cga_addr) {
    // 1. SHA-256 hash of public key
    uint8_t hash[32];
    sha256(pubkey, 32, hash);

    // 2. Format as IPv6 link-local
    char derived[40];
    sprintf(derived, "fe80::%02x%02x:%02x%02x:%02x%02x:%02x%02x",
            hash[0], hash[1], hash[2], hash[3],
            hash[4], hash[5], hash[6], hash[7]);

    // 3. Compare with CGA-Address header
    return strcmp(derived, cga_addr) == 0;
}
```

### Ed25519 Signature Verification

```c
bool verify_message(const char *message, const uint8_t *pubkey, const uint8_t *signature) {
    // Use ed25519_verify() or similar
    // message = canonical message text (headers + body, no Signature header)
    return ed25519_verify(signature, message, strlen(message), pubkey);
}
```

**If signature verification fails**: Still display message, but mark as "unverified".

---

## Optional: Moderation System

### Moderation Files

Location: `/srv/bbs/moderation/<timestamp>`

Format:
```
From: moderator-handle
Date: 1738005432123
Subject: Moderation
Room: moderation
Message-ID: <1738005432123-mod-device1>
Mod-Target: <target-message-id>
Mod-Value: +1
Mod-Reason: Insightful
Mod-Points-Earned: 42
Mod-Points-Spent: 1

(Moderation action)
```

### Point Tracking

```
/srv/bbs/moderation/points/<device-id>     # Text file: "5"
/srv/bbs/moderation/earned/<device-id>     # Text file: "42"
```

---

## Optional: FSXNet Bridge

### Carried Networks File

`/srv/bbs/etc/nets/fsxnet`:
```
FSX_GEN
FSX_BBS
FSX_HAM
```

Each line is an FSXNet echoarea that maps to a room in `/srv/bbs/rooms/`.

### Message Translation

**FSXNet → 9BBS**:
1. Parse FTS-0001 packet
2. Convert to RFC-822 format
3. Write to `/srv/bbs/rooms/FSX_GEN/<timestamp>-<msgid>`

**9BBS → FSXNet**:
1. Read message from `/srv/bbs/rooms/FSX_GEN/`
2. Convert to FTS-0001 packet
3. Send to FSXNet uplink

---

## Implementation Phases

### Phase 1: Basic BBS (MVP)
- [ ] Create `/srv/bbs/etc/` files (boardname, sysop, motd, version)
- [ ] Create `/srv/bbs/rooms/` directory structure
- [ ] Support reading messages (9P read operations)
- [ ] Support posting messages (9P write operations)
- [ ] Message filename: `<timestamp>-<msgid>`

### Phase 2: Identity Support
- [ ] Parse `CGA-Address`, `Pubkey`, `Signature` headers
- [ ] Verify CGA addresses (SHA-256 of pubkey)
- [ ] Verify Ed25519 signatures
- [ ] Mark messages as verified/unverified

### Phase 3: Moderation
- [ ] Create `/srv/bbs/moderation/` directory
- [ ] Point tracking files
- [ ] Parse moderation messages
- [ ] Deduct points on moderation action

### Phase 4: FSXNet Bridge (if desired)
- [ ] FSXNet uplink connection (BINKP/TCP)
- [ ] FTS-0001 packet parser
- [ ] Message translation (FSXNet ↔ 9BBS)
- [ ] `/srv/bbs/etc/nets/fsxnet` file

---

## Testing Checklist

### Basic Functionality
- [ ] iOS app connects via 9P
- [ ] iOS app reads `/srv/bbs/etc/boardname`
- [ ] iOS app lists rooms from `/srv/bbs/rooms/`
- [ ] iOS app reads messages from `/srv/bbs/rooms/lobby/`
- [ ] iOS app posts message to `/srv/bbs/rooms/lobby/`
- [ ] Posted message appears with correct filename

### Identity (if implemented)
- [ ] iOS app posts signed message (with CGA-Address, Pubkey, Signature)
- [ ] Server verifies signature correctly
- [ ] Invalid signatures are marked as unverified

### Moderation (if implemented)
- [ ] iOS app reads points from `/srv/bbs/moderation/points/<device>`
- [ ] iOS app posts moderation action
- [ ] Server deducts point correctly

---

## Example: Simple Zephyr Implementation

```c
// Minimal BBS server pseudocode

void setup_bbs() {
    // Create directory structure
    mkdir("/srv/bbs/etc");
    mkdir("/srv/bbs/rooms");
    mkdir("/srv/bbs/rooms/lobby");

    // Write metadata files
    write_file("/srv/bbs/etc/boardname", "Zephyr BBS Node");
    write_file("/srv/bbs/etc/sysop", "SysOp: admin");
    write_file("/srv/bbs/etc/motd", "Welcome to Zephyr BBS!");
    write_file("/srv/bbs/etc/version", "9BBS/Zephyr 1.0");
}

void handle_write_message(const char *path, const char *data) {
    // Client writes: /srv/bbs/rooms/lobby/1738005432123-abc123

    // 1. Parse path to extract room
    char room[64];
    extract_room(path, room);  // "lobby"

    // 2. Validate message format (has required headers)
    if (!validate_message(data)) {
        return ERROR_INVALID;
    }

    // 3. Write to filesystem
    write_file(path, data);

    // 4. Optional: Verify signature if present
    if (has_signature(data)) {
        bool valid = verify_signature(data);
        mark_as_verified(path, valid);
    }
}

void handle_read_metadata(const char *path) {
    // Client reads: /srv/bbs/etc/boardname
    // Just return file contents (read-only)
    return read_file(path);
}
```

---

## Key Design Principles

1. **Simple is better**: Start with plain text files, no database needed
2. **9P native**: Everything is files and directories
3. **Offline-first**: Write messages even when disconnected
4. **Optional crypto**: Signature verification is nice-to-have, not required
5. **Backward compatible**: Unsigned messages work fine
6. **Filesystem as API**: No special commands, just read/write files

---

## iOS App Assumptions

The iOS app expects:

1. **Rooms at `/srv/bbs/rooms/`** (not `/srv/bbs/` directly)
2. **Metadata at `/srv/bbs/etc/`** (boardname, sysop, motd)
3. **RFC-822 message format** (headers + blank line + body)
4. **Message filenames**: `<13-digit-timestamp>-<16-char-msgid>`

---

## Questions?

See [SPEC.md](./SPEC.md) for complete details on:
- Message format (all headers)
- CGA address generation
- Signature algorithms
- Moderation point system
- FSXNet integration

---

**Last Updated**: 2025-01-27
**Status**: Ready for implementation
