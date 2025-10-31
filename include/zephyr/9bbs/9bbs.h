/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_9BBS_H_
#define ZEPHYR_INCLUDE_9BBS_H_

#include <zephyr/kernel.h>
#include <zephyr/9p/server.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 9bbs - A Plan 9-style BBS for Zephyr
 *
 * This is a filesystem-oriented bulletin board system inspired by Citadel
 * and the 9bbs implementation for Plan 9. The BBS exposes itself as a 9P
 * filesystem that can be accessed locally or over the network.
 *
 * Filesystem structure:
 *   /rooms/
 *     lobby/
 *       1          - Message #1
 *       2          - Message #2
 *       ...
 *     tech/
 *       1
 *       ...
 *   /etc/
 *     roomlist     - List of rooms
 *     users/
 *       alice/
 *         password
 *         room       - Current room
 *         rooms      - Read positions (lobby/5, tech/3)
 *         sig        - Signature
 *
 * Message format:
 *   From: alice
 *   To: lobby
 *   Date: Wed, 31 Dec 1969 19:00:01 EST
 *   X-Date-N: 1234567890
 *
 *   [message body]
 *
 *   [signature]
 */

/* Configuration */
#ifndef CONFIG_9BBS_MAX_ROOMS
#define CONFIG_9BBS_MAX_ROOMS 8
#endif

#ifndef CONFIG_9BBS_MAX_USERS
#define CONFIG_9BBS_MAX_USERS 8
#endif

#ifndef CONFIG_9BBS_MAX_MESSAGES_PER_ROOM
#define CONFIG_9BBS_MAX_MESSAGES_PER_ROOM 20
#endif

#ifndef CONFIG_9BBS_MAX_MESSAGE_SIZE
#define CONFIG_9BBS_MAX_MESSAGE_SIZE 2048
#endif

#ifndef CONFIG_9BBS_MAX_USERNAME_LEN
#define CONFIG_9BBS_MAX_USERNAME_LEN 32
#endif

#ifndef CONFIG_9BBS_MAX_PASSWORD_LEN
#define CONFIG_9BBS_MAX_PASSWORD_LEN 64
#endif

#ifndef CONFIG_9BBS_MAX_ROOMNAME_LEN
#define CONFIG_9BBS_MAX_ROOMNAME_LEN 32
#endif

/**
 * @brief BBS message
 */
struct bbs_message {
	uint32_t id;                                     /* Message number */
	char from[CONFIG_9BBS_MAX_USERNAME_LEN];         /* Author */
	char to[CONFIG_9BBS_MAX_ROOMNAME_LEN];           /* Room name */
	char subject[128];                               /* Subject line */
	uint64_t date;                                   /* Unix timestamp */
	uint32_t reply_to;                               /* Reply to message # (0 = none) */
	char *body;                                      /* Message body (dynamically allocated) */
	size_t body_len;                                 /* Body length */
	char sig[128];                                   /* Signature */
	bool deleted;                                    /* Marked as deleted */
};

/**
 * @brief BBS room
 */
struct bbs_room {
	char name[CONFIG_9BBS_MAX_ROOMNAME_LEN];
	struct bbs_message messages[CONFIG_9BBS_MAX_MESSAGES_PER_ROOM];
	uint32_t message_count;
	uint32_t next_message_id;
	bool active;
};

/**
 * @brief BBS user read position in a room
 */
struct bbs_user_room {
	char room[CONFIG_9BBS_MAX_ROOMNAME_LEN];
	uint32_t last_read;                              /* Last read message ID */
};

/**
 * @brief BBS user
 */
struct bbs_user {
	char username[CONFIG_9BBS_MAX_USERNAME_LEN];
	char password[CONFIG_9BBS_MAX_PASSWORD_LEN];
	char sig[128];
	char current_room[CONFIG_9BBS_MAX_ROOMNAME_LEN];
	struct bbs_user_room rooms[CONFIG_9BBS_MAX_ROOMS];
	uint32_t room_count;
	bool active;
};

/**
 * @brief BBS instance
 */
struct bbs_instance {
	struct bbs_room rooms[CONFIG_9BBS_MAX_ROOMS];
	uint32_t room_count;

	struct bbs_user users[CONFIG_9BBS_MAX_USERS];
	uint32_t user_count;

	struct k_mutex lock;  /* Protects the entire BBS state */
};

/**
 * @brief Initialize a BBS instance
 *
 * @param bbs BBS instance to initialize
 * @return 0 on success, negative errno on failure
 */
int bbs_init(struct bbs_instance *bbs);

/**
 * @brief Create a BBS room
 *
 * @param bbs BBS instance
 * @param name Room name
 * @return 0 on success, negative errno on failure
 */
int bbs_create_room(struct bbs_instance *bbs, const char *name);

/**
 * @brief Create a BBS user
 *
 * @param bbs BBS instance
 * @param username Username
 * @param password Password
 * @return 0 on success, negative errno on failure
 */
int bbs_create_user(struct bbs_instance *bbs, const char *username,
                    const char *password);

/**
 * @brief Post a message to a room
 *
 * @param bbs BBS instance
 * @param room Room name
 * @param from Username
 * @param body Message body
 * @param reply_to Reply to message ID (0 for new thread)
 * @return Message ID on success, negative errno on failure
 */
int bbs_post_message(struct bbs_instance *bbs, const char *room,
                     const char *from, const char *body, uint32_t reply_to);

/**
 * @brief Get a message from a room
 *
 * @param bbs BBS instance
 * @param room Room name
 * @param msg_id Message ID
 * @return Pointer to message, or NULL if not found
 */
struct bbs_message *bbs_get_message(struct bbs_instance *bbs, const char *room,
                                    uint32_t msg_id);

/**
 * @brief Get BBS filesystem operations
 *
 * Returns the filesystem operations structure for the BBS.
 * Use this with ninep_server_init() to create a BBS server.
 *
 * @return Pointer to BBS filesystem operations
 */
const struct ninep_fs_ops *bbs_get_fs_ops(void);

/**
 * @brief Register BBS as a 9P server
 *
 * Creates a 9P server instance that exposes the BBS filesystem.
 * This server can then be mounted using ns_mount_server() or posted
 * to /srv using srv_post().
 *
 * @param bbs BBS instance
 * @return Server instance, or NULL on failure
 */
struct ninep_server *bbs_register_server(struct bbs_instance *bbs);

/**
 * @brief Unregister BBS server
 *
 * @param server Server instance (returned from bbs_register_server)
 */
void bbs_unregister_server(struct ninep_server *server);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_9BBS_H_ */
