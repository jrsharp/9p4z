/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9BBS_CHAT_H_
#define ZEPHYR_INCLUDE_9BBS_CHAT_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 9BBS Interactive Chat
 *
 * Real-time chat system for 9BBS using ring buffers and poll signals.
 * Supports multiple chat rooms with blocking reads for new messages.
 *
 * Filesystem structure:
 *   /chat/
 *     lobby          - Main chat room (read for messages, blocks until new)
 *     post           - Write to send message (format: "room:message" or just "message")
 *     users          - List of active users and idle times
 */

/* Configuration */
#ifndef CONFIG_9BBS_CHAT_MAX_ROOMS
#define CONFIG_9BBS_CHAT_MAX_ROOMS 4
#endif

#ifndef CONFIG_9BBS_CHAT_MAX_MESSAGES
#define CONFIG_9BBS_CHAT_MAX_MESSAGES 50
#endif

#ifndef CONFIG_9BBS_CHAT_MAX_MESSAGE_LEN
#define CONFIG_9BBS_CHAT_MAX_MESSAGE_LEN 256
#endif

#ifndef CONFIG_9BBS_CHAT_MAX_USERS
#define CONFIG_9BBS_CHAT_MAX_USERS 8
#endif

#ifndef CONFIG_9BBS_CHAT_READ_TIMEOUT_SEC
#define CONFIG_9BBS_CHAT_READ_TIMEOUT_SEC 30
#endif

/**
 * @brief Chat message in ring buffer
 */
struct chat_message {
	char from[32];                           /* Username */
	char text[CONFIG_9BBS_CHAT_MAX_MESSAGE_LEN];  /* Message text */
	uint64_t timestamp;                      /* Uptime in milliseconds */
	uint32_t id;                             /* Monotonically increasing ID */
};

/**
 * @brief Chat room with ring buffer and wake-up mechanism
 */
struct chat_room {
	char name[32];                           /* Room name */
	struct chat_message messages[CONFIG_9BBS_CHAT_MAX_MESSAGES];
	uint32_t head;                           /* Next write position */
	uint32_t tail;                           /* Oldest message position */
	uint32_t next_msg_id;                    /* Next message ID */
	bool active;
	bool admin_only;                         /* Only admins can post/read */

	/* Wake-up mechanism for blocking reads */
	struct k_poll_signal new_message_signal;
};

/**
 * @brief Connected chat user tracking
 */
struct chat_user {
	char username[32];
	uint32_t last_read_msg_id[CONFIG_9BBS_CHAT_MAX_ROOMS];  /* Per-room read position */
	uint64_t last_activity;                  /* Last read/write timestamp */
	bool active;
};

/**
 * @brief Chat subsystem instance
 */
struct chat_instance {
	struct chat_room rooms[CONFIG_9BBS_CHAT_MAX_ROOMS];
	uint32_t room_count;

	struct chat_user users[CONFIG_9BBS_CHAT_MAX_USERS];
	uint32_t user_count;

	struct k_mutex lock;  /* Protects chat state */
};

/**
 * @brief Initialize chat instance
 *
 * @param chat Chat instance to initialize
 * @return 0 on success, negative errno on failure
 */
int chat_init(struct chat_instance *chat);

/**
 * @brief Create a chat room
 *
 * @param chat Chat instance
 * @param name Room name
 * @param admin_only True if only admins can access
 * @return 0 on success, negative errno on failure
 */
int chat_create_room(struct chat_instance *chat, const char *name, bool admin_only);

/**
 * @brief Post a message to a chat room
 *
 * @param chat Chat instance
 * @param room_name Room to post to
 * @param username User posting
 * @param message Message text
 * @return 0 on success, negative errno on failure
 */
int chat_post_message(struct chat_instance *chat, const char *room_name,
                      const char *username, const char *message);

/**
 * @brief Get new messages from a room (blocking)
 *
 * Blocks until new messages are available or timeout expires.
 *
 * @param chat Chat instance
 * @param room_name Room to read from
 * @param username User reading (to track last_read position)
 * @param buf Buffer to write formatted messages
 * @param buf_len Buffer length
 * @param timeout_ms Timeout in milliseconds (0 = no block, -1 = forever)
 * @return Number of bytes written, 0 on timeout, negative errno on failure
 */
int chat_read_messages(struct chat_instance *chat, const char *room_name,
                       const char *username, char *buf, size_t buf_len,
                       int32_t timeout_ms);

/**
 * @brief Get list of active users
 *
 * @param chat Chat instance
 * @param buf Buffer to write user list
 * @param buf_len Buffer length
 * @return Number of bytes written, negative errno on failure
 */
int chat_get_users(struct chat_instance *chat, char *buf, size_t buf_len);

/**
 * @brief Update user activity timestamp
 *
 * @param chat Chat instance
 * @param username User to update
 */
void chat_update_activity(struct chat_instance *chat, const char *username);

/**
 * @brief Register/find a chat user
 *
 * @param chat Chat instance
 * @param username Username to register/find
 * @return Pointer to chat_user, NULL on failure
 */
struct chat_user *chat_register_user(struct chat_instance *chat, const char *username);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9BBS_CHAT_H_ */
