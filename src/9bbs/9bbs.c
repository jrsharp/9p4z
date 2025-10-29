/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9bbs/9bbs.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
#include <zephyr/fs/fs.h>
#endif

#ifdef CONFIG_DATE_TIME
#include <date_time.h>
#endif

LOG_MODULE_REGISTER(bbs, CONFIG_NINEP_LOG_LEVEL);

/* LittleFS mount point */
#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
#define BBS_LFS_MOUNT_POINT "/lfs_bbs"
#define BBS_ROOMS_PATH BBS_LFS_MOUNT_POINT "/rooms"
#endif

/* ========================================================================
 * LittleFS Helper Functions
 * ======================================================================== */

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS

/* Save a message to LittleFS */
static int save_message_to_lfs(const char *room_name, const struct bbs_message *msg)
{
	char path[128];
	snprintf(path, sizeof(path), "%s/%s/%u", BBS_ROOMS_PATH, room_name, msg->id);

	struct fs_file_t file;
	fs_file_t_init(&file);

	int ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		LOG_ERR("Failed to open %s: %d", path, ret);
		return ret;
	}

	/* Write message headers */
	char header[256];
	int len = snprintf(header, sizeof(header),
	                   "From: %s\nTo: %s\nDate: %llu\nReply-To: %u\n\n",
	                   msg->from, msg->to, msg->date, msg->reply_to);

	ret = fs_write(&file, header, len);
	if (ret < 0) {
		LOG_ERR("Failed to write header to %s: %d", path, ret);
		fs_close(&file);
		return ret;
	}

	/* Write message body */
	if (msg->body && msg->body_len > 0) {
		ret = fs_write(&file, msg->body, msg->body_len);
		if (ret < 0) {
			LOG_ERR("Failed to write body to %s: %d", path, ret);
			fs_close(&file);
			return ret;
		}
	}

	/* Write signature if present */
	if (msg->sig[0] != '\0') {
		char sig_line[256];
		len = snprintf(sig_line, sizeof(sig_line), "\n\n%s\n", msg->sig);
		ret = fs_write(&file, sig_line, len);
		if (ret < 0) {
			LOG_ERR("Failed to write signature to %s: %d", path, ret);
		}
	}

	fs_close(&file);
	LOG_DBG("Saved message %u to %s", msg->id, path);
	return 0;
}

/* Load a message from LittleFS by filename */
static int load_message_from_lfs_by_filename(const char *room_name, const char *filename,
                                               struct bbs_message *msg)
{
	char path[128];
	snprintf(path, sizeof(path), "%s/%s/%s", BBS_ROOMS_PATH, room_name, filename);

	struct fs_file_t file;
	fs_file_t_init(&file);

	int ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		return ret;  /* File doesn't exist or can't be opened */
	}

	/* Allocate buffer for reading file - use heap instead of stack */
	size_t buf_size = CONFIG_9BBS_MAX_MESSAGE_SIZE + 256;  /* Extra space for headers */
	char *buf = k_malloc(buf_size);
	if (!buf) {
		fs_close(&file);
		return -ENOMEM;
	}

	/* Read entire file */
	ssize_t bytes_read = fs_read(&file, buf, buf_size - 1);
	fs_close(&file);

	if (bytes_read < 0) {
		LOG_ERR("Failed to read %s: %d", path, bytes_read);
		k_free(buf);
		return bytes_read;
	}
	buf[bytes_read] = '\0';

	/* Parse headers and body */
	char *line = buf;
	char *body_start = NULL;

	/* Initialize message - extract ID from filename */
	memset(msg, 0, sizeof(*msg));

	/* Parse timestamp from filename (format: "timestamp-msgid" or just "number") */
	char *dash = strchr(filename, '-');
	if (dash) {
		/* Extract timestamp portion */
		size_t ts_len = dash - filename;
		char ts_str[16];
		if (ts_len < sizeof(ts_str)) {
			memcpy(ts_str, filename, ts_len);
			ts_str[ts_len] = '\0';
			uint64_t timestamp = strtoull(ts_str, NULL, 10);
			msg->id = (uint32_t)(timestamp & 0xFFFFFFFF);
		} else {
			msg->id = 0;  /* Will be set from Date header */
		}
	} else {
		/* Simple numeric filename */
		msg->id = strtoul(filename, NULL, 10);
	}

	while (*line) {
		char *next_line = strchr(line, '\n');
		if (next_line) {
			*next_line = '\0';
			next_line++;
		}

		/* Empty line marks end of headers */
		if (*line == '\0') {
			body_start = next_line;
			break;
		}

		/* Parse header lines */
		if (strncmp(line, "From: ", 6) == 0) {
			strncpy(msg->from, line + 6, sizeof(msg->from) - 1);
		} else if (strncmp(line, "To: ", 4) == 0) {
			strncpy(msg->to, line + 4, sizeof(msg->to) - 1);
		} else if (strncmp(line, "Date: ", 6) == 0) {
			msg->date = strtoull(line + 6, NULL, 10);
		} else if (strncmp(line, "Reply-To: ", 10) == 0) {
			msg->reply_to = strtoul(line + 10, NULL, 10);
		}

		if (!next_line) break;
		line = next_line;
	}

	/* Allocate and copy body */
	msg->body = k_malloc(CONFIG_9BBS_MAX_MESSAGE_SIZE);
	if (!msg->body) {
		k_free(buf);
		return -ENOMEM;
	}

	if (body_start && *body_start) {
		size_t body_len = strlen(body_start);
		if (body_len >= CONFIG_9BBS_MAX_MESSAGE_SIZE) {
			body_len = CONFIG_9BBS_MAX_MESSAGE_SIZE - 1;
		}
		memcpy(msg->body, body_start, body_len);
		msg->body[body_len] = '\0';
		msg->body_len = body_len;
	} else {
		msg->body[0] = '\0';
		msg->body_len = 0;
	}

	msg->deleted = false;

	/* Free temporary read buffer */
	k_free(buf);

	return 0;
}

