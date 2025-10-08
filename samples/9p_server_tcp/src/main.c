/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P TCP Server - Serves a synthetic filesystem over TCP/IP
 */

#include <zephyr/kernel.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/transport_tcp.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_context.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static struct ninep_transport g_transport;
static struct ninep_server g_server;
static struct ninep_sysfs g_sysfs;
static struct ninep_sysfs_entry g_sysfs_entries[32];
static struct ninep_tcp_config tcp_config;
static struct ninep_server_config server_config;

/* Static content for demo files */
static const char *hello_content = "Hello from Zephyr 9P TCP server!\n";
static const char *readme_content =
	"9P Server on Zephyr RTOS over TCP/IP\n"
	"======================================\n\n"
	"This is a demonstration 9P server running on Zephyr.\n"
	"It serves both static demo files and dynamic system info.\n\n"
	"Connection:\n"
	"  Port:    564 (standard 9P port)\n\n"
	"Demo Files:\n"
	"  /hello.txt  - Static greeting\n"
	"  /readme.txt - This file\n"
	"  /docs/*     - Demo documents\n\n"
	"System Info (dynamic):\n"
	"  /sys/uptime  - System uptime\n"
	"  /sys/version - Kernel version\n"
	"  /sys/board   - Board name\n\n"
	"Try:\n"
	"  9p -a tcp!<IP>!564 ls /\n"
	"  9p -a tcp!<IP>!564 read /sys/uptime\n";
static const char *doc1_content = "This is document 1\n";
static const char *doc2_content = "This is document 2\n";

/* Generator for static content (allows reuse of const strings) */
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

/* Generator for /sys/uptime */
static int gen_sys_uptime(uint8_t *buf, size_t buf_size,
                          uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;  /* Single-read file */
	}

	int64_t uptime_ms = k_uptime_get();
	int uptime_sec = uptime_ms / 1000;
	int uptime_min = uptime_sec / 60;
	int uptime_hr = uptime_min / 60;
	int uptime_days = uptime_hr / 24;

	int len = snprintf((char *)buf, buf_size,
	                   "%d days, %d hours, %d minutes, %d seconds (%lld ms)\n",
	                   uptime_days,
	                   uptime_hr % 24,
	                   uptime_min % 60,
	                   uptime_sec % 60,
	                   uptime_ms);

	return len;
}

/* Generator for /sys/version */
static int gen_sys_version(uint8_t *buf, size_t buf_size,
                           uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	uint32_t version = sys_kernel_version_get();
	int len = snprintf((char *)buf, buf_size,
	                   "Zephyr %d.%d.%d\n9P4Z TCP Server\n",
	                   SYS_KERNEL_VER_MAJOR(version),
	                   SYS_KERNEL_VER_MINOR(version),
	                   SYS_KERNEL_VER_PATCHLEVEL(version));

	return len;
}

/* Generator for /sys/board */
static int gen_sys_board(uint8_t *buf, size_t buf_size,
                         uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	int len = snprintf((char *)buf, buf_size, "%s\n", CONFIG_BOARD);

	return len;
}

/* Generator for /net/interfaces */
static int gen_net_interfaces(uint8_t *buf, size_t buf_size,
                               uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	size_t len = 0;
	struct net_if *iface;

	/* Iterate through all network interfaces */
	STRUCT_SECTION_FOREACH(net_if, iface) {
		char addr_str[NET_IPV4_ADDR_LEN];
		const char *status;

		/* Interface name and index */
		len += snprintf((char *)buf + len, buf_size - len,
		                "Interface %d: %p\n", net_if_get_by_iface(iface), iface);

		if (len >= buf_size) break;

		/* Link address (MAC) */
		struct net_linkaddr *link_addr = net_if_get_link_addr(iface);
		if (link_addr && link_addr->len > 0) {
			len += snprintf((char *)buf + len, buf_size - len, "  MAC: ");
			if (len >= buf_size) break;

			for (int i = 0; i < link_addr->len; i++) {
				len += snprintf((char *)buf + len, buf_size - len,
				                "%02x%s", link_addr->addr[i],
				                i < link_addr->len - 1 ? ":" : "");
				if (len >= buf_size) break;
			}
			len += snprintf((char *)buf + len, buf_size - len, "\n");
			if (len >= buf_size) break;
		}

		/* Interface status */
		if (net_if_is_up(iface)) {
			status = "UP";
		} else {
			status = "DOWN";
		}
		len += snprintf((char *)buf + len, buf_size - len, "  Status: %s\n", status);
		if (len >= buf_size) break;

		/* IPv4 addresses */
		struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
		if (ipv4) {
			for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
				if (ipv4->unicast[i].ipv4.addr_state != NET_ADDR_ANY_STATE) {
					net_addr_ntop(AF_INET,
					              &ipv4->unicast[i].ipv4.address.in_addr,
					              addr_str, sizeof(addr_str));
					len += snprintf((char *)buf + len, buf_size - len,
					                "  IPv4: %s\n", addr_str);
					if (len >= buf_size) break;
				}
			}
		}

		len += snprintf((char *)buf + len, buf_size - len, "\n");
		if (len >= buf_size) break;
	}

	return len;
}

