/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P Thread Server - Serves a synthetic filesystem over OpenThread (IPv6)
 *
 * This demonstrates 9P over Thread mesh networking using the existing TCP
 * transport. Thread provides IPv6 connectivity, so we simply use the TCP
 * transport which already supports IPv6.
 *
 * Architecture:
 *   9P Protocol → TCP Transport → IPv6 → OpenThread → IEEE 802.15.4
 *
 * This allows standard 9P clients to connect via the Thread mesh network,
 * and enables seamless routing through Thread border routers.
 */

#include <zephyr/kernel.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/transport_tcp.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/openthread.h>

#if defined(CONFIG_OPENTHREAD_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static struct ninep_transport g_transport;
static struct ninep_server g_server;
static struct ninep_sysfs g_sysfs;
static struct ninep_sysfs_entry g_sysfs_entries[32];
static struct ninep_tcp_config tcp_config;
static struct ninep_server_config server_config;

/* Static content for demo files */
static const char *hello_content = "Hello from Zephyr 9P over Thread!\n";
static const char *readme_content =
	"9P Server on Zephyr RTOS over OpenThread\n"
	"==========================================\n\n"
	"This is a demonstration 9P server running on Zephyr with Thread.\n"
	"It serves both static demo files and dynamic Thread mesh info.\n\n"
	"Architecture:\n"
	"  9P → TCP → IPv6 → OpenThread → IEEE 802.15.4\n\n"
	"Connection:\n"
	"  Port:     564 (standard 9P port)\n"
	"  Transport: IPv6 over Thread mesh\n"
	"  Protocol:  9P2000\n\n"
	"Demo Files:\n"
	"  /hello.txt  - Static greeting\n"
	"  /readme.txt - This file\n"
	"  /mesh/*     - Thread mesh information\n\n"
	"System Info (dynamic):\n"
	"  /sys/uptime  - System uptime\n"
	"  /sys/version - Kernel version\n"
	"  /sys/board   - Board name\n\n"
	"Thread Info:\n"
	"  /thread/role    - Thread role (Leader/Router/Child)\n"
	"  /thread/rloc    - Routing locator address\n"
	"  /thread/network - Network name\n\n"
	"Usage Examples:\n"
	"  # Get Thread IPv6 address from device logs, then:\n"
	"  9p -a tcp![fd11:22::1234]!564 ls /\n"
	"  9p -a tcp![fd11:22::1234]!564 read /thread/role\n"
	"  9p -a tcp![fd11:22::1234]!564 read /sys/uptime\n\n"
	"Setup:\n"
	"  1. Form Thread network or join existing one\n"
	"  2. Wait for IPv6 address assignment\n"
	"  3. Connect via 9P client using device's IPv6 address\n";

static const char *mesh1_content = "Thread Mesh Node 1\n";
static const char *mesh2_content = "Thread Mesh Node 2\n";

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

