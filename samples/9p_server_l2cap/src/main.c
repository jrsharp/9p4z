/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/transport_l2cap.h>
#include <zephyr/9p/sysfs.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* GPIO for LED control */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool led_state = false;

/* Firmware update tracking */
static size_t firmware_bytes_written = 0;
static char firmware_last_write[32] = "No uploads yet";

/* 9P server instance */
static struct ninep_server server;
static struct ninep_transport transport;

/* Sysfs instance - increased for all our demo files! */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[64];

/* RX buffer for L2CAP transport */
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* Bluetooth advertising data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Bluetooth connection callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		LOG_ERR("Failed to connect to %s (err %u)", addr, err);
		return;
	}

	LOG_INF("Connected: %s", addr);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason %u)", addr, reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Generate hello.txt content */
static int gen_hello(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	const char *msg = "Hello from 9P over L2CAP!\n";
	size_t msg_len = strlen(msg);

	if (offset >= msg_len) {
		return 0;
	}

	size_t remaining = msg_len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, msg + offset, to_copy);
	return to_copy;
}

/* Generate sys/version content */
static int gen_version(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char version[128];
	uint32_t ver = sys_kernel_version_get();
	int len = snprintf(version, sizeof(version),
	                  "9p4z L2CAP Server\nZephyr: %d.%d.%d\nBuild: %s %s\n",
	                  SYS_KERNEL_VER_MAJOR(ver), SYS_KERNEL_VER_MINOR(ver), SYS_KERNEL_VER_PATCHLEVEL(ver),
	                  __DATE__, __TIME__);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, version + offset, to_copy);
	return to_copy;
}

/* Generate sys/uptime content */
static int gen_uptime(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	uint64_t uptime_ms = k_uptime_get();
	uint64_t uptime_sec = uptime_ms / 1000;
	uint64_t hours = uptime_sec / 3600;
	uint64_t minutes = (uptime_sec % 3600) / 60;
	uint64_t seconds = uptime_sec % 60;

	char uptime_str[64];
	int len = snprintf(uptime_str, sizeof(uptime_str),
	                  "%llu:%02llu:%02llu (%llu ms)\n",
	                  hours, minutes, seconds, uptime_ms);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, uptime_str + offset, to_copy);
	return to_copy;
}

/* Generate sys/memory content - LIVE heap statistics! */
static int gen_memory(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char mem_str[256];
	int len = snprintf(mem_str, sizeof(mem_str),
	                  "Heap Statistics\n"
	                  "===============\n"
	                  "Pool Size: %d bytes\n"
	                  "Note: See kernel stats for memory info\n",
	                  CONFIG_HEAP_MEM_POOL_SIZE);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, mem_str + offset, to_copy);
	return to_copy;
}

/* Thread iteration context for k_thread_foreach callback */
struct thread_iter_ctx {
	char *buf;
	size_t buf_size;
	size_t pos;
};

/* Callback for k_thread_foreach */
static void thread_list_cb(const struct k_thread *thread, void *user_data)
{
	struct thread_iter_ctx *ctx = user_data;

	if (ctx->pos >= ctx->buf_size - 50) {
		return;  /* Buffer nearly full */
	}

	const char *name = k_thread_name_get(thread);
	if (!name) {
		name = "<unnamed>";
	}

	ctx->pos += snprintf(ctx->buf + ctx->pos, ctx->buf_size - ctx->pos,
	                     "%s (prio=%d)\n",
	                     name, k_thread_priority_get(thread));
}

/* Generate sys/threads content - LIVE thread list! */
static int gen_threads(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	static char thread_buf[1024];
	static uint64_t last_gen_time = 0;
	uint64_t now = k_uptime_get();

	/* Regenerate thread list if stale (>100ms old) */
	if (now - last_gen_time > 100) {
		struct thread_iter_ctx iter_ctx = {
			.buf = thread_buf,
			.buf_size = sizeof(thread_buf),
			.pos = 0
		};

		iter_ctx.pos += snprintf(thread_buf, sizeof(thread_buf),
		                         "Active Threads\n"
		                         "==============\n");

		/* Iterate through all threads */
		k_thread_foreach(thread_list_cb, &iter_ctx);

		last_gen_time = now;
	}

	int len = strlen(thread_buf);
	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, thread_buf + offset, to_copy);
	return to_copy;
}

