/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Stress and Regression Tests
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
	bool simulate_timeout;
	int send_error;
};

static struct mock_transport client_transport;
static struct mock_transport server_transport;
static struct ninep_client client;
static struct ninep_server server;
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[32];

/* Mock transport send */
static int mock_send(struct ninep_transport *transport, const uint8_t *buf, size_t len)
{
	struct mock_transport *mock = CONTAINER_OF(transport, struct mock_transport, base);
	struct mock_transport *peer = CONTAINER_OF(mock->peer, struct mock_transport, base);

	/* Simulate errors if configured */
	if (mock->send_error) {
		return mock->send_error;
	}

	/* Simulate timeout - don't deliver message */
	if (mock->simulate_timeout) {
		return 0;
	}

	/* Normal delivery */
	memcpy(peer->buf, buf, len);
	peer->len = len;

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
static const char *small_content = "tiny";
static const char *deep_content = "Deep file";
static char large_data[4096];

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

/* Generator for large binary data */
static int gen_large_bin(uint8_t *buf, size_t buf_size,
                         uint64_t offset, void *ctx)
{
	size_t len = 4096; /* large_data is 4096 bytes */

	if (offset >= len) {
		return 0;
	}

	size_t to_copy = len - offset;
	if (to_copy > buf_size) {
		to_copy = buf_size;
	}

	memcpy(buf, large_data + offset, to_copy);
	return to_copy;
}

/* Setup */
static void *stress_setup(void)
{
	int ret;

	memset(&client_transport, 0, sizeof(client_transport));
	memset(&server_transport, 0, sizeof(server_transport));

	client_transport.base.ops = &mock_ops;
	client_transport.peer = &server_transport.base;

	server_transport.base.ops = &mock_ops;
	server_transport.peer = &client_transport.base;

	/* Initialize large data array */
	memset(large_data, 'A', sizeof(large_data));

	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	zassert_equal(ret, 0, "Failed to init sysfs");

	/* Add diverse test files */
	ninep_sysfs_register_file(&sysfs, "/small.txt", gen_static,
	                           (void *)small_content);

	/* Large file (near max message size) */
	ninep_sysfs_register_file(&sysfs, "/large.bin", gen_large_bin, NULL);

	ninep_sysfs_register_dir(&sysfs, "/dir1");
	ninep_sysfs_register_dir(&sysfs, "/dir1/dir2");
	ninep_sysfs_register_file(&sysfs, "/dir1/dir2/deep.txt", gen_static,
	                           (void *)deep_content);

	/* Writable file for stress tests */
	ninep_sysfs_register_file(&sysfs, "/writable.dat", gen_static,
	                           (void *)"");

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
		.timeout_ms = 500,  /* Shorter timeout for stress tests */
	};

	ret = ninep_client_init(&client, &client_config, &client_transport.base);
	zassert_equal(ret, 0, "Failed to init client");

	return NULL;
}

static void stress_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	ninep_server_stop(&server);
}

/* Test: Large file read (near max message size) */
ZTEST(stress, test_large_file_read)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[4096];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "large.bin");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open failed");

	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_equal(ret, 4096, "Read size mismatch");

	/* Verify data integrity */
	for (int i = 0; i < 4096; i++) {
		zassert_equal(buf[i], 'A', "Data corruption at offset %d", i);
	}

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Deep path walk */
ZTEST(stress, test_deep_path)
{
	uint32_t root_fid, file_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "dir1/dir2/deep.txt");
	zassert_equal(ret, 0, "Deep walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open deep file failed");

	uint8_t buf[32];
	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_equal(ret, 9, "Read deep file failed");
	zassert_mem_equal(buf, "Deep file", 9, "Deep file content mismatch");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Multiple sequential writes */
