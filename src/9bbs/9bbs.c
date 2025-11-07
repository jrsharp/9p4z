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

/* TODO Phase 2: Cryptographic Signature Verification
 *
 * Future enhancement to support cryptographic signatures on messages, allowing
 * users to post with verified identities from any client:
 *
 * 1. Public Key Storage:
 *    - Add /etc/pubkeys/ directory for storing user public keys
 *    - Keys stored as PEM or DER format (Ed25519, ECDSA P-256)
 *    - One file per user: /etc/pubkeys/<username>
 *
 * 2. Signature Verification:
 *    - Extract PGP-style signature blocks from message bodies
 *    - Verify using PSA Crypto API (psa_verify_message)
 *    - Support Ed25519 and ECDSA P-256 signatures
 *
 * 3. Authentication Flow:
 *    - First post from user: Submit public key + signed challenge
 *    - Server stores public key in /etc/pubkeys/<username>
 *    - Subsequent posts: Include signature in message body
 *    - Server verifies signature against stored public key
 *
 * 4. Integration Points:
 *    - bbs_post_message(): Verify signature before accepting message
 *    - bbs_create_user(): Accept public key during registration
 *    - Add 'verified' flag to struct bbs_message
 *
 * References:
 *    - Zephyr PSA Crypto: https://docs.zephyrproject.org/latest/services/crypto/index.html
 *    - Nordic PSA driver: nrf_security library in NCS
 */

/* LittleFS mount point */
#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
#define BBS_LFS_MOUNT_POINT "/lfs_bbs"
#define BBS_ROOMS_PATH BBS_LFS_MOUNT_POINT "/rooms"
#define BBS_ETC_PATH BBS_LFS_MOUNT_POINT "/etc"
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
	char header[384];
	int len = snprintf(header, sizeof(header),
	                   "From: %s\nTo: %s\nSubject: %s\nDate: %llu\nReply-To: %u\n\n",
	                   msg->from, msg->to, msg->subject, msg->date, msg->reply_to);

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
		} else if (strncmp(line, "Subject: ", 9) == 0) {
			strncpy(msg->subject, line + 9, sizeof(msg->subject) - 1);
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

/* Save metadata to LittleFS */
static int save_metadata_to_lfs(struct bbs_instance *bbs)
{
	int ret;

	/* Ensure /etc directory exists */
	ret = fs_mkdir(BBS_ETC_PATH);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("Failed to create %s: %d", BBS_ETC_PATH, ret);
		return ret;
	}

	/* Save each metadata field to a separate file */
	const char *filenames[] = {"boardname", "sysop", "motd", "location", "description"};
	const char *values[] = {bbs->boardname, bbs->sysop, bbs->motd,
	                        bbs->location, bbs->description};

	for (size_t i = 0; i < 5; i++) {
		char path[128];
		snprintf(path, sizeof(path), "%s/%s", BBS_ETC_PATH, filenames[i]);

		struct fs_file_t file;
		fs_file_t_init(&file);

		ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
		if (ret < 0) {
			LOG_ERR("Failed to open %s: %d", path, ret);
			continue;  /* Try to save other fields */
		}

		size_t len = strlen(values[i]);
		if (len > 0) {
			ret = fs_write(&file, values[i], len);
			if (ret < 0) {
				LOG_ERR("Failed to write %s: %d", path, ret);
			}
		}

		fs_close(&file);
	}

	LOG_DBG("Saved metadata to LittleFS");
	return 0;
}

