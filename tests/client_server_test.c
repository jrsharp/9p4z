/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Client-Server Integration Tests
 */

#include <zephyr/ztest.h>
#include <zephyr/9p/client.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/transport.h>
#include <string.h>

/* Mock transport for loopback testing */
struct mock_transport {
	struct ninep_transport base;
	uint8_t buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];
	size_t len;
	struct ninep_transport *peer;
};

static struct mock_transport client_transport;
static struct mock_transport server_transport;
static struct ninep_client client;
static struct ninep_server server;
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[32];

/* Mock transport send - delivers directly to peer */
static int mock_send(struct ninep_transport *transport, const uint8_t *buf, size_t len)
{
	struct mock_transport *mock = CONTAINER_OF(transport, struct mock_transport, base);
	struct mock_transport *peer = CONTAINER_OF(mock->peer, struct mock_transport, base);

	/* Copy to peer's buffer */
	memcpy(peer->buf, buf, len);
	peer->len = len;

	/* Deliver to peer's callback */
	if (peer->base.recv_cb) {
		peer->base.recv_cb(&peer->base, peer->buf, peer->len, peer->base.user_data);
	}

	return 0;
}

static int mock_start(struct ninep_transport *transport)
{
	return 0;
}

static int mock_stop(struct ninep_transport *transport)
{
	return 0;
}

static const struct ninep_transport_ops mock_ops = {
	.send = mock_send,
	.start = mock_start,
	.stop = mock_stop,
};

/* Static content for test files */
static const char *hello_content = "Hello from 9P!\n";
static const char *data_content = "\x01\x02\x03\x04";
static const char *nested_content = "Nested file\n";

/* Generator for static content */
static int gen_static(uint8_t *buf, size_t buf_size,
                      uint64_t offset, void *ctx)
{
	const char *content = (const char *)ctx;
	size_t len = strlen(content);

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = len - offset;
	if (to_copy > buf_size) {
		to_copy = buf_size;
	}

	memcpy(buf, content + offset, to_copy);
	return to_copy;
}

/* Generator for binary data (data.bin) */
static int gen_data_bin(uint8_t *buf, size_t buf_size,
                        uint64_t offset, void *ctx)
{
	const uint8_t *content = (const uint8_t *)ctx;
	size_t len = 4; /* data_content is 4 bytes */

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = len - offset;
	if (to_copy > buf_size) {
		to_copy = buf_size;
	}

	memcpy(buf, content + offset, to_copy);
	return to_copy;
}

/* Test setup */
static void *client_server_setup(void)
{
	int ret;

	/* Initialize mock transports */
	memset(&client_transport, 0, sizeof(client_transport));
	memset(&server_transport, 0, sizeof(server_transport));

	client_transport.base.ops = &mock_ops;
	client_transport.peer = &server_transport.base;

	server_transport.base.ops = &mock_ops;
	server_transport.peer = &client_transport.base;

	/* Setup server filesystem */
	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	zassert_equal(ret, 0, "Failed to init sysfs");

	/* Add test files */
	ninep_sysfs_register_file(&sysfs, "/hello.txt", gen_static,
	                           (void *)hello_content);
	ninep_sysfs_register_file(&sysfs, "/data.bin", gen_data_bin,
	                           (void *)data_content);

	ninep_sysfs_register_dir(&sysfs, "/testdir");
	ninep_sysfs_register_file(&sysfs, "/testdir/nested.txt", gen_static,
	                           (void *)nested_content);

	/* Initialize server */
	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &server_config, &server_transport.base);
	zassert_equal(ret, 0, "Failed to init server");

	ret = ninep_server_start(&server);
	zassert_equal(ret, 0, "Failed to start server");

	/* Initialize client */
	struct ninep_client_config client_config = {
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
		.timeout_ms = 1000,
	};

	ret = ninep_client_init(&client, &client_config, &client_transport.base);
	zassert_equal(ret, 0, "Failed to init client");

	return NULL;
}

static void client_server_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	ninep_server_stop(&server);
}

/* Test: Version negotiation */
ZTEST(client_server, test_version)
{
	int ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version negotiation failed");
	zassert_true(client.msize > 0, "Invalid msize");
	zassert_true(client.msize <= CONFIG_NINEP_MAX_MESSAGE_SIZE, "msize too large");
}

/* Test: Attach to root */
ZTEST(client_server, test_attach)
{
	uint32_t root_fid;

	int ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");
	zassert_true(root_fid != NINEP_NOFID, "Invalid root FID");

	/* Cleanup */
	ninep_client_clunk(&client, root_fid);
}

/* Test: Walk to file */
ZTEST(client_server, test_walk)
{
	uint32_t root_fid, file_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "hello.txt");
	zassert_equal(ret, 0, "Walk failed");
	zassert_true(file_fid != NINEP_NOFID, "Invalid file FID");

	/* Cleanup */
	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Walk to nested file */
ZTEST(client_server, test_walk_nested)
{
	uint32_t root_fid, file_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "testdir/nested.txt");
	zassert_equal(ret, 0, "Walk to nested file failed");
	zassert_true(file_fid != NINEP_NOFID, "Invalid file FID");

	/* Cleanup */
	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Open and read file */
