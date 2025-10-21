/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 *
 * 9P CoAP Cloud Client - Serves filesystem from behind NAT via cloud
 *
 * This sample demonstrates a device that connects to a cloud-hosted CoAP
 * server to serve 9P filesystems remotely, even when behind NAT/firewall.
 *
 * The device initiates a connection to the cloud using CoAP Observe,
 * enabling bidirectional communication without port forwarding.
 *
 * Architecture:
 *   [Device behind NAT] --Observe--> [Cloud CoAP Server] <--9P--> [Remote Users]
 */

#include <zephyr/kernel.h>
#include <zephyr/9p/server.h>
#include <zephyr/9p/sysfs.h>
#include <zephyr/9p/transport_coap_client.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/random/random.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * Cloud CoAP server configuration
 *
 * In production, this would be:
 *   - A cloud-hosted CoAP server (AWS IoT, Azure IoT, custom server)
 *   - Running on a public IP or domain name
 *   - Handling Observe registrations from devices
 *   - Proxying 9P requests from users to devices
 *
 * For testing:
 *   - Run a local CoAP server on your development machine
 *   - Or use CoAP cloud platforms like ThingsBoard, AWS IoT, etc.
 */
#define CLOUD_COAP_HOST "192.168.1.100"  /* Replace with your cloud IP */
#define CLOUD_COAP_PORT 5683

static struct ninep_transport g_transport;
static struct ninep_server g_server;
static struct ninep_sysfs g_sysfs;
static struct ninep_sysfs_entry g_sysfs_entries[32];
static struct ninep_transport_coap_client_config coap_config;
static struct ninep_server_config server_config;
static struct sockaddr_in cloud_addr;
static char device_id[16];

/* Static content for demo files */
static const char *hello_content = "Hello from 9P device behind NAT!\n";
static const char *readme_content =
	"9P Server via Cloud (NAT Traversal)\n"
	"====================================\n\n"
	"This device is running behind NAT/firewall, but you can still\n"
	"access its filesystem remotely via a cloud-hosted CoAP gateway.\n\n"
	"How it works:\n"
	"1. Device connects to cloud CoAP server (Observe)\n"
	"2. Cloud receives 9P requests from remote users\n"
	"3. Cloud forwards requests to device via Observe notifications\n"
	"4. Device processes and sends responses back\n"
	"5. Cloud relays responses to users\n\n"
	"No port forwarding needed!\n\n"
	"Device Info:\n"
	"  Location: Behind NAT/Firewall\n"
	"  Protocol: 9P over CoAP with Observe (RFC 7641)\n"
	"  Cloud:    CoAP gateway\n\n"
	"Try:\n"
	"  mount -t 9p -o trans=tcp cloud.example.com:564 /mnt/device\n";

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
	int len = snprintf((char *)buf, buf_size,
	                   "%lld ms\n", uptime_ms);
	return len;
}

/* Generator for /sys/device_id */
static int gen_device_id(uint8_t *buf, size_t buf_size,
                         uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	int len = snprintf((char *)buf, buf_size, "%s\n", device_id);
	return len;
}

/* Generator for /sys/location */
static int gen_location(uint8_t *buf, size_t buf_size,
                        uint64_t offset, void *ctx)
{
	ARG_UNUSED(ctx);

	if (offset > 0) {
		return 0;
	}

	int len = snprintf((char *)buf, buf_size, "Behind NAT/Firewall\n");
	return len;
}