/* Load all messages from a room directory */
static int load_room_from_lfs(struct bbs_instance *bbs, const char *room_name)
{
	char path[128];
	snprintf(path, sizeof(path), "%s/%s", BBS_ROOMS_PATH, room_name);

	/* Find the room in our array */
	struct bbs_room *room = NULL;
	for (uint32_t i = 0; i < bbs->room_count; i++) {
		if (strcmp(bbs->rooms[i].name, room_name) == 0) {
			room = &bbs->rooms[i];
			break;
		}
	}

	if (!room) {
		LOG_ERR("Room '%s' not found in memory", room_name);
		return -ENOENT;
	}

	/* Open directory */
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	int ret = fs_opendir(&dir, path);
	if (ret < 0) {
		LOG_WRN("Room directory %s doesn't exist yet: %d", path, ret);
		return 0;  /* Not an error - room exists but has no messages yet */
	}

	/* Read all message files */
	struct fs_dirent entry;
	while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
		/* Skip . and .. */
		if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
			continue;
		}

		/* Parse message ID from filename
		 * Supports two formats:
		 *   1. Timestamp-msgid format: "1738005432123-abc123def456ghi7"
		 *   2. Simple numeric format: "1", "2", "3" (legacy)
		 */
		uint64_t timestamp = 0;
		char *dash = strchr(entry.name, '-');

		if (dash) {
			/* Format: timestamp-msgid - extract timestamp portion */
			size_t ts_len = dash - entry.name;
			char ts_str[16];

			if (ts_len >= sizeof(ts_str)) {
				LOG_WRN("Timestamp too long in filename: %s", entry.name);
				continue;
			}

			memcpy(ts_str, entry.name, ts_len);
			ts_str[ts_len] = '\0';

			timestamp = strtoull(ts_str, NULL, 10);
			if (timestamp == 0) {
				LOG_WRN("Invalid timestamp in filename: %s", entry.name);
				continue;
			}

			LOG_DBG("Parsed timestamp %llu from filename: %s", timestamp, entry.name);
		} else {
			/* Legacy format: simple numeric message ID */
			timestamp = strtoull(entry.name, NULL, 10);
			if (timestamp == 0) {
				LOG_WRN("Invalid message filename: %s", entry.name);
				continue;
			}
			LOG_DBG("Parsed legacy message ID %llu from filename: %s", timestamp, entry.name);
		}

		/* Check if we have space */
		if (room->message_count >= CONFIG_9BBS_MAX_MESSAGES_PER_ROOM) {
			LOG_WRN("Room '%s' full, can't load message from file: %s",
			        room_name, entry.name);
			break;
		}

		/* Load message into next slot using the full filename */
		struct bbs_message *msg = &room->messages[room->message_count];

		/* Use timestamp as message ID (will be overridden by Date header in file) */
		ret = load_message_from_lfs_by_filename(room_name, entry.name, msg);
		if (ret == 0) {
			room->message_count++;
			/* Update next_message_id based on the timestamp */
			uint32_t msg_id = (uint32_t)(timestamp & 0xFFFFFFFF);
			if (msg_id >= room->next_message_id) {
				room->next_message_id = msg_id + 1;
			}
			LOG_DBG("Loaded message from file '%s' in room '%s'", entry.name, room_name);
		} else {
			LOG_ERR("Failed to load message from file '%s' in room '%s': %d",
			        entry.name, room_name, ret);
		}
	}

	fs_closedir(&dir);
	LOG_INF("Loaded %u messages from room '%s'", room->message_count, room_name);
	return 0;
}

#endif /* CONFIG_FILE_SYSTEM_LITTLEFS */

/* ========================================================================
 * BBS Core Functions
 * ======================================================================== */

int bbs_init(struct bbs_instance *bbs)
{
	int ret;

	if (!bbs) {
		return -EINVAL;
	}

	memset(bbs, 0, sizeof(*bbs));
	k_mutex_init(&bbs->lock);

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
	/* Check if LittleFS is available (it should be automounted via devicetree) */
	{
		struct fs_dir_t test_dir;
		fs_dir_t_init(&test_dir);
		ret = fs_opendir(&test_dir, BBS_LFS_MOUNT_POINT);
		if (ret == 0) {
			fs_closedir(&test_dir);
			LOG_INF("LittleFS available at %s", BBS_LFS_MOUNT_POINT);

		/* Create rooms directory if it doesn't exist */
		ret = fs_mkdir(BBS_ROOMS_PATH);
		if (ret < 0 && ret != -EEXIST) {
			LOG_WRN("Failed to create %s: %d", BBS_ROOMS_PATH, ret);
		}

		/* Scan for existing room directories */
		struct fs_dir_t dir;
		fs_dir_t_init(&dir);

		ret = fs_opendir(&dir, BBS_ROOMS_PATH);
		if (ret == 0) {
			struct fs_dirent entry;
			while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
				/* Skip . and .. */
				if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
					continue;
				}

				/* Only process directories (room directories) */
				if (entry.type == FS_DIR_ENTRY_DIR) {
					LOG_INF("Found room directory: %s", entry.name);

					/* Create room in memory */
					ret = bbs_create_room(bbs, entry.name);
					if (ret < 0 && ret != -EEXIST) {
						LOG_ERR("Failed to create room '%s': %d", entry.name, ret);
						continue;
					}

					/* Load messages from LFS into this room */
					ret = load_room_from_lfs(bbs, entry.name);
					if (ret < 0) {
						LOG_ERR("Failed to load room '%s': %d", entry.name, ret);
					}
				}
			}
			fs_closedir(&dir);
		} else {
			LOG_WRN("Failed to open rooms directory: %d", ret);
		}
	} else {
		LOG_WRN("LittleFS not available at %s (ret=%d), running in RAM-only mode",
		        BBS_LFS_MOUNT_POINT, ret);
	}
	}
