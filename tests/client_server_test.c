/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * Client-Server Integration Tests
 * Only compile when both client and server are enabled
 */

#include <zephyr/ztest.h>

#if defined(CONFIG_NINEP_CLIENT) && defined(CONFIG_NINEP_SERVER)

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

/* A genuinely writable RAM-backed file: exercises the intended sysfs
 * writer-callback path (register_writable_file), as opposed to the
 * read-only generator files above. Reset per test in the before hook. */
static uint8_t rw_buf[512];
static size_t rw_len;

static int gen_rw(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);
	if (offset >= rw_len) {
		return 0;
	}
	size_t to_copy = rw_len - offset;
	if (to_copy > buf_size) {
		to_copy = buf_size;
	}
	memcpy(buf, rw_buf + offset, to_copy);
	return to_copy;
}

static int write_rw(const uint8_t *buf, uint32_t count, uint64_t offset,
                    void *ctx)
{
	ARG_UNUSED(ctx);
	if (offset + count > sizeof(rw_buf)) {
		return -ENOSPC;
	}
	memcpy(rw_buf + offset, buf, count);
	if (offset + count > rw_len) {
		rw_len = offset + count;
	}
	return count;
}

/* Per-test setup: a fresh client + server for every test so state (fids,
 * tags, auth pools, negotiated msize) never leaks between tests. */
static void client_server_before(void *fixture)
{
	int ret;

	ARG_UNUSED(fixture);

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

	/* A writable file (distinct from the read-only generator files). */
	rw_len = 0;
	ninep_sysfs_register_writable_file(&sysfs, "/rw.dat", gen_rw, write_rw,
	                                   NULL);

	/* Initialize server. NOTE: the client stores its config BY POINTER
	 * (ninep_client_init keeps &config), so the config must outlive this
	 * function — hence `static`. A stack local would dangle the moment
	 * this hook returns and ninep_client_version() would strlen() a freed
	 * config->version. (The server copies its config by value, but keep it
	 * static too for symmetry.) */
	static struct ninep_server_config server_config;
	server_config = (struct ninep_server_config){
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &server_config, &server_transport.base);
	zassert_equal(ret, 0, "Failed to init server");

	ret = ninep_server_start(&server);
	zassert_equal(ret, 0, "Failed to start server");

	/* Initialize client (config held by pointer — see note above). */
	static struct ninep_client_config client_config;
	client_config = (struct ninep_client_config){
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
		.timeout_ms = 1000,
	};

	ret = ninep_client_init(&client, &client_config, &client_transport.base);
	zassert_equal(ret, 0, "Failed to init client");
}

/* Per-test teardown: clunk any open fids and free the server's RX/TX buffers
 * so the next test's init starts from a clean heap. */
static void client_server_after(void *fixture)
{
	ARG_UNUSED(fixture);

	ninep_server_stop(&server);
	ninep_server_cleanup(&server);
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

	/* Write to a writable file, then read it back. */
	ret = ninep_client_walk(&client, root_fid, &file_fid, "rw.dat");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OWRITE | NINEP_OTRUNC);
	zassert_equal(ret, 0, "Open for write failed");

	ret = ninep_client_write(&client, file_fid, 0,
	                         (const uint8_t *)test_data, strlen(test_data));
	zassert_equal(ret, strlen(test_data), "Write failed");

	/* Close and reopen to read back */
	ninep_client_clunk(&client, file_fid);

	ret = ninep_client_walk(&client, root_fid, &file_fid, "rw.dat");
	zassert_equal(ret, 0, "Walk failed");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open for read failed");

	ret = ninep_client_read(&client, file_fid, 0, read_buf, sizeof(read_buf));
	zassert_equal(ret, strlen(test_data), "Read size mismatch");
	zassert_mem_equal(read_buf, test_data, strlen(test_data), "Read data mismatch");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* A read-only generator file must reject an open-for-write: sysfs returns
 * EACCES at Topen, so the client open fails. Pins the intended behavior. */
