/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/9bbs/9bbs.h>
#include <zephyr/namespace/namespace.h>
#include <zephyr/namespace/srv.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* BBS instance (static allocation) */
static struct bbs_instance bbs;

void main(void)
{
	printk("\n");
	printk("==============================================\n");
	printk(" 9bbs Demo - Plan 9-style BBS with Namespaces\n");
	printk("==============================================\n");
	printk("\n");

	/* Initialize namespace support */
	int ret = ns_init();
	if (ret < 0) {
		printk("ERROR: Failed to initialize namespaces: %d\n", ret);
		return;
	}

	/* Create namespace for this thread */
	ret = ns_create(NULL);
	if (ret < 0) {
		printk("ERROR: Failed to create namespace: %d\n", ret);
		return;
	}

	/* Initialize BBS */
	ret = bbs_init(&bbs);
	if (ret < 0) {
		printk("ERROR: Failed to initialize BBS: %d\n", ret);
		return;
	}

	/* Create some demo users */
	ret = bbs_create_user(&bbs, "alice", "password123");
	if (ret < 0) {
		printk("ERROR: Failed to create user alice: %d\n", ret);
		return;
	}

	ret = bbs_create_user(&bbs, "bob", "password456");
	if (ret < 0) {
		printk("ERROR: Failed to create user bob: %d\n", ret);
		return;
	}

	/* Create a "tech" room */
	ret = bbs_create_room(&bbs, "tech");
	if (ret < 0) {
		printk("ERROR: Failed to create tech room: %d\n", ret);
		return;
	}

	/* Register BBS as a 9P server */
	struct ninep_server *bbs_server = bbs_register_server(&bbs);
	if (!bbs_server) {
		printk("ERROR: Failed to register BBS server\n");
		return;
	}

	/* Mount BBS via namespaces */
	ret = ns_mount_server(bbs_server, "/bbs", 0);
	if (ret < 0) {
		printk("ERROR: Failed to mount BBS: %d\n", ret);
		return;
	}

	printk("\n--- BBS mounted at /bbs ---\n\n");

	/* Post some demo messages */
	ret = bbs_post_message(&bbs, "lobby", "alice",
	                       "Hello, this is the first message!", 0);
	if (ret < 0) {
		printk("ERROR: Failed to post message: %d\n", ret);
		return;
	}

	ret = bbs_post_message(&bbs, "lobby", "bob",
	                       "Hi Alice! Welcome to the BBS.", 0);
	if (ret < 0) {
		printk("ERROR: Failed to post message: %d\n", ret);
		return;
	}

	ret = bbs_post_message(&bbs, "tech", "alice",
	                       "Discussing embedded systems here!", 0);
	if (ret < 0) {
		printk("ERROR: Failed to post message: %d\n", ret);
		return;
	}

	printk("\n--- Demo messages posted ---\n\n");

	/* Read messages through the filesystem interface */
	printk("Reading /bbs/rooms/lobby/1:\n");
	printk("-----------------------------\n");
	int fd = ns_open("/bbs/rooms/lobby/1", FS_O_READ);
	if (fd >= 0) {
		char buf[512];
		ssize_t bytes = ns_read(fd, buf, sizeof(buf) - 1);
		if (bytes > 0) {
			buf[bytes] = '\0';
			printk("%s\n", buf);
		} else {
			printk("ERROR: Read failed: %zd\n", bytes);
		}
		ns_close(fd);
	} else {
		printk("ERROR: Failed to open message: %d\n", fd);
	}

	printk("\nReading /bbs/rooms/lobby/2:\n");
	printk("-----------------------------\n");
	fd = ns_open("/bbs/rooms/lobby/2", FS_O_READ);
	if (fd >= 0) {
		char buf[512];
		ssize_t bytes = ns_read(fd, buf, sizeof(buf) - 1);
		if (bytes > 0) {
			buf[bytes] = '\0';
			printk("%s\n", buf);
		} else {
			printk("ERROR: Read failed: %zd\n", bytes);
		}
		ns_close(fd);
	} else {
		printk("ERROR: Failed to open message: %d\n", fd);
	}

	printk("\nReading /bbs/etc/roomlist:\n");
	printk("-----------------------------\n");
	fd = ns_open("/bbs/etc/roomlist", FS_O_READ);
	if (fd >= 0) {
		char buf[256];
		ssize_t bytes = ns_read(fd, buf, sizeof(buf) - 1);
		if (bytes > 0) {
			buf[bytes] = '\0';
			printk("%s\n", buf);
		} else {
			printk("ERROR: Read failed: %zd\n", bytes);
		}
		ns_close(fd);
	} else {
		printk("ERROR: Failed to open roomlist: %d\n", fd);
	}

	printk("\n");
	printk("==============================================\n");
	printk(" 9bbs Demo Complete!\n");
	printk("==============================================\n");
	printk("\n");

	printk("The BBS is now accessible at /bbs\n");
	printk("It could also be exported over:\n");
	printk("  - Bluetooth L2CAP (for mobile apps)\n");
	printk("  - TCP/IP (for network access)\n");
	printk("  - CoAP (for IoT platforms)\n");
	printk("\n");

	/* Keep running */
	while (1) {
		k_sleep(K_SECONDS(10));
	}
}