#endif /* CONFIG_FILE_SYSTEM_LITTLEFS */

	/* Create default "lobby" room if it doesn't exist */
	ret = bbs_create_room(bbs, "lobby");
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create lobby: %d", ret);
		return ret;
	}

	LOG_INF("BBS initialized with %u rooms", bbs->room_count);
	return 0;
}

int bbs_create_room(struct bbs_instance *bbs, const char *name)
{
	if (!bbs || !name) {
		return -EINVAL;
	}

	k_mutex_lock(&bbs->lock, K_FOREVER);

	if (bbs->room_count >= CONFIG_9BBS_MAX_ROOMS) {
		k_mutex_unlock(&bbs->lock);
		return -ENOSPC;
	}

	/* Check if room already exists */
	for (uint32_t i = 0; i < bbs->room_count; i++) {
		if (strcmp(bbs->rooms[i].name, name) == 0) {
			k_mutex_unlock(&bbs->lock);
			return -EEXIST;
		}
	}

	/* Create new room */
	struct bbs_room *room = &bbs->rooms[bbs->room_count];
	strncpy(room->name, name, sizeof(room->name) - 1);
	room->message_count = 0;
	room->next_message_id = 1;
	room->active = true;

	bbs->room_count++;

	k_mutex_unlock(&bbs->lock);

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
	/* Create room directory in LittleFS if it doesn't exist */
	char path[128];
	snprintf(path, sizeof(path), "%s/%s", BBS_ROOMS_PATH, name);

	/* Check if directory already exists */
	struct fs_dirent stat;
	int ret = fs_stat(path, &stat);
	if (ret == 0 && stat.type == FS_DIR_ENTRY_DIR) {
		/* Directory already exists, no need to create */
		LOG_DBG("Room directory already exists: %s", path);
	} else {
		/* Directory doesn't exist, create it */
		ret = fs_mkdir(path);
		if (ret < 0 && ret != -EEXIST) {
			LOG_WRN("Failed to create room directory %s: %d", path, ret);
			/* Don't fail - room exists in RAM */
		} else {
			LOG_DBG("Created room directory: %s", path);
		}
	}
#endif

	LOG_INF("Created room: %s", name);
	return 0;
}

int bbs_create_user(struct bbs_instance *bbs, const char *username,
                    const char *password)
{
	if (!bbs || !username || !password) {
		return -EINVAL;
	}

	k_mutex_lock(&bbs->lock, K_FOREVER);

	if (bbs->user_count >= CONFIG_9BBS_MAX_USERS) {
		k_mutex_unlock(&bbs->lock);
		return -ENOSPC;
	}

	/* Check if user already exists */
	for (uint32_t i = 0; i < bbs->user_count; i++) {
		if (strcmp(bbs->users[i].username, username) == 0) {
			k_mutex_unlock(&bbs->lock);
			return -EEXIST;
		}
	}

	/* Create new user */
	struct bbs_user *user = &bbs->users[bbs->user_count];
	strncpy(user->username, username, sizeof(user->username) - 1);
	strncpy(user->password, password, sizeof(user->password) - 1);
	strncpy(user->sig, username, sizeof(user->sig) - 1);  /* Default sig = username */
	strncpy(user->current_room, "lobby", sizeof(user->current_room) - 1);
	user->room_count = 0;
	user->active = true;

	/* Initialize read positions for all rooms */
	for (uint32_t i = 0; i < bbs->room_count; i++) {
		strncpy(user->rooms[i].room, bbs->rooms[i].name,
		        sizeof(user->rooms[i].room) - 1);
		user->rooms[i].last_read = 0;
	}
	user->room_count = bbs->room_count;

	bbs->user_count++;

	k_mutex_unlock(&bbs->lock);

	LOG_INF("Created user: %s", username);
	return 0;
}