/* Generate sys/stats content - Kernel statistics */
static int gen_stats(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	uint32_t cycles = k_cycle_get_32();

	char stats_str[256];
	int len = snprintf(stats_str, sizeof(stats_str),
	                  "Kernel Statistics\n"
	                  "=================\n"
	                  "CPU Cycles: %u\n"
	                  "Uptime:     %llu ms\n"
	                  "Tick Rate:  %d Hz\n",
	                  cycles,
	                  k_uptime_get(),
	                  CONFIG_SYS_CLOCK_TICKS_PER_SEC);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, stats_str + offset, to_copy);
	return to_copy;
}

/* ========== DEVICE CONTROL - LED ========== */

/* Generate dev/led content - show LED state */
static int gen_led(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	const char *state_str = led_state ? "on\n" : "off\n";
	int len = strlen(state_str);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, state_str + offset, to_copy);
	return to_copy;
}

/* Write to dev/led - control LED! */
static int write_led(const uint8_t *buf, uint32_t count, uint64_t offset, void *ctx)
{
	char cmd[16];
	size_t to_copy = MIN(count, sizeof(cmd) - 1);

	memcpy(cmd, buf, to_copy);
	cmd[to_copy] = '\0';

	/* Check command */
	if (strstr(cmd, "on") || strstr(cmd, "1")) {
		led_state = true;
		gpio_pin_set_dt(&led, 1);
		LOG_INF("LED turned ON via 9P!");
	} else if (strstr(cmd, "off") || strstr(cmd, "0")) {
		led_state = false;
		gpio_pin_set_dt(&led, 0);
		LOG_INF("LED turned OFF via 9P!");
	}

	return count;
}

/* ========== SENSORS - Temperature ========== */

/* Generate sensors/temp0 content - die temperature */
static int gen_temp(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	/* Simulated temperature - varies with uptime for demo purposes */
	int temp_c = 22 + ((k_uptime_get() / 1000) % 8);  /* Simulate 22-30°C */

	char temp_str[128];
	int len = snprintf(temp_str, sizeof(temp_str),
	                  "Die Temperature\n"
	                  "===============\n"
	                  "Temperature: %d °C\n"
	                  "Source: Simulated\n"
	                  "Status: Normal\n",
	                  temp_c);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, temp_str + offset, to_copy);
	return to_copy;
}

/* ========== FIRMWARE UPDATE ========== */

/* Generate sys/firmware content - firmware upload status */
static int gen_firmware(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char fw_str[256];
	int len = snprintf(fw_str, sizeof(fw_str),
	                  "Firmware Upload Status\n"
	                  "======================\n"
	                  "Bytes Written: %zu\n"
	                  "Last Write:    %s\n"
	                  "\n"
	                  "Write firmware image to this file for OTA update!\n"
	                  "(This is a DEMO - not actually flashing)\n",
	                  firmware_bytes_written,
	                  firmware_last_write);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, fw_str + offset, to_copy);
	return to_copy;
}

/* Write to sys/firmware - FIRMWARE UPLOAD! */
static int write_firmware(const uint8_t *buf, uint32_t count, uint64_t offset, void *ctx)
{
	firmware_bytes_written += count;

	uint64_t now = k_uptime_get();
	snprintf(firmware_last_write, sizeof(firmware_last_write),
	         "%llu ms ago", now);

	LOG_INF("Firmware write: %u bytes at offset %llu (total: %zu)",
	        count, offset, firmware_bytes_written);

	/* In a real implementation, you would:
	 * 1. Validate the firmware image
	 * 2. Write to flash partition
	 * 3. Mark for MCUboot upgrade
	 * 4. Reboot to apply
	 */

	return count;
}

/* ========== LIBRARY FILES - Reference Material ========== */