/* Generator for /sys/uptime */
static int gen_sys_uptime(uint8_t *buf, size_t buf_size,
                          uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
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
	                   "Zephyr %d.%d.%d\n9P4Z Thread Server\n",
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

/* Generator for /thread/role */
static int gen_thread_role(uint8_t *buf, size_t buf_size,
                           uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	struct openthread_context *ot_context = openthread_get_default_context();
	if (!ot_context) {
		return snprintf((char *)buf, buf_size, "Thread not available\n");
	}

	const char *role_str = "Unknown";
	/* Note: In a real implementation, you'd use OpenThread API to get role:
	 *   otDeviceRole role = otThreadGetDeviceRole(ot_context->instance);
	 * For this demo, we show the concept */

	int len = snprintf((char *)buf, buf_size,
	                   "Thread Role: %s\n"
	                   "(Use OpenThread CLI 'state' for actual role)\n",
	                   role_str);

	return len;
}

/* Generator for /thread/rloc */
static int gen_thread_rloc(uint8_t *buf, size_t buf_size,
                           uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	/* Get Thread RLOC16 from network interface */
	struct net_if *iface = net_if_get_default();
	if (!iface) {
		return snprintf((char *)buf, buf_size, "No network interface\n");
	}

	/* In real implementation, extract RLOC from Thread API */
	int len = snprintf((char *)buf, buf_size,
	                   "Thread RLOC information\n"
	                   "(Use OpenThread CLI 'rloc16' for actual value)\n");

	return len;
}

/* Generator for /thread/network */
static int gen_thread_network(uint8_t *buf, size_t buf_size,
                              uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	int len = snprintf((char *)buf, buf_size,
	                   "Thread Network Information\n"
	                   "Network Name: (see 'ot networkname')\n"
	                   "PAN ID: (see 'ot panid')\n"
	                   "Extended PAN ID: (see 'ot extpanid')\n"
	                   "Channel: (see 'ot channel')\n");

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

	/* Register /mesh directory and files */
	ninep_sysfs_register_dir(&g_sysfs, "/mesh");
	ninep_sysfs_register_file(&g_sysfs, "/mesh/node1.txt", gen_static,
	                           (void *)mesh1_content);
	ninep_sysfs_register_file(&g_sysfs, "/mesh/node2.txt", gen_static,
	                           (void *)mesh2_content);

	/* Register /sys directory */
	ninep_sysfs_register_dir(&g_sysfs, "/sys");
	ninep_sysfs_register_file(&g_sysfs, "/sys/uptime", gen_sys_uptime, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/sys/version", gen_sys_version, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/sys/board", gen_sys_board, NULL);

	/* Register /thread directory */
	ninep_sysfs_register_dir(&g_sysfs, "/thread");
	ninep_sysfs_register_file(&g_sysfs, "/thread/role", gen_thread_role, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/thread/rloc", gen_thread_rloc, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/thread/network", gen_thread_network, NULL);

	LOG_INF("Filesystem initialized");
	LOG_INF("Demo files:");
	LOG_INF("  /hello.txt");
	LOG_INF("  /readme.txt");
	LOG_INF("  /mesh/node1.txt");
	LOG_INF("  /mesh/node2.txt");
	LOG_INF("System info:");
	LOG_INF("  /sys/uptime   - system uptime");
	LOG_INF("  /sys/version  - kernel version");
	LOG_INF("  /sys/board    - board name");
	LOG_INF("Thread info:");
	LOG_INF("  /thread/role    - Thread device role");
	LOG_INF("  /thread/rloc    - Routing locator");
	LOG_INF("  /thread/network - Network information");
}

static void print_network_info(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		LOG_ERR("No network interface available");
		return;
	}

	LOG_INF("Network interface: %p", iface);

	/* Print IPv6 addresses (Thread uses IPv6) */
	struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;

	if (ipv6) {
		for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
			if (ipv6->unicast[i].addr_state != NET_ADDR_ANY_STATE) {
				char addr_str[NET_IPV6_ADDR_LEN];

				net_addr_ntop(AF_INET6,
				              &ipv6->unicast[i].address.in6_addr,
				              addr_str, sizeof(addr_str));
				LOG_INF("  IPv6 address: %s", addr_str);
			}
		}
	}
}

static void wait_for_thread_ready(void)
{
	LOG_INF("Waiting for Thread network...");

	/* Wait for network interface to be up and have IPv6 address */
	struct net_if *iface = net_if_get_default();
	int retries = 30; /* Wait up to 30 seconds */

	while (retries > 0) {
		if (iface && net_if_is_up(iface)) {
			struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;

			if (ipv6) {
				for (int i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
					if (ipv6->unicast[i].addr_state == NET_ADDR_PREFERRED) {
						LOG_INF("Thread network ready!");
						return;
					}
				}
			}
		}

		k_sleep(K_SECONDS(1));
		retries--;
	}

	LOG_WRN("Thread network not fully ready, continuing anyway...");
}

int main(void)
{
	int ret;

	LOG_INF("=== 9P Thread Server ===");
	LOG_INF("9P over OpenThread (IPv6 mesh networking)");

	/* Wait for Thread to initialize and get IPv6 address */
	wait_for_thread_ready();

	print_network_info();

	/* Setup sysfs */
	setup_demo_filesystem();

	/* Configure TCP transport (works over IPv6/Thread) */
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

	LOG_INF("9P server ready on Thread mesh network!");
	LOG_INF("  Protocol: 9P2000");
	LOG_INF("  Transport: TCP over IPv6 over OpenThread");
	LOG_INF("  Port: 564");
	LOG_INF("");
	LOG_INF("Connect from any device on the Thread mesh:");
	LOG_INF("  9p -a tcp![YOUR_IPV6_ADDR]!564 ls /");
	LOG_INF("");
	LOG_INF("Use OpenThread CLI to check network status:");
	LOG_INF("  ot state     - Device role");
	LOG_INF("  ot ipaddr    - IPv6 addresses");
	LOG_INF("  ot rloc16    - Routing locator");

	/* Server runs in background via transport callbacks */
	return 0;
}
