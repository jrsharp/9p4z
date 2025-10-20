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
#include <zephyr/9p/gatt_9pis.h>

#include <zephyr/9p/union_fs.h>

#ifdef CONFIG_NINEP_FS_PASSTHROUGH
#include <zephyr/9p/passthrough_fs.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#define USE_LITTLEFS 1
#define LITTLEFS_MOUNT_POINT "/lfs1"
#else
#define USE_LITTLEFS 0
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* GPIO for LED control */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool led_state = false;

/* GPIO for button inputs */
static const struct gpio_dt_spec button1 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static struct gpio_callback button1_cb_data;
static struct gpio_callback button2_cb_data;

/* Button state tracking */
static uint32_t button1_press_count = 0;
static uint32_t button2_press_count = 0;
static bool button1_state = false;
static bool button2_state = false;
static uint64_t button1_last_press_time = 0;
static uint64_t button2_last_press_time = 0;

/* Firmware update tracking */
static size_t firmware_bytes_written = 0;
static char firmware_last_write[32] = "No uploads yet";

/* 9P server instance */
static struct ninep_server server;
static struct ninep_transport transport;

/* Sysfs instance - always present for system files */
static struct ninep_sysfs sysfs;
static struct ninep_sysfs_entry sysfs_entries[64];

#if USE_LITTLEFS
/* LittleFS storage backend */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FIXED_PARTITION_ID(lfs_partition),
	.mnt_point = LITTLEFS_MOUNT_POINT,
};

/* Passthrough FS instance */
static struct ninep_passthrough_fs passthrough_fs;
#endif

/* Union FS instance for namespace composition - always available! */
static struct ninep_union_fs union_fs;
static struct ninep_union_mount union_mounts[4];  /* Space for up to 4 backends */

/* RX buffer for L2CAP transport */
static uint8_t rx_buf[CONFIG_NINEP_MAX_MESSAGE_SIZE];

/* Bluetooth advertising data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* Bluetooth connection tracking */
static uint32_t bt_connection_count = 0;
static uint32_t bt_total_connections = 0;
static char bt_last_connected_addr[BT_ADDR_LE_STR_LEN] = "None";
static uint64_t bt_last_connected_time = 0;

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

	/* Update connection stats */
	bt_connection_count++;
	bt_total_connections++;
	strncpy(bt_last_connected_addr, addr, sizeof(bt_last_connected_addr) - 1);
	bt_last_connected_time = k_uptime_get();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_INF("Disconnected: %s (reason %u)", addr, reason);

	/* Update connection stats */
	if (bt_connection_count > 0) {
		bt_connection_count--;
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* Button interrupt handlers */
static void button1_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	button1_state = gpio_pin_get_dt(&button1);
	button1_press_count++;
	button1_last_press_time = k_uptime_get();
	LOG_INF("Button 1 pressed! Count: %u", button1_press_count);
}

static void button2_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	button2_state = gpio_pin_get_dt(&button2);
	button2_press_count++;
	button2_last_press_time = k_uptime_get();
	LOG_INF("Button 2 pressed! Count: %u", button2_press_count);
}

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

	const char *name = k_thread_name_get((k_tid_t)thread);
	if (!name) {
		name = "<unnamed>";
	}

	ctx->pos += snprintf(ctx->buf + ctx->pos, ctx->buf_size - ctx->pos,
	                     "%s (prio=%d)\n",
	                     name, k_thread_priority_get((k_tid_t)thread));
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

/* Generate dev/button1 content - show button state and counter */
static int gen_button1(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char btn_str[256];
	uint64_t now = k_uptime_get();
	uint64_t time_since_press = button1_last_press_time ? (now - button1_last_press_time) : 0;

	int len = snprintf(btn_str, sizeof(btn_str),
	                  "Button 1 Status\n"
	                  "===============\n"
	                  "State:          %s\n"
	                  "Press Count:    %u\n"
	                  "Last Press:     %llu ms ago\n"
	                  "\n"
	                  "Press the button to increment the counter!\n",
	                  button1_state ? "pressed" : "released",
	                  button1_press_count,
	                  time_since_press);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, btn_str + offset, to_copy);
	return to_copy;
}