int bbs_post_message(struct bbs_instance *bbs, const char *room_name,
                     const char *from, const char *body, uint32_t reply_to)
{
	if (!bbs || !room_name || !from || !body) {
		return -EINVAL;
	}

	k_mutex_lock(&bbs->lock, K_FOREVER);

	/* Find room */
	struct bbs_room *room = NULL;
	for (uint32_t i = 0; i < bbs->room_count; i++) {
		if (strcmp(bbs->rooms[i].name, room_name) == 0) {
			room = &bbs->rooms[i];
			break;
		}
	}

	if (!room) {
		k_mutex_unlock(&bbs->lock);
		return -ENOENT;
	}

	if (room->message_count >= CONFIG_9BBS_MAX_MESSAGES_PER_ROOM) {
		k_mutex_unlock(&bbs->lock);
		return -ENOSPC;
	}

	/* Create message */
	struct bbs_message *msg = &room->messages[room->message_count];
	msg->id = room->next_message_id++;
	strncpy(msg->from, from, sizeof(msg->from) - 1);
	strncpy(msg->to, room_name, sizeof(msg->to) - 1);

	/* Set timestamp: try date_time library first, fallback to uptime */
#ifdef CONFIG_DATE_TIME
	int64_t unix_time_ms;
	if (date_time_now(&unix_time_ms) == 0) {
		msg->date = (uint64_t)unix_time_ms;
		LOG_DBG("Using Unix timestamp: %llu", msg->date);
	} else {
		msg->date = k_uptime_get();  /* Milliseconds since boot */
		LOG_DBG("Using uptime timestamp: %llu", msg->date);
	}
#else
	msg->date = k_uptime_get();  /* Milliseconds since boot */
#endif

	msg->reply_to = reply_to;
	msg->deleted = false;

	/* Allocate body buffer - always allocate max size for later writes */
	msg->body = k_malloc(CONFIG_9BBS_MAX_MESSAGE_SIZE);
	if (!msg->body) {
		k_mutex_unlock(&bbs->lock);
		return -ENOMEM;
	}

	/* Copy initial body content if provided */
	size_t body_len = strlen(body);
	if (body_len >= CONFIG_9BBS_MAX_MESSAGE_SIZE) {
		body_len = CONFIG_9BBS_MAX_MESSAGE_SIZE - 1;
	}
	memcpy(msg->body, body, body_len);
	msg->body[body_len] = '\0';
	msg->body_len = body_len;

	/* Find user's signature */
	for (uint32_t i = 0; i < bbs->user_count; i++) {
		if (strcmp(bbs->users[i].username, from) == 0) {
			strncpy(msg->sig, bbs->users[i].sig, sizeof(msg->sig) - 1);
			break;
		}
	}

	room->message_count++;

	uint32_t msg_id = msg->id;

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
	/* Persist message to LittleFS before unlocking */
	int ret = save_message_to_lfs(room_name, msg);
	if (ret < 0) {
		LOG_WRN("Failed to persist message %u to LFS: %d", msg_id, ret);
		/* Don't fail - message is in RAM */
	}
#endif

	k_mutex_unlock(&bbs->lock);

	LOG_INF("Posted message %u to %s by %s", msg_id, room_name, from);
	return msg_id;
}

struct bbs_message *bbs_get_message(struct bbs_instance *bbs, const char *room_name,
                                    uint32_t msg_id)
{
	if (!bbs || !room_name) {
		return NULL;
	}

	k_mutex_lock(&bbs->lock, K_FOREVER);

	/* Find room */
	struct bbs_room *room = NULL;
	for (uint32_t i = 0; i < bbs->room_count; i++) {
		if (strcmp(bbs->rooms[i].name, room_name) == 0) {
			room = &bbs->rooms[i];
			break;
		}
	}

	if (!room) {
		k_mutex_unlock(&bbs->lock);
		return NULL;
	}

	/* Find message */
	for (uint32_t i = 0; i < room->message_count; i++) {
		if (room->messages[i].id == msg_id) {
			struct bbs_message *msg = &room->messages[i];
			k_mutex_unlock(&bbs->lock);
			return msg;
		}
	}

	k_mutex_unlock(&bbs->lock);
	return NULL;
}

/* ========================================================================
 * 9P Filesystem Implementation
 * ======================================================================== */

/**
 * @brief BBS filesystem node types
 */
enum bbs_node_type {
	BBS_NODE_ROOT,
	BBS_NODE_ROOMS_DIR,
	BBS_NODE_ROOM_DIR,
	BBS_NODE_MESSAGE_FILE,
	BBS_NODE_ETC_DIR,
	BBS_NODE_ETC_FILE,
	BBS_NODE_ETC_NETS_DIR,
	BBS_NODE_ROOMLIST_FILE,
};

/**
 * @brief Create filesystem node for BBS
 */
static struct ninep_fs_node *bbs_create_node(enum bbs_node_type type,
                                              const char *name,
                                              void *data)
{
	struct ninep_fs_node *node = k_malloc(sizeof(*node));
	if (!node) {
		return NULL;
	}

	memset(node, 0, sizeof(*node));

	if (name) {
		strncpy(node->name, name, sizeof(node->name) - 1);
	}

	node->data = data;
	node->type = (type == BBS_NODE_MESSAGE_FILE || type == BBS_NODE_ETC_FILE) ? NINEP_NODE_FILE : NINEP_NODE_DIR;
	node->mode = (node->type == NINEP_NODE_FILE) ? 0644 : 0755;

	/* Generate QID */
	node->qid.type = (node->type == NINEP_NODE_DIR) ? NINEP_QTDIR : NINEP_QTFILE;
	node->qid.version = 0;
	node->qid.path = (uint64_t)type << 32 | (uint64_t)(uintptr_t)data;

	return node;
}

static struct ninep_fs_node *bbs_get_root(void *fs_ctx)
{
	return bbs_create_node(BBS_NODE_ROOT, "/", fs_ctx);
}