ZTEST(client_server, test_write_readonly_denied)
{
	uint32_t root_fid, file_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");
	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid, "hello.txt");
	zassert_equal(ret, 0, "Walk failed");

	/* hello.txt is a read-only generator file — OWRITE must be refused. */
	ret = ninep_client_open(&client, file_fid, NINEP_OWRITE);
	zassert_true(ret < 0, "Opening a read-only file for write must fail");

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

	/* sysfs is a static synthetic filesystem (.create == NULL), so Tcreate
	 * must be refused cleanly rather than silently succeeding. This pins
	 * that intended behavior; a create-capable backend would need its own
	 * round-trip test. */
	ret = ninep_client_create(&client, dir_fid, "newfile.txt", 0644,
	                          NINEP_OWRITE);
	zassert_true(ret < 0, "Create on a non-create-capable fs must fail");

	/* The failed create must not have created anything. */
	uint32_t probe_fid;
	ret = ninep_client_walk(&client, root_fid, &probe_fid,
	                        "testdir/newfile.txt");
	zassert_true(ret < 0, "No file should exist after a rejected create");

	ninep_client_clunk(&client, dir_fid);
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

/*
 * Test: ninep_client_auth basic functionality
 *
 * Note: Full auth testing with signature verification requires crypto setup.
 * This test verifies the Tauth message is sent correctly but the server
 * (without auth config) will reject it with an error, which we expect.
 * A proper integration test should be added when testing with a real
 * auth-enabled server (e.g., aetherd BBS).
 */
ZTEST(client_server, test_auth_no_server_support)
{
	uint32_t afid;
	struct ninep_qid aqid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");

	/* Try auth - server has no auth config, so this should fail gracefully */
	ret = ninep_client_auth(&client, &afid, &aqid, "testuser", "");

	/* Without server auth support, we expect failure */
	/* This tests that the client handles the error response correctly */
	zassert_true(ret != 0, "Auth should fail when server has no auth support");
}

/* ------------------------------------------------------------------ *
 * Conformance regression tests for the 2026-07 protocol fixes.
 * Each targets a specific spec corner the base suite did not cover.
 * ------------------------------------------------------------------ */

/* A1 — Tversion prefix negotiation: a client offering a dotted version
 * ("9P2000.u") must negotiate down to plain "9P2000" and succeed, rather
 * than being refused. */
ZTEST(client_server, test_version_dotted)
{
	static struct ninep_client_config cfg;
	cfg = (struct ninep_client_config){
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000.u",
		.timeout_ms = 1000,
	};
	int ret = ninep_client_init(&client, &cfg, &client_transport.base);
	zassert_equal(ret, 0, "Client re-init failed");

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Dotted version must negotiate down to 9P2000");
}

/* A3 — partial walk: walking a path whose leading element exists but a
 * later element does not must fail (server returns a partial Rwalk with
 * fewer qids and no newfid; the client reports the shortfall as an error
 * rather than believing it holds a valid fid). */
ZTEST(client_server, test_walk_partial_nested)
{
	uint32_t root_fid, file_fid;
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");
	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	/* "testdir" exists, "nope.txt" does not -> failure at element 2. */
	ret = ninep_client_walk(&client, root_fid, &file_fid,
	                        "testdir/nope.txt");
	zassert_true(ret < 0, "Partial walk (missing 2nd element) must fail");

	ninep_client_clunk(&client, root_fid);
}

/* A4 — read/write require a prior open: reading a fid that was walked but
 * never opened must be rejected, so the open-time permission check cannot
 * be bypassed. */
ZTEST(client_server, test_read_requires_open)
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

	/* No open() here — the server must refuse the read. */
	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_true(ret < 0, "Read on an unopened fid must be rejected");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* A6 — "." / ".." in a walk: "testdir/.." returns to the root, so
 * "testdir/../hello.txt" resolves to the root's hello.txt. */
