/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
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

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_ninep_uart)
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

/* Forward declarations */
static int send_and_wait(const uint8_t *req, size_t req_len, uint32_t timeout_ms);

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

/* Helper to print 9P error messages */
static void print_9p_error(const char *operation)
{
	size_t offset = 7;
	const char *errstr;
	uint16_t errlen;

	if (ninep_parse_string(response_buffer, response_len, &offset, &errstr, &errlen) == 0) {
		printk("%s error: %.*s\n", operation, errlen, errstr);
	} else {
		printk("%s error: (unable to parse error message)\n", operation);
	}
}

/* Helper to clunk a FID */
static int do_clunk(uint32_t fid)
{
	int ret;
	uint16_t tag;
	struct ninep_msg_header hdr;

	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		return -ENOMEM;
	}

	ret = ninep_build_tclunk(tx_buffer, sizeof(tx_buffer), tag, fid);
	if (ret < 0) {
		ninep_tag_free(&tag_table, tag);
		return ret;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_tag_free(&tag_table, tag);
		return ret;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	ninep_tag_free(&tag_table, tag);

	if (ret < 0 || hdr.type == NINEP_RERROR) {
		return -EIO;
	}

	ninep_fid_free(&fid_table, fid);
	return 0;
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
	struct ninep_msg_header hdr;

	LOG_INF("Negotiating protocol version...");

	/* Build Tversion - uses NOTAG per 9P spec */
	ret = ninep_build_tversion(tx_buffer, sizeof(tx_buffer), NINEP_NOTAG,
	                            CONFIG_NINEP_MAX_MESSAGE_SIZE, "9P2000", 6);
	if (ret < 0) {
		LOG_ERR("Failed to build Tversion");
		return ret;
	}

	/* Send and wait */
	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		return ret;
	}

	/* Parse response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		LOG_ERR("Failed to parse response header");
		return ret;
	}

	if (hdr.type != NINEP_RVERSION || hdr.tag != NINEP_NOTAG) {
		LOG_ERR("Unexpected response: type=%d, tag=%d", hdr.type, hdr.tag);
		return -EPROTO;
	}

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
	printk("  help            - Show this help\n");
	printk("  connect         - Connect to 9P server\n");
	printk("  pwd             - Print working directory\n");
	printk("  ls [path]       - List directory\n");
	printk("  cd <path>       - Change directory\n");
	printk("  cat <file>      - Display file contents\n");
	printk("  stat <path>     - Show file information\n");
	printk("  echo <text> <file> - Write text to file\n");
	printk("  touch <file>    - Create empty file\n");
	printk("  mkdir <dir>     - Create directory\n");
	printk("  rm <path>       - Delete file/directory\n");
	printk("  quit            - Exit client\n\n");
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

	ninep_tag_free(&tag_table, tag);

	/* Open the directory for reading */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		ninep_fid_free(&fid_table, walk_fid);
		printk("Failed to allocate tag for open\n");
		return;
	}

	ret = ninep_build_topen(tx_buffer, sizeof(tx_buffer), tag,
	                         walk_fid, NINEP_OREAD);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Topen: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Open request failed: %d\n", ret);
		return;
	}

	/* Parse open response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Open failed\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Read directory entries */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		ninep_fid_free(&fid_table, walk_fid);
		printk("Failed to allocate tag for read\n");
		return;
	}

	ret = ninep_build_tread(tx_buffer, sizeof(tx_buffer), tag,
	                         walk_fid, 0, 8192);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Tread: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Read request failed: %d\n", ret);
		return;
	}

	/* Parse read response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to parse read response\n");
		return;
	}

	if (hdr.type == NINEP_RERROR) {
		size_t offset = 7;
		const char *errstr;
		uint16_t errlen;
		ninep_parse_string(response_buffer, response_len, &offset, &errstr, &errlen);
		printk("Read error: %.*s\n", errlen, errstr);
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		return;
	}

	if (hdr.type == NINEP_RREAD) {
		/* Parse directory entries */
		uint32_t count = get_u32(response_buffer, 7);
		size_t offset = 11;  /* Data starts after size[4] + type[1] + tag[2] + count[4] */

		if (count == 0) {
			printk("(empty directory)\n");
		} else {
			/* Parse stat structures */
			while (offset < 11 + count) {
				/* Each stat has: size[2] + stat_data */
				uint16_t stat_size = get_u16(response_buffer, offset);
				size_t stat_start = offset + 2;

				/* Skip: type[2] dev[4] qid[13] mode[4] atime[4] mtime[4] length[8] */
				/* Note: stat_start already points past size[2] */
				size_t name_offset = stat_start + 2 + 4 + 13 + 4 + 4 + 4 + 8;

				/* Parse name string */
				const char *name;
				uint16_t name_len;
				if (ninep_parse_string(response_buffer, response_len, &name_offset, &name, &name_len) == 0) {
					/* Check if it's a directory (from qid type) */
					/* stat_start points to type[2], skip type[2]+dev[4] to get to qid */
					uint8_t qid_type = response_buffer[stat_start + 2 + 4];
					const char *type_indicator = (qid_type & NINEP_QTDIR) ? "/" : "";
					printk("  %.*s%s\n", name_len, name, type_indicator);
				}

				/* Move to next stat */
				offset = offset + 2 + stat_size;
			}
		}
	}

	/* Clean up */
	ninep_tag_free(&tag_table, tag);
	do_clunk(walk_fid);
}