static struct ninep_fs_node *bbs_walk(struct ninep_fs_node *parent,
                                       const char *name, uint16_t name_len,
                                       void *fs_ctx)
{
	struct bbs_instance *bbs = (struct bbs_instance *)fs_ctx;
	char name_str[64];

	/* Copy name to null-terminated string */
	if (name_len >= sizeof(name_str)) {
		return NULL;
	}
	memcpy(name_str, name, name_len);
	name_str[name_len] = '\0';

	enum bbs_node_type parent_type = (enum bbs_node_type)((uint64_t)parent->qid.path >> 32);

	/* ROOT directory - walk to "etc" or "rooms" */
	if (parent_type == BBS_NODE_ROOT) {
		if (strcmp(name_str, "etc") == 0) {
			return bbs_create_node(BBS_NODE_ETC_DIR, "etc", fs_ctx);
		} else if (strcmp(name_str, "rooms") == 0) {
			return bbs_create_node(BBS_NODE_ROOMS_DIR, "rooms", fs_ctx);
		}
	}

	/* ROOMS directory - walk to individual room names */
	if (parent_type == BBS_NODE_ROOMS_DIR) {
		k_mutex_lock(&bbs->lock, K_FOREVER);
		for (uint32_t i = 0; i < bbs->room_count; i++) {
			if (strncmp(bbs->rooms[i].name, name_str, name_len) == 0 &&
			    bbs->rooms[i].name[name_len] == '\0') {
				struct ninep_fs_node *node = bbs_create_node(
					BBS_NODE_ROOM_DIR, bbs->rooms[i].name, &bbs->rooms[i]);
				k_mutex_unlock(&bbs->lock);
				return node;
			}
		}
		k_mutex_unlock(&bbs->lock);
	}

	/* ROOM directory - list messages */
	if (parent_type == BBS_NODE_ROOM_DIR) {
		struct bbs_room *room = (struct bbs_room *)parent->data;

		if (!room) {
			LOG_ERR("Room directory walk: room pointer is NULL!");
			return NULL;
		}

		/* Parse message ID (handle both 13-digit timestamps and 10-digit IDs) */
		uint64_t requested_value = strtoull(name_str, NULL, 10);
		uint32_t msg_id = (uint32_t)(requested_value & 0xFFFFFFFF);

		k_mutex_lock(&bbs->lock, K_FOREVER);
		LOG_DBG("Walking to message '%s' (id=%u) in room '%s' (count=%u)",
		        name_str, msg_id, room->name, room->message_count);

		for (uint32_t i = 0; i < room->message_count; i++) {
			if (room->messages[i].id == msg_id) {
				LOG_DBG("Found message %u at index %u", msg_id, i);
				struct ninep_fs_node *node = bbs_create_node(
					BBS_NODE_MESSAGE_FILE, name_str, &room->messages[i]);
				k_mutex_unlock(&bbs->lock);
				return node;
			}
		}

		LOG_WRN("Message '%s' (id=%u) not found in room '%s'", name_str, msg_id, room->name);
		k_mutex_unlock(&bbs->lock);
	}

	/* ETC directory - walk to metadata files or nets subdirectory */
	if (parent_type == BBS_NODE_ETC_DIR) {
		const char *etc_files[] = {"boardname", "sysop", "motd", "location", "description", "version"};
		for (size_t i = 0; i < sizeof(etc_files) / sizeof(etc_files[0]); i++) {
			if (strcmp(name_str, etc_files[i]) == 0) {
				return bbs_create_node(BBS_NODE_ETC_FILE, name_str, (void *)etc_files[i]);
			}
		}
		if (strcmp(name_str, "nets") == 0) {
			return bbs_create_node(BBS_NODE_ETC_NETS_DIR, "nets", fs_ctx);
		}
	}

	/* ETC/NETS directory - walk to network files */
	if (parent_type == BBS_NODE_ETC_NETS_DIR) {
		const char *net_files[] = {"fsxnet", "aethernet"};
		for (size_t i = 0; i < sizeof(net_files) / sizeof(net_files[0]); i++) {
			if (strcmp(name_str, net_files[i]) == 0) {
				return bbs_create_node(BBS_NODE_ETC_FILE, name_str, (void *)net_files[i]);
			}
		}
	}

	return NULL;
}

static int bbs_open(struct ninep_fs_node *node, uint8_t mode, void *fs_ctx)
{
	/* All opens succeed for now */
	return 0;
}

