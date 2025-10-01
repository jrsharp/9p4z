/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Interactive 9P client - connects to 9pserve and provides shell commands
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/9p/protocol.h>
#include <zephyr/9p/message.h>
#include <zephyr/9p/transport.h>
#include <zephyr/9p/transport_uart.h>
#include <zephyr/9p/fid.h>
#include <zephyr/9p/tag.h>
#include <zephyr/logging/log.h>
#include <zephyr/console/console.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(ninep_client, LOG_LEVEL_DBG);

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_console)
#define MAX_CMD_LEN 128
#define MAX_PATH_LEN 256

static uint8_t rx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t tx_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static uint8_t response_buffer[CONFIG_NINEP_MAX_MESSAGE_SIZE];
static struct ninep_transport g_transport;
static struct ninep_fid_table fid_table;
static struct ninep_tag_table tag_table;

/* Client state */
static bool connected = false;
static bool response_ready = false;
static size_t response_len = 0;
static uint32_t root_fid = NINEP_NOFID;
static uint32_t cwd_fid = NINEP_NOFID;
static char cwd_path[MAX_PATH_LEN] = "/";

/* Synchronization */
static K_SEM_DEFINE(response_sem, 0, 1);

/* Helper to extract uint32 from message */
static uint32_t get_u32(const uint8_t *buf, size_t offset)
{
	return buf[offset] |
	       (buf[offset + 1] << 8) |
	       (buf[offset + 2] << 16) |
	       (buf[offset + 3] << 24);
}

/* Helper to extract uint16 from message */
static uint16_t get_u16(const uint8_t *buf, size_t offset)
{
	return buf[offset] | (buf[offset + 1] << 8);
}

/* Transport receive callback */
static void message_received(struct ninep_transport *transport,
                             const uint8_t *buf, size_t len,
                             void *user_data)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(user_data);

	if (len > sizeof(response_buffer)) {
		LOG_ERR("Response too large: %d bytes", len);
		return;
	}

	memcpy(response_buffer, buf, len);
	response_len = len;
	response_ready = true;

	k_sem_give(&response_sem);
}

/* Send request and wait for response */
static int send_and_wait(const uint8_t *req, size_t req_len, uint32_t timeout_ms)
{
	int ret;

	response_ready = false;
	response_len = 0;

	ret = ninep_transport_send(&g_transport, req, req_len);
	if (ret < 0) {
		LOG_ERR("Failed to send request: %d", ret);
		return ret;
	}

	ret = k_sem_take(&response_sem, K_MSEC(timeout_ms));
	if (ret < 0) {
		LOG_ERR("Timeout waiting for response");
		return -ETIMEDOUT;
	}

	if (!response_ready) {
		LOG_ERR("Response not ready");
		return -EIO;
	}

	return 0;
}

/* Perform version negotiation */
static int do_version(void)
{
	int ret;
	uint16_t tag;
	struct ninep_msg_header hdr;

	LOG_INF("Negotiating protocol version...");

	/* Allocate tag */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		LOG_ERR("Failed to allocate tag");
		return -ENOMEM;
	}

	/* Build Tversion */
	ret = ninep_build_tversion(tx_buffer, sizeof(tx_buffer), tag,
	                            CONFIG_NINEP_MAX_MESSAGE_SIZE, "9P2000", 6);
	if (ret < 0) {
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Failed to build Tversion");
		return ret;
	}

	/* Send and wait */
	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_tag_free(&tag_table, tag);
		return ret;
	}

	/* Parse response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Failed to parse response header");
		return ret;
	}

	if (hdr.type != NINEP_RVERSION || hdr.tag != tag) {
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Unexpected response: type=%d, tag=%d", hdr.type, hdr.tag);
		return -EPROTO;
	}

	ninep_tag_free(&tag_table, tag);

	/* Parse version response */
	uint32_t msize = get_u32(response_buffer, 7);
	uint16_t version_len = get_u16(response_buffer, 11);

	LOG_INF("Version negotiated: msize=%u, version=%.*s",
	        msize, version_len, &response_buffer[13]);

	return 0;
}

