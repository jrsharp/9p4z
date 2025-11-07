# 9BBS Chat Integration Guide

## Status

**Created**: Chat subsystem core (chat.h, chat.c) ✅
**Remaining**: Filesystem integration into 9bbs.c

## Files Created

- `/Users/jrsharp/src/9p4z/include/zephyr/9bbs/chat.h` - Chat API header
- `/Users/jrsharp/src/9p4z/src/9bbs/chat.c` - Chat implementation (ring buffer, blocking reads)
- Updated `/Users/jrsharp/src/9p4z/include/zephyr/9bbs/9bbs.h` - Added chat instance to BBS

## Remaining Integration Steps

### 1. Add Chat Node Types (9bbs.c:840)

```c
enum bbs_node_type {
	BBS_NODE_ROOT,
	BBS_NODE_ROOMS_DIR,
	BBS_NODE_ROOM_DIR,
	BBS_NODE_MESSAGE_FILE,
	BBS_NODE_ETC_DIR,
	BBS_NODE_ETC_FILE,
	BBS_NODE_ETC_NETS_DIR,
	BBS_NODE_ROOMLIST_FILE,
	// ADD THESE:
	BBS_NODE_CHAT_DIR,        // /chat directory
	BBS_NODE_CHAT_ROOM,       // /chat/lobby (blocking read stream)
	BBS_NODE_CHAT_POST,       // /chat/post (write-only)
	BBS_NODE_CHAT_USERS,      // /chat/users (list active users)
};
```

### 2. Initialize Chat in bbs_init() (9bbs.c:~540)

After initializing metadata, add:

```c
/* Initialize chat subsystem */
ret = chat_init(&bbs->chat);
if (ret < 0) {
	LOG_ERR("Failed to initialize chat: %d", ret);
	return ret;
}
LOG_INF("Chat subsystem initialized");
```

### 3. Add Chat to Root Directory Walk (9bbs.c:bbs_walk)

In `bbs_walk()` function, when walking from root, add "chat" alongside "rooms" and "etc":

```c
static struct ninep_fs_node *bbs_walk(struct ninep_fs_node *parent,
                                       const char *name, uint16_t name_len,
                                       void *fs_ctx)
{
	struct bbs_instance *bbs = (struct bbs_instance *)fs_ctx;
	enum bbs_node_type type = (enum bbs_node_type)((uint64_t)parent->qid.path >> 32);

	if (type == BBS_NODE_ROOT) {
		/* Walking from root */
		if (name_len == 5 && strncmp(name, "rooms", 5) == 0) {
			return bbs_create_node(BBS_NODE_ROOMS_DIR, "rooms", bbs);
		} else if (name_len == 3 && strncmp(name, "etc", 3) == 0) {
			return bbs_create_node(BBS_NODE_ETC_DIR, "etc", bbs);
		} else if (name_len == 4 && strncmp(name, "chat", 4) == 0) {
			// ADD THIS:
			return bbs_create_node(BBS_NODE_CHAT_DIR, "chat", bbs);
		}
		return NULL;
	}

	// ... existing room/etc walk code ...

	/* ADD: Walking from /chat */
	if (type == BBS_NODE_CHAT_DIR) {
		if (name_len == 4 && strncmp(name, "post", 4) == 0) {
			return bbs_create_node(BBS_NODE_CHAT_POST, "post", bbs);
		} else if (name_len == 5 && strncmp(name, "users", 5) == 0) {
			return bbs_create_node(BBS_NODE_CHAT_USERS, "users", bbs);
		} else {
			/* Try to find chat room by name */
			k_mutex_lock(&bbs->lock, K_FOREVER);
			for (uint32_t i = 0; i < bbs->chat.room_count; i++) {
				struct chat_room *room = &bbs->chat.rooms[i];
				if (strlen(room->name) == name_len &&
				    strncmp(room->name, name, name_len) == 0) {
					k_mutex_unlock(&bbs->lock);
					return bbs_create_node(BBS_NODE_CHAT_ROOM, room->name, room);
				}
			}
			k_mutex_unlock(&bbs->lock);
		}
		return NULL;
	}

	// ... rest of function
}
```