static int bbs_read(struct ninep_fs_node *node, uint64_t offset,
                    uint8_t *buf, uint32_t count, void *fs_ctx)
{
	struct bbs_instance *bbs = (struct bbs_instance *)fs_ctx;
	enum bbs_node_type type = (enum bbs_node_type)((uint64_t)node->qid.path >> 32);

	if (type == BBS_NODE_MESSAGE_FILE) {
		/* Read a message */
		struct bbs_message *msg = (struct bbs_message *)node->data;

		if (!msg) {
			LOG_ERR("BBS_NODE_MESSAGE_FILE has NULL data pointer!");
			return -EINVAL;
		}

		LOG_INF("Reading message %u (from=%s, body_len=%zu)",
		        msg->id, msg->from, msg->body_len);

		/* Format message as RFC822-style */
		char temp[CONFIG_9BBS_MAX_MESSAGE_SIZE];
		int len = snprintf(temp, sizeof(temp),
		                   "From: %s\n"
		                   "To: %s\n"
		                   "Date: %llu\n"
		                   "X-Date-N: %llu\n"
		                   "\n"
		                   "%s\n"
		                   "\n"
		                   "%s\n",
		                   msg->from, msg->to,
		                   (unsigned long long)msg->date,
		                   (unsigned long long)msg->date,
		                   msg->body ? msg->body : "",
		                   msg->sig);

		if (offset >= len) {
			return 0;  /* EOF */
		}

		size_t to_copy = (offset + count > len) ? (len - offset) : count;
		memcpy(buf, temp + offset, to_copy);
		return to_copy;

	} else if (type == BBS_NODE_ETC_FILE) {
		/* Read /etc/ file from LittleFS */
#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
		const char *filename = (const char *)node->data;
		char path[128];

		/* Check if this is a nets file */
		if (strcmp(filename, "fsxnet") == 0 || strcmp(filename, "aethernet") == 0) {
			snprintf(path, sizeof(path), "%s/etc/nets/%s", BBS_LFS_MOUNT_POINT, filename);
		} else {
			snprintf(path, sizeof(path), "%s/etc/%s", BBS_LFS_MOUNT_POINT, filename);
		}

		struct fs_file_t file;
		fs_file_t_init(&file);

		int ret = fs_open(&file, path, FS_O_READ);
		if (ret < 0) {
			LOG_ERR("Failed to open %s: %d", path, ret);
			return ret;
		}

		/* Seek to offset */
		ret = fs_seek(&file, offset, FS_SEEK_SET);
		if (ret < 0) {
			fs_close(&file);
			return ret;
		}

		/* Read data */
		ssize_t bytes_read = fs_read(&file, buf, count);
		fs_close(&file);

		return (bytes_read >= 0) ? bytes_read : -EIO;
#else
		return -ENOTSUP;
#endif

	} else if (type == BBS_NODE_ROOT) {
		/* Directory listing for root: list "etc" and "rooms" */
		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		const char *entries[] = {"etc", "rooms"};
		const enum bbs_node_type entry_types[] = {BBS_NODE_ETC_DIR, BBS_NODE_ROOMS_DIR};

		for (size_t i = 0; i < 2; i++) {
			struct ninep_qid entry_qid = {
				.type = NINEP_QTDIR,
				.version = 0,
				.path = ((uint64_t)entry_types[i] << 32) | i
			};
			uint32_t mode = 0755 | NINEP_DMDIR;
			uint16_t name_len = strlen(entries[i]);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode, 0,
				                           entries[i], name_len);
				if (ret < 0) break;
				buf_offset += write_offset;
				current_offset += write_offset;
			} else {
				current_offset += entry_size;
			}
		}
		return buf_offset;

	} else if (type == BBS_NODE_ROOMS_DIR) {
		/* Directory listing for /rooms: list all room names */
		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		k_mutex_lock(&bbs->lock, K_FOREVER);
		for (uint32_t i = 0; i < bbs->room_count; i++) {
			struct ninep_qid entry_qid = {
				.type = NINEP_QTDIR,
				.version = 0,
				.path = ((uint64_t)BBS_NODE_ROOM_DIR << 32) | (uint64_t)(uintptr_t)&bbs->rooms[i]
			};
			uint32_t mode = 0755 | NINEP_DMDIR;
			uint16_t name_len = strlen(bbs->rooms[i].name);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode, 0,
				                           bbs->rooms[i].name, name_len);
				if (ret < 0) break;
				buf_offset += write_offset;
				current_offset += write_offset;
			} else {
				current_offset += entry_size;
			}
		}
		k_mutex_unlock(&bbs->lock);
		return buf_offset;

	} else if (type == BBS_NODE_ETC_DIR) {
		/* Directory listing for /etc: list metadata files and nets directory */
		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		const char *entries[] = {"boardname", "sysop", "motd", "location", "description", "version", "nets"};
		const bool is_dir[] = {false, false, false, false, false, false, true};

		for (size_t i = 0; i < 7; i++) {
			struct ninep_qid entry_qid = {
				.type = is_dir[i] ? NINEP_QTDIR : NINEP_QTFILE,
				.version = 0,
				.path = ((uint64_t)(is_dir[i] ? BBS_NODE_ETC_NETS_DIR : BBS_NODE_ETC_FILE) << 32) | i
			};
			uint32_t mode = is_dir[i] ? (0755 | NINEP_DMDIR) : 0644;
			uint16_t name_len = strlen(entries[i]);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode, 0,
				                           entries[i], name_len);
				if (ret < 0) break;
				buf_offset += write_offset;
				current_offset += write_offset;
			} else {
				current_offset += entry_size;
			}
		}
		return buf_offset;

	} else if (type == BBS_NODE_ETC_NETS_DIR) {
		/* Directory listing for /etc/nets: list network files */
		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		const char *entries[] = {"fsxnet", "aethernet"};

		for (size_t i = 0; i < 2; i++) {
			struct ninep_qid entry_qid = {
				.type = NINEP_QTFILE,
				.version = 0,
				.path = ((uint64_t)BBS_NODE_ETC_FILE << 32) | (i + 100)  /* offset to avoid conflicts */
			};
			uint32_t mode = 0644;
			uint16_t name_len = strlen(entries[i]);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode, 0,
				                           entries[i], name_len);
				if (ret < 0) break;
				buf_offset += write_offset;
				current_offset += write_offset;
			} else {
				current_offset += entry_size;
			}
		}
		return buf_offset;

	} else if (type == BBS_NODE_ROOM_DIR) {
		/* Directory listing for /rooms/<room>: list message numbers */
		struct bbs_room *room = (struct bbs_room *)node->data;

		if (!room) {
			LOG_ERR("BBS_NODE_ROOM_DIR has NULL data pointer!");
			return -EINVAL;
		}

		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		k_mutex_lock(&bbs->lock, K_FOREVER);
		LOG_INF("Reading room '%s' directory: %u messages (offset=%llu)",
		        room->name, room->message_count, offset);
		for (uint32_t i = 0; i < room->message_count; i++) {
			char msg_id_str[16];
			snprintf(msg_id_str, sizeof(msg_id_str), "%u", room->messages[i].id);
			LOG_DBG("  Message %u: id=%u, from=%s",
			        i, room->messages[i].id, room->messages[i].from);

			struct ninep_qid entry_qid = {
				.type = NINEP_QTFILE,
				.version = 0,
				.path = ((uint64_t)BBS_NODE_MESSAGE_FILE << 32) | (uint64_t)(uintptr_t)&room->messages[i]
			};
			uint32_t mode = 0644;
			uint16_t name_len = strlen(msg_id_str);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				size_t body_len = (room->messages[i].body) ? strlen(room->messages[i].body) : 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode,
				                           body_len,
				                           msg_id_str, name_len);
				if (ret < 0) break;
				buf_offset += write_offset;
				current_offset += write_offset;
			} else {
				current_offset += entry_size;
			}
		}
		k_mutex_unlock(&bbs->lock);
		return buf_offset;

	}

	return -EINVAL;
}