ZTEST(stress, test_sequential_writes)
{
	uint32_t root_fid, file_fid;
	uint8_t write_buf[256];
	uint8_t read_buf[1024];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "writable.dat");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OWRITE | NINEP_OTRUNC);
	zassert_equal(ret, 0, "Open failed");

	/* Write in chunks */
	uint64_t offset = 0;
	for (int i = 0; i < 4; i++) {
		memset(write_buf, 'A' + i, sizeof(write_buf));
		ret = ninep_client_write(&client, file_fid, offset, write_buf, sizeof(write_buf));
		zassert_equal(ret, sizeof(write_buf), "Write %d failed", i);
		offset += sizeof(write_buf);
	}

	/* Close and reopen to read */
	ninep_client_clunk(&client, file_fid);

	ret = ninep_client_walk(&client, root_fid, &file_fid, "writable.dat");
	zassert_equal(ret, 0, "Walk for read failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open for read failed");

	/* Verify writes */
	ret = ninep_client_read(&client, file_fid, 0, read_buf, 1024);
	zassert_equal(ret, 1024, "Read size mismatch");

	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 256; j++) {
			zassert_equal(read_buf[i * 256 + j], 'A' + i,
			             "Data mismatch at chunk %d offset %d", i, j);
		}
	}

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: FID exhaustion and recovery */
ZTEST(stress, test_fid_exhaustion)
{
	uint32_t root_fid;
	uint32_t fids[CONFIG_NINEP_MAX_FIDS];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	/* Allocate all available FIDs */
	int allocated = 1; /* root_fid already allocated */
	for (int i = 1; i < CONFIG_NINEP_MAX_FIDS; i++) {
		ret = ninep_client_walk(&client, root_fid, &fids[i], "small.txt");
		if (ret == 0) {
			allocated++;
		} else {
			break;
		}
	}

	zassert_true(allocated > 1, "Should allocate multiple FIDs");

	/* Try to allocate one more - should fail */
	uint32_t overflow_fid;
	ret = ninep_client_walk(&client, root_fid, &overflow_fid, "small.txt");
	zassert_true(ret < 0, "Should fail when FIDs exhausted");

	/* Free half the FIDs */
	for (int i = 1; i < allocated / 2; i++) {
		ninep_client_clunk(&client, fids[i]);
	}

	/* Should be able to allocate again */
	ret = ninep_client_walk(&client, root_fid, &overflow_fid, "small.txt");
	zassert_equal(ret, 0, "Should succeed after freeing FIDs");

	/* Cleanup */
	ninep_client_clunk(&client, overflow_fid);
	for (int i = allocated / 2; i < allocated; i++) {
		ninep_client_clunk(&client, fids[i]);
	}
	ninep_client_clunk(&client, root_fid);
}

/* Test: Timeout handling */
ZTEST(stress, test_timeout)
{
	uint32_t root_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version should work before timeout");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach should work before timeout");

	/* Enable timeout simulation */
	client_transport.simulate_timeout = true;

	/* This should timeout */
	uint32_t file_fid;
	ret = ninep_client_walk(&client, root_fid, &file_fid, "small.txt");
	zassert_true(ret < 0, "Should timeout");
	zassert_equal(ret, -ETIMEDOUT, "Should return timeout error");

	/* Disable timeout */
	client_transport.simulate_timeout = false;

	/* Should work again */
	ret = ninep_client_walk(&client, root_fid, &file_fid, "small.txt");
	zassert_equal(ret, 0, "Should work after timeout cleared");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Send error handling */
ZTEST(stress, test_send_error)
{
	uint32_t root_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version should work");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach should work");

	/* Simulate send error */
	client_transport.send_error = -EIO;

	uint32_t file_fid;
	ret = ninep_client_walk(&client, root_fid, &file_fid, "small.txt");
	zassert_true(ret < 0, "Should fail on send error");

	/* Clear error */
	client_transport.send_error = 0;

	/* Should work again */
	ret = ninep_client_walk(&client, root_fid, &file_fid, "small.txt");
	zassert_equal(ret, 0, "Should work after error cleared");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Partial read (offset and count) */
ZTEST(stress, test_partial_read)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[10];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "large.bin");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open failed");

	/* Read from middle of file */
	ret = ninep_client_read(&client, file_fid, 1000, buf, sizeof(buf));
	zassert_equal(ret, sizeof(buf), "Partial read failed");

	for (int i = 0; i < sizeof(buf); i++) {
		zassert_equal(buf[i], 'A', "Data at offset 1000+%d incorrect", i);
	}

	/* Read near end of file */
	ret = ninep_client_read(&client, file_fid, 4090, buf, sizeof(buf));
	zassert_equal(ret, 6, "Should only read 6 bytes at end");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Zero-byte operations */
ZTEST(stress, test_zero_operations)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[1];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "writable.dat");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OWRITE | NINEP_OTRUNC);
	zassert_equal(ret, 0, "Open failed");

	/* Zero-byte write */
	ret = ninep_client_write(&client, file_fid, 0, buf, 0);
	zassert_equal(ret, 0, "Zero-byte write should succeed");

	ninep_client_clunk(&client, file_fid);

	/* Reopen and read */
	ret = ninep_client_walk(&client, root_fid, &file_fid, "writable.dat");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open failed");

	/* Zero-byte read (empty file) */
	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_equal(ret, 0, "Should read 0 bytes from empty file");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* Test: Repeated operations (regression test) */
ZTEST(stress, test_repeated_operations)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[16];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	/* Repeat same operation 10 times */
	for (int i = 0; i < 10; i++) {
		ret = ninep_client_walk(&client, root_fid, &file_fid, "small.txt");
		zassert_equal(ret, 0, "Walk iteration %d failed", i);

		ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
		zassert_equal(ret, 0, "Open iteration %d failed", i);

		ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
		zassert_equal(ret, 4, "Read iteration %d failed", i);
		zassert_mem_equal(buf, "tiny", 4, "Data iteration %d mismatch", i);

		ninep_client_clunk(&client, file_fid);
	}

	ninep_client_clunk(&client, root_fid);
}

ZTEST_SUITE(stress, NULL, stress_setup, NULL, NULL, stress_teardown);