ZTEST(client_server, test_dotdot_walk)
{
	uint32_t root_fid, file_fid;
	uint8_t buf[64];
	int ret;

	ret = ninep_client_version(&client);
	zassert_equal(ret, 0, "Version failed");
	ret = ninep_client_attach(&client, &root_fid, NINEP_NOFID, "user", "");
	zassert_equal(ret, 0, "Attach failed");

	ret = ninep_client_walk(&client, root_fid, &file_fid,
	                        "testdir/../hello.txt");
	zassert_equal(ret, 0, "Walk through '..' must resolve");

	ret = ninep_client_open(&client, file_fid, NINEP_OREAD);
	zassert_equal(ret, 0, "Open failed");
	ret = ninep_client_read(&client, file_fid, 0, buf, sizeof(buf));
	zassert_true(ret >= 15, "Read failed");
	zassert_mem_equal(buf, "Hello from 9P!\n", 15,
	                  "'..' walk reached the wrong file");

	ninep_client_clunk(&client, file_fid);
	ninep_client_clunk(&client, root_fid);
}

/* ==================================================================
 * Malformed-message safety (B4). A truncated or lying frame must be
 * refused with an Rerror, never read past the received buffer. We
 * inject raw frames straight into the server; ninep_parse_header only
 * requires a 7-byte header, so short bodies reach the handlers and
 * exercise their bounds checks.
 * ================================================================== */

/* Build [size|type|tag|body], feed it to the server, and copy the ename
 * string from the server's Rerror into `out` ("" if it wasn't an Rerror).
 * We assert on the specific ename because each bounds check fires BEFORE
 * find_fid — so a fix regression (which would instead reach find_fid and
 * answer "unknown fid", or read past the frame) is caught, not masked. */
static void inject_ename(uint8_t type, const uint8_t *body, size_t body_len,
                         char *out, size_t outsz)
{
	uint8_t msg[64];
	size_t len = 7 + body_len;

	msg[0] = len & 0xFF;         /* size[4] = actual length */
	msg[1] = (len >> 8) & 0xFF;
	msg[2] = 0;
	msg[3] = 0;
	msg[4] = type;               /* type[1] */
	msg[5] = 0x11;               /* tag[2] */
	msg[6] = 0x22;
	if (body_len) {
		memcpy(&msg[7], body, body_len);
	}

	out[0] = '\0';
	client_transport.len = 0;
	ninep_server_process_message(&server, msg, len);

	if (client_transport.len >= 9 &&
	    client_transport.buf[4] == NINEP_RERROR) {
		uint16_t el = client_transport.buf[7] |
		              (client_transport.buf[8] << 8);
		if ((size_t)(9 + el) <= client_transport.len && el < outsz) {
			memcpy(out, &client_transport.buf[9], el);
			out[el] = '\0';
		}
	}
}

ZTEST(client_server, test_malformed_short_tversion)
{
	/* Tversion needs msize[4]+count[2] after the header (13 bytes). */
	uint8_t body[2] = {0};
	char ename[64];
	inject_ename(NINEP_TVERSION, body, sizeof(body), ename, sizeof(ename));
	zassert_true(strcmp(ename, "bad Tversion") == 0,
	             "short Tversion -> '%s' (want 'bad Tversion')", ename);
}

ZTEST(client_server, test_malformed_short_tread)
{
	/* Tread needs 23 bytes. */
	uint8_t body[4] = {0};
	char ename[64];
	inject_ename(NINEP_TREAD, body, sizeof(body), ename, sizeof(ename));
	zassert_true(strcmp(ename, "malformed Tread") == 0,
	             "short Tread -> '%s' (want 'malformed Tread')", ename);
}

ZTEST(client_server, test_malformed_twrite_count_past_frame)
{
	/* fid[4] offset[8] count[4]=65535 with zero data bytes: the server
	 * must refuse before reading `count` bytes past the frame. */
	uint8_t body[16] = {0};
	char ename[64];
	body[12] = 0xFF;   /* count low  */
	body[13] = 0xFF;   /* count high */
	inject_ename(NINEP_TWRITE, body, sizeof(body), ename, sizeof(ename));
	zassert_true(strcmp(ename, "malformed Twrite") == 0,
	             "Twrite count-past-frame -> '%s' (want 'malformed Twrite')",
	             ename);
}