/* Attach to filesystem root */
static int do_attach(const char *aname, const char *uname)
{
	int ret;
	uint16_t tag;
	struct ninep_msg_header hdr;

	LOG_INF("Attaching to filesystem: aname=%s, uname=%s", aname, uname);

	/* Allocate tag and FID */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		LOG_ERR("Failed to allocate tag");
		return -ENOMEM;
	}

	/* Use FID 0 for root */
	root_fid = 0;
	if (ninep_fid_alloc(&fid_table, root_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Failed to allocate root FID");
		return -ENOMEM;
	}

	/* Build Tattach */
	uint16_t uname_len = strlen(uname);
	uint16_t aname_len = strlen(aname);

	ret = ninep_build_tattach(tx_buffer, sizeof(tx_buffer), tag,
	                          root_fid, NINEP_NOFID,
	                          uname, uname_len, aname, aname_len);
	if (ret < 0) {
		ninep_fid_free(&fid_table, root_fid);
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Failed to build Tattach");
		return ret;
	}

	/* Send and wait */
	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, root_fid);
		ninep_tag_free(&tag_table, tag);
		return ret;
	}

	/* Parse response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		ninep_fid_free(&fid_table, root_fid);
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Failed to parse response header");
		return ret;
	}

	if (hdr.type != NINEP_RATTACH || hdr.tag != tag) {
		ninep_fid_free(&fid_table, root_fid);
		ninep_tag_free(&tag_table, tag);
		LOG_ERR("Unexpected response: type=%d, tag=%d", hdr.type, hdr.tag);
		return -EPROTO;
	}

	ninep_tag_free(&tag_table, tag);

	/* Current directory is root */
	cwd_fid = root_fid;
	strcpy(cwd_path, "/");

	LOG_INF("Attached successfully, root FID=%u", root_fid);
	connected = true;

	return 0;
}

/* Command: help */
static void cmd_help(void)
{
	printk("\n9P Client Commands:\n");
	printk("  help          - Show this help\n");
	printk("  connect       - Connect to 9P server\n");
	printk("  pwd           - Print working directory\n");
	printk("  ls [path]     - List directory (basic walk test)\n");
	printk("  quit          - Exit client\n");
	printk("\nComing soon: cd, cat, stat, full ls\n\n");
}

/* Command: pwd */
static void cmd_pwd(void)
{
	printk("%s\n", cwd_path);
}

/* Command: connect */
static void cmd_connect(void)
{
	int ret;

	if (connected) {
		printk("Already connected\n");
		return;
	}

	ret = do_version();
	if (ret < 0) {
		printk("Version negotiation failed: %d\n", ret);
		return;
	}

	ret = do_attach("/", "zephyr");
	if (ret < 0) {
		printk("Attach failed: %d\n", ret);
		return;
	}

	printk("Connected successfully\n");
}

/* Command: ls - list directory contents */
static void cmd_ls(const char *path)
{
	int ret;
	uint16_t tag;
	uint32_t walk_fid;
	struct ninep_msg_header hdr;
	const char *use_path;
	char path_buf[MAX_PATH_LEN];

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	/* Use current directory if no path given */
	if (path == NULL || path[0] == '\0') {
		use_path = ".";
	} else {
		use_path = path;
	}

	printk("Listing: %s\n", use_path);

	/* Allocate tag and FID for the walk */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	walk_fid = 1; /* Use FID 1 for walk operations */
	if (ninep_fid_alloc(&fid_table, walk_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate walk FID\n");
		return;
	}

	/* Build Twalk message - walk from root to target path */
	/* For now, just walk to "." (current directory) */
	const char *wnames[1] = {"."};
	uint16_t wname_lens[1] = {1};

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         root_fid, walk_fid,
	                         use_path[0] == '.' && use_path[1] == '\0' ? 0 : 1,
	                         wnames, wname_lens);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Twalk: %d\n", ret);
		return;
	}

	/* Send and wait */
	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk request failed: %d\n", ret);
		return;
	}

	/* Parse response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to parse walk response\n");
		return;
	}

	if (hdr.type == NINEP_RERROR) {
		/* Parse error message */
		size_t offset = 7;
		const char *errstr;
		uint16_t errlen;
		ninep_parse_string(response_buffer, response_len, &offset, &errstr, &errlen);
		printk("Walk error: %.*s\n", errlen, errstr);
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		return;
	}

	if (hdr.type != NINEP_RWALK || hdr.tag != tag) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Unexpected walk response: type=%d, tag=%d\n", hdr.type, hdr.tag);
		return;
	}

	/* Parse Rwalk response */
	uint16_t nwqid = get_u16(response_buffer, 7);
	printk("Walk successful: %d qids returned\n", nwqid);

	/* TODO: Now we need to Topen the directory and Tread to get entries */
	printk("(Full directory listing not yet implemented)\n");

	/* Clean up */
	ninep_fid_free(&fid_table, walk_fid);
	ninep_tag_free(&tag_table, tag);
}