ZTEST(client_server, test_read_file)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[64];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "hello.txt");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open failed");

	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_true(ret > 0, "Read failed");
	zassert_mem_equal(buf, "Hello from 9P!\n", 16, "Data mismatch");

	/* Cleanup */
	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Read binary data */
ZTEST(client_server, test_read_binary)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[64];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "data.bin");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open failed");

	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_equal(ret, 4, "Read size mismatch");
	zassert_mem_equal(buf, "\x01\x02\x03\x04", 4, "Binary data mismatch");

	/* Cleanup */
	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Write to file */
ZTEST(client_server, test_write_file)
{
	uint32_t root_fid, file_fid;
	uint8_t read_buf[64];
	const char *test_data = "Test write data";
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "hello.txt");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OWRITE | NINEP_OTRUNC);
	zassert_equal(ret, 0, "Open for write failed");

	ret = ninep_client_write(&client, file_fid, 0,
	                         (const uint8_t *)test_data, strlen(test_data));
	zassert_equal(ret, strlen(test_data), "Write failed");

	/* Close and reopen to read back */
	ninep_client_clunk(&client, file_fid);

	ret = ninep_client_walk(&client, root_fid, &file_fid, "hello.txt");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open for read failed");

	ret = ninep_client_read(&client, file_fid, 0, read_buf, sizeof(read_buf));
	zassert_equal(ret, strlen(test_data), "Read size mismatch");
	zassert_mem_equal(read_buf, test_data, strlen(test_data), "Read data mismatch");

	/* Cleanup */
	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Stat file */
ZTEST(client_server, test_stat)
{
	uint32_t root_fid, file_fid;
	struct ninep_stat stat;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "hello.txt");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_stat(&client, file_fid, &stat);
	zassert_equal(ret, 0, "Stat failed");

	/* Cleanup */
	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Create and remove file */
ZTEST(client_server, test_create_remove)
{
	uint32_t root_fid, dir_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	/* Walk to testdir */
	ret = ninep_client_walk(&client, root_fid, &dir_fid, "testdir");
	zassert_equal(ret, 0, "Walk to testdir failed");

	/* Create new file */
	ret = ninep_client_create(&client, dir_fid, "newfile.txt", 0644, NINEP_OWRITE);
	zassert_equal(ret, 0, "Create failed");

	/* Write to it */
	const char *data = "New file content";
	ret = ninep_client_write(&client, dir_fid, 0,
	                         (const uint8_t *)data, strlen(data));
	zassert_equal(ret, strlen(data), "Write to new file failed");

	/* Close */
	ninep_client_clunk(&client, dir_fid);

	/* Walk to new file to verify it exists */
	ret = ninep_client_walk(&client, root_fid, &dir_fid, "testdir/newfile.txt");
	zassert_equal(ret, 0, "Walk to new file failed");

	/* Remove it */
	ret = ninep_client_remove(&client, dir_fid);
	zassert_equal(ret, 0, "Remove failed");
	/* Note: remove auto-clunks the FID */

	/* Cleanup */
	ninep_client_clunk(&client, root_fid);
}

/* Test: Multiple concurrent operations */
ZTEST(client_server, test_concurrent_ops)
{
	uint32_t root_fid, fid1, fid2;
	uint8_t buf1[64], buf2[64];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	/* Open two files */
	ret = ninep_client_walk(&client, root_fid, &fid1, "hello.txt");
	zassert_equal(ret, 0, "Walk 1 failed");

	ret = ninep_client_walk(&client, root_fid, &fid2, "data.bin");
	zassert_equal(ret, 0, "Walk 2 failed");

	ret = ninep_client_open(&client, fid1, NINEP_OREAD);
	zassert_equal(ret, 0, "Open 1 failed");

	ret = ninep_client_open(&client, fid2, NINEP_OREAD);
	zassert_equal(ret, 0, "Open 2 failed");

	/* Read from both */
	ret = ninep_client_read(&client, fid1, 0, buf1, sizeof(buf1));
	zassert_true(ret > 0, "Read 1 failed");

	ret = ninep_client_read(&client, fid2, 0, buf2, sizeof(buf2));
	zassert_equal(ret, 4, "Read 2 failed");

	/* Verify data */
	zassert_mem_equal(buf1, "Hello from 9P!\n", 16, "Data 1 mismatch");
	zassert_mem_equal(buf2, "\x01\x02\x03\x04", 4, "Data 2 mismatch");

	/* Cleanup */
	ninep_client_clunk(&client, fid1);
	ninep_client_clunk(&client, fid2);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Error handling - walk to nonexistent file */
ZTEST(client_server, test_walk_error)
{
	uint32_t root_fid, file_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "nonexistent.txt");
	zassert_true(ret < 0, "Walk should fail for nonexistent file");

	/* Cleanup */
	ninep_client_clunk(&client, root_fid);
}

ZTEST_SUITE(client_server, NULL, client_server_setup, NULL, NULL, client_server_teardown);