ZTEST(client_server, test_malformed_tcreate_namelen_past_frame)
{
	/* fid[4] name_len[2]=4095 with no name/perm/mode following. Without
	 * the bounds check the server would index perm/mode ~4 KB past the
	 * frame. */
	uint8_t body[6] = {0};
	char ename[64];
	body[4] = 0xFF;    /* name_len low  */
	body[5] = 0x0F;    /* name_len high (0x0FFF) */
	inject_ename(NINEP_TCREATE, body, sizeof(body), ename, sizeof(ename));
	zassert_true(strcmp(ename, "malformed Tcreate") == 0,
	             "Tcreate namelen-past-frame -> '%s' (want 'malformed Tcreate')",
	             ename);
}

/* ==================================================================
 * Auth uname binding (H). A Tattach uname must match the authenticated
 * identity EXACTLY — a prefix like "ad" must not authenticate as
 * "admin". Uses a mock verify callback (no real crypto): we test the
 * uname<->identity binding, not the signature check.
 * ================================================================== */

static int mock_verify_ok(const char *identity,
                          const uint8_t *pubkey, size_t pubkey_len,
                          const uint8_t *signature, size_t sig_len,
                          const uint8_t *challenge, size_t challenge_len,
                          void *auth_ctx)
{
	ARG_UNUSED(identity); ARG_UNUSED(pubkey); ARG_UNUSED(pubkey_len);
	ARG_UNUSED(signature); ARG_UNUSED(sig_len);
	ARG_UNUSED(challenge); ARG_UNUSED(challenge_len); ARG_UNUSED(auth_ctx);
	return 0;   /* accept any response */
}

static int mock_check_perm(const char *identity, const char *path,
                           uint8_t mode, void *auth_ctx)
{
	ARG_UNUSED(identity); ARG_UNUSED(path);
	ARG_UNUSED(mode); ARG_UNUSED(auth_ctx);
	return 0;   /* allow */
}

static const struct ninep_auth_config mock_auth = {
	.verify_auth = mock_verify_ok,
	.check_perm = mock_check_perm,
	.required = true,
};

/* Replace the before-hook server with one that requires auth. */
static void reinit_server_with_auth(void)
{
	ninep_server_stop(&server);
	ninep_server_cleanup(&server);

	static struct ninep_server_config cfg;
	cfg = (struct ninep_server_config){
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
		.auth_config = &mock_auth,
	};
	zassert_equal(ninep_server_init(&server, &cfg, &server_transport.base), 0,
	              "auth server init");
	zassert_equal(ninep_server_start(&server), 0, "auth server start");
}

/* Authenticate as `identity`: Tauth, read challenge, write response. */
static int do_auth_handshake(uint32_t *afid, const char *identity)
{
	struct ninep_qid aqid;
	uint8_t chal[64];
	uint8_t resp[96] = {0};
	int ret;

	ret = ninep_client_auth(&client, afid, &aqid, identity, "");
	if (ret) {
		return ret;
	}
	ret = ninep_client_read(&client, *afid, 0, chal, sizeof(chal));
	if (ret < 0) {
		return ret;
	}
	return ninep_client_write(&client, *afid, 0, resp, sizeof(resp));
}

ZTEST(client_server, test_auth_uname_prefix_rejected)
{
	uint32_t afid, fid;
	int ret;

	reinit_server_with_auth();
	zassert_equal(ninep_client_version(&client), 0, "version");
	zassert_true(do_auth_handshake(&afid, "admin") >= 0, "auth as admin");

	/* "ad" is a prefix of the authenticated "admin" — must be refused. */
	ret = ninep_client_attach(&client, &fid, afid, "ad", "");
	zassert_true(ret < 0, "prefix uname must not authenticate as 'admin'");
}

ZTEST(client_server, test_auth_uname_exact_accepted)
{
	uint32_t afid, fid;
	int ret;

	reinit_server_with_auth();
	zassert_equal(ninep_client_version(&client), 0, "version");
	zassert_true(do_auth_handshake(&afid, "admin") >= 0, "auth as admin");

	/* The exact authenticated uname must attach successfully (no false
	 * rejection from the length check). */
	ret = ninep_client_attach(&client, &fid, afid, "admin", "");
	zassert_equal(ret, 0, "exact uname must authenticate");
	ninep_client_clunk(&client, fid);
}