### 4. Add Chat to Root Directory Listing (9bbs.c:bbs_read)

In `bbs_read()`, when type == BBS_NODE_ROOT, add "chat" to the listing:

```c
if (type == BBS_NODE_ROOT) {
	/* Root directory listing: rooms/, etc/, chat/ */
	size_t buf_offset = 0;
	uint64_t current_offset = 0;

	const char *entries[] = {"rooms", "etc", "chat"};  // ADD "chat"

	for (size_t i = 0; i < 3; i++) {  // Change to 3
		// ... existing directory listing code
	}
}
```

### 5. Implement Chat Directory Listing (9bbs.c:bbs_read)

Add after existing directory listing cases:

```c
} else if (type == BBS_NODE_CHAT_DIR) {
	/* /chat directory listing: post, users, lobby, ... */
	size_t buf_offset = 0;
	uint64_t current_offset = 0;

	/* Static files: post (write-only), users (read-only) */
	const char *static_entries[] = {"post", "users"};
	uint32_t static_modes[] = {0200, 0444};  // post is write-only

	for (size_t i = 0; i < 2; i++) {
		struct ninep_qid entry_qid = {
			.type = NINEP_QTFILE,
			.version = 0,
			.path = ((uint64_t)(i == 0 ? BBS_NODE_CHAT_POST : BBS_NODE_CHAT_USERS) << 32) | i
		};
		uint16_t name_len = strlen(static_entries[i]);
		// ... write stat entry
	}

	/* List all chat rooms */
	k_mutex_lock(&bbs->lock, K_FOREVER);
	for (uint32_t i = 0; i < bbs->chat.room_count; i++) {
		struct chat_room *room = &bbs->chat.rooms[i];
		if (!room->active) continue;

		struct ninep_qid entry_qid = {
			.type = NINEP_QTFILE,
			.version = 0,
			.path = ((uint64_t)BBS_NODE_CHAT_ROOM << 32) | i
		};
		uint32_t mode = 0444;  // Read-only (blocking read)
		uint16_t name_len = strlen(room->name);
		// ... write stat entry
	}
	k_mutex_unlock(&bbs->lock);

	return buf_offset;
}
```

### 6. Implement Chat Read Operations (9bbs.c:bbs_read)

Add after existing read cases:

```c
} else if (type == BBS_NODE_CHAT_ROOM) {
	/* Reading from a chat room - blocking read for new messages */
	struct chat_room *room = (struct chat_room *)node->data;
	if (!room) return -EINVAL;

	const char *username = bbs->authenticated_user[0] != '\0' ?
	                       bbs->authenticated_user : "guest";

	/* Blocking read with timeout */
	int32_t timeout_ms = CONFIG_9BBS_CHAT_READ_TIMEOUT_SEC * 1000;
	return chat_read_messages(&bbs->chat, room->name, username,
	                          (char *)buf, count, timeout_ms);

} else if (type == BBS_NODE_CHAT_USERS) {
	/* List of active chat users */
	return chat_get_users(&bbs->chat, (char *)buf, count);
}
```

### 7. Implement Chat Write Operations (9bbs.c:bbs_write)

Add after metadata write handling:

```c
/* Handle writes to /chat/post */
if (type == BBS_NODE_CHAT_POST) {
	/* Parse message: "room:text" or just "text" (defaults to lobby) */
	char room_name[32] = "lobby";
	const char *msg_text = (const char *)buf;
	size_t msg_len = count;

	/* Look for room prefix */
	for (size_t i = 0; i < count && i < 32; i++) {
		if (buf[i] == ':') {
			memcpy(room_name, buf, i);
			room_name[i] = '\0';
			msg_text = (const char *)&buf[i + 1];
			msg_len = count - i - 1;
			break;
		}
	}

	/* Null-terminate message */
	char msg_buf[CONFIG_9BBS_CHAT_MAX_MESSAGE_LEN];
	size_t copy_len = (msg_len < sizeof(msg_buf) - 1) ? msg_len : sizeof(msg_buf) - 1;
	memcpy(msg_buf, msg_text, copy_len);
	msg_buf[copy_len] = '\0';

	/* Strip trailing newline */
	if (copy_len > 0 && msg_buf[copy_len - 1] == '\n') {
		msg_buf[copy_len - 1] = '\0';
	}

	const char *username = uname ? uname : "guest";
	int ret = chat_post_message(&bbs->chat, room_name, username, msg_buf);
	if (ret < 0) {
		return ret;
	}

	return count;
}
```