static int bbs_write(struct ninep_fs_node *node, uint64_t offset,
                     const uint8_t *buf, uint32_t count, const char *uname,
                     void *fs_ctx)
{
	struct bbs_instance *bbs = (struct bbs_instance *)fs_ctx;
	enum bbs_node_type type = (enum bbs_node_type)((uint64_t)node->qid.path >> 32);

	ARG_UNUSED(uname);

	/* Only message files can be written to */
	if (type != BBS_NODE_MESSAGE_FILE) {
		return -EISDIR;
	}

	struct bbs_message *msg = (struct bbs_message *)node->data;
	if (!msg || !msg->body) {
		return -EINVAL;
	}

	k_mutex_lock(&bbs->lock, K_FOREVER);

	/* Calculate how much we can write */
	size_t body_size = CONFIG_9BBS_MAX_MESSAGE_SIZE;
	if (offset >= body_size) {
		k_mutex_unlock(&bbs->lock);
		return 0;  /* Beyond end of file */
	}

	size_t available = body_size - offset;
	size_t to_write = (count > available) ? available : count;

	/* Write to message body */
	memcpy(msg->body + offset, buf, to_write);

	/* Update body_len if write extended the message */
	size_t new_len = offset + to_write;
	if (new_len > msg->body_len) {
		msg->body_len = new_len;
	}

	/* Ensure null termination */
	if (offset + to_write < body_size) {
		msg->body[offset + to_write] = '\0';
	}

	LOG_INF("Wrote %zu bytes to message %u (offset=%llu): '%.*s'",
	        to_write, msg->id, offset, (int)to_write, buf);

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
	/* Persist updated message to LittleFS */
	int ret = save_message_to_lfs(msg->to, msg);
	if (ret < 0) {
		LOG_WRN("Failed to persist updated message %u to LFS: %d", msg->id, ret);
		/* Don't fail - message is updated in RAM */
	}
#endif

	k_mutex_unlock(&bbs->lock);

	return to_write;
}

static int bbs_stat(struct ninep_fs_node *node, uint8_t *buf,
                    size_t buf_len, void *fs_ctx)
{
	if (!node || !buf) {
		return -EINVAL;
	}

	size_t offset = 0;
	uint16_t name_len = strlen(node->name);

	int ret = ninep_write_stat(buf, buf_len, &offset, &node->qid,
	                            node->mode, node->length,
	                            node->name, name_len);
	if (ret < 0) {
		return ret;
	}

	return offset;
}

