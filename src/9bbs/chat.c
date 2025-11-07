/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9bbs/chat.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(chat, CONFIG_NINEP_LOG_LEVEL);

int chat_init(struct chat_instance *chat)
{
	if (!chat) {
		return -EINVAL;
	}

	memset(chat, 0, sizeof(*chat));
	k_mutex_init(&chat->lock);

	/* Initialize poll signals for each room */
	for (uint32_t i = 0; i < CONFIG_9BBS_CHAT_MAX_ROOMS; i++) {
		k_poll_signal_init(&chat->rooms[i].new_message_signal);
	}

	/* Create default "lobby" room */
	int ret = chat_create_room(chat, "lobby", false);
	if (ret < 0) {
		LOG_ERR("Failed to create lobby: %d", ret);
		return ret;
	}

	LOG_INF("Chat initialized with %u rooms", chat->room_count);
	return 0;
}

int chat_create_room(struct chat_instance *chat, const char *name, bool admin_only)
{
	if (!chat || !name) {
		return -EINVAL;
	}

	k_mutex_lock(&chat->lock, K_FOREVER);

	if (chat->room_count >= CONFIG_9BBS_CHAT_MAX_ROOMS) {
		k_mutex_unlock(&chat->lock);
		return -ENOSPC;
	}

	/* Check if room already exists */
	for (uint32_t i = 0; i < chat->room_count; i++) {
		if (strcmp(chat->rooms[i].name, name) == 0) {
			k_mutex_unlock(&chat->lock);
			return -EEXIST;
		}
	}

	struct chat_room *room = &chat->rooms[chat->room_count];
	strncpy(room->name, name, sizeof(room->name) - 1);
	room->name[sizeof(room->name) - 1] = '\0';
	room->head = 0;
	room->tail = 0;
	room->next_msg_id = 1;
	room->active = true;
	room->admin_only = admin_only;

	chat->room_count++;

	k_mutex_unlock(&chat->lock);

	LOG_INF("Created chat room '%s' (admin_only=%d)", name, admin_only);
	return 0;
}

struct chat_user *chat_register_user(struct chat_instance *chat, const char *username)
{
	if (!chat || !username) {
		return NULL;
	}

	k_mutex_lock(&chat->lock, K_FOREVER);

	/* Check if user already exists */
	for (uint32_t i = 0; i < chat->user_count; i++) {
		if (chat->users[i].active &&
		    strcmp(chat->users[i].username, username) == 0) {
			/* Update activity and return existing user */
			chat->users[i].last_activity = k_uptime_get();
			k_mutex_unlock(&chat->lock);
			return &chat->users[i];
		}
	}

	/* Try to reuse inactive slot */
	for (uint32_t i = 0; i < chat->user_count; i++) {
		if (!chat->users[i].active) {
			struct chat_user *user = &chat->users[i];
			strncpy(user->username, username, sizeof(user->username) - 1);
			user->username[sizeof(user->username) - 1] = '\0';
			user->last_activity = k_uptime_get();
			user->active = true;
			memset(user->last_read_msg_id, 0, sizeof(user->last_read_msg_id));
			k_mutex_unlock(&chat->lock);
			LOG_INF("Reactivated chat user '%s'", username);
			return user;
		}
	}

	/* Create new user */
	if (chat->user_count >= CONFIG_9BBS_CHAT_MAX_USERS) {
		k_mutex_unlock(&chat->lock);
		LOG_WRN("Cannot register user '%s': max users reached", username);
		return NULL;
	}

	struct chat_user *user = &chat->users[chat->user_count];
	strncpy(user->username, username, sizeof(user->username) - 1);
	user->username[sizeof(user->username) - 1] = '\0';
	user->last_activity = k_uptime_get();
	user->active = true;
	memset(user->last_read_msg_id, 0, sizeof(user->last_read_msg_id));

	chat->user_count++;

	k_mutex_unlock(&chat->lock);

	LOG_INF("Registered new chat user '%s'", username);
	return user;
}

void chat_update_activity(struct chat_instance *chat, const char *username)
{
	if (!chat || !username) {
		return;
	}

	k_mutex_lock(&chat->lock, K_FOREVER);

	for (uint32_t i = 0; i < chat->user_count; i++) {
		if (chat->users[i].active &&
		    strcmp(chat->users[i].username, username) == 0) {
			chat->users[i].last_activity = k_uptime_get();
			break;
		}
	}

	k_mutex_unlock(&chat->lock);
}

