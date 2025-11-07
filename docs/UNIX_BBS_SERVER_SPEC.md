# Unix 9P BBS Server Specification

## Overview

A lightweight C implementation of a 9P2000 BBS server for Unix (BSD/Linux/macOS) that provides the same filesystem interface as the Zephyr embedded BBS. This server enables iOS app testing without requiring physical Bluetooth hardware.

**Target**: Single-file C server (~1500 lines) using BSD sockets and POSIX APIs.

## Architecture

```
[iOS App] --TCP 9P--> [Unix Server] --> [In-Memory BBS State]
                            |
                            └--> [Optional: LittleFS persistence]
```

## Filesystem Interface (Must Match Zephyr)

```
/
├── rooms/
│   ├── lobby/
│   │   ├── 1            (message file)
│   │   ├── 2
│   │   └── ...
│   └── tech/
│       └── ...
├── chat/
│   ├── lobby            (blocking read stream)
│   ├── tech
│   ├── post             (write-only)
│   └── users            (read-only)
└── etc/
    ├── roomlist         (read-only)
    ├── boardname        (read/write if admin)
    ├── motd             (read/write if admin)
    └── users/
        └── alice/
            ├── password (write-only)
            ├── room     (current room)
            └── sig      (signature)
```

## Core Data Structures

### Message

```c
#define MAX_USERNAME 32
#define MAX_ROOMNAME 32
#define MAX_MESSAGE_SIZE 2048

struct bbs_message {
    uint32_t id;
    char from[MAX_USERNAME];
    char to[MAX_ROOMNAME];
    time_t timestamp;
    uint32_t reply_to;  // 0 = none
    char *body;         // malloc'd
    size_t body_len;
    bool deleted;
};
```

### Room

```c
#define MAX_ROOMS 8
#define MAX_MESSAGES_PER_ROOM 20

struct bbs_room {
    char name[MAX_ROOMNAME];
    struct bbs_message messages[MAX_MESSAGES_PER_ROOM];
    uint32_t message_count;
    uint32_t next_id;
    bool active;
};
```

### Chat Room (Ring Buffer)

```c
#define MAX_CHAT_ROOMS 4
#define MAX_CHAT_MESSAGES 50
#define MAX_CHAT_MESSAGE_LEN 256
#define MAX_CHAT_USERS 8

struct chat_message {
    char username[MAX_USERNAME];
    char text[MAX_CHAT_MESSAGE_LEN];
    time_t timestamp;
};

struct chat_room {
    char name[MAX_ROOMNAME];
    struct chat_message messages[MAX_CHAT_MESSAGES];
    uint32_t read_pos;   // Oldest message
    uint32_t write_pos;  // Next write position
    uint32_t count;      // Number of messages in buffer
    pthread_mutex_t lock;
    pthread_cond_t new_msg;  // Signal for blocking readers
};

struct chat_user {
    char username[MAX_USERNAME];
    uint32_t read_positions[MAX_CHAT_ROOMS];  // Per-room read pos
    time_t last_seen;
};

struct chat_instance {
    struct chat_room rooms[MAX_CHAT_ROOMS];
    struct chat_user users[MAX_CHAT_USERS];
    uint32_t room_count;
    uint32_t user_count;
    pthread_mutex_t lock;
};
```

### User

```c
#define MAX_USERS 8

struct bbs_user {
    char username[MAX_USERNAME];
    char password[64];   // bcrypt hash
    char sig[128];
    char current_room[MAX_ROOMNAME];
    bool active;
    bool is_admin;
};
```

### BBS Instance

```c
struct bbs_instance {
    struct bbs_room rooms[MAX_ROOMS];
    uint32_t room_count;

    struct bbs_user users[MAX_USERS];
    uint32_t user_count;

    struct chat_instance chat;

    char boardname[64];
    char sysop[64];
    char motd[256];

    pthread_mutex_t lock;
};
```

## 9P Protocol Implementation

### Minimal 9P2000 Subset