static int bbs_create(struct ninep_fs_node *parent, const char *name,
                      uint16_t name_len, uint32_t perm, uint8_t mode,
                      const char *uname, struct ninep_fs_node **new_node,
                      void *fs_ctx)
{
	struct bbs_instance *bbs = (struct bbs_instance *)fs_ctx;
	enum bbs_node_type parent_type = (enum bbs_node_type)((uint64_t)parent->qid.path >> 32);

	/* Extract name as null-terminated string */
	char name_str[64];
	if (name_len >= sizeof(name_str)) {
		return -ENAMETOOLONG;
	}
	memcpy(name_str, name, name_len);
	name_str[name_len] = '\0';

	/* Creating at root - creates a new room */
	if (parent_type == BBS_NODE_ROOT) {
		int ret = bbs_create_room(bbs, name_str);
		if (ret < 0) {
			return ret;
		}

		/* Return new room node */
		*new_node = bbs_walk(parent, name, name_len, fs_ctx);
		return (*new_node != NULL) ? 0 : -EIO;
	}

	/* Creating in a room directory - posts a new message */
	if (parent_type == BBS_NODE_ROOM_DIR) {
		struct bbs_room *room = (struct bbs_room *)parent->data;

		/* Use uname as the "from" field, or "anonymous" if not provided */
		const char *from = (uname && uname[0]) ? uname : "anonymous";

		/* Parse the requested filename to extract timestamp (supports both formats):
		 *   1. Pure timestamp: "1761708118466" (13 digits)
		 *   2. Timestamp-msgid: "1761708118466-abc123..."
		 */
		uint64_t requested_timestamp = 0;
		char *dash = strchr(name_str, '-');

		if (dash) {
			/* Format: timestamp-msgid */
			size_t ts_len = dash - name_str;
			char ts_str[16];
			if (ts_len < sizeof(ts_str)) {
				memcpy(ts_str, name_str, ts_len);
				ts_str[ts_len] = '\0';
				requested_timestamp = strtoull(ts_str, NULL, 10);
			}
		} else {
			/* Pure timestamp */
			requested_timestamp = strtoull(name_str, NULL, 10);
		}

		/* If client provided a valid timestamp, use its lower 32 bits as message ID */
		uint32_t requested_msg_id = 0;
		if (requested_timestamp > 0) {
			requested_msg_id = (uint32_t)(requested_timestamp & 0xFFFFFFFF);
			LOG_DBG("Client requested filename '%s' -> timestamp %llu -> msg_id %u",
			        name_str, requested_timestamp, requested_msg_id);
		}

		/* Post message with empty body (will be populated via write if needed) */
		int ret_signed = bbs_post_message(bbs, room->name, from, "", 0);

		/* Note: bbs_post_message returns message ID (uint32_t) cast to int
		 * Error codes are small negative values (-1 to -200)
		 * Large message IDs (> INT32_MAX) appear as large negative values when cast to int
		 * We distinguish by checking if the value is a "reasonable" error code
		 */
		if (ret_signed < 0 && ret_signed >= -200) {
			/* This is an actual error code */
			return ret_signed;
		}

		/* Otherwise, this is a message ID (possibly > INT32_MAX) */
		uint32_t msg_id = (uint32_t)ret_signed;

		/* If client requested a specific message ID, override the auto-generated one */
		if (requested_msg_id > 0 && msg_id > 0 && requested_msg_id != msg_id) {
			k_mutex_lock(&bbs->lock, K_FOREVER);
			/* Find the just-created message (last in array) */
			if (room->message_count > 0) {
				struct bbs_message *msg = &room->messages[room->message_count - 1];
				if (msg->id == msg_id) {
					uint32_t old_id = msg->id;
					LOG_INF("Overriding auto-generated ID %u with client-requested ID %u",
					        old_id, requested_msg_id);
					msg->id = requested_msg_id;
					msg_id = requested_msg_id;

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
					/* Rename the LittleFS file to match the new ID */
					char old_path[128], new_path[128];
					snprintf(old_path, sizeof(old_path), "%s/%s/%u",
					         BBS_ROOMS_PATH, room->name, old_id);
					snprintf(new_path, sizeof(new_path), "%s/%s/%u",
					         BBS_ROOMS_PATH, room->name, requested_msg_id);

					int ret = fs_rename(old_path, new_path);
					if (ret < 0) {
						LOG_ERR("Failed to rename %s to %s: %d", old_path, new_path, ret);
					} else {
						LOG_DBG("Renamed %s -> %s", old_path, new_path);
					}
#endif
				}
			}
			k_mutex_unlock(&bbs->lock);
		}

		LOG_INF("Created message %u in room %s (client requested name: %.*s)",
		        msg_id, room->name, name_len, name);

		/* Return new message node using the actual message ID */
		char msg_id_str[16];
		snprintf(msg_id_str, sizeof(msg_id_str), "%u", msg_id);
		*new_node = bbs_walk(parent, msg_id_str, strlen(msg_id_str), fs_ctx);

		LOG_INF("Returning node %p for message %u", *new_node, msg_id);

		return (*new_node != NULL) ? 0 : -EIO;
	}

	/* Creating not supported in other locations */
	return -EPERM;
}

static int bbs_remove(struct ninep_fs_node *node, void *fs_ctx)
{
	/* TODO: Implement remove (for deleting messages) */
	return -ENOTSUP;
}

static int bbs_clunk(struct ninep_fs_node *node, void *fs_ctx)
{
	/* Free the node */
	k_free(node);
	return 0;
}

static const struct ninep_fs_ops bbs_fs_ops = {
	.get_root = bbs_get_root,
	.walk = bbs_walk,
	.open = bbs_open,
	.read = bbs_read,
	.write = bbs_write,
	.stat = bbs_stat,
	.create = bbs_create,
	.remove = bbs_remove,
	.clunk = bbs_clunk,
};

const struct ninep_fs_ops *bbs_get_fs_ops(void)
{
	return &bbs_fs_ops;
}

/* ========================================================================
 * Server Registration
 * ======================================================================== */

struct ninep_server *bbs_register_server(struct bbs_instance *bbs)
{
	if (!bbs) {
		return NULL;
	}

	/* Allocate server config */
	struct ninep_server_config *config = k_malloc(sizeof(*config));
	if (!config) {
		return NULL;
	}

	config->fs_ops = &bbs_fs_ops;
	config->fs_ctx = bbs;
	config->max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	config->version = "9P2000";

	/* Allocate server instance */
	struct ninep_server *server = k_malloc(sizeof(*server));
	if (!server) {
		k_free(config);
		return NULL;
	}

	memset(server, 0, sizeof(*server));
	server->config = config;

	LOG_INF("Registered BBS as 9P server");
	return server;
}

void bbs_unregister_server(struct ninep_server *server)
{
	if (!server) {
		return;
	}

	if (server->config) {
		k_free((void *)server->config);
	}
	k_free(server);

	LOG_INF("Unregistered BBS server");
}