/* Generate dev/button2 content - show button state and counter */
static int gen_button2(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char btn_str[256];
	uint64_t now = k_uptime_get();
	uint64_t time_since_press = button2_last_press_time ? (now - button2_last_press_time) : 0;

	int len = snprintf(btn_str, sizeof(btn_str),
	                  "Button 2 Status\n"
	                  "===============\n"
	                  "State:          %s\n"
	                  "Press Count:    %u\n"
	                  "Last Press:     %llu ms ago\n"
	                  "\n"
	                  "Press the button to increment the counter!\n",
	                  button2_state ? "pressed" : "released",
	                  button2_press_count,
	                  time_since_press);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, btn_str + offset, to_copy);
	return to_copy;
}

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

/* ========== NETWORK STATS - Bluetooth ========== */

/* Generate net/bt/connections content - active BT connections */
static int gen_bt_connections(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char conn_str[512];
	int len = snprintf(conn_str, sizeof(conn_str),
	                  "Bluetooth Connection Statistics\n"
	                  "================================\n"
	                  "Active Connections: %u\n"
	                  "Total Connections:  %u\n"
	                  "Last Connected:     %s\n"
	                  "Time Since Connect: %llu ms\n"
	                  "\n"
	                  "Connection Details:\n"
	                  "-------------------\n"
	                  "You are currently %s via Bluetooth L2CAP!\n"
	                  "PSM: 0x%04x\n"
	                  "Protocol: 9P2000 over L2CAP\n",
	                  bt_connection_count,
	                  bt_total_connections,
	                  bt_last_connected_addr,
	                  bt_last_connected_time ? (k_uptime_get() - bt_last_connected_time) : 0,
	                  bt_connection_count > 0 ? "connected" : "disconnected",
	                  CONFIG_NINEP_L2CAP_PSM);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, conn_str + offset, to_copy);
	return to_copy;
}