**Required messages:**
- `Tversion/Rversion` - Protocol negotiation
- `Tattach/Rattach` - Root filesystem access
- `Twalk/Rwalk` - Path traversal
- `Topen/Ropen` - Open files
- `Tread/Rread` - Read data
- `Twrite/Rwrite` - Write data
- `Tclunk/Rclunk` - Close files
- `Tstat/Rstat` - File metadata
- `Rerror` - Error responses

**Message structure:**
```c
struct p9_header {
    uint32_t size;   // Total message size
    uint8_t type;    // Message type
    uint16_t tag;    // Request tag
} __attribute__((packed));

#define P9_TVERSION 100
#define P9_RVERSION 101
#define P9_TATTACH  104
#define P9_RATTACH  105
#define P9_TWALK    110
#define P9_RWALK    111
#define P9_TOPEN    112
#define P9_ROPEN    113
#define P9_TREAD    116
#define P9_RREAD    117
#define P9_TWRITE   118
#define P9_RWRITE   119
#define P9_TCLUNK   120
#define P9_RCLUNK   121
#define P9_TSTAT    124
#define P9_RSTAT    125
#define P9_RERROR   107
```

### Fid Table

```c
#define MAX_FIDS 256

enum fid_type {
    FID_NONE,
    FID_ROOT,
    FID_ROOMS_DIR,
    FID_ROOM_DIR,
    FID_MESSAGE_FILE,
    FID_CHAT_DIR,
    FID_CHAT_ROOM,     // Blocking read
    FID_CHAT_POST,     // Write-only
    FID_CHAT_USERS,
    FID_ETC_DIR,
    FID_ETC_FILE,
};

struct fid_entry {
    uint32_t fid;
    enum fid_type type;
    void *data;        // Points to room, message, etc.
    uint32_t offset;   // Read/write position
    bool open;
    char path[256];
};

struct fid_table {
    struct fid_entry entries[MAX_FIDS];
    pthread_mutex_t lock;
};
```

### Connection State

```c
struct p9_connection {
    int sockfd;
    struct fid_table fids;
    char username[MAX_USERNAME];  // Authenticated user
    bool authenticated;
    uint32_t msize;    // Negotiated max message size
    pthread_t thread;  // Per-connection thread
};
```

## Core Functions

### Server Lifecycle

```c
// Initialize BBS state
int bbs_init(struct bbs_instance *bbs);

// Create default rooms (lobby, tech)
int bbs_create_default_rooms(struct bbs_instance *bbs);

// Initialize chat subsystem
int chat_init(struct chat_instance *chat);

// Start TCP server
int server_start(struct bbs_instance *bbs, uint16_t port);

// Handle client connection (runs in thread)
void *handle_connection(void *arg);

// Cleanup
void bbs_destroy(struct bbs_instance *bbs);
```

### Chat Operations

```c
// Post message to chat room
int chat_post(struct chat_instance *chat, const char *room,
              const char *username, const char *text);

// Read messages (blocking with timeout)
// Returns formatted string: "[HH:MM:SS] username: text\n..."
ssize_t chat_read(struct chat_instance *chat, const char *room,
                  const char *username, char *buf, size_t bufsize,
                  int timeout_sec);

// Get active users
ssize_t chat_get_users(struct chat_instance *chat, char *buf, size_t bufsize);

// Create/find chat room
struct chat_room *chat_find_room(struct chat_instance *chat, const char *name);
```

### BBS Operations

```c
// Post message to BBS room
int bbs_post_message(struct bbs_instance *bbs, const char *room,
                     const char *username, const char *body,
                     uint32_t reply_to);

// Get message
struct bbs_message *bbs_get_message(struct bbs_instance *bbs,
                                    const char *room, uint32_t id);

// Format message for reading
ssize_t bbs_format_message(struct bbs_message *msg, char *buf, size_t bufsize);

// List messages in room (directory listing)
ssize_t bbs_list_room(struct bbs_room *room, char *buf, size_t bufsize,
                      uint64_t offset);
```

### 9P Protocol Handlers