/* ==================================================================
 * Completeness for A1 / A3 / A4 / A6.
 * ================================================================== */

/* A1: an unknown version must be refused (server replies Rversion
 * "unknown"; client reports failure rather than proceeding). */
ZTEST(client_server, test_version_unknown_rejected)
{
	static struct ninep_client_config cfg;
	cfg = (struct ninep_client_config){
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "XYZZY",
		.timeout_ms = 1000,
	};
	zassert_equal(ninep_client_init(&client, &cfg, &client_transport.base), 0,
	              "reinit client");
	zassert_true(ninep_client_version(&client) < 0,
	             "server must refuse an unknown version");
}

/* A3: it is illegal to walk an open fid. */
ZTEST(client_server, test_walk_from_open_fid_rejected)
{
	uint32_t root, fid, fid2;

	zassert_equal(ninep_client_version(&client), 0, "version");
	zassert_equal(ninep_client_attach(&client, &root, NINEP_NOFID, "user", ""),
	              0, "attach");
	zassert_equal(ninep_client_walk(&client, root, &fid, "hello.txt"), 0, "walk");
	zassert_equal(ninep_client_open(&client, fid, NINEP_OREAD), 0, "open");

	zassert_true(ninep_client_walk(&client, fid, &fid2, ".") < 0,
	             "walking an open fid must be rejected");

	ninep_client_clunk(&client, fid);
	ninep_client_clunk(&client, root);
}

/* A4: writing a walked-but-unopened fid must be rejected. */
ZTEST(client_server, test_write_requires_open)
{
	uint32_t root, fid;
	uint8_t d = 'x';

	zassert_equal(ninep_client_version(&client), 0, "version");
	zassert_equal(ninep_client_attach(&client, &root, NINEP_NOFID, "user", ""),
	              0, "attach");
	zassert_equal(ninep_client_walk(&client, root, &fid, "rw.dat"), 0, "walk");

	zassert_true(ninep_client_write(&client, fid, 0, &d, 1) < 0,
	             "write on an unopened fid must be rejected");

	ninep_client_clunk(&client, fid);
	ninep_client_clunk(&client, root);
}

/* A4: a fid may be opened only once. */
ZTEST(client_server, test_double_open_rejected)
{
	uint32_t root, fid;

	zassert_equal(ninep_client_version(&client), 0, "version");
	zassert_equal(ninep_client_attach(&client, &root, NINEP_NOFID, "user", ""),
	              0, "attach");
	zassert_equal(ninep_client_walk(&client, root, &fid, "hello.txt"), 0, "walk");
	zassert_equal(ninep_client_open(&client, fid, NINEP_OREAD), 0, "first open");

	zassert_true(ninep_client_open(&client, fid, NINEP_OREAD) < 0,
	             "a second open on the same fid must fail");

	ninep_client_clunk(&client, fid);
	ninep_client_clunk(&client, root);
}

/* A6: "." stays put and ".." at the root stays at the root. */
ZTEST(client_server, test_dot_and_dotdot_at_root)
{
	uint32_t root, fid;

	zassert_equal(ninep_client_version(&client), 0, "version");
	zassert_equal(ninep_client_attach(&client, &root, NINEP_NOFID, "user", ""),
	              0, "attach");

	/* "./hello.txt" resolves the same as "hello.txt". */
	zassert_equal(ninep_client_walk(&client, root, &fid, "./hello.txt"), 0,
	              "'.' element must resolve");
	ninep_client_clunk(&client, fid);

	/* "..", at the root, stays at the root, so "../hello.txt" still hits it. */
	zassert_equal(ninep_client_walk(&client, root, &fid, "../hello.txt"), 0,
	              "'..' at root must stay at root");
	ninep_client_clunk(&client, fid);

	ninep_client_clunk(&client, root);
}

ZTEST_SUITE(client_server, NULL, NULL, client_server_before, client_server_after, NULL);

#endif /* CONFIG_NINEP_CLIENT && CONFIG_NINEP_SERVER */