int chat_post_message(struct chat_instance *chat, const char *room_name,
                      const char *username, const char *message)
{
	if (!chat || !room_name || !username || !message) {
		return -EINVAL;
	}

	k_mutex_lock(&chat->lock, K_FOREVER);

	/* Find room */
	struct chat_room *room = NULL;
	for (uint32_t i = 0; i < chat->room_count; i++) {
		if (strcmp(chat->rooms[i].name, room_name) == 0) {
			room = &chat->rooms[i];
			break;
		}
	}

	if (!room) {
		k_mutex_unlock(&chat->lock);
		LOG_WRN("Room '%s' not found", room_name);
		return -ENOENT;
	}

	/* Add message to ring buffer */
	struct chat_message *msg = &room->messages[room->head];
	msg->id = room->next_msg_id++;
	msg->timestamp = k_uptime_get();
	strncpy(msg->from, username, sizeof(msg->from) - 1);
	msg->from[sizeof(msg->from) - 1] = '\0';
	strncpy(msg->text, message, sizeof(msg->text) - 1);
	msg->text[sizeof(msg->text) - 1] = '\0';

	room->head = (room->head + 1) % CONFIG_9BBS_CHAT_MAX_MESSAGES;
	if (room->head == room->tail) {
		/* Buffer full - evict oldest message */
		room->tail = (room->tail + 1) % CONFIG_9BBS_CHAT_MAX_MESSAGES;
	}

	/* Wake up all readers waiting on this room */
	k_poll_signal_raise(&room->new_message_signal, 0);

	k_mutex_unlock(&chat->lock);

	/* Update user activity */
	chat_update_activity(chat, username);

	LOG_DBG("Posted message to '%s' from '%s': '%s'", room_name, username, message);
	return 0;
}

int chat_read_messages(struct chat_instance *chat, const char *room_name,
                       const char *username, char *buf, size_t buf_len,
                       int32_t timeout_ms)
{
	if (!chat || !room_name || !username || !buf) {
		return -EINVAL;
	}

	/* Register/find user */
	struct chat_user *user = chat_register_user(chat, username);
	if (!user) {
		return -ENOMEM;
	}

	k_mutex_lock(&chat->lock, K_FOREVER);

	/* Find room */
	struct chat_room *room = NULL;
	uint32_t room_idx = 0;
	for (uint32_t i = 0; i < chat->room_count; i++) {
		if (strcmp(chat->rooms[i].name, room_name) == 0) {
			room = &chat->rooms[i];
			room_idx = i;
			break;
		}
	}

	if (!room) {
		k_mutex_unlock(&chat->lock);
		return -ENOENT;
	}

	/* Get last message ID this user has seen */
	uint32_t last_seen = user->last_read_msg_id[room_idx];

	/* Check if there are new messages */
	if (room->next_msg_id <= last_seen && timeout_ms != 0) {
		/* No new messages - prepare to block */
		struct k_poll_event event = K_POLL_EVENT_INITIALIZER(
			K_POLL_TYPE_SIGNAL,
			K_POLL_MODE_NOTIFY_ONLY,
			&room->new_message_signal);

		/* Reset signal */
		k_poll_signal_reset(&room->new_message_signal);

		k_mutex_unlock(&chat->lock);

		/* Block until new message or timeout */
		k_timeout_t timeout = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);
		int ret = k_poll(&event, 1, timeout);

		k_mutex_lock(&chat->lock, K_FOREVER);

		if (ret == -EAGAIN) {
			/* Timeout */
			k_mutex_unlock(&chat->lock);
			return 0;
		} else if (ret < 0) {
			k_mutex_unlock(&chat->lock);
			return ret;
		}
	}

	/* Format new messages since last_seen */
	char *pos = buf;
	size_t remaining = buf_len;
	uint32_t count = 0;

	/* Iterate through ring buffer to find messages > last_seen */
	for (uint32_t i = 0; i < CONFIG_9BBS_CHAT_MAX_MESSAGES; i++) {
		uint32_t idx = (room->tail + i) % CONFIG_9BBS_CHAT_MAX_MESSAGES;
		struct chat_message *msg = &room->messages[idx];

		/* Stop if we hit an empty slot or reached head */
		if (idx == room->head && i > 0) {
			break;
		}

		/* Only include messages newer than last_seen */
		if (msg->id > last_seen && msg->id < room->next_msg_id) {
			/* Format: [uptime] username: message\n */
			uint64_t seconds = msg->timestamp / 1000;
			uint64_t minutes = seconds / 60;
			uint64_t hours = minutes / 60;

			int len = snprintf(pos, remaining, "[%02llu:%02llu:%02llu] %s: %s\n",
			                   hours, minutes % 60, seconds % 60,
			                   msg->from, msg->text);

			if (len < 0 || len >= remaining) {
				/* Buffer full */
				break;
			}

			pos += len;
			remaining -= len;
			count++;

			/* Update user's last read position */
			if (msg->id > user->last_read_msg_id[room_idx]) {
				user->last_read_msg_id[room_idx] = msg->id;
			}
		}
	}

	k_mutex_unlock(&chat->lock);

	/* Update activity */
	chat_update_activity(chat, username);

	LOG_DBG("Read %u messages from '%s' for user '%s'", count, room_name, username);
	return pos - buf;
}

int chat_get_users(struct chat_instance *chat, char *buf, size_t buf_len)
{
	if (!chat || !buf) {
		return -EINVAL;
	}

	k_mutex_lock(&chat->lock, K_FOREVER);

	char *pos = buf;
	size_t remaining = buf_len;
	uint64_t now = k_uptime_get();

	for (uint32_t i = 0; i < chat->user_count; i++) {
		if (!chat->users[i].active) {
			continue;
		}

		uint64_t idle_ms = now - chat->users[i].last_activity;
		uint64_t idle_sec = idle_ms / 1000;

		int len = snprintf(pos, remaining, "%s (idle: %llus)\n",
		                   chat->users[i].username, idle_sec);

		if (len < 0 || len >= remaining) {
			break;
		}

		pos += len;
		remaining -= len;
	}

	k_mutex_unlock(&chat->lock);

	return pos - buf;
}