/* Generator for /net/ip */
static int gen_net_ip(uint8_t *buf, size_t buf_size,
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

	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
	if (!ipv4) {
		return snprintf((char *)buf, buf_size, "N/A\n");
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (ipv4->unicast[i].ipv4.addr_state != NET_ADDR_ANY_STATE) {
			char addr_str[NET_IPV4_ADDR_LEN];
			net_addr_ntop(AF_INET,
			              &ipv4->unicast[i].ipv4.address.in_addr,
			              addr_str, sizeof(addr_str));
			return snprintf((char *)buf, buf_size, "%s (private)\n", addr_str);
		}
	}

	return snprintf((char *)buf, buf_size, "N/A\n");
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

	/* Register /sys directory and files */
	ninep_sysfs_register_dir(&g_sysfs, "/sys");
	ninep_sysfs_register_file(&g_sysfs, "/sys/uptime", gen_sys_uptime, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/sys/device_id", gen_device_id, NULL);
	ninep_sysfs_register_file(&g_sysfs, "/sys/location", gen_location, NULL);

	/* Register /net directory and files */
	ninep_sysfs_register_dir(&g_sysfs, "/net");
	ninep_sysfs_register_file(&g_sysfs, "/net/ip", gen_net_ip, NULL);

	LOG_INF("Filesystem initialized");
	LOG_INF("  /hello.txt    - greeting");
	LOG_INF("  /readme.txt   - info about this setup");
	LOG_INF("  /sys/uptime   - system uptime");
	LOG_INF("  /sys/device_id - unique device identifier");
	LOG_INF("  /sys/location - device location info");
	LOG_INF("  /net/ip       - device IP (private)");
}

/* Generate unique device ID */
static void generate_device_id(void)
{
	uint32_t random = sys_rand32_get();
	snprintf(device_id, sizeof(device_id), "dev%08x", random);
	LOG_INF("Device ID: %s", device_id);
}

static void wait_for_network(void)
{
	struct net_if *iface = net_if_get_default();
	int retry = 0;

	LOG_INF("Waiting for network connectivity...");
	LOG_INF("Use shell to connect: wifi connect -s \"SSID\" -k 3 -p \"password\"");

	while (!net_if_is_up(iface) || !net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED)) {
		k_sleep(K_SECONDS(1));
		retry++;

		if (retry % 10 == 0) {
			LOG_INF("Still waiting... (%d seconds)", retry);
		}
	}

	/* Print IP address */
	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
	if (ipv4) {
		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (ipv4->unicast[i].ipv4.addr_state != NET_ADDR_ANY_STATE) {
				char addr_str[NET_IPV4_ADDR_LEN];
				net_addr_ntop(AF_INET,
				              &ipv4->unicast[i].ipv4.address.in_addr,
				              addr_str, sizeof(addr_str));
				LOG_INF("Network ready - IP: %s", addr_str);
			}
		}
	}
}

int main(void)
{
	int ret;

	LOG_INF("=== 9P CoAP Cloud Client (NAT Traversal) ===");

	/* Generate unique device ID */
	generate_device_id();

	/* Wait for network */
	k_sleep(K_SECONDS(2));
	wait_for_network();

	/* Setup filesystem */
	setup_demo_filesystem();

	/* Configure cloud CoAP server address */
	memset(&cloud_addr, 0, sizeof(cloud_addr));
	cloud_addr.sin_family = AF_INET;
	cloud_addr.sin_port = htons(CLOUD_COAP_PORT);

	ret = zsock_inet_pton(AF_INET, CLOUD_COAP_HOST, &cloud_addr.sin_addr);
	if (ret != 1) {
		LOG_ERR("Invalid cloud server address: %s", CLOUD_COAP_HOST);
		return -1;
	}

	LOG_INF("Cloud CoAP server: %s:%d", CLOUD_COAP_HOST, CLOUD_COAP_PORT);

	/* Configure CoAP client transport */
	coap_config.server_addr = (struct sockaddr *)&cloud_addr;
	coap_config.server_addr_len = sizeof(cloud_addr);
	coap_config.device_id = device_id;
	coap_config.inbox_path = NULL;   /* Use default: /device/{id}/inbox */
	coap_config.outbox_path = NULL;  /* Use default: /device/{id}/outbox */
	coap_config.rx_buf_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;

	/* Initialize CoAP client transport */
	ret = ninep_transport_coap_client_init(&g_transport, &coap_config, NULL, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to initialize CoAP client transport: %d", ret);
		return -1;
	}

	/* Configure 9P server */
	server_config.fs_ops = ninep_sysfs_get_ops();
	server_config.fs_ctx = &g_sysfs;
	server_config.max_message_size = CONFIG_NINEP_MAX_MESSAGE_SIZE;
	server_config.version = "9P2000";

	/* Initialize 9P server */
	ret = ninep_server_init(&g_server, &server_config, &g_transport);
	if (ret < 0) {
		LOG_ERR("Failed to initialize 9P server: %d", ret);
		return -1;
	}

	/* Start server (connects to cloud and registers for Observe) */
	ret = ninep_server_start(&g_server);
	if (ret < 0) {
		LOG_ERR("Failed to start server: %d", ret);
		return -1;
	}

	LOG_INF("=================================================");
	LOG_INF("Device is now accessible via cloud!");
	LOG_INF("Device ID: %s", device_id);
	LOG_INF("=================================================");
	LOG_INF("");
	LOG_INF("The device is listening for 9P requests from the cloud.");
	LOG_INF("Remote users can access this device's filesystem even though");
	LOG_INF("it's behind NAT/firewall.");
	LOG_INF("");
	LOG_INF("Note: You need to run a CoAP cloud gateway that:");
	LOG_INF("  1. Receives Observe registrations from devices");
	LOG_INF("  2. Accepts 9P connections from users");
	LOG_INF("  3. Forwards requests via Observe notifications");
	LOG_INF("  4. Relays responses back to users");

	/* Server and transport run in background.
	 * Main thread can do other work or just return.
	 */
	return 0;
}