```c
// Message dispatch
int p9_handle_message(struct p9_connection *conn, struct bbs_instance *bbs,
                      uint8_t *req, size_t req_len,
                      uint8_t **resp, size_t *resp_len);

// Individual handlers
int p9_version(struct p9_connection *conn, uint8_t *req, uint8_t **resp);
int p9_attach(struct p9_connection *conn, uint8_t *req, uint8_t **resp);
int p9_walk(struct p9_connection *conn, struct bbs_instance *bbs,
            uint8_t *req, uint8_t **resp);
int p9_open(struct p9_connection *conn, struct bbs_instance *bbs,
            uint8_t *req, uint8_t **resp);
int p9_read(struct p9_connection *conn, struct bbs_instance *bbs,
            uint8_t *req, uint8_t **resp);
int p9_write(struct p9_connection *conn, struct bbs_instance *bbs,
             uint8_t *req, uint8_t **resp);
int p9_clunk(struct p9_connection *conn, uint8_t *req, uint8_t **resp);
int p9_stat(struct p9_connection *conn, struct bbs_instance *bbs,
            uint8_t *req, uint8_t **resp);
```

### Utility Functions

```c
// Encode/decode 9P types (little-endian)
void put_u8(uint8_t **p, uint8_t v);
void put_u16(uint8_t **p, uint16_t v);
void put_u32(uint8_t **p, uint32_t v);
void put_u64(uint8_t **p, uint64_t v);
void put_string(uint8_t **p, const char *s, uint16_t len);

uint8_t get_u8(uint8_t **p);
uint16_t get_u16(uint8_t **p);
uint32_t get_u32(uint8_t **p);
uint64_t get_u64(uint8_t **p);
void get_string(uint8_t **p, char *buf, size_t bufsize);

// Qid generation (unique file identifiers)
uint64_t make_qid(enum fid_type type, uint32_t index);

// Stat structure encoding
size_t encode_stat(uint8_t *buf, const char *name, uint64_t qid,
                   uint32_t mode, uint64_t length, const char *owner);

// Error response
int send_error(struct p9_connection *conn, uint16_t tag, const char *errstr);
```

## Implementation Details

### Chat Blocking Read

**Key feature**: When client reads `/chat/lobby` and no new messages exist, server **blocks** until:
1. New message arrives (another client writes to `/chat/post`)
2. Timeout expires (30 seconds default)

```c
ssize_t chat_read(struct chat_instance *chat, const char *room,
                  const char *username, char *buf, size_t bufsize,
                  int timeout_sec)
{
    struct chat_room *r = chat_find_room(chat, room);
    if (!r) return -1;

    struct chat_user *user = find_or_create_user(chat, username);
    uint32_t user_read_pos = user->read_positions[room_index];

    pthread_mutex_lock(&r->lock);

    // Check for new messages
    while (r->count == 0 || user_read_pos >= r->write_pos) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_sec;

        // Wait for signal or timeout
        int ret = pthread_cond_timedwait(&r->new_msg, &r->lock, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&r->lock);
            return 0;  // No new messages
        }
    }

    // Format new messages
    size_t written = 0;
    while (user_read_pos < r->write_pos && written < bufsize - 100) {
        struct chat_message *msg = &r->messages[user_read_pos % MAX_CHAT_MESSAGES];

        struct tm *tm = localtime(&msg->timestamp);
        int n = snprintf(buf + written, bufsize - written,
                        "[%02d:%02d:%02d] %s: %s\n",
                        tm->tm_hour, tm->tm_min, tm->tm_sec,
                        msg->username, msg->text);

        written += n;
        user_read_pos++;
    }

    user->read_positions[room_index] = user_read_pos;
    pthread_mutex_unlock(&r->lock);

    return written;
}
```

### Chat Post (Signal Waiting Readers)

