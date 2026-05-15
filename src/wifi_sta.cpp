/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wifi_sta.h"

#include "sntp_sync.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
extern "C" {
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
}

LOG_MODULE_REGISTER(wifi_sta, LOG_LEVEL_INF);

#define NVS_WIFI_SSID_ID  1
#define NVS_WIFI_PSK_ID   2

static struct nvs_fs nvs_storage;

static struct net_mgmt_event_callback wifi_mgmt_cb;

static volatile bool wifi_connected = false;
static volatile bool pending_wifi_icon_update = false;
static volatile bool pending_ap_network_setup = false;

/* When credentials come from Kconfig and WiFi connects, save them to NVS
 * so the next boot auto-connects without rebuilding. */
static char pending_save_ssid[33];
static char pending_save_psk[65];
static bool pending_save_credentials = false;

static int nvs_init_storage(void)
{
	struct flash_pages_info info;
	const struct device *flash_dev = FIXED_PARTITION_DEVICE(storage_partition);

	if (!device_is_ready(flash_dev)) {
		LOG_ERR("NVS flash device not ready");
		return -ENODEV;
	}

	nvs_storage.flash_device = flash_dev;
	nvs_storage.offset = FIXED_PARTITION_OFFSET(storage_partition);

	int ret = flash_get_page_info_by_offs(flash_dev, nvs_storage.offset, &info);
	if (ret < 0) {
		LOG_ERR("Failed to get flash page info: %d", ret);
		return ret;
	}

	nvs_storage.sector_size = info.size;
	nvs_storage.sector_count = 3;

	ret = nvs_mount(&nvs_storage);
	if (ret < 0) {
		LOG_ERR("NVS mount failed: %d", ret);
		return ret;
	}

	LOG_INF("NVS initialized");
	return 0;
}

bool wifi_load_credentials(char *ssid, size_t ssid_size,
			   char *psk, size_t psk_size)
{
	if (nvs_read(&nvs_storage, NVS_WIFI_SSID_ID, ssid, ssid_size) <= 0) {
		return false;
	}
	if (nvs_read(&nvs_storage, NVS_WIFI_PSK_ID, psk, psk_size) <= 0) {
		return false;
	}
	LOG_INF("Loaded WiFi credentials from NVS (SSID: %s)", ssid);
	return true;
}

int wifi_save_credentials(const char *ssid, const char *psk)
{
	ssize_t r1 = nvs_write(&nvs_storage, NVS_WIFI_SSID_ID, ssid, strlen(ssid) + 1);
	ssize_t r2 = nvs_write(&nvs_storage, NVS_WIFI_PSK_ID, psk, strlen(psk) + 1);
	if (r1 < 0) {
		return (int)r1;
	}
	if (r2 < 0) {
		return (int)r2;
	}
	LOG_INF("WiFi credentials saved to NVS (SSID: %s)", ssid);
	return 0;
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
				    uint64_t mgmt_event,
				    struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		LOG_INF("WiFi connected!");
		if (pending_save_credentials) {
			pending_save_credentials = false;
			wifi_save_credentials(pending_save_ssid, pending_save_psk);
		}
		wifi_connected = true;
		pending_wifi_icon_update = true;
		/* Only kick the initial SNTP sync on the first connect.
		 * Later reassociations would otherwise reset the hourly
		 * cadence by triggering an unscheduled sync each time. */
		if (!sntp_is_synced()) {
			sntp_request_initial_sync();
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_INF("WiFi disconnected");
		wifi_connected = false;
		pending_wifi_icon_update = true;
		break;
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		LOG_INF("AP enable event fired");
		pending_ap_network_setup = true;
		break;
	case NET_EVENT_WIFI_AP_DISABLE_RESULT:
		LOG_INF("AP disable event fired");
		break;
	default:
		LOG_INF("WiFi event: 0x%llx", (unsigned long long)mgmt_event);
		break;
	}
}

int wifi_sta_init(void)
{
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
		NET_EVENT_WIFI_CONNECT_RESULT |
		NET_EVENT_WIFI_DISCONNECT_RESULT |
		NET_EVENT_WIFI_AP_ENABLE_RESULT |
		NET_EVENT_WIFI_AP_DISABLE_RESULT);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	return nvs_init_storage();
}

int wifi_connect(const char *ssid, const char *psk)
{
	struct net_if *iface = net_if_get_default();
	struct wifi_connect_req_params params = {};

	params.ssid = (const uint8_t *)ssid;
	params.ssid_length = strlen(ssid);
	params.psk = (const uint8_t *)psk;
	params.psk_length = strlen(psk);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;

	LOG_INF("Connecting to WiFi SSID: %s", ssid);
	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (ret < 0) {
		LOG_ERR("WiFi connect request failed: %d", ret);
	}
	return ret;
}

void wifi_arm_pending_save(const char *ssid, const char *psk)
{
	strncpy(pending_save_ssid, ssid, sizeof(pending_save_ssid) - 1);
	pending_save_ssid[sizeof(pending_save_ssid) - 1] = '\0';
	strncpy(pending_save_psk, psk, sizeof(pending_save_psk) - 1);
	pending_save_psk[sizeof(pending_save_psk) - 1] = '\0';
	pending_save_credentials = true;
}

bool wifi_is_connected(void)
{
	return wifi_connected;
}

bool wifi_take_pending_icon_update(void)
{
	if (pending_wifi_icon_update) {
		pending_wifi_icon_update = false;
		return true;
	}
	return false;
}

bool wifi_take_pending_ap_setup(void)
{
	if (pending_ap_network_setup) {
		pending_ap_network_setup = false;
		return true;
	}
	return false;
}
