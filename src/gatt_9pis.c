/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/gatt_9pis.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_9pis, CONFIG_NINEP_LOG_LEVEL);

/* 9P Information Service UUID: 39500001-feed-4a91-ba88-a1e0f6e4c001 */
#define BT_UUID_9PIS_VAL \
	BT_UUID_128_ENCODE(0x39500001, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)

/* Characteristic UUIDs */
#define BT_UUID_9PIS_DESC_VAL \
	BT_UUID_128_ENCODE(0x39500002, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)

#define BT_UUID_9PIS_FEATURES_VAL \
	BT_UUID_128_ENCODE(0x39500003, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)

#define BT_UUID_9PIS_TRANSPORT_VAL \
	BT_UUID_128_ENCODE(0x39500004, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)

#define BT_UUID_9PIS_APPSTORE_VAL \
	BT_UUID_128_ENCODE(0x39500005, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)

#define BT_UUID_9PIS_VERSION_VAL \
	BT_UUID_128_ENCODE(0x39500006, 0xfeed, 0x4a91, 0xba88, 0xa1e0f6e4c001)

/* Define UUIDs */
static struct bt_uuid_128 uuid_9pis = BT_UUID_INIT_128(BT_UUID_9PIS_VAL);
static struct bt_uuid_128 uuid_9pis_desc = BT_UUID_INIT_128(BT_UUID_9PIS_DESC_VAL);
static struct bt_uuid_128 uuid_9pis_features = BT_UUID_INIT_128(BT_UUID_9PIS_FEATURES_VAL);
static struct bt_uuid_128 uuid_9pis_transport = BT_UUID_INIT_128(BT_UUID_9PIS_TRANSPORT_VAL);
static struct bt_uuid_128 uuid_9pis_appstore = BT_UUID_INIT_128(BT_UUID_9PIS_APPSTORE_VAL);
static struct bt_uuid_128 uuid_9pis_version = BT_UUID_INIT_128(BT_UUID_9PIS_VERSION_VAL);

/* Configuration storage */
static const struct ninep_9pis_config *service_config;

/* Read callback for service description */
static ssize_t read_service_description(struct bt_conn *conn,
                                         const struct bt_gatt_attr *attr,
                                         void *buf, uint16_t len, uint16_t offset)
{
	const char *value = service_config->service_description;

	if (!value) {
		value = "9P Server";
	}

	LOG_DBG("Read service description: %s", value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}

/* Read callback for service features */
static ssize_t read_service_features(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset)
{
	const char *value = service_config->service_features;

	if (!value) {
		value = "file-sharing";
	}

	LOG_DBG("Read service features: %s", value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}

/* Read callback for transport information */
static ssize_t read_transport_info(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset)
{
	const char *value = service_config->transport_info;

	if (!value) {
		value = "l2cap:psm=0x0009,mtu=4096";
	}

	LOG_DBG("Read transport info: %s", value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}

/* Read callback for app store link */
static ssize_t read_app_store_link(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset)
{
	const char *value = service_config->app_store_link;

	if (!value) {
		value = "https://9p4z.org/clients";
	}

	LOG_DBG("Read app store link: %s", value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}

/* Read callback for protocol version */
static ssize_t read_protocol_version(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset)
{
	const char *value = service_config->protocol_version;

	if (!value) {
		value = "9P2000;9p4z;1.0.0";
	}

	LOG_DBG("Read protocol version: %s", value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, strlen(value));
}

/* 9PIS GATT Service Declaration */
BT_GATT_SERVICE_DEFINE(ninep_9pis_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_9pis),

	/* Service Description Characteristic */
	BT_GATT_CHARACTERISTIC(&uuid_9pis_desc.uuid,
	                       BT_GATT_CHRC_READ,
	                       BT_GATT_PERM_READ,
	                       read_service_description, NULL, NULL),

	/* Service Features Characteristic */
	BT_GATT_CHARACTERISTIC(&uuid_9pis_features.uuid,
	                       BT_GATT_CHRC_READ,
	                       BT_GATT_PERM_READ,
	                       read_service_features, NULL, NULL),

	/* Transport Information Characteristic */
	BT_GATT_CHARACTERISTIC(&uuid_9pis_transport.uuid,
	                       BT_GATT_CHRC_READ,
	                       BT_GATT_PERM_READ,
	                       read_transport_info, NULL, NULL),

	/* App Store Link Characteristic */
	BT_GATT_CHARACTERISTIC(&uuid_9pis_appstore.uuid,
	                       BT_GATT_CHRC_READ,
	                       BT_GATT_PERM_READ,
	                       read_app_store_link, NULL, NULL),

	/* Protocol Version Characteristic */
	BT_GATT_CHARACTERISTIC(&uuid_9pis_version.uuid,
	                       BT_GATT_CHRC_READ,
	                       BT_GATT_PERM_READ,
	                       read_protocol_version, NULL, NULL),
);

int ninep_9pis_init(const struct ninep_9pis_config *config)
{
	if (!config) {
		LOG_ERR("Invalid configuration");
		return -EINVAL;
	}

	/* Validate string lengths */
	if (config->service_description &&
	    strlen(config->service_description) > 64) {
		LOG_ERR("Service description too long (max 64 bytes)");
		return -EINVAL;
	}

	if (config->service_features &&
	    strlen(config->service_features) > 128) {
		LOG_ERR("Service features too long (max 128 bytes)");
		return -EINVAL;
	}

	if (config->transport_info &&
	    strlen(config->transport_info) > 64) {
		LOG_ERR("Transport info too long (max 64 bytes)");
		return -EINVAL;
	}

	if (config->app_store_link &&
	    strlen(config->app_store_link) > 256) {
		LOG_ERR("App store link too long (max 256 bytes)");
		return -EINVAL;
	}

	if (config->protocol_version &&
	    strlen(config->protocol_version) > 32) {
		LOG_ERR("Protocol version too long (max 32 bytes)");
		return -EINVAL;
	}

	/* Store configuration */
	service_config = config;

	LOG_INF("9P Information Service (9PIS) initialized");
	LOG_INF("  Description: %s",
	        config->service_description ?: "9P Server");
	LOG_INF("  Features: %s",
	        config->service_features ?: "file-sharing");
	LOG_INF("  Transport: %s",
	        config->transport_info ?: "l2cap:psm=0x0009,mtu=4096");
	LOG_INF("  App Link: %s",
	        config->app_store_link ?: "https://9p4z.org/clients");
	LOG_INF("  Version: %s",
	        config->protocol_version ?: "9P2000;9p4z;1.0.0");

	return 0;
}

const struct bt_uuid *ninep_9pis_get_uuid(void)
{
	return &uuid_9pis.uuid;
}