/* Generate lib/9p-intro.txt - large reference file */
static int gen_9p_intro(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	static const char intro[] =
		"Introduction to the 9P Protocol\n"
		"================================\n"
		"\n"
		"The 9P protocol (originally named Styx) is a network protocol\n"
		"developed for Plan 9 from Bell Labs. It provides a unified\n"
		"interface to distributed resources using a file system paradigm.\n"
		"\n"
		"Key Concepts:\n"
		"-------------\n"
		"\n"
		"1. Everything is a File\n"
		"   All resources are represented as files in a hierarchical\n"
		"   namespace. Want to control hardware? Read/write a file!\n"
		"\n"
		"2. Simple Protocol\n"
		"   9P uses a simple request-response model with just 14 message\n"
		"   types. This makes it easy to implement and debug.\n"
		"\n"
		"3. Network Transparent\n"
		"   Resources can be accessed locally or remotely using the same\n"
		"   file operations. The network becomes transparent.\n"
		"\n"
		"Why 9P for Embedded/IoT?\n"
		"------------------------\n"
		"\n"
		"- Lightweight: Small memory footprint\n"
		"- Flexible: Works over any transport (TCP, BLE, UART...)\n"
		"- Powerful: Expose ANY device capability as a file!\n"
		"- Standard: Well-defined protocol with multiple implementations\n"
		"\n"
		"This Demo:\n"
		"---------\n"
		"\n"
		"You're viewing this file over Bluetooth L2CAP using 9P!\n"
		"Browse the filesystem to see:\n"
		"\n"
		"/dev/led      - Control an LED by writing 'on' or 'off'\n"
		"/sensors/temp0 - Read live temperature sensor\n"
		"/sys/threads  - See all running threads\n"
		"/sys/firmware - Upload firmware over BLE!\n"
		"\n"
		"The Future:\n"
		"----------\n"
		"\n"
		"Imagine your IoT devices exposing everything as files:\n"
		"- Configuration via text files\n"
		"- Sensor data as readable streams\n"
		"- Control interfaces as writable files\n"
		"- Firmware updates as file uploads\n"
		"\n"
		"All accessible from your phone, computer, or another device\n"
		"using a simple, universal protocol. That's the power of 9P!\n"
		"\n"
		"Learn more: http://9p.io/\n";

	/* Use sizeof() - 1 to get length (excluding null terminator) */
	const int len = sizeof(intro) - 1;

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, intro + offset, to_copy);
	return to_copy;
}