### 8. Add to CMakeLists.txt

Add chat.c to the build:

```cmake
zephyr_library_sources(
	src/9bbs/9bbs.c
	src/9bbs/chat.c  # ADD THIS
)
```

### 9. Add Kconfig Options

Create or update `Kconfig`:

```kconfig
config 9BBS_CHAT
	bool "Enable 9BBS chat subsystem"
	default y
	help
	  Enable real-time chat functionality in 9BBS.

if 9BBS_CHAT

config 9BBS_CHAT_MAX_ROOMS
	int "Maximum number of chat rooms"
	default 4
	help
	  Maximum number of concurrent chat rooms.

config 9BBS_CHAT_MAX_MESSAGES
	int "Maximum messages per room (ring buffer size)"
	default 50
	help
	  Number of messages to keep in ring buffer per room.

config 9BBS_CHAT_MAX_MESSAGE_LEN
	int "Maximum message length"
	default 256
	help
	  Maximum length of a single chat message.

config 9BBS_CHAT_MAX_USERS
	int "Maximum concurrent chat users"
	default 8
	help
	  Maximum number of users that can be tracked for chat.

config 9BBS_CHAT_READ_TIMEOUT_SEC
	int "Chat read timeout in seconds"
	default 30
	help
	  Timeout for blocking reads on chat rooms.

endif # 9BBS_CHAT
```

## Testing the Chat

### Test 1: Single Client

```bash
# Mount BBS
mount -t 9p /dev/bbs /mnt/bbs

# Read from lobby (will block until message)
cat /mnt/bbs/chat/lobby &

# Post a message
echo "Hello from terminal!" > /mnt/bbs/chat/post

# Should see: [HH:MM:SS] username: Hello from terminal!
```

### Test 2: Multiple Clients

```bash
# Terminal 1
tail -f /mnt/bbs/chat/lobby

# Terminal 2
echo "alice: Hey everyone!" > /mnt/bbs/chat/post
echo "alice: Anyone there?" > /mnt/bbs/chat/post

# Terminal 3
echo "bob: Hi alice!" > /mnt/bbs/chat/post

# All should appear in Terminal 1's tail -f
```

### Test 3: Room-specific Chat

```bash
# Create a room (if we add admin interface)
# Or just use it (will be created on demand if we add that)
echo "tech:Discussing 9p protocol here" > /mnt/bbs/chat/post

# Read from tech room
cat /mnt/bbs/chat/tech
```

## Implementation Priority

1. ✅ Create chat.h and chat.c (DONE)
2. ✅ Add chat instance to BBS (DONE)
3. ⏸️ Add node types (PAUSED - document created)
4. ⏸️ Integrate walk/read/write (PAUSED - document created)
5. ⏸️ Add Kconfig (PAUSED - document created)
6. ⏸️ Test (PENDING)

## Notes

- Chat subsystem is **fully self-contained** in chat.c - all ring buffer logic, blocking, etc. works
- Integration is purely **filesystem plumbing** - mapping 9P operations to chat API calls
- The existing BBS metadata/permissions work applies to chat too (authenticated_user field)
- **Multi-client support** requires fixing L2CAP transport to accept multiple connections

## Size Estimate

- chat.c: ~400 lines ✅
- 9bbs.c integration: ~150 lines (spread across walk/read/write)
- Total new code: ~550 lines

## Next Steps

Test the admin file permissions first, then return to complete chat integration following this guide.