/* Load metadata from LittleFS */
static int load_metadata_from_lfs(struct bbs_instance *bbs)
{
	struct fs_file_t file;
	char buf[256];
	int ret;

	const char *filenames[] = {"boardname", "sysop", "motd", "location", "description"};
	char *targets[] = {bbs->boardname, bbs->sysop, bbs->motd,
	                   bbs->location, bbs->description};
	size_t target_sizes[] = {sizeof(bbs->boardname), sizeof(bbs->sysop),
	                          sizeof(bbs->motd), sizeof(bbs->location),
	                          sizeof(bbs->description)};

	for (size_t i = 0; i < 5; i++) {
		char path[128];
		snprintf(path, sizeof(path), "%s/%s", BBS_ETC_PATH, filenames[i]);

		fs_file_t_init(&file);
		ret = fs_open(&file, path, FS_O_READ);
		if (ret < 0) {
			continue;  /* File doesn't exist, use default */
		}

		/* Read file content */
		ret = fs_read(&file, buf, sizeof(buf) - 1);
		fs_close(&file);

		if (ret > 0) {
			buf[ret] = '\0';  /* Null terminate */
			/* Copy to target, respecting size */
			size_t copy_len = (ret < target_sizes[i] - 1) ? ret : target_sizes[i] - 1;
			memcpy(targets[i], buf, copy_len);
			targets[i][copy_len] = '\0';
			LOG_DBG("Loaded %s: '%s'", filenames[i], targets[i]);
		}
	}

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

		/* Load metadata from LittleFS */
		load_metadata_from_lfs(bbs);
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

	/* Enable registration if no users exist */
	if (bbs->user_count == 0) {
		bbs->allow_registration = true;
		LOG_WRN("BBS has no users - first user will become SysOp");
	} else {
		bbs->allow_registration = false;
		LOG_INF("BBS has %u user(s) - registration disabled", bbs->user_count);
	}

	/* Initialize authenticated user (empty = no auth yet) */
	bbs->authenticated_user[0] = '\0';

	/* Initialize chat subsystem */
	ret = chat_init(&bbs->chat);
	if (ret < 0) {
		LOG_ERR("Failed to initialize chat: %d", ret);
		return ret;
	}
	LOG_INF("Chat subsystem initialized");

	/* Initialize metadata with defaults if not set */
	if (bbs->boardname[0] == '\0') {
		strncpy(bbs->boardname, "9BBS", sizeof(bbs->boardname) - 1);
	}
	if (bbs->sysop[0] == '\0') {
		strncpy(bbs->sysop, "sysop", sizeof(bbs->sysop) - 1);
	}
	if (bbs->motd[0] == '\0') {
		strncpy(bbs->motd, "Welcome to 9BBS - A Plan 9 style BBS for Zephyr",
		        sizeof(bbs->motd) - 1);
	}
	if (bbs->location[0] == '\0') {
		strncpy(bbs->location, "Cyberspace", sizeof(bbs->location) - 1);
	}
	if (bbs->description[0] == '\0') {
		strncpy(bbs->description, "A 9P bulletin board system",
		        sizeof(bbs->description) - 1);
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

	/* First user becomes sysop */
	if (bbs->user_count == 0) {
		user->is_admin = true;
		bbs->allow_registration = false;  /* Disable registration after first user */
		LOG_INF("First user '%s' registered as SysOp", username);
	} else {
		user->is_admin = false;
	}

	/* Initialize read positions for all rooms */
	for (uint32_t i = 0; i < bbs->room_count; i++) {
		strncpy(user->rooms[i].room, bbs->rooms[i].name,
		        sizeof(user->rooms[i].room) - 1);
		user->rooms[i].last_read = 0;
	}
	user->room_count = bbs->room_count;

	bbs->user_count++;

	k_mutex_unlock(&bbs->lock);

	LOG_INF("Created user: %s (admin=%d)", username, user->is_admin);
	return 0;
}

/**
 * @brief Check if a user is an administrator
 *
 * Helper function to look up a user by name and check their admin status.
 * Used for permission checking on administrative operations.
 *
 * @param bbs BBS instance
 * @param username Username to check
 * @return true if user exists and is admin, false otherwise
 */
static bool bbs_is_user_admin(struct bbs_instance *bbs, const char *username)
{
	if (!bbs || !username) {
		return false;
	}

	for (uint32_t i = 0; i < bbs->user_count; i++) {
		if (bbs->users[i].active &&
		    strcmp(bbs->users[i].username, username) == 0) {
			return bbs->users[i].is_admin;
		}
	}

	return false;  /* User not found or not admin */
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
	msg->subject[0] = '\0';  /* No subject for messages created via post */

	/* TODO Phase 2: Verify cryptographic signature on message body
	 * - Extract signature from message body (e.g., PGP signature block)
	 * - Verify signature matches claimed 'from' user's public key
	 * - Use PSA Crypto API for signature verification (ECDSA, Ed25519)
	 * - Reject messages with invalid or missing signatures
	 * - Store verified identity flag in message metadata
	 */

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
	BBS_NODE_CHAT_DIR,        /* /chat directory */
	BBS_NODE_CHAT_ROOM,       /* /chat/lobby (blocking read stream) */
	BBS_NODE_CHAT_POST,       /* /chat/post (write-only) */
	BBS_NODE_CHAT_USERS,      /* /chat/users (list active users) */
};

/* Cached root node - allocated once, reused forever (like srv.c does) */
static struct ninep_fs_node *bbs_root_node = NULL;

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
	/* Allocate root node on first call, then reuse it forever */
	if (!bbs_root_node) {
		bbs_root_node = bbs_create_node(BBS_NODE_ROOT, "/", fs_ctx);
		if (bbs_root_node) {
			LOG_DBG("Allocated BBS root node: %p", bbs_root_node);
		} else {
			LOG_ERR("Failed to allocate BBS root node");
		}
	}
	return bbs_root_node;
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

	/* ROOT directory - walk to "etc", "rooms", or "chat" */
	if (parent_type == BBS_NODE_ROOT) {
		if (strcmp(name_str, "etc") == 0) {
			return bbs_create_node(BBS_NODE_ETC_DIR, "etc", fs_ctx);
		} else if (strcmp(name_str, "rooms") == 0) {
			return bbs_create_node(BBS_NODE_ROOMS_DIR, "rooms", fs_ctx);
		} else if (strcmp(name_str, "chat") == 0) {
			return bbs_create_node(BBS_NODE_CHAT_DIR, "chat", fs_ctx);
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
		const char *etc_files[] = {"boardname", "sysop", "motd", "location", "description", "version", "registration"};
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

	/* CHAT directory - walk to post, users, or chat rooms */
	if (parent_type == BBS_NODE_CHAT_DIR) {
		if (strcmp(name_str, "post") == 0) {
			return bbs_create_node(BBS_NODE_CHAT_POST, "post", fs_ctx);
		} else if (strcmp(name_str, "users") == 0) {
			return bbs_create_node(BBS_NODE_CHAT_USERS, "users", fs_ctx);
		} else {
			/* Try to find chat room by name */
			k_mutex_lock(&bbs->lock, K_FOREVER);
			for (uint32_t i = 0; i < bbs->chat.room_count; i++) {
				struct chat_room *room = &bbs->chat.rooms[i];
				if (strcmp(room->name, name_str) == 0 && room->active) {
					struct ninep_fs_node *node = bbs_create_node(
						BBS_NODE_CHAT_ROOM, room->name, room);
					k_mutex_unlock(&bbs->lock);
					return node;
				}
			}
			k_mutex_unlock(&bbs->lock);
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
		                   "Subject: %s\n"
		                   "Date: %llu\n"
		                   "X-Date-N: %llu\n"
		                   "\n"
		                   "%s\n"
		                   "\n"
		                   "%s\n",
		                   msg->from, msg->to, msg->subject,
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
		/* Read /etc/ metadata files from memory */
		const char *filename = (const char *)node->data;
		const char *content = NULL;
		char version_buf[32];

		/* Map filename to metadata field */
		if (strcmp(filename, "boardname") == 0) {
			content = bbs->boardname;
		} else if (strcmp(filename, "sysop") == 0) {
			content = bbs->sysop;
		} else if (strcmp(filename, "motd") == 0) {
			content = bbs->motd;
		} else if (strcmp(filename, "location") == 0) {
			content = bbs->location;
		} else if (strcmp(filename, "description") == 0) {
			content = bbs->description;
		} else if (strcmp(filename, "version") == 0) {
			snprintf(version_buf, sizeof(version_buf), "9BBS v0.1.0\n");
			content = version_buf;
		} else if (strcmp(filename, "registration") == 0) {
			content = bbs->allow_registration ? "enabled\n" : "disabled\n";
		} else {
			/* Unknown /etc/ file */
			LOG_WRN("Unknown /etc/ file: %s", filename);
			return -ENOENT;
		}

		/* Add newline if not already present (except registration which has it) */
		char temp[512];
		size_t len;
		if (strcmp(filename, "registration") == 0 || strcmp(filename, "version") == 0) {
			len = strlen(content);
			if (len >= sizeof(temp)) len = sizeof(temp) - 1;
			memcpy(temp, content, len);
		} else {
			len = snprintf(temp, sizeof(temp), "%s\n", content);
		}

		if (offset >= len) {
			return 0;  /* EOF */
		}

		size_t to_copy = (offset + count > len) ? (len - offset) : count;
		memcpy(buf, temp + offset, to_copy);
		return to_copy;

	} else if (type == BBS_NODE_ROOT) {
		/* Directory listing for root: list "etc", "rooms", and "chat" */
		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		const char *entries[] = {"etc", "rooms", "chat"};
		const enum bbs_node_type entry_types[] = {BBS_NODE_ETC_DIR, BBS_NODE_ROOMS_DIR, BBS_NODE_CHAT_DIR};

		for (size_t i = 0; i < 3; i++) {
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

		/* Check if authenticated user is admin for permission bits */
		bool is_admin = bbs_is_user_admin(bbs, bbs->authenticated_user);

		const char *entries[] = {"boardname", "sysop", "motd", "location", "description", "version", "registration", "nets"};
		const bool is_dir[] = {false, false, false, false, false, false, false, true};
		const bool is_writable[] = {true, true, true, true, true, false, false, false};  /* version & registration are read-only */

		for (size_t i = 0; i < 8; i++) {
			struct ninep_qid entry_qid = {
				.type = is_dir[i] ? NINEP_QTDIR : NINEP_QTFILE,
				.version = 0,
				.path = ((uint64_t)(is_dir[i] ? BBS_NODE_ETC_NETS_DIR : BBS_NODE_ETC_FILE) << 32) | i
			};
			/* Dynamic permissions: writable (0644) for admin, read-only (0444) for others */
			uint32_t mode;
			if (is_dir[i]) {
				mode = 0755 | NINEP_DMDIR;  /* Directories always 0755 */
			} else if (is_writable[i] && is_admin) {
				mode = 0644;  /* Writable for admin */
			} else {
				mode = 0444;  /* Read-only for everyone else */
			}
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

	} else if (type == BBS_NODE_CHAT_DIR) {
		/* Directory listing for /chat: list post, users, and all chat rooms */
		size_t buf_offset = 0;
		uint64_t current_offset = 0;

		/* Static files: post (write-only), users (read-only) */
		const char *static_entries[] = {"post", "users"};
		const enum bbs_node_type static_types[] = {BBS_NODE_CHAT_POST, BBS_NODE_CHAT_USERS};
		const uint32_t static_modes[] = {0200, 0444};  /* post is write-only, users is read-only */

		for (size_t i = 0; i < 2; i++) {
			struct ninep_qid entry_qid = {
				.type = NINEP_QTFILE,
				.version = 0,
				.path = ((uint64_t)static_types[i] << 32) | i
			};
			uint16_t name_len = strlen(static_entries[i]);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, static_modes[i], 0,
				                           static_entries[i], name_len);
				if (ret < 0) break;
				buf_offset += write_offset;
				current_offset += write_offset;
			} else {
				current_offset += entry_size;
			}
		}

		/* List all chat rooms */
		k_mutex_lock(&bbs->lock, K_FOREVER);
		for (uint32_t i = 0; i < bbs->chat.room_count; i++) {
			struct chat_room *room = &bbs->chat.rooms[i];
			if (!room->active) continue;

			struct ninep_qid entry_qid = {
				.type = NINEP_QTFILE,
				.version = 0,
				.path = ((uint64_t)BBS_NODE_CHAT_ROOM << 32) | (uint64_t)(uintptr_t)room
			};
			uint32_t mode = 0444;  /* Read-only (blocking read) */
			uint16_t name_len = strlen(room->name);
			uint16_t stat_size = 2 + 4 + 13 + 4 + 4 + 4 + 8 +
			                     (2 + name_len) + (2 + 6) + (2 + 6) + (2 + 6);
			uint32_t entry_size = 2 + stat_size;

			if (current_offset >= offset) {
				if (buf_offset + entry_size > count) break;
				size_t write_offset = 0;
				int ret = ninep_write_stat(buf + buf_offset, count - buf_offset,
				                           &write_offset, &entry_qid, mode, 0,
				                           room->name, name_len);
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

	} else if (type == BBS_NODE_CHAT_ROOM) {
		/* Reading from a chat room - blocking read for new messages */
		struct chat_room *room = (struct chat_room *)node->data;
		if (!room) {
			LOG_ERR("BBS_NODE_CHAT_ROOM has NULL data pointer!");
			return -EINVAL;
		}

		const char *username = (bbs->authenticated_user[0] != '\0') ?
		                       bbs->authenticated_user : "guest";

		/* Blocking read with timeout */
		int32_t timeout_ms = CONFIG_9BBS_CHAT_READ_TIMEOUT_SEC * 1000;
		int ret = chat_read_messages(&bbs->chat, room->name, username,
		                              (char *)buf, count, timeout_ms);
		return ret;

	} else if (type == BBS_NODE_CHAT_USERS) {
		/* List of active chat users */
		int ret = chat_get_users(&bbs->chat, (char *)buf, count);
		return ret;
	}

	return -EINVAL;
}

static int bbs_write(struct ninep_fs_node *node, uint64_t offset,
                     const uint8_t *buf, uint32_t count, const char *uname,
                     void *fs_ctx)
{
	struct bbs_instance *bbs = (struct bbs_instance *)fs_ctx;
	enum bbs_node_type type = (enum bbs_node_type)((uint64_t)node->qid.path >> 32);

	/* Update authenticated user whenever we see a write (has uname) */
	if (uname && uname[0] != '\0') {
		strncpy(bbs->authenticated_user, uname, sizeof(bbs->authenticated_user) - 1);
		bbs->authenticated_user[sizeof(bbs->authenticated_user) - 1] = '\0';
	}

	/* Handle writes to /etc/ metadata files (admin only) */
	if (type == BBS_NODE_ETC_FILE) {
		/* Check if user is admin */
		if (!bbs_is_user_admin(bbs, uname)) {
			LOG_WRN("User '%s' attempted to write /etc/ file without admin privileges",
			        uname ? uname : "(null)");
			return -EPERM;
		}

		/* Get the filename from node data */
		const char *filename = (const char *)node->data;
		if (!filename) {
			return -EINVAL;
		}

		/* Read-only files */
		if (strcmp(filename, "version") == 0 || strcmp(filename, "registration") == 0) {
			LOG_WRN("Attempted write to read-only file: /etc/%s", filename);
			return -EPERM;
		}

		k_mutex_lock(&bbs->lock, K_FOREVER);

		/* Determine which metadata field to update */
		char *target = NULL;
		size_t target_size = 0;

		if (strcmp(filename, "boardname") == 0) {
			target = bbs->boardname;
			target_size = sizeof(bbs->boardname);
		} else if (strcmp(filename, "sysop") == 0) {
			target = bbs->sysop;
			target_size = sizeof(bbs->sysop);
		} else if (strcmp(filename, "motd") == 0) {
			target = bbs->motd;
			target_size = sizeof(bbs->motd);
		} else if (strcmp(filename, "location") == 0) {
			target = bbs->location;
			target_size = sizeof(bbs->location);
		} else if (strcmp(filename, "description") == 0) {
			target = bbs->description;
			target_size = sizeof(bbs->description);
		} else {
			k_mutex_unlock(&bbs->lock);
			LOG_WRN("Unknown /etc/ file: %s", filename);
			return -ENOENT;
		}

		/* Handle write at offset 0 as replacement */
		if (offset == 0) {
			/* Clear the field first */
			memset(target, 0, target_size);
		}

		/* Calculate how much we can write */
		if (offset >= target_size - 1) {
			k_mutex_unlock(&bbs->lock);
			return 0;  /* Beyond end of field */
		}

		size_t available = target_size - 1 - offset;  /* -1 for null terminator */
		size_t to_write = (count > available) ? available : count;

		/* Write the data */
		memcpy(target + offset, buf, to_write);

		/* Ensure null termination */
		target[offset + to_write] = '\0';

		/* Strip trailing newline if present (common from echo commands) */
		size_t len = strlen(target);
		if (len > 0 && target[len - 1] == '\n') {
			target[len - 1] = '\0';
		}

		LOG_INF("Admin '%s' updated /etc/%s: '%s'", uname, filename, target);

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
		/* Persist metadata changes to LittleFS */
		int persist_ret = save_metadata_to_lfs(bbs);
		if (persist_ret < 0) {
			LOG_WRN("Failed to persist metadata to LittleFS: %d", persist_ret);
			/* Don't fail the write - metadata is updated in RAM */
		}
#endif

		k_mutex_unlock(&bbs->lock);
		return to_write;
	}

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
			LOG_WRN("Failed to post chat message: %d", ret);
			return ret;
		}

		LOG_DBG("Chat message posted to '%s' by '%s': '%s'", room_name, username, msg_buf);
		return count;
	}

	/* Only message files can be written to (below this point) */
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

	/* Parse headers from the message body (RFC-822 style) */
	char *body_start = msg->body;
	char *line = body_start;
	char *body_content = body_start;  /* Will point to actual body after headers */

	while (line && *line) {
		char *next_line = strchr(line, '\n');
		size_t line_len = next_line ? (next_line - line) : strlen(line);

		/* Empty line marks end of headers */
		if (line_len == 0 || (line_len == 1 && line[0] == '\r')) {
			body_content = next_line ? (next_line + 1) : (line + line_len);
			break;
		}

		/* Parse header: "Header: value" */
		char *colon = memchr(line, ':', line_len);
		if (colon && colon > line) {
			size_t header_name_len = colon - line;
			char *value = colon + 1;

			/* Skip leading whitespace in value */
			while (value < line + line_len && (*value == ' ' || *value == '\t')) {
				value++;
			}

			size_t value_len = (line + line_len) - value;
			/* Remove trailing \r if present */
			if (value_len > 0 && value[value_len - 1] == '\r') {
				value_len--;
			}

			/* Extract Subject header */
			if (header_name_len == 7 && strncmp(line, "Subject", 7) == 0) {
				size_t copy_len = (value_len < sizeof(msg->subject) - 1) ?
				                  value_len : sizeof(msg->subject) - 1;
				memcpy(msg->subject, value, copy_len);
				msg->subject[copy_len] = '\0';
				LOG_DBG("Parsed Subject: '%s'", msg->subject);
			}
		}

		line = next_line ? (next_line + 1) : NULL;
	}

	/* If we parsed headers, adjust body_len to exclude them */
	if (body_content > body_start) {
		size_t header_size = body_content - body_start;
		if (msg->body_len > header_size) {
			/* Move body content to start of buffer */
			size_t actual_body_len = msg->body_len - header_size;
			memmove(body_start, body_content, actual_body_len);
			body_start[actual_body_len] = '\0';
			msg->body_len = actual_body_len;
			LOG_DBG("Extracted headers (%zu bytes), body now %zu bytes",
			        header_size, actual_body_len);
		}
	}

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
	ARG_UNUSED(fs_ctx);

	/* DON'T free the root node - it's cached and reused */
	if (node == bbs_root_node) {
		LOG_DBG("bbs_clunk: Root node clunked (not freeing, will be reused)");
		return 0;
	}

	/* Free all other nodes */
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
