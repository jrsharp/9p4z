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

/* Load a message from LittleFS */
static int load_message_from_lfs(const char *room_name, uint32_t msg_id,
                                  struct bbs_message *msg)
{
	char path[128];
	snprintf(path, sizeof(path), "%s/%s/%u", BBS_ROOMS_PATH, room_name, msg_id);

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

	/* Initialize message */
	memset(msg, 0, sizeof(*msg));
	msg->id = msg_id;

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

		/* Parse message ID from filename */
		uint32_t msg_id = strtoul(entry.name, NULL, 10);
		if (msg_id == 0) {
			LOG_WRN("Invalid message filename: %s", entry.name);
			continue;
		}

		/* Check if we have space */
		if (room->message_count >= CONFIG_9BBS_MAX_MESSAGES_PER_ROOM) {
			LOG_WRN("Room '%s' full, can't load message %u", room_name, msg_id);
			break;
		}

		/* Load message into next slot */
		struct bbs_message *msg = &room->messages[room->message_count];
		ret = load_message_from_lfs(room_name, msg_id, msg);
		if (ret == 0) {
			room->message_count++;
			/* Update next_message_id to be one past the highest ID */
			if (msg_id >= room->next_message_id) {
				room->next_message_id = msg_id + 1;
			}
			LOG_DBG("Loaded message %u from room '%s'", msg_id, room_name);
		} else {
			LOG_ERR("Failed to load message %u from room '%s': %d",
			        msg_id, room_name, ret);
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
	msg->date = k_uptime_get() / 1000;  /* Seconds since boot */
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
	node->type = (type == BBS_NODE_MESSAGE_FILE) ? NINEP_NODE_FILE : NINEP_NODE_DIR;
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

	/* ROOT directory - walk directly to rooms */
	enum bbs_node_type parent_type = (enum bbs_node_type)((uint64_t)parent->qid.path >> 32);
	if (parent_type == BBS_NODE_ROOT) {
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
		uint32_t msg_id = atoi(name_str);

		k_mutex_lock(&bbs->lock, K_FOREVER);
		for (uint32_t i = 0; i < room->message_count; i++) {
			if (room->messages[i].id == msg_id) {
				struct ninep_fs_node *node = bbs_create_node(
					BBS_NODE_MESSAGE_FILE, name_str, &room->messages[i]);
				k_mutex_unlock(&bbs->lock);
				return node;
			}
		}
		k_mutex_unlock(&bbs->lock);
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

	} else if (type == BBS_NODE_ROOT) {
		/* Directory listing for root: list all rooms directly */
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
			uint32_t entry_size = 2 + stat_size;  /* size[2] + stat data */

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

	} else if (type == BBS_NODE_ROOM_DIR) {
		/* Directory listing for /rooms/<room>: list message numbers */
		struct bbs_room *room = (struct bbs_room *)node->data;
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
			uint32_t entry_size = 2 + stat_size;  /* size[2] + stat data */

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode,
				                           strlen(room->messages[i].body),
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

		/* Post message with empty body (will be populated via write if needed) */
		int msg_id = bbs_post_message(bbs, room->name, from, "", 0);
		if (msg_id < 0) {
			return msg_id;
		}

		LOG_INF("Created message %d in room %s (client requested name: %.*s)",
		        msg_id, room->name, name_len, name);

		/* Return new message node using the actual message ID */
		char msg_id_str[16];
		snprintf(msg_id_str, sizeof(msg_id_str), "%d", msg_id);
		*new_node = bbs_walk(parent, msg_id_str, strlen(msg_id_str), fs_ctx);

		LOG_INF("Returning node %p for message %d", *new_node, msg_id);

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