/* Setup synthetic filesystem */
static int setup_filesystem(void)
{
	int ret;

	/* Initialize sysfs */
	ret = ninep_sysfs_init(&sysfs, sysfs_entries, ARRAY_SIZE(sysfs_entries));
	if (ret < 0) {
		LOG_ERR("Failed to initialize sysfs: %d", ret);
		return ret;
	}

	/* Create /hello.txt */
	ret = ninep_sysfs_register_file(&sysfs, "hello.txt", gen_hello, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add hello.txt: %d", ret);
		return ret;
	}

	/* Create /sys directory */
	ret = ninep_sysfs_register_dir(&sysfs, "sys");
	if (ret < 0) {
		LOG_ERR("Failed to add sys directory: %d", ret);
		return ret;
	}

	/* Create /sys/version */
	ret = ninep_sysfs_register_file(&sysfs, "sys/version", gen_version, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/version: %d", ret);
		return ret;
	}

	/* Create /sys/uptime */
	ret = ninep_sysfs_register_file(&sysfs, "sys/uptime", gen_uptime, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/uptime: %d", ret);
		return ret;
	}

	/* Create /sys/memory - LIVE heap stats! */
	ret = ninep_sysfs_register_file(&sysfs, "sys/memory", gen_memory, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/memory: %d", ret);
		return ret;
	}

	/* Create /sys/threads - LIVE thread list! */
	ret = ninep_sysfs_register_file(&sysfs, "sys/threads", gen_threads, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/threads: %d", ret);
		return ret;
	}

	/* Create /sys/stats - Kernel statistics */
	ret = ninep_sysfs_register_file(&sysfs, "sys/stats", gen_stats, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/stats: %d", ret);
		return ret;
	}

	/* Create /docs directory */
	ret = ninep_sysfs_register_dir(&sysfs, "docs");
	if (ret < 0) {
		LOG_ERR("Failed to add docs directory: %d", ret);
		return ret;
	}

	/* Create /docs/readme.md */
	ret = ninep_sysfs_register_file(&sysfs, "docs/readme.md", gen_hello, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add docs/readme.md: %d", ret);
		return ret;
	}

	/* ========== DEVICE CONTROL ========== */

	/* Create /dev directory */
	ret = ninep_sysfs_register_dir(&sysfs, "dev");
	if (ret < 0) {
		LOG_ERR("Failed to add dev directory: %d", ret);
		return ret;
	}

	/* Create /dev/led - WRITABLE! Control LED over BLE! */
	ret = ninep_sysfs_register_writable_file(&sysfs, "dev/led",
	                                          gen_led, write_led, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add dev/led: %d", ret);
		return ret;
	}

	/* ========== SENSORS ========== */

	/* Create /sensors directory */
	ret = ninep_sysfs_register_dir(&sysfs, "sensors");
	if (ret < 0) {
		LOG_ERR("Failed to add sensors directory: %d", ret);
		return ret;
	}

	/* Create /sensors/temp0 - Live temperature! */
	ret = ninep_sysfs_register_file(&sysfs, "sensors/temp0", gen_temp, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sensors/temp0: %d", ret);
		return ret;
	}

	/* ========== FIRMWARE UPDATES ========== */

	/* Create /sys/firmware - WRITABLE! Upload firmware over BLE! */
	ret = ninep_sysfs_register_writable_file(&sysfs, "sys/firmware",
	                                          gen_firmware, write_firmware, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add sys/firmware: %d", ret);
		return ret;
	}

	/* ========== LIBRARY / REFERENCE MATERIAL ========== */

	/* Create /lib directory */
	ret = ninep_sysfs_register_dir(&sysfs, "lib");
	if (ret < 0) {
		LOG_ERR("Failed to add lib directory: %d", ret);
		return ret;
	}

	/* Create /lib/9p-intro.txt - Large reference file */
	ret = ninep_sysfs_register_file(&sysfs, "lib/9p-intro.txt", gen_9p_intro, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add lib/9p-intro.txt: %d", ret);
		return ret;
	}

	LOG_INF("===================================================");
	LOG_INF("Filesystem setup complete with AMAZING DEMO FEATURES!");
	LOG_INF("===================================================");
	LOG_INF("- /dev/led: Control hardware by writing 'on'/'off'");
	LOG_INF("- /sensors/temp0: Live temperature readings");
	LOG_INF("- /sys/firmware: Upload firmware over BLE!");
	LOG_INF("- /lib/9p-intro.txt: Learn about 9P");
	LOG_INF("===================================================");
	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("9P L2CAP Server Sample");
	LOG_INF("======================");

	/* Initialize Bluetooth */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed (err %d)", ret);
		return 0;
	}

	LOG_INF("Bluetooth initialized");

	/* Start advertising */
	ret = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
	                                       BT_GAP_ADV_FAST_INT_MAX_2, NULL),
	                      ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("Advertising failed to start (err %d)", ret);
		return 0;
	}

	LOG_INF("Advertising started");

	/* Initialize LED GPIO */
	if (!gpio_is_ready_dt(&led)) {
		LOG_WRN("LED GPIO not ready - LED control will not work");
	} else {
		ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_WRN("Failed to configure LED GPIO: %d", ret);
		} else {
			LOG_INF("LED GPIO configured - ready for 9P control!");
		}
	}

	/* Setup filesystem */
	ret = setup_filesystem();
	if (ret < 0) {
		LOG_ERR("Failed to setup filesystem: %d", ret);
		return 0;
	}

	/* Initialize L2CAP transport */
	struct ninep_transport_l2cap_config l2cap_config = {
		.psm = CONFIG_NINEP_L2CAP_PSM,
		.rx_buf = rx_buf,
		.rx_buf_size = sizeof(rx_buf),
	};

	ret = ninep_transport_l2cap_init(&transport, &l2cap_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize L2CAP transport: %d", ret);
		return 0;
	}

	LOG_INF("L2CAP transport initialized");

	/* Initialize 9P server with sysfs */
	struct ninep_server_config server_config = {
		.fs_ops = ninep_sysfs_get_ops(),
		.fs_ctx = &sysfs,
		.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE,
		.version = "9P2000",
	};

	ret = ninep_server_init(&server, &server_config, &transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9P server: %d", ret);
		return 0;
	}

	LOG_INF("9P server initialized");

	/* Start server (starts transport and begins accepting messages) */
	ret = ninep_server_start(&server);
	if (ret < 0) {
		LOG_ERR("Failed to start server: %d", ret);
		return 0;
	}

	LOG_INF("L2CAP server started on PSM 0x%04x", CONFIG_NINEP_L2CAP_PSM);
	LOG_INF("Waiting for iOS client connection...");

	/* Server runs in background via callbacks */
	return 0;
}