/* Command: cd - change directory */
static void cmd_cd(const char *path)
{
	int ret;
	uint16_t tag;
	uint32_t walk_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (path == NULL || path[0] == '\0') {
		printk("Usage: cd <path>\n");
		return;
	}

	/* Allocate tag and FID for the walk */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	walk_fid = 2; /* Use FID 2 for cd operations */
	if (ninep_fid_alloc(&fid_table, walk_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate walk FID\n");
		return;
	}

	/* Build Twalk message to target path */
	const char *wnames[1] = {path};
	uint16_t wname_lens[1] = {strlen(path)};

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, walk_fid, 1, wnames, wname_lens);
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
		printk("cd error: %.*s\n", errlen, errstr);
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

	/* Update current directory */
	if (cwd_fid != root_fid) {
		do_clunk(cwd_fid);
	}
	cwd_fid = walk_fid;

	/* Update path string */
	if (strcmp(path, "..") == 0) {
		/* Go up one level */
		char *last_slash = strrchr(cwd_path, '/');
		if (last_slash != NULL && last_slash != cwd_path) {
			*last_slash = '\0';
		} else {
			strcpy(cwd_path, "/");
		}
	} else if (strcmp(cwd_path, "/") == 0) {
		snprintf(cwd_path, MAX_PATH_LEN, "/%s", path);
	} else {
		snprintf(cwd_path, MAX_PATH_LEN, "%s/%s", cwd_path, path);
	}

	ninep_tag_free(&tag_table, tag);
}

/* Command: cat - display file contents */
static void cmd_cat(const char *path)
{
	int ret;
	uint16_t tag;
	uint32_t walk_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (path == NULL || path[0] == '\0') {
		printk("Usage: cat <file>\n");
		return;
	}

	/* Allocate tag and FID for the walk */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	walk_fid = 3; /* Use FID 3 for cat operations */
	if (ninep_fid_alloc(&fid_table, walk_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate walk FID\n");
		return;
	}

	/* Build Twalk message to file */
	const char *wnames[1] = {path};
	uint16_t wname_lens[1] = {strlen(path)};

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, walk_fid, 1, wnames, wname_lens);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Twalk: %d\n", ret);
		return;
	}

	/* Send walk and wait */
	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk request failed: %d\n", ret);
		return;
	}

	/* Parse walk response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk to file failed\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Open the file for reading */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		ninep_fid_free(&fid_table, walk_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_topen(tx_buffer, sizeof(tx_buffer), tag,
	                         walk_fid, NINEP_OREAD);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Topen: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Open request failed\n");
		return;
	}

	/* Parse open response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Open file failed\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Read file contents */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		ninep_fid_free(&fid_table, walk_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_tread(tx_buffer, sizeof(tx_buffer), tag,
	                         walk_fid, 0, 8192);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Tread: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Read request failed\n");
		return;
	}

	/* Parse read response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to parse read response\n");
		return;
	}

	if (hdr.type == NINEP_RERROR) {
		size_t offset = 7;
		const char *errstr;
		uint16_t errlen;
		ninep_parse_string(response_buffer, response_len, &offset, &errstr, &errlen);
		printk("Read error: %.*s\n", errlen, errstr);
	} else if (hdr.type == NINEP_RREAD) {
		/* Data starts at offset 7 + 4 (count) */
		uint32_t count = get_u32(response_buffer, 7);
		if (count > 0) {
			printk("%.*s", (int)count, &response_buffer[11]);
		}
	}

	/* Clean up */
	ninep_tag_free(&tag_table, tag);
	do_clunk(walk_fid);
}