/* Generator for /net/stats */
static int gen_net_stats(uint8_t *buf, size_t buf_size,
                         uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

#if defined(CONFIG_NET_STATISTICS)
	struct net_stats stats;
	net_mgmt(NET_REQUEST_STATS_GET_ALL, NULL, &stats, sizeof(stats));

	size_t len = snprintf((char *)buf, buf_size,
	                      "Network Statistics:\n"
	                      "  TX: %llu bytes, %llu packets\n"
	                      "  RX: %llu bytes, %llu packets\n"
	                      "  Errors: %llu\n",
	                      (unsigned long long)stats.bytes.sent,
	                      (unsigned long long)stats.pkts.tx,
	                      (unsigned long long)stats.bytes.received,
	                      (unsigned long long)stats.pkts.rx,
	                      (unsigned long long)stats.processing_error);
	return len;
#else
	return snprintf((char *)buf, buf_size, "Network statistics not available\n");
#endif
}

/* Generator for /wifi/status */
static int gen_wifi_status(uint8_t *buf, size_t buf_size,
                           uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	struct net_if *iface = net_if_get_default();
	if (!iface) {
		return snprintf((char *)buf, buf_size, "No network interface\n");
	}

	/* Allocate status struct from heap to avoid stack overflow */
	struct wifi_iface_status *status = k_malloc(sizeof(struct wifi_iface_status));
	if (!status) {
		return snprintf((char *)buf, buf_size, "Error: Out of memory\n");
	}

	memset(status, 0, sizeof(*status));
	int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
	                   status, sizeof(*status));

	if (ret < 0) {
		k_free(status);
		return snprintf((char *)buf, buf_size, "WiFi status unavailable (error %d)\n", ret);
	}

	const char *state_str;
	switch (status->state) {
	case WIFI_STATE_DISCONNECTED:
		state_str = "DISCONNECTED";
		break;
	case WIFI_STATE_INTERFACE_DISABLED:
		state_str = "DISABLED";
		break;
	case WIFI_STATE_SCANNING:
		state_str = "SCANNING";
		break;
	case WIFI_STATE_AUTHENTICATING:
		state_str = "AUTHENTICATING";
		break;
	case WIFI_STATE_ASSOCIATING:
		state_str = "ASSOCIATING";
		break;
	case WIFI_STATE_ASSOCIATED:
		state_str = "ASSOCIATED";
		break;
	case WIFI_STATE_4WAY_HANDSHAKE:
		state_str = "4WAY_HANDSHAKE";
		break;
	case WIFI_STATE_GROUP_HANDSHAKE:
		state_str = "GROUP_HANDSHAKE";
		break;
	case WIFI_STATE_COMPLETED:
		state_str = "CONNECTED";
		break;
	default:
		state_str = "UNKNOWN";
		break;
	}

	int len = snprintf((char *)buf, buf_size,
	                   "State: %s\n"
	                   "SSID: %s\n"
	                   "Channel: %d\n"
	                   "Link Mode: %s\n"
	                   "RSSI: %d dBm\n",
	                   state_str,
	                   status->ssid,
	                   status->channel,
	                   status->link_mode == WIFI_LINK_MODE_UNKNOWN ? "UNKNOWN" :
	                   status->link_mode == WIFI_1 ? "802.11b" :
	                   status->link_mode == WIFI_2 ? "802.11a" :
	                   status->link_mode == WIFI_3 ? "802.11g" :
	                   status->link_mode == WIFI_4 ? "802.11n" :
	                   status->link_mode == WIFI_5 ? "802.11ac" :
	                   status->link_mode == WIFI_6 ? "802.11ax" : "OTHER",
	                   status->rssi);

	k_free(status);
	return len;
}

/* Generator for /wifi/rssi */
static int gen_wifi_rssi(uint8_t *buf, size_t buf_size,
                         uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	struct net_if *iface = net_if_get_default();
	if (!iface) {
		return snprintf((char *)buf, buf_size, "N/A\n");
	}

	/* Allocate status struct from heap to avoid stack overflow */
	struct wifi_iface_status *status = k_malloc(sizeof(struct wifi_iface_status));
	if (!status) {
		return snprintf((char *)buf, buf_size, "Error: Out of memory\n");
	}

	memset(status, 0, sizeof(*status));
	int ret = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface,
	                   status, sizeof(*status));

	if (ret < 0) {
		k_free(status);
		return snprintf((char *)buf, buf_size, "N/A\n");
	}

	int len = snprintf((char *)buf, buf_size, "%d dBm\n", status->rssi);
	k_free(status);
	return len;
}