```c
int chat_post(struct chat_instance *chat, const char *room,
              const char *username, const char *text)
{
    struct chat_room *r = chat_find_room(chat, room);
    if (!r) return -1;

    pthread_mutex_lock(&r->lock);

    // Add to ring buffer
    struct chat_message *msg = &r->messages[r->write_pos % MAX_CHAT_MESSAGES];
    strncpy(msg->username, username, MAX_USERNAME - 1);
    strncpy(msg->text, text, MAX_CHAT_MESSAGE_LEN - 1);
    msg->timestamp = time(NULL);

    r->write_pos++;
    if (r->count < MAX_CHAT_MESSAGES) {
        r->count++;
    } else {
        r->read_pos++;  // Overwrite oldest
    }

    // Wake up all blocked readers
    pthread_cond_broadcast(&r->new_msg);

    pthread_mutex_unlock(&r->lock);
    return 0;
}
```

### 9P Read Handler (Chat vs Regular Files)

```c
int p9_read(struct p9_connection *conn, struct bbs_instance *bbs,
            uint8_t *req, uint8_t **resp)
{
    uint32_t fid = get_u32(&req);
    uint64_t offset = get_u64(&req);
    uint32_t count = get_u32(&req);

    struct fid_entry *fe = fid_lookup(&conn->fids, fid);
    if (!fe) return send_error(conn, tag, "invalid fid");

    switch (fe->type) {
    case FID_CHAT_ROOM: {
        // BLOCKING READ
        char *room_name = (char *)fe->data;
        char buf[8192];

        ssize_t n = chat_read(&bbs->chat, room_name,
                             conn->username, buf, sizeof(buf), 30);

        // Send Rread with data
        *resp = malloc(7 + 4 + n);
        uint8_t *p = *resp;
        put_u32(&p, 7 + 4 + n);  // size
        put_u8(&p, P9_RREAD);
        put_u16(&p, tag);
        put_u32(&p, n);  // count
        memcpy(p, buf, n);
        return 0;
    }

    case FID_MESSAGE_FILE: {
        // Regular file read
        struct bbs_message *msg = (struct bbs_message *)fe->data;
        char buf[4096];
        ssize_t n = bbs_format_message(msg, buf, sizeof(buf));

        // Handle offset
        if (offset >= n) n = 0;
        else if (offset + count < n) n = count;
        else n = n - offset;

        // Send Rread
        *resp = malloc(7 + 4 + n);
        // ... encode response ...
        return 0;
    }

    // ... other file types ...
    }
}
```

## Server Main Loop

```c
int main(int argc, char **argv)
{
    struct bbs_instance bbs;
    uint16_t port = 9564;  // Default 9P port

    // Parse args
    if (argc > 1) port = atoi(argv[1]);

    // Initialize BBS
    if (bbs_init(&bbs) < 0) {
        fprintf(stderr, "Failed to initialize BBS\n");
        return 1;
    }

    // Create default content
    bbs_create_default_rooms(&bbs);
    chat_init(&bbs.chat);

    // Start server
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listenfd, 5) < 0) {
        perror("listen");
        return 1;
    }

    printf("9P BBS server listening on port %d\n", port);

    // Accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int connfd = accept(listenfd, (struct sockaddr *)&client_addr, &client_len);
        if (connfd < 0) {
            perror("accept");
            continue;
        }

        printf("Client connected from %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        // Spawn thread to handle connection
        struct p9_connection *conn = malloc(sizeof(*conn));
        conn->sockfd = connfd;
        conn->authenticated = false;
        fid_table_init(&conn->fids);

        pthread_create(&conn->thread, NULL, handle_connection, conn);
        pthread_detach(conn->thread);
    }

    // Cleanup (never reached)
    bbs_destroy(&bbs);
    close(listenfd);
    return 0;
}

void *handle_connection(void *arg)
{
    struct p9_connection *conn = arg;
    uint8_t *req = NULL, *resp = NULL;
    size_t req_len = 0, resp_len = 0;

    while (1) {
        // Read 9P message header
        struct p9_header hdr;
        ssize_t n = read(conn->sockfd, &hdr, sizeof(hdr));
        if (n <= 0) break;

        // Read message body
        req_len = hdr.size - sizeof(hdr);
        req = malloc(req_len);
        n = read(conn->sockfd, req, req_len);
        if (n <= 0) break;

        // Handle message
        if (p9_handle_message(conn, &global_bbs, req, req_len,
                             &resp, &resp_len) < 0) {
            break;
        }

        // Send response
        write(conn->sockfd, resp, resp_len);

        free(req);
        free(resp);
        req = resp = NULL;
    }

    // Cleanup
    close(conn->sockfd);
    fid_table_destroy(&conn->fids);
    free(conn);
    return NULL;
}
```