/* Command: stat - display file information */
static void cmd_stat(const char *path)
{
	int ret;
	uint16_t tag;
	uint32_t walk_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (path == NULL || path[0] == '\0') {
		printk("Usage: stat <path>\n");
		return;
	}

	/* Allocate tag and FID for the walk */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	walk_fid = 4; /* Use FID 4 for stat operations */
	if (ninep_fid_alloc(&fid_table, walk_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate walk FID\n");
		return;
	}

	/* Build Twalk message to file */
	const char *wnames[1] = {path};
	uint16_t wname_lens[1] = {strlen(path)};

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, walk_fid, 1, wnames, wname_lens);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Twalk: %d\n", ret);
		return;
	}

	/* Send walk and wait */
	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk request failed: %d\n", ret);
		return;
	}

	/* Parse walk response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk to path failed\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Send Tstat */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		ninep_fid_free(&fid_table, walk_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_tstat(tx_buffer, sizeof(tx_buffer), tag, walk_fid);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Tstat: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Stat request failed\n");
		return;
	}

	/* Parse stat response */
	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to parse stat response\n");
		return;
	}

	if (hdr.type == NINEP_RERROR) {
		size_t offset = 7;
		const char *errstr;
		uint16_t errlen;
		ninep_parse_string(response_buffer, response_len, &offset, &errstr, &errlen);
		printk("Stat error: %.*s\n", errlen, errstr);
	} else if (hdr.type == NINEP_RSTAT) {
		/* Parse stat structure - simplified output */
		/* Stat format: size[2] type[2] dev[4] qid[13] mode[4]
		   atime[4] mtime[4] length[8] name[s] uid[s] gid[s] muid[s] */
		size_t offset = 7;
		uint16_t stat_size = get_u16(response_buffer, offset);
		offset += 2;  /* Now pointing at type[2] */

		/* Skip to qid (skip type + dev) */
		offset += 2 + 4;

		/* Parse qid */
		uint8_t qid_type = response_buffer[offset++];
		offset += 4; /* skip version */
		uint64_t qid_path = get_u32(response_buffer, offset);
		offset += 8;

		/* Parse mode and length */
		uint32_t mode = get_u32(response_buffer, offset);
		offset += 4 + 4 + 4; /* skip atime, mtime */
		uint64_t length = (uint64_t)get_u32(response_buffer, offset) |
		                  ((uint64_t)get_u32(response_buffer, offset + 4) << 32);
		offset += 8;

		/* Parse name */
		const char *name;
		uint16_t name_len;
		ninep_parse_string(response_buffer, response_len, &offset, &name, &name_len);

		printk("File: %.*s\n", name_len, name);
		printk("Type: %s\n", (qid_type & NINEP_QTDIR) ? "directory" : "file");
		printk("Mode: 0x%08x\n", mode);
		printk("Size: %llu bytes\n", length);
	}

	/* Clean up */
	ninep_tag_free(&tag_table, tag);
	do_clunk(walk_fid);
}