/* Parse and execute command */
static void execute_command(const char *line)
{
	char cmd[MAX_CMD_LEN];
	char arg[MAX_CMD_LEN];
	int n;

	/* Parse command and argument */
	n = sscanf(line, "%s %s", cmd, arg);
	if (n < 1) {
		return;
	}

	if (strcmp(cmd, "help") == 0) {
		cmd_help();
	} else if (strcmp(cmd, "pwd") == 0) {
		cmd_pwd();
	} else if (strcmp(cmd, "connect") == 0) {
		cmd_connect();
	} else if (strcmp(cmd, "ls") == 0) {
		cmd_ls(n > 1 ? arg : NULL);
	} else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
		printk("Goodbye!\n");
		k_sleep(K_MSEC(100));
		/* In real app, would exit cleanly */
	} else {
		printk("Unknown command: %s (type 'help' for commands)\n", cmd);
	}
}

/* Interactive shell loop */
static void shell_loop(void)
{
	char line[MAX_CMD_LEN];
	int pos = 0;
	int c;

	printk("\n9P Interactive Client\n");
	printk("Type 'help' for commands\n\n");

	while (1) {
		printk("9p> ");

		/* Read line */
		pos = 0;
		while (1) {
			c = console_getchar();

			if (c == '\r' || c == '\n') {
				printk("\n");
				line[pos] = '\0';
				break;
			} else if (c == '\b' || c == 0x7F) {  /* Backspace */
				if (pos > 0) {
					pos--;
					printk("\b \b");  /* Erase character */
				}
			} else if (c >= 32 && c < 127) {  /* Printable */
				if (pos < MAX_CMD_LEN - 1) {
					line[pos++] = c;
					printk("%c", c);
				}
			}
		}

		/* Execute command */
		if (pos > 0) {
			execute_command(line);
		}
	}
}

int main(void)
{
	const struct device *uart_dev;
	struct ninep_transport_uart_config uart_config;
	int ret;

	LOG_INF("=== 9P Interactive Client ===");

	/* Initialize FID and tag tables */
	ninep_fid_table_init(&fid_table);
	ninep_tag_table_init(&tag_table);

	/* Get UART device */
	uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -1;
	}

	/* Configure UART transport */
	uart_config.uart_dev = uart_dev;
	uart_config.rx_buf = rx_buffer;
	uart_config.rx_buf_size = sizeof(rx_buffer);

	/* Initialize transport */
	ret = ninep_transport_uart_init(&g_transport, &uart_config,
	                                message_received, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize UART transport: %d", ret);
		return -1;
	}

	/* Start transport */
	ret = ninep_transport_start(&g_transport);
	if (ret < 0) {
		LOG_ERR("Failed to start transport: %d", ret);
		return -1;
	}

	LOG_INF("Transport initialized");

	/* Initialize console */
	console_init();

	/* Wait a bit for things to settle */
	k_sleep(K_MSEC(500));

	/* Run shell */
	shell_loop();

	return 0;
}