## Build and Run

### Makefile

```makefile
CC = cc
CFLAGS = -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

TARGET = 9pbbs
SRCS = 9pbbs.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
```

### Usage

```bash
# Build
make

# Run on default port (9564)
./9pbbs

# Run on custom port
./9pbbs 5640

# Test with 9p mount (Linux)
mount -t 9p -o trans=tcp,port=9564 127.0.0.1 /mnt/bbs

# Read chat
tail -f /mnt/bbs/chat/lobby &

# Post message
echo "Hello from Unix!" > /mnt/bbs/chat/post
```

## Compatibility Matrix

| Feature | Zephyr BBS | Unix BBS | iOS Client |
|---------|-----------|----------|------------|
| 9P2000 protocol | ✓ | ✓ | ✓ |
| TCP transport | ✓ | ✓ | ✓ |
| L2CAP transport | ✓ | ✗ | ✓ |
| BBS rooms | ✓ | ✓ | ✓ |
| Chat (blocking) | ✓ | ✓ | ✓ |
| User auth | ✓ | ✓ | ✓ |
| LittleFS persist | ✓ | Optional | N/A |

## Testing

### Unit Tests

```c
void test_chat_post_and_read(void) {
    struct chat_instance chat;
    chat_init(&chat);

    chat_post(&chat, "lobby", "alice", "Hello!");

    char buf[1024];
    ssize_t n = chat_read(&chat, "lobby", "bob", buf, sizeof(buf), 1);

    assert(n > 0);
    assert(strstr(buf, "alice: Hello!") != NULL);
}

void test_chat_blocking(void) {
    // Fork and test timeout
    // ...
}
```

### Integration Tests

```bash
#!/bin/bash
# Start server
./9pbbs &
PID=$!
sleep 1

# Mount
mkdir -p /tmp/bbs
mount -t 9p -o trans=tcp,port=9564 127.0.0.1 /tmp/bbs

# Test chat
cat /tmp/bbs/chat/lobby &
echo "test message" > /tmp/bbs/chat/post
sleep 1

# Cleanup
kill $PID
umount /tmp/bbs
```

## File Size Estimate

- Protocol implementation: ~400 lines
- Fid management: ~150 lines
- BBS operations: ~300 lines
- Chat implementation: ~400 lines
- Network/threading: ~200 lines
- Main/utilities: ~150 lines
- **Total: ~1600 lines**

## Dependencies

- POSIX threads (`pthread`)
- BSD sockets
- Standard C library
- Optional: `libbsd` for `strlcpy` etc.

## Performance

- **Concurrent clients**: Limited by threads (~100s)
- **Message latency**: <1ms (local network)
- **Throughput**: Limited by 9P protocol overhead (~10MB/s)
- **Memory**: ~100KB base + ~10KB per connection

## Future Enhancements

1. **SQLite persistence**: Store messages/users in database
2. **TLS support**: Encrypt TCP connections
3. **WebSocket transport**: For web clients
4. **Room moderation**: Ban users, delete messages
5. **File attachments**: Upload images/files to BBS

## Reference

- 9P2000 spec: http://9p.cat-v.org/
- Plan 9 manual: http://man.cat-v.org/plan_9/5/intro
- Zephyr implementation: `/src/9bbs/9bbs.c`, `/src/9bbs/chat.c`
- iOS client guide: `/docs/IOS_CHAT_CLIENT_GUIDE.md`