/* Command: echo - write text to file */
static void cmd_echo(const char *text, const char *file)
{
	int ret;
	uint16_t tag;
	uint32_t walk_fid, open_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (!text || !file || file[0] == '\0') {
		printk("Usage: echo <text> <file>\n");
		return;
	}

	/* Walk to file (or parent if creating) */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	walk_fid = 5;
	if (ninep_fid_alloc(&fid_table, walk_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate FID\n");
		return;
	}

	const char *wnames[1] = {file};
	uint16_t wname_lens[1] = {strlen(file)};

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, walk_fid, 1, wnames, wname_lens);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Twalk: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk failed - file may not exist\n");
		return;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("File not found\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Open for writing */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		do_clunk(walk_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_topen(tx_buffer, sizeof(tx_buffer), tag,
	                         walk_fid, NINEP_OWRITE | NINEP_OTRUNC);
	if (ret < 0) {
		do_clunk(walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Topen: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0 || (ninep_parse_header(response_buffer, response_len, &hdr) < 0) ||
	    hdr.type == NINEP_RERROR) {
		do_clunk(walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to open file for writing\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Write data */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		do_clunk(walk_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	size_t text_len = strlen(text);
	ret = ninep_build_twrite(tx_buffer, sizeof(tx_buffer), tag,
	                          walk_fid, 0, text_len, (const uint8_t *)text);
	if (ret < 0) {
		do_clunk(walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Twrite: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		do_clunk(walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Write failed\n");
		return;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		print_9p_error("Write");
		ninep_tag_free(&tag_table, tag);
		do_clunk(walk_fid);
		return;
	}

	/* Get bytes written */
	uint32_t count = get_u32(response_buffer, 7);
	printk("Wrote %u bytes\n", count);

	ninep_tag_free(&tag_table, tag);
	do_clunk(walk_fid);
}

/* Command: touch - create empty file */
static void cmd_touch(const char *file)
{
	int ret;
	uint16_t tag;
	uint32_t dir_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (!file || file[0] == '\0') {
		printk("Usage: touch <file>\n");
		return;
	}

	/* Clone current directory FID for create */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	dir_fid = 6;
	if (ninep_fid_alloc(&fid_table, dir_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate FID\n");
		return;
	}

	/* Walk to current dir (clone cwd_fid) */
	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, dir_fid, 0, NULL, NULL);
	if (ret < 0 || send_and_wait(tx_buffer, ret, 5000) < 0) {
		ninep_fid_free(&fid_table, dir_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to clone directory FID\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Create file */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		do_clunk(dir_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_tcreate(tx_buffer, sizeof(tx_buffer), tag,
	                           dir_fid, file, strlen(file),
	                           0644, NINEP_OWRITE);
	if (ret < 0) {
		do_clunk(dir_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Tcreate: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		do_clunk(dir_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Create failed\n");
		return;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		print_9p_error("Create");
		ninep_tag_free(&tag_table, tag);
		do_clunk(dir_fid);
		return;
	}

	printk("Created: %s\n", file);
	ninep_tag_free(&tag_table, tag);
	do_clunk(dir_fid);
}

/* Command: mkdir - create directory */
static void cmd_mkdir(const char *dir)
{
	int ret;
	uint16_t tag;
	uint32_t dir_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (!dir || dir[0] == '\0') {
		printk("Usage: mkdir <dir>\n");
		return;
	}

	/* Clone current directory FID */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	dir_fid = 7;
	if (ninep_fid_alloc(&fid_table, dir_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate FID\n");
		return;
	}

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, dir_fid, 0, NULL, NULL);
	if (ret < 0 || send_and_wait(tx_buffer, ret, 5000) < 0) {
		ninep_fid_free(&fid_table, dir_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to clone directory FID\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Create directory with DMDIR flag */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		do_clunk(dir_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_tcreate(tx_buffer, sizeof(tx_buffer), tag,
	                           dir_fid, dir, strlen(dir),
	                           NINEP_DMDIR | 0755, NINEP_OREAD);
	if (ret < 0) {
		do_clunk(dir_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Tcreate: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		do_clunk(dir_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Create directory failed\n");
		return;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		print_9p_error("Create directory");
		ninep_tag_free(&tag_table, tag);
		do_clunk(dir_fid);
		return;
	}

	printk("Created directory: %s\n", dir);
	ninep_tag_free(&tag_table, tag);
	do_clunk(dir_fid);
}

/* Command: rm - remove file or directory */
static void cmd_rm(const char *path)
{
	int ret;
	uint16_t tag;
	uint32_t walk_fid;
	struct ninep_msg_header hdr;

	if (!connected) {
		printk("Not connected. Use 'connect' first.\n");
		return;
	}

	if (!path || path[0] == '\0') {
		printk("Usage: rm <path>\n");
		return;
	}

	/* Walk to target */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		printk("Failed to allocate tag\n");
		return;
	}

	walk_fid = 8;
	if (ninep_fid_alloc(&fid_table, walk_fid) == NULL) {
		ninep_tag_free(&tag_table, tag);
		printk("Failed to allocate FID\n");
		return;
	}

	const char *wnames[1] = {path};
	uint16_t wname_lens[1] = {strlen(path)};

	ret = ninep_build_twalk(tx_buffer, sizeof(tx_buffer), tag,
	                         cwd_fid, walk_fid, 1, wnames, wname_lens);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Twalk: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Walk failed\n");
		return;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("File not found\n");
		return;
	}

	ninep_tag_free(&tag_table, tag);

	/* Remove */
	tag = ninep_tag_alloc(&tag_table);
	if (tag == NINEP_NOTAG) {
		ninep_fid_free(&fid_table, walk_fid);
		printk("Failed to allocate tag\n");
		return;
	}

	ret = ninep_build_tremove(tx_buffer, sizeof(tx_buffer), tag, walk_fid);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Failed to build Tremove: %d\n", ret);
		return;
	}

	ret = send_and_wait(tx_buffer, ret, 5000);
	if (ret < 0) {
		ninep_fid_free(&fid_table, walk_fid);
		ninep_tag_free(&tag_table, tag);
		printk("Remove failed\n");
		return;
	}

	ret = ninep_parse_header(response_buffer, response_len, &hdr);
	if (ret < 0 || hdr.type == NINEP_RERROR) {
		print_9p_error("Remove");
		ninep_tag_free(&tag_table, tag);
		ninep_fid_free(&fid_table, walk_fid);
		return;
	}

	printk("Removed: %s\n", path);
	ninep_tag_free(&tag_table, tag);
	/* Note: Tremove automatically clunks the FID, so just free from table */
	ninep_fid_free(&fid_table, walk_fid);
}

/* Parse and execute command */
static void execute_command(const char *line)
{
	char cmd[MAX_CMD_LEN];
	char arg1[MAX_CMD_LEN];
	char arg2[MAX_CMD_LEN];
	int n;

	/* Parse command and arguments */
	n = sscanf(line, "%s %s %s", cmd, arg1, arg2);
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
		cmd_ls(n > 1 ? arg1 : NULL);
	} else if (strcmp(cmd, "cd") == 0) {
		cmd_cd(n > 1 ? arg1 : NULL);
	} else if (strcmp(cmd, "cat") == 0) {
		cmd_cat(n > 1 ? arg1 : NULL);
	} else if (strcmp(cmd, "stat") == 0) {
		cmd_stat(n > 1 ? arg1 : NULL);
	} else if (strcmp(cmd, "echo") == 0) {
		if (n >= 3) {
			cmd_echo(arg1, arg2);
		} else {
			printk("Usage: echo <text> <file>\n");
		}
	} else if (strcmp(cmd, "touch") == 0) {
		cmd_touch(n > 1 ? arg1 : NULL);
	} else if (strcmp(cmd, "mkdir") == 0) {
		cmd_mkdir(n > 1 ? arg1 : NULL);
	} else if (strcmp(cmd, "rm") == 0) {
		cmd_rm(n > 1 ? arg1 : NULL);
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