static void setup_demo_filesystem(void)
{
	int ret;

	/* Initialize sysfs */
	ret = ninep_sysfs_init(&g_sysfs, g_sysfs_entries,
	                        ARRAY_SIZE(g_sysfs_entries));
	if (ret < 0) {
		LOG_ERR("Failed to initialize sysfs: %d", ret);
		return;
	}

	/* Register demo files */
	ninep_sysfs_register_file(&g_sysfs, "/hello.txt", gen_static,
	                           (void *)hello_content);
	ninep_sysfs_register_file(&g_sysfs, "/readme.txt", gen_static,
	                           (void *)readme_content);

	/* Register docs directory and files */
	ninep_sysfs_register_dir(&g_sysfs, "/docs");
	ninep_sysfs_register_file(&g_sysfs, "/docs/doc1.txt", gen_static,
	                           (void *)doc1_content);
	ninep_sysfs_register_file(&g_sysfs, "/docs/doc2.txt", gen_static,
	                           (void *)doc2_content);

	/* Register /sys directory */
	ninep_sysfs_register_dir(&g_sysfs, "/sys");

	/* Register system information files */
	ninep_sysfs_register_file(&g_sysfs, "/sys/uptime", gen_sys_uptime, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/sys/version", gen_sys_version, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/sys/board", gen_sys_board, NULL);

	/* Register /net directory */
	ninep_sysfs_register_dir(&g_sysfs, "/net");

	/* Register network information files */
	ninep_sysfs_register_file(&g_sysfs, "/net/interfaces", gen_net_interfaces, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/net/stats", gen_net_stats, NULL);

	/* Register /wifi directory */
	ninep_sysfs_register_dir(&g_sysfs, "/wifi");

	/* Register WiFi information files */
	ninep_sysfs_register_file(&g_sysfs, "/wifi/status", gen_wifi_status, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/wifi/rssi", gen_wifi_rssi, NULL);

	LOG_INF("Filesystem initialized");
	LOG_INF("Demo files:");
	LOG_INF("  /hello.txt");
	LOG_INF("  /readme.txt");
	LOG_INF("  /docs/doc1.txt");
	LOG_INF("  /docs/doc2.txt");
	LOG_INF("System info:");
	LOG_INF("  /sys/uptime   - system uptime");
	LOG_INF("  /sys/version  - kernel version");
	LOG_INF("  /sys/board    - board name");
	LOG_INF("Network info:");
	LOG_INF("  /net/interfaces - network interface details");
	LOG_INF("  /net/stats      - network statistics");
	LOG_INF("WiFi info:");
	LOG_INF("  /wifi/status    - WiFi connection status");
	LOG_INF("  /wifi/rssi      - signal strength");
}

static void print_network_info(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface available");
		return;
	}

	LOG_INF("Network interface: %p", iface);

	/* Print IPv4 addresses */
	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4) {
		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (ipv4->unicast[i].ipv4.addr_state != NET_ADDR_ANY_STATE) {
				char addr_str[NET_IPV4_ADDR_LEN];

				net_addr_ntop(AF_INET,
				              &ipv4->unicast[i].ipv4.address.in_addr,
				              addr_str, sizeof(addr_str));
				LOG_INF("  IPv4 address: %s", addr_str);
			}
		}
	}
}

int main(void)
{
	int ret;

	LOG_INF("=== 9P TCP Server ===");

	/* Wait a bit for network to come up */
	k_sleep(K_SECONDS(1));

	print_network_info();

	/* Setup sysfs */
	setup_demo_filesystem();

	/* Configure TCP transport */
	tcp_config.port = 564;  /* Standard 9P port */
	tcp_config.rx_buf_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;

	/* Initialize transport */
	ret = ninep_tcp_transport_init(&g_transport, &tcp_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize TCP transport: %d", ret);
		return -1;
	}

	/* Configure server */
	server_config.fs_ops = ninep_sysfs_get_ops();
	server_config.fs_ctx = &g_sysfs;
	server_config.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	server_config.version = "9P2000";

	/* Initialize server */
	ret = ninep_server_init(&g_server, &server_config, &g_transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize server: %d", ret);
		return -1;
	}

	/* Start server */
	ret = ninep_server_start(&g_server);
	if (ret < 0) {
		LOG_ERR("Failed to start server: %d", ret);
		return -1;
	}

	LOG_INF("9P server listening on tcp!192.0.2.1!564");
	LOG_INF("Connect with: 9p -a tcp!192.0.2.1!564 ls /");

	/* Server runs in background via transport callbacks.
	 * Shell runs automatically on console.
	 * Just return and let the system run.
	 */
	return 0;
}