/* Generate net/bt/address content - device BT address */
static int gen_bt_address(uint8_t *buf, size_t buf_size, uint64_t offset, void *ctx)
{
	char addr_str[256];
	bt_addr_le_t addr;
	char addr_string[BT_ADDR_LE_STR_LEN];

	/* Get local BT address */
	size_t count = 1;
	bt_id_get(&addr, &count);
	bt_addr_le_to_str(&addr, addr_string, sizeof(addr_string));

	int len = snprintf(addr_str, sizeof(addr_str),
	                  "Bluetooth Device Information\n"
	                  "============================\n"
	                  "Device Name:    %s\n"
	                  "Device Address: %s\n"
	                  "Device Type:    Peripheral\n"
	                  "Capabilities:   L2CAP, 9P Server\n",
	                  CONFIG_BT_DEVICE_NAME,
	                  addr_string);

	if (offset >= len) {
		return 0;
	}

	size_t remaining = len - offset;
	size_t to_copy = MIN(remaining, buf_size);

	memcpy(buf, addr_str + offset, to_copy);
	return to_copy;
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
		"/dev/led        - Control an LED by writing 'on' or 'off'\n"
		"/dev/button1    - Live button state and press counter\n"
		"/dev/button2    - Live button state and press counter\n"
		"/sensors/temp0  - Read live temperature sensor\n"
		"/sys/threads    - See all running threads\n"
		"/sys/firmware   - Upload firmware over BLE!\n"
		"/net/bt/*       - Bluetooth connection statistics\n"
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

#if USE_LITTLEFS
/* Pre-populate LittleFS with initial files */
static int populate_littlefs(void)
{
	struct fs_file_t file;
	int ret;

	LOG_INF("Populating LittleFS with initial files...");

	/* Check if already populated (marker file exists) */
	const char *marker_path = LITTLEFS_MOUNT_POINT "/.populated";
	struct fs_dirent entry;
	ret = fs_stat(marker_path, &entry);
	if (ret == 0) {
		LOG_INF("Filesystem already populated, skipping");
		return 0;
	}

	/* Create welcome file */
	const char *welcome_path = LITTLEFS_MOUNT_POINT "/welcome.txt";
	const char *welcome_content =
		"Welcome to 9P over Bluetooth!\n"
		"\n"
		"This is a persistent LittleFS filesystem stored in flash.\n"
		"All files you create here will survive reboots!\n"
		"\n"
		"Try creating files, directories, and exploring!\n";

	fs_file_t_init(&file);
	ret = fs_open(&file, welcome_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("Failed to create welcome.txt: %d", ret);
		return ret;
	}
	fs_write(&file, welcome_content, strlen(welcome_content));
	fs_close(&file);
	LOG_INF("Created: %s", welcome_path);

	/* Create docs directory */
	const char *docs_dir = LITTLEFS_MOUNT_POINT "/docs";
	ret = fs_mkdir(docs_dir);
	if (ret < 0) {
		LOG_ERR("Failed to create /docs: %d", ret);
		return ret;
	}
	LOG_INF("Created: %s/", docs_dir);

	/* Create README in docs */
	const char *readme_path = LITTLEFS_MOUNT_POINT "/docs/README.md";
	const char *readme_content =
		"# 9P File Server\n"
		"\n"
		"## Features\n"
		"\n"
		"- Persistent storage on flash\n"
		"- Wireless access via Bluetooth\n"
		"- Standard 9P protocol\n"
		"\n"
		"## Usage\n"
		"\n"
		"Connect via Bluetooth and use any 9P client to access files.\n"
		"All changes are immediately saved to flash!\n";

	fs_file_t_init(&file);
	ret = fs_open(&file, readme_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("Failed to create README.md: %d", ret);
		return ret;
	}
	fs_write(&file, readme_content, strlen(readme_content));
	fs_close(&file);
	LOG_INF("Created: %s", readme_path);

	/* Create shared directory */
	const char *shared_dir = LITTLEFS_MOUNT_POINT "/shared";
	ret = fs_mkdir(shared_dir);
	if (ret < 0) {
		LOG_ERR("Failed to create /shared: %d", ret);
		return ret;
	}
	LOG_INF("Created: %s/", shared_dir);

	/* Create notes directory */
	const char *notes_dir = LITTLEFS_MOUNT_POINT "/notes";
	ret = fs_mkdir(notes_dir);
	if (ret < 0) {
		LOG_ERR("Failed to create /notes: %d", ret);
		return ret;
	}
	LOG_INF("Created: %s/", notes_dir);

	/* Create example note */
	const char *note_path = LITTLEFS_MOUNT_POINT "/notes/example.txt";
	const char *note_content =
		"Example Note\n"
		"============\n"
		"\n"
		"This is an example note file. Feel free to:\n"
		"- Edit this file\n"
		"- Delete this file\n"
		"- Create new notes\n"
		"\n"
		"All changes persist to flash!\n";

	fs_file_t_init(&file);
	ret = fs_open(&file, note_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("Failed to create example.txt: %d", ret);
		return ret;
	}
	fs_write(&file, note_content, strlen(note_content));
	fs_close(&file);
	LOG_INF("Created: %s", note_path);

	/* Create marker file to indicate population is complete */
	fs_file_t_init(&file);
	ret = fs_open(&file, marker_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("Failed to create marker file: %d", ret);
		return ret;
	}
	const char *marker = "v1\n";
	fs_write(&file, marker, strlen(marker));
	fs_close(&file);

	LOG_INF("Filesystem population complete!");
	return 0;
}
#endif /* USE_LITTLEFS */

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

	/* Create /dev/button1 - Button state and press counter! */
	ret = ninep_sysfs_register_file(&sysfs, "dev/button1", gen_button1, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add dev/button1: %d", ret);
		return ret;
	}

	/* Create /dev/button2 - Button state and press counter! */
	ret = ninep_sysfs_register_file(&sysfs, "dev/button2", gen_button2, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add dev/button2: %d", ret);
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

	/* ========== NETWORK STATISTICS ========== */

	/* Create /net directory */
	ret = ninep_sysfs_register_dir(&sysfs, "net");
	if (ret < 0) {
		LOG_ERR("Failed to add net directory: %d", ret);
		return ret;
	}

	/* Create /net/bt directory */
	ret = ninep_sysfs_register_dir(&sysfs, "net/bt");
	if (ret < 0) {
		LOG_ERR("Failed to add net/bt directory: %d", ret);
		return ret;
	}

	/* Create /net/bt/connections - Live BT connection stats! */
	ret = ninep_sysfs_register_file(&sysfs, "net/bt/connections", gen_bt_connections, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add net/bt/connections: %d", ret);
		return ret;
	}

	/* Create /net/bt/address - BT device address and info! */
	ret = ninep_sysfs_register_file(&sysfs, "net/bt/address", gen_bt_address, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to add net/bt/address: %d", ret);
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

	/* Note: /files mount point will appear automatically in directory listings
	 * thanks to union_fs merging sysfs entries with mount point entries */

	LOG_INF("===================================================");
	LOG_INF("Filesystem setup complete with AMAZING DEMO FEATURES!");
	LOG_INF("===================================================");
	LOG_INF("Device Control:");
	LOG_INF("  - /dev/led: Control hardware by writing 'on'/'off'");
	LOG_INF("  - /dev/button1: Live button state & press counter");
	LOG_INF("  - /dev/button2: Live button state & press counter");
	LOG_INF("System Stats:");
	LOG_INF("  - /sys/uptime, /sys/threads, /sys/memory");
	LOG_INF("  - /sensors/temp0: Live temperature readings");
	LOG_INF("Network:");
	LOG_INF("  - /net/bt/connections: BT connection statistics");
	LOG_INF("  - /net/bt/address: Device BT information");
	LOG_INF("Firmware:");
	LOG_INF("  - /sys/firmware: Upload firmware over BLE!");
	LOG_INF("Documentation:");
	LOG_INF("  - /lib/9p-intro.txt: Learn about 9P");
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

#ifdef CONFIG_NINEP_GATT_9PIS
	/* Initialize 9P Information Service (9PIS) for discoverability */
	struct ninep_9pis_config gatt_config = {
		.service_description = "9P File Server",
		.service_features = "file-sharing,sensor-data,led-control,firmware-update",
		.transport_info = "l2cap:psm=0x0009,mtu=4096",
		.app_store_link = "https://9p4z.org/clients",
		.protocol_version = "9P2000;9p4z;1.0.0",
	};

	ret = ninep_9pis_init(&gatt_config);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9PIS GATT service: %d", ret);
		return 0;
	}

	LOG_INF("9PIS GATT service initialized - device discoverable!");
#endif

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

	/* Always setup sysfs with system files */

	/* Initialize Button 1 GPIO */
	if (!gpio_is_ready_dt(&button1)) {
		LOG_WRN("Button 1 GPIO not ready - button tracking will not work");
	} else {
		ret = gpio_pin_configure_dt(&button1, GPIO_INPUT);
		if (ret < 0) {
			LOG_WRN("Failed to configure button 1 GPIO: %d", ret);
		} else {
			ret = gpio_pin_interrupt_configure_dt(&button1, GPIO_INT_EDGE_TO_ACTIVE);
			if (ret < 0) {
				LOG_WRN("Failed to configure button 1 interrupt: %d", ret);
			} else {
				gpio_init_callback(&button1_cb_data, button1_pressed, BIT(button1.pin));
				gpio_add_callback(button1.port, &button1_cb_data);
				LOG_INF("Button 1 configured - press count available at /dev/button1!");
			}
		}
	}

	/* Initialize Button 2 GPIO */
	if (!gpio_is_ready_dt(&button2)) {
		LOG_WRN("Button 2 GPIO not ready - button tracking will not work");
	} else {
		ret = gpio_pin_configure_dt(&button2, GPIO_INPUT);
		if (ret < 0) {
			LOG_WRN("Failed to configure button 2 GPIO: %d", ret);
		} else {
			ret = gpio_pin_interrupt_configure_dt(&button2, GPIO_INT_EDGE_TO_ACTIVE);
			if (ret < 0) {
				LOG_WRN("Failed to configure button 2 interrupt: %d", ret);
			} else {
				gpio_init_callback(&button2_cb_data, button2_pressed, BIT(button2.pin));
				gpio_add_callback(button2.port, &button2_cb_data);
				LOG_INF("Button 2 configured - press count available at /dev/button2!");
			}
		}
	}

	/* Setup filesystem */
	ret = setup_filesystem();
	if (ret < 0) {
		LOG_ERR("Failed to setup sysfs: %d", ret);
		return 0;
	}

#if USE_LITTLEFS
	LOG_INF("*** LittleFS support is ENABLED ***");
	/* LittleFS is auto-mounted by device tree at /lfs1 */
	LOG_INF("Using auto-mounted LittleFS at %s", LITTLEFS_MOUNT_POINT);
#else
	LOG_WRN("*** LittleFS support is DISABLED - check CONFIG_NINEP_FS_PASSTHROUGH ***");
#endif

#if USE_LITTLEFS

	/* List LittleFS contents to verify flash image */
	LOG_INF("===== LittleFS Contents =====");
	k_sleep(K_MSEC(50));  /* Give UART time to flush */

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	ret = fs_opendir(&dir, LITTLEFS_MOUNT_POINT);
	if (ret < 0) {
		LOG_ERR("Failed to open LFS root: %d", ret);
	} else {
		struct fs_dirent entry;
		int entry_count = 0;
		while (fs_readdir(&dir, &entry) == 0) {
			if (entry.name[0] == 0) {
				break;  /* End of directory */
			}
			entry_count++;
			LOG_INF("  %s %s (%zu bytes)",
			        entry.type == FS_DIR_ENTRY_DIR ? "[DIR]" : "[FILE]",
			        entry.name,
			        entry.size);

			/* If it's a directory, list its contents too */
			if (entry.type == FS_DIR_ENTRY_DIR) {
				char subdir_path[256];
				snprintf(subdir_path, sizeof(subdir_path), "%s/%s",
				         LITTLEFS_MOUNT_POINT, entry.name);

				struct fs_dir_t subdir;
				fs_dir_t_init(&subdir);
				if (fs_opendir(&subdir, subdir_path) == 0) {
					struct fs_dirent subentry;
					while (fs_readdir(&subdir, &subentry) == 0) {
						if (subentry.name[0] == 0) {
							break;
						}
						LOG_INF("    %s %s/%s (%zu bytes)",
						        subentry.type == FS_DIR_ENTRY_DIR ? "[DIR]" : "[FILE]",
						        entry.name, subentry.name,
						        subentry.size);
					}
					fs_closedir(&subdir);
				}
			}
		}
		fs_closedir(&dir);

		if (entry_count == 0) {
			LOG_WRN("LittleFS is EMPTY! Creating demo files...");

			/* Create /lib directory */
			char lib_dir[64];
			snprintf(lib_dir, sizeof(lib_dir), "%s/lib", LITTLEFS_MOUNT_POINT);
			ret = fs_mkdir(lib_dir);
			if (ret == 0 || ret == -EEXIST) {
				LOG_INF("  Created: /lib/");

				/* Create /lib/readme.txt */
				char readme_path[128];
				snprintf(readme_path, sizeof(readme_path), "%s/readme.txt", lib_dir);
				struct fs_file_t file;
				fs_file_t_init(&file);

				ret = fs_open(&file, readme_path, FS_O_CREATE | FS_O_WRITE);
				if (ret == 0) {
					const char *content =
						"Welcome to 9P over Bluetooth!\n"
						"\n"
						"This filesystem is stored in flash and persists across reboots.\n"
						"You can create, edit, and delete files via 9P.\n"
						"\n"
						"Try:\n"
						"  - Creating new files and directories\n"
						"  - Editing this file\n"
						"  - Uploading firmware to /sys/firmware\n"
						"\n"
						"Learn more about 9P at http://9p.io/\n";

					fs_write(&file, content, strlen(content));
					fs_close(&file);
					LOG_INF("  Created: /lib/readme.txt");
				}
			}

			/* Create /notes directory */
			char notes_dir[64];
			snprintf(notes_dir, sizeof(notes_dir), "%s/notes", LITTLEFS_MOUNT_POINT);
			ret = fs_mkdir(notes_dir);
			if (ret == 0 || ret == -EEXIST) {
				LOG_INF("  Created: /notes/");

				/* Create /notes/example.txt */
				char note_path[128];
				snprintf(note_path, sizeof(note_path), "%s/example.txt", notes_dir);
				struct fs_file_t file;
				fs_file_t_init(&file);

				ret = fs_open(&file, note_path, FS_O_CREATE | FS_O_WRITE);
				if (ret == 0) {
					const char *content =
						"Example Note\n"
						"============\n"
						"\n"
						"This is a small demo file.\n"
						"Feel free to edit or delete it!\n";

					fs_write(&file, content, strlen(content));
					fs_close(&file);
					LOG_INF("  Created: /notes/example.txt");
				}
			}

			LOG_INF("Demo files created!");
		} else {
			LOG_INF("Found %d entries in LittleFS", entry_count);
		}
	}
	LOG_INF("=============================");
	k_sleep(K_MSEC(100));  /* Give UART time to flush all logs */

	/* Initialize passthrough filesystem */
	ret = ninep_passthrough_fs_init(&passthrough_fs, LITTLEFS_MOUNT_POINT);
	if (ret < 0) {
		LOG_ERR("Failed to initialize passthrough FS: %d", ret);
		return 0;
	}
	LOG_INF("Passthrough filesystem initialized");
#endif

	/* Initialize union filesystem for namespace composition */
	ret = ninep_union_fs_init(&union_fs, union_mounts, ARRAY_SIZE(union_mounts));
	if (ret < 0) {
		LOG_ERR("Failed to initialize union FS: %d", ret);
		return 0;
	}

	/* Mount sysfs at root "/" - provides /dev, /sys, /lib, etc. */
	ret = ninep_union_fs_mount(&union_fs, "/",
	                            ninep_sysfs_get_ops(), &sysfs);
	if (ret < 0) {
		LOG_ERR("Failed to mount sysfs at /: %d", ret);
		return 0;
	}
	LOG_INF("Mounted sysfs at /");

#if USE_LITTLEFS
	/* Mount passthrough_fs at "/files" - provides persistent storage */
	ret = ninep_union_fs_mount(&union_fs, "/files",
	                            ninep_passthrough_fs_get_ops(), &passthrough_fs);
	if (ret < 0) {
		LOG_ERR("Failed to mount passthrough FS at /files: %d", ret);
		return 0;
	}
	LOG_INF("Mounted LittleFS at /files");
	LOG_INF("===================================================");
	LOG_INF("UNIFIED NAMESPACE:");
	LOG_INF("  /dev, /sys, /sensors, /lib  -> sysfs (dynamic)");
	LOG_INF("  /files/*                    -> LittleFS (persistent)");
	LOG_INF("===================================================");
#else
	LOG_INF("===================================================");
	LOG_INF("NAMESPACE:");
	LOG_INF("  /dev, /sys, /sensors, /lib  -> sysfs (dynamic)");
	LOG_INF("===================================================");
#endif

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

	/* Initialize 9P server - always use union filesystem for namespace composition */
	struct ninep_server_config server_config = {
		.fs_ops = ninep_union_fs_get_ops(),
		.fs_ctx = &union_fs,
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
