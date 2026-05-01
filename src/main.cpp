/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
extern "C" {
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/sntp.h>
}

#include <errno.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(weather_sensor, LOG_LEVEL_INF);

/* ========================================================================
 * Hardware GPIO specs
 * ======================================================================== */

/* Display power control (GPIO7 on Reverse board) - MUST be HIGH before display init */
static const struct gpio_dt_spec display_power =
	GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(i2c_reg), enable_gpios, 0);

/* Backlight control (GPIO45 on Reverse board) - uses LED subsystem alias */
static const struct gpio_dt_spec backlight =
	GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);


/* ========================================================================
 * Device pointers
 * ======================================================================== */

/* Display device */
static const struct device *display_dev;

/* Sensor devices */
static const struct device *bme280;

/* Fuel gauge device */
static const struct device *fuel_gauge_dev;

/* ========================================================================
 * Sensor data (global, updated by sensors_read())
 * ======================================================================== */

static float temperature = 0.0f;
static float humidity = 0.0f;
static float pressure = 0.0f;

/* ========================================================================
 * Battery state
 * ======================================================================== */

static bool battery_available = false;
static uint8_t battery_soc = 100; /* State of charge %, mocked at 100% */

/* ========================================================================
 * UI constants
 * ======================================================================== */

#define STATUS_BAR_H    22
#define CONTENT_Y_START (STATUS_BAR_H + 4)
#define CONTENT_X_PAD   5

/* ========================================================================
 * LVGL objects
 * ======================================================================== */

/* Screen objects */
static lv_obj_t *screen_weather;
static lv_obj_t *screen_drift;

/* Screen array for indexed navigation. */
static const int NUM_SCREENS = 2;
static int current_screen = 0;
static lv_obj_t *screens[NUM_SCREENS];

/* Status-bar labels — one pair per screen. The same WiFi/battery state
 * is reflected on whichever screen the user happens to be viewing. */
static lv_obj_t *battery_labels[NUM_SCREENS];
static lv_obj_t *wifi_labels[NUM_SCREENS];

/* Weather screen labels */
static lv_obj_t *weather_time_label;
static lv_obj_t *weather_temp_value;
static lv_obj_t *weather_temp_unit;
static lv_obj_t *weather_hum_value;
static lv_obj_t *weather_hum_unit;
static lv_obj_t *weather_press_value;
static lv_obj_t *weather_press_unit;

/* Drift screen labels — one row per ring slot, plus a header. */
#define DRIFT_HISTORY_SIZE 8
static lv_obj_t *drift_title_label;
static lv_obj_t *drift_row_labels[DRIFT_HISTORY_SIZE];

/* Time counter (seconds since boot) */
static uint32_t seconds_counter = 0;

/* ========================================================================
 * Button navigation
 * ======================================================================== */

/* Flag set by button callback, consumed by main loop.
 * +1 = next screen, -1 = previous screen.
 * Using volatile to ensure visibility between ISR context and main loop. */
static volatile int pending_navigate = 0;

/* Flag set when D1 pressed — main loop starts provisioning SoftAP. */
static volatile bool pending_provisioning = false;

/* Software-side hold filter — additional defense against phantom presses
 * that sneak through the gpio-keys debounce. button_input_cb queues a
 * delayable work for BUTTON_HOLD_MS; if a release arrives before then,
 * we cancel the work and the press is dropped. Real human presses last
 * far longer than BUTTON_HOLD_MS so the work fires and the press is
 * dispatched as if it had arrived directly. */
#define BUTTON_HOLD_MS 80
static int button_pending_code = 0;             /* 0 = nothing pending */
static void button_hold_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(button_hold_work, button_hold_handler);

/* Flag set by WiFi event handler when AP becomes enabled — main loop
 * assigns the IP address and starts the DHCP server. */
static volatile bool pending_ap_network_setup = false;

/* Tracked by WiFi event handler; main loop pushes it to the LVGL icon. */
static volatile bool wifi_connected = false;
static volatile bool pending_wifi_icon_update = false;

/* SNTP time-of-day sync state.
 * - time_synced: false until the first successful sync; until then the time
 *   display falls back to uptime so the screen isn't blank at boot.
 * - seconds_since_last_sync: counts main-loop ticks since the last successful
 *   sync; main loop triggers a re-sync when it crosses TIME_SYNC_INTERVAL_S.
 * - pending_initial_sync: set by the WiFi connect event handler so the very
 *   first sync runs as soon as we have an IP, not on a fixed cadence boundary.
 */
static bool time_synced = false;
static uint32_t seconds_since_last_sync = 0;
static volatile bool pending_initial_sync = false;

/* Ring buffer of recent SNTP drift measurements (last DRIFT_HISTORY_SIZE
 * comparisons; the very first sync is excluded — there's nothing to
 * compare it against). Written by sntp_sync_time() and read by
 * update_drift_display() in the main loop. */
struct drift_entry {
	int64_t drift_ms;
	uint32_t interval_s;
	struct tm local_tm;
	bool valid;
};
static struct drift_entry drift_history[DRIFT_HISTORY_SIZE];
static int drift_head = 0;  /* next slot to overwrite */
static volatile bool pending_drift_screen_update = false;


/**
 * @brief Input event callback for hardware buttons
 *
 * Called by Zephyr input subsystem (synchronous mode).
 * We only set a flag here — LVGL calls happen in the main loop.
 */
static void button_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	if (evt->value == 1) {
		/* Press: queue dispatch for BUTTON_HOLD_MS from now. */
		button_pending_code = (int)evt->code;
		k_work_reschedule(&button_hold_work, K_MSEC(BUTTON_HOLD_MS));
	} else {
		/* Release: if dispatch hasn't fired yet (k_work_cancel_delayable
		 * returns the prior queued state), the press was too short — drop. */
		if (button_pending_code == (int)evt->code) {
			if (k_work_cancel_delayable(&button_hold_work) != 0) {
				LOG_INF("Phantom press rejected: code=%u",
					evt->code);
			}
			button_pending_code = 0;
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);

/**
 * @brief Delayed handler that runs BUTTON_HOLD_MS after a press if it
 * wasn't cancelled by a quick release. Sets the appropriate pending_*
 * flag for the main loop to act on.
 */
static void button_hold_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int code = button_pending_code;
	button_pending_code = 0;

	switch (code) {
	case INPUT_KEY_0:  /* D0 = UP = next screen */
		pending_navigate = 1;
		LOG_INF("Button D0 (UP) pressed");
		break;
	case INPUT_KEY_3:  /* D2 = DOWN = previous screen */
		pending_navigate = -1;
		LOG_INF("Button D2 (DOWN) pressed");
		break;
	case INPUT_KEY_1:  /* D1 = SET = start SoftAP provisioning */
		pending_provisioning = true;
		LOG_INF("Button D1 (SET) pressed - provisioning requested");
		break;
	default:
		LOG_INF("Unknown button code: %d", code);
		break;
	}
}

/**
 * @brief Navigate to the next or previous screen
 *
 * @param direction +1 for next, -1 for previous (wraps around)
 */
static void screen_navigate(int direction)
{
	current_screen = (current_screen + direction + NUM_SCREENS) % NUM_SCREENS;
	lv_screen_load(screens[current_screen]);
	LOG_INF("Switched to screen %d", current_screen);
}

/* ========================================================================
 * Hardware initialization (unchanged from before)
 * ======================================================================== */

/**
 * @brief Initialize display power (GPIO7)
 *
 * CRITICAL: This MUST be called before display_init()!
 * GPIO7 controls the display power rail and must be HIGH before
 * any communication with the ST7789V controller.
 *
 * @return 0 on success, negative errno on failure
 */
static int display_power_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&display_power)) {
		LOG_ERR("Display power GPIO not ready - display will not work!");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&display_power, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure display power: %d", ret);
		return ret;
	}

	gpio_pin_set_dt(&display_power, 1);
	LOG_INF("Display power enabled (GPIO%d)", display_power.pin);

	/* Wait for power rail to stabilize */
	k_msleep(10);

	return 0;
}

/**
 * @brief Initialize backlight (GPIO45)
 *
 * @return 0 on success, negative errno on failure
 */
static int backlight_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&backlight)) {
		LOG_ERR("Backlight GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure backlight: %d", ret);
		return ret;
	}

	gpio_pin_set_dt(&backlight, 1);
	LOG_INF("Backlight enabled (GPIO%d)", backlight.pin);

	return 0;
}

/**
 * @brief Initialize the display subsystem
 *
 * @return 0 on success, negative errno on failure
 */
static int display_init(void)
{
	int ret;

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	LOG_INF("Display device initialized: %s", display_dev->name);

	/* Turn off display blanking - ST7789V may have it on by default */
	ret = display_blanking_off(display_dev);
	if (ret < 0) {
		LOG_ERR("Failed to turn off display blanking: %d", ret);
		return ret;
	}
	LOG_INF("Display blanking disabled");

	return 0;
}

/* ========================================================================
 * Sensor initialization and reading (unchanged logic)
 * ======================================================================== */

/**
 * @brief Initialize sensors
 *
 * @return 0 on success, negative errno on failure
 */
static int sensors_init(void)
{
	LOG_INF("=== SENSOR INITIALIZATION START ===");

	/* BME280 initialization */
	LOG_INF("Looking up BME280 device...");
	bme280 = DEVICE_DT_GET_ONE(bosch_bme280);

	if (!device_is_ready(bme280)) {
		LOG_ERR("BME280: Device not ready!");
		return -ENODEV;
	}
	LOG_INF("BME280: Device ready - %s", bme280->name);

	LOG_INF("=== SENSOR INITIALIZATION COMPLETE ===");
	return 0;
}

/**
 * @brief Read sensors
 *
 * @return 0 on success, negative errno on failure
 */
static int sensors_read(void)
{
	struct sensor_value val;
	int ret;

	LOG_INF("=== SENSOR READ START ===");

	/* Read BME280 */
	LOG_INF("BME280: Fetching sample...");
	ret = sensor_sample_fetch(bme280);
	if (ret < 0) {
		LOG_ERR("BME280: sample_fetch failed: %d", ret);
	} else {
		LOG_INF("BME280: Sample fetch OK");

		sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &val);
		temperature = sensor_value_to_float(&val);
		LOG_INF("BME280: Temperature = %.2f\xC2\xB0""C (val1=%d val2=%d)",
			(double)temperature, val.val1, val.val2);

		sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &val);
		humidity = sensor_value_to_float(&val);
		LOG_INF("BME280: Humidity = %.2f%% (val1=%d val2=%d)",
			(double)humidity, val.val1, val.val2);

		sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &val);
		pressure = sensor_value_to_float(&val) * 10.0f;  /* kPa to hPa */
		LOG_INF("BME280: Pressure = %.2f hPa (val1=%d val2=%d)",
			(double)pressure, val.val1, val.val2);
	}

	LOG_INF("=== SENSOR READ COMPLETE ===");
	return 0;
}

/* ========================================================================
 * Fuel gauge (battery monitoring)
 * ======================================================================== */

/**
 * @brief Initialize the MAX17048 fuel gauge
 *
 * Called after display_power_init() since GPIO7 powers the I2C bus.
 *
 * @return 0 on success, negative errno on failure
 */
static int fuel_gauge_init_device(void)
{
	fuel_gauge_dev = DEVICE_DT_GET_ONE(maxim_max17048);

	if (!device_is_ready(fuel_gauge_dev)) {
		LOG_WRN("MAX17048: not ready (will use mocked battery)");
		battery_available = false;
		return -ENODEV;
	}

	LOG_INF("MAX17048: Device ready - %s", fuel_gauge_dev->name);
	battery_available = true;
	return 0;
}

/**
 * @brief Read battery state of charge from fuel gauge
 */
static void battery_read(void)
{
	if (!battery_available) {
		return;
	}

	union fuel_gauge_prop_val val;
	int ret = fuel_gauge_get_prop(fuel_gauge_dev,
				      FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE, &val);
	if (ret < 0) {
		LOG_WRN("Failed to read battery SOC: %d", ret);
		return;
	}

	battery_soc = val.relative_state_of_charge;
	LOG_INF("Battery SOC: %u%%", battery_soc);
}

/**
 * @brief Get the battery symbol based on state of charge
 */
static const char *battery_symbol(uint8_t soc)
{
	if (soc >= 80) {
		return LV_SYMBOL_BATTERY_FULL;
	} else if (soc >= 60) {
		return LV_SYMBOL_BATTERY_3;
	} else if (soc >= 40) {
		return LV_SYMBOL_BATTERY_2;
	} else if (soc >= 20) {
		return LV_SYMBOL_BATTERY_1;
	} else {
		return LV_SYMBOL_BATTERY_EMPTY;
	}
}

/**
 * @brief Update the battery indicator in the status bar
 */
static void update_battery_display(void)
{
	const char *sym = battery_symbol(battery_soc);
	for (int i = 0; i < NUM_SCREENS; i++) {
		lv_label_set_text(battery_labels[i], sym);
	}
}

/* ========================================================================
 * NVS credential storage (Step 2)
 * ======================================================================== */

#define NVS_WIFI_SSID_ID  1
#define NVS_WIFI_PSK_ID   2

static struct nvs_fs nvs_storage;

/**
 * @brief Mount the NVS filesystem on the storage partition.
 */
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

/**
 * @brief Load WiFi credentials from NVS.
 *
 * @return true on success, false if credentials not stored.
 */
static bool wifi_load_credentials(char *ssid, size_t ssid_size,
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

/**
 * @brief Save WiFi credentials to NVS.
 */
static void wifi_save_credentials(const char *ssid, const char *psk)
{
	nvs_write(&nvs_storage, NVS_WIFI_SSID_ID, ssid, strlen(ssid) + 1);
	nvs_write(&nvs_storage, NVS_WIFI_PSK_ID, psk, strlen(psk) + 1);
	LOG_INF("WiFi credentials saved to NVS (SSID: %s)", ssid);
}

/* ========================================================================
 * WiFi connection (Step 3)
 * ======================================================================== */

static struct net_mgmt_event_callback wifi_mgmt_cb;

/* When credentials come from Kconfig and WiFi connects, save them to NVS
 * so the next boot auto-connects without rebuilding. */
static char pending_save_ssid[33];
static char pending_save_psk[65];
static bool pending_save_credentials = false;

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
		pending_initial_sync = true;
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

/**
 * @brief Connect to WiFi using the given SSID/password (WPA2-PSK).
 */
static int wifi_connect(const char *ssid, const char *psk)
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

/* ========================================================================
 * SNTP wall-clock sync
 * ========================================================================
 *
 * sntp_simple() blocks for up to its timeout (5 s here) including DNS
 * resolution. That's an acceptable stall for a 1-hour cadence — never
 * during user interaction — so we run it inline from the main loop
 * rather than spawning a workqueue.
 */

#define SNTP_SERVER             "pool.ntp.org"
#define SNTP_TIMEOUT_MS         5000
#define TIME_SYNC_INTERVAL_S    3600  /* 1 hour */

/* Convert an SNTP fractional-second value (Q32.32 fixed-point) to nanoseconds. */
static inline uint32_t sntp_fraction_to_ns(uint64_t fraction)
{
	return (uint32_t)((fraction * NSEC_PER_SEC) >> 32);
}

static int sntp_sync_time(void)
{
	struct sntp_time st;
	int ret = sntp_simple(SNTP_SERVER, SNTP_TIMEOUT_MS, &st);
	if (ret < 0) {
		LOG_WRN("SNTP sync failed: %d (keeping previous time)", ret);
		return ret;
	}

	struct timespec new_ts = {
		.tv_sec  = (time_t)st.seconds,
		.tv_nsec = sntp_fraction_to_ns(st.fraction),
	};

	if (time_synced) {
		struct timespec before;
		clock_gettime(CLOCK_REALTIME, &before);
		int64_t before_ms = (int64_t)before.tv_sec * 1000 +
				    before.tv_nsec / 1000000;
		int64_t new_ms    = (int64_t)new_ts.tv_sec * 1000 +
				    new_ts.tv_nsec / 1000000;
		int64_t drift_ms  = before_ms - new_ms;
		LOG_INF("SNTP synced: drift = %+lld ms over %u s",
			(long long)drift_ms, seconds_since_last_sync);

		struct drift_entry *slot = &drift_history[drift_head];
		slot->drift_ms = drift_ms;
		slot->interval_s = seconds_since_last_sync;
		localtime_r(&new_ts.tv_sec, &slot->local_tm);
		slot->valid = true;
		drift_head = (drift_head + 1) % DRIFT_HISTORY_SIZE;
		pending_drift_screen_update = true;
	} else {
		struct tm utc;
		gmtime_r(&new_ts.tv_sec, &utc);
		LOG_INF("SNTP synced: initial value %04d-%02d-%02d %02d:%02d:%02d UTC",
			utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
			utc.tm_hour, utc.tm_min, utc.tm_sec);
	}

	if (clock_settime(CLOCK_REALTIME, &new_ts) < 0) {
		LOG_ERR("clock_settime failed: %d", errno);
		return -errno;
	}

	time_synced = true;
	seconds_since_last_sync = 0;
	return 0;
}

/* ========================================================================
 * SoftAP provisioning (Step 5 — AP mode only, no DHCP/HTTP server yet)
 * ======================================================================== */

#define PROVISIONING_AP_SSID "WeatherSensor"

static bool provisioning_active = false;

/**
 * @brief Start the SoftAP so a phone can see the "WeatherSensor" network.
 *
 * Step 5 scope is intentionally minimal: just enable AP mode and log.
 * DHCP server and HTTP provisioning page come in later steps.
 */
/**
 * @brief Configure AP interface IP + start DHCP server.
 * Called from the AP_ENABLE_RESULT event handler once the AP is actually up.
 */
static bool ap_network_configured = false;

/* Forward declaration — defined in the HTTP server section below. */
static void http_server_start(void);

static void provisioning_configure_ap_network(void)
{
	if (ap_network_configured) {
		return;
	}
	struct net_if *ap_iface = net_if_get_default();
	if (!ap_iface) {
		LOG_ERR("AP interface not available in event handler");
		return;
	}
	ap_network_configured = true;

	struct in_addr ap_addr, netmask, pool_start;
	net_addr_pton(AF_INET, "192.168.4.1", &ap_addr);
	net_addr_pton(AF_INET, "255.255.255.0", &netmask);
	net_addr_pton(AF_INET, "192.168.4.10", &pool_start);

	net_if_ipv4_set_gw(ap_iface, &ap_addr);
	if (!net_if_ipv4_addr_add(ap_iface, &ap_addr, NET_ADDR_MANUAL, 0)) {
		LOG_ERR("Failed to set AP IP address");
		return;
	}
	if (!net_if_ipv4_set_netmask_by_addr(ap_iface, &ap_addr, &netmask)) {
		LOG_ERR("Failed to set AP netmask");
	}

	int ret = net_dhcpv4_server_start(ap_iface, &pool_start);
	if (ret < 0) {
		LOG_ERR("DHCP server start failed: %d", ret);
	} else {
		LOG_INF("DHCP server started (pool: 192.168.4.10 - .13)");
	}

	http_server_start();

	LOG_INF("AP IP: 192.168.4.1 — connect to '%s' and open http://192.168.4.1/",
		PROVISIONING_AP_SSID);
}

/* ========================================================================
 * Minimal HTTP server for SoftAP provisioning
 * ========================================================================
 *
 * Serves a single HTML form at http://192.168.4.1/ while the AP is up.
 * GET /         → form with SSID + password fields
 * POST /connect → URL-decode the form body, persist credentials to NVS,
 *                 then sys_reboot(SYS_REBOOT_COLD). The new credentials
 *                 take effect on the clean cold boot rather than via a
 *                 fragile runtime AP→STA mode transition.
 */

#define HTTP_PORT      80
#define HTTP_STACK_SIZE 3072
#define HTTP_RECV_BUF  2048

K_THREAD_STACK_DEFINE(http_stack, HTTP_STACK_SIZE);
static struct k_thread http_thread_data;
static k_tid_t http_thread_tid = NULL;
static volatile bool http_server_run = false;

/* Credentials received via POST /connect, waiting for main loop to
 * persist them to NVS and reboot. We reboot rather than transition the
 * WiFi driver from AP→STA at runtime because the latter is fragile on
 * ESP32-S2 with Zephyr 4.4 — see plan velvet-zooming-hanrahan.md. */
static char pending_apply_ssid[33];
static char pending_apply_psk[65];
static volatile bool pending_reboot = false;

static const char PROVISIONING_HTML[] =
	"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>WeatherSensor setup</title>"
	"<style>body{font-family:sans-serif;margin:2em;max-width:420px}"
	"label{display:block;margin-top:1em}input{width:100%;padding:.5em;"
	"font-size:1em;box-sizing:border-box}button{margin-top:1.5em;padding:"
	".7em 1.5em;font-size:1em}</style></head><body>"
	"<h1>WeatherSensor WiFi setup</h1>"
	"<form method=\"POST\" action=\"/connect\">"
	"<label>SSID<input name=\"ssid\" required></label>"
	"<label>Password<input name=\"psk\" type=\"password\"></label>"
	"<button type=\"submit\">Connect</button></form></body></html>";

static void http_send_form(int client_fd)
{
	char header[128];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned)(sizeof(PROVISIONING_HTML) - 1));
	zsock_send(client_fd, header, hlen, 0);
	zsock_send(client_fd, PROVISIONING_HTML,
		   sizeof(PROVISIONING_HTML) - 1, 0);
}

static void http_send_simple(int client_fd, const char *status,
			      const char *body)
{
	char buf[256];
	int len = snprintf(buf, sizeof(buf),
		"HTTP/1.0 %s\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n%s",
		status, (unsigned)strlen(body), body);
	zsock_send(client_fd, buf, len, 0);
}

/* Decode a URL-encoded substring (`+` → space, `%XX` → byte) from src[0..src_len)
 * into dst[0..dst_size). Always NUL-terminates. Returns decoded length. */
static size_t url_decode(const char *src, size_t src_len,
			 char *dst, size_t dst_size)
{
	size_t di = 0;
	for (size_t si = 0; si < src_len && di + 1 < dst_size; si++) {
		char c = src[si];
		if (c == '+') {
			dst[di++] = ' ';
		} else if (c == '%' && si + 2 < src_len) {
			char hex[3] = {src[si + 1], src[si + 2], '\0'};
			dst[di++] = (char)strtol(hex, NULL, 16);
			si += 2;
		} else {
			dst[di++] = c;
		}
	}
	dst[di] = '\0';
	return di;
}

/* Find `name=...` in a URL-encoded form body and URL-decode the value into
 * out[0..out_size). Returns true on success. */
static bool form_get_field(const char *body, const char *name,
			   char *out, size_t out_size)
{
	size_t name_len = strlen(name);
	const char *p = body;
	while (p && *p) {
		if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
			const char *val = p + name_len + 1;
			const char *end = strchr(val, '&');
			size_t vlen = end ? (size_t)(end - val) : strlen(val);
			url_decode(val, vlen, out, out_size);
			return true;
		}
		p = strchr(p, '&');
		if (p) {
			p++;
		}
	}
	return false;
}

/* Handle POST /connect: parse the form body, stash credentials for the
 * main loop to apply. On success, send an HTML page telling the user to
 * reconnect to their home WiFi. */
static void http_handle_post_connect(int client_fd, const char *request,
				     int received)
{
	/* Body starts after the blank line \r\n\r\n */
	const char *body = strstr(request, "\r\n\r\n");
	if (!body) {
		http_send_simple(client_fd, "400 Bad Request",
				 "Missing body");
		return;
	}
	body += 4;

	char ssid[33] = {};
	char psk[65] = {};
	if (!form_get_field(body, "ssid", ssid, sizeof(ssid)) ||
	    strlen(ssid) == 0) {
		http_send_simple(client_fd, "400 Bad Request",
				 "SSID required");
		return;
	}
	form_get_field(body, "psk", psk, sizeof(psk));

	LOG_INF("HTTP: received credentials for SSID '%s' (psk len=%u)",
		ssid, (unsigned)strlen(psk));

	strncpy(pending_apply_ssid, ssid, sizeof(pending_apply_ssid) - 1);
	pending_apply_ssid[sizeof(pending_apply_ssid) - 1] = '\0';
	strncpy(pending_apply_psk, psk, sizeof(pending_apply_psk) - 1);
	pending_apply_psk[sizeof(pending_apply_psk) - 1] = '\0';
	pending_reboot = true;

	static const char OK_HTML[] =
		"<!DOCTYPE html><html><body><h1>Saved</h1>"
		"<p>WeatherSensor will now reboot and connect to your WiFi. "
		"You can reconnect your phone to your normal network.</p>"
		"</body></html>";
	char header[128];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned)(sizeof(OK_HTML) - 1));
	zsock_send(client_fd, header, hlen, 0);
	zsock_send(client_fd, OK_HTML, sizeof(OK_HTML) - 1, 0);
}

/* Single static recv buffer — accept loop is serial so no concurrency.
 * Sized to fit a typical browser POST (User-Agent and friends can be large). */
static char http_buf[HTTP_RECV_BUF];

/* Read until we have the full HTTP request (headers + Content-Length bytes
 * of body, if any) or the buffer fills / peer closes. Returns total bytes. */
static int http_recv_request(int client_fd)
{
	int total = 0;
	int header_end = -1;
	int content_length = 0;

	while (total < (int)sizeof(http_buf) - 1) {
		int n = zsock_recv(client_fd, http_buf + total,
				   sizeof(http_buf) - 1 - total, 0);
		if (n <= 0) {
			break;
		}
		total += n;
		http_buf[total] = '\0';

		if (header_end < 0) {
			char *hdr_end = strstr(http_buf, "\r\n\r\n");
			if (hdr_end) {
				header_end = (hdr_end - http_buf) + 4;
				/* Case-insensitive search for "content-length:". */
				static const char NEEDLE[] = "content-length:";
				for (char *p = http_buf; p + 15 < hdr_end; p++) {
					bool match = true;
					for (int i = 0; i < 15; i++) {
						char c = p[i];
						if (c >= 'A' && c <= 'Z') {
							c = c - 'A' + 'a';
						}
						if (c != NEEDLE[i]) {
							match = false;
							break;
						}
					}
					if (match) {
						content_length =
							(int)strtol(p + 15,
								    NULL, 10);
						break;
					}
				}
			}
		}
		if (header_end >= 0 &&
		    total - header_end >= content_length) {
			break;
		}
	}
	return total;
}

static void http_handle_client(int client_fd)
{
	int received = http_recv_request(client_fd);
	if (received <= 0) {
		zsock_close(client_fd);
		return;
	}

	/* Log the request line (first line up to \r or \n) for debugging. */
	char req_line[80];
	size_t n = 0;
	while (n < sizeof(req_line) - 1 && n < (size_t)received &&
	       http_buf[n] != '\r' && http_buf[n] != '\n') {
		req_line[n] = http_buf[n];
		n++;
	}
	req_line[n] = '\0';
	LOG_INF("HTTP: %s", req_line);

	if (strncmp(http_buf, "GET / ", 6) == 0 ||
	    strncmp(http_buf, "GET /index", 10) == 0) {
		http_send_form(client_fd);
	} else if (strncmp(http_buf, "POST /connect", 13) == 0) {
		http_handle_post_connect(client_fd, http_buf, received);
	} else if (strncmp(http_buf, "GET ", 4) == 0) {
		http_send_simple(client_fd, "404 Not Found", "Not found");
	} else {
		http_send_simple(client_fd, "400 Bad Request", "Bad request");
	}

	zsock_close(client_fd);
}

static void http_server_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	int server_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("HTTP: socket() failed: %d", errno);
		return;
	}

	int opt = 1;
	zsock_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
			 &opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(HTTP_PORT);

	if (zsock_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("HTTP: bind() failed: %d", errno);
		zsock_close(server_fd);
		return;
	}

	if (zsock_listen(server_fd, 2) < 0) {
		LOG_ERR("HTTP: listen() failed: %d", errno);
		zsock_close(server_fd);
		return;
	}

	LOG_INF("HTTP server listening on 192.168.4.1:%d", HTTP_PORT);

	while (http_server_run) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = zsock_accept(server_fd,
					     (struct sockaddr *)&client_addr,
					     &client_len);
		if (client_fd < 0) {
			LOG_WRN("HTTP: accept() failed: %d", errno);
			break;
		}
		LOG_INF("HTTP: client connected");
		http_handle_client(client_fd);
	}

	zsock_close(server_fd);
	LOG_INF("HTTP server stopped");
}

static void http_server_start(void)
{
	if (http_thread_tid) {
		return;
	}
	http_server_run = true;
	http_thread_tid = k_thread_create(
		&http_thread_data, http_stack,
		K_THREAD_STACK_SIZEOF(http_stack),
		http_server_thread, NULL, NULL, NULL,
		5, 0, K_NO_WAIT);
	k_thread_name_set(http_thread_tid, "http_srv");
}

static int provisioning_start(void)
{
	if (provisioning_active) {
		LOG_WRN("Provisioning already active");
		return 0;
	}

	struct net_if *iface = net_if_get_default();

	/* Disconnect STA first — the esp32 driver switches between STA and AP
	 * via esp_wifi_set_mode() internally when ap_enable is called. The
	 * disconnect is needed so the driver sees STA as idle. */
	LOG_INF("Disconnecting STA before enabling AP...");
	net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	k_msleep(500);

	struct wifi_connect_req_params ap_config = {};
	ap_config.ssid = (const uint8_t *)PROVISIONING_AP_SSID;
	ap_config.ssid_length = strlen(PROVISIONING_AP_SSID);
	ap_config.security = WIFI_SECURITY_TYPE_NONE;
	ap_config.channel = WIFI_CHANNEL_ANY;
	ap_config.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("Starting SoftAP: SSID='%s' (open)", PROVISIONING_AP_SSID);
	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface,
			   &ap_config, sizeof(ap_config));
	if (ret < 0) {
		LOG_ERR("AP enable failed: %d", ret);
		return ret;
	}

	provisioning_active = true;
	/* Network config (IP + DHCP server) happens in the AP_ENABLE_RESULT
	 * event handler once the interface is actually up. */
	return 0;
}

/* ========================================================================
 * LVGL UI construction
 * ======================================================================== */

/**
 * @brief Helper to create a positioned label
 */
static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font,
			       lv_color_t color, int32_t x, int32_t y,
			       const char *text)
{
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, color, 0);
	lv_obj_set_pos(label, x, y);
	return label;
}

/**
 * @brief Position a unit label to the right of a value label, baseline-aligned
 *
 * Manually calculates position instead of using lv_obj_align_to to avoid
 * live dependency issues across screen switches.
 */
static void position_unit_label(lv_obj_t *unit, lv_obj_t *value)
{
	lv_obj_update_layout(value);
	int32_t vx = lv_obj_get_x(value);
	int32_t vy = lv_obj_get_y(value);
	int32_t vw = lv_obj_get_width(value);
	int32_t vh = lv_obj_get_height(value);
	int32_t uh = lv_obj_get_height(unit);

	/* Place unit to the right, bottom-aligned with a small upward nudge */
	lv_obj_set_pos(unit, vx + vw + 2, vy + vh - uh - 3);
}

/**
 * @brief Set common screen style (white background)
 */
static void setup_screen_style(lv_obj_t *scr)
{
	lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

/**
 * @brief Add status bar widgets to a screen
 *
 * Creates battery and wifi indicator labels directly on the given screen.
 * Returns pointers via output parameters.
 */
static void build_status_bar(lv_obj_t *screen, lv_obj_t **bat_out, lv_obj_t **wifi_out)
{
	/* Battery icon (left side, top) */
	*bat_out = lv_label_create(screen);
	lv_label_set_text(*bat_out, LV_SYMBOL_BATTERY_FULL);
	lv_obj_set_style_text_font(*bat_out, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(*bat_out, lv_color_black(), 0);
	lv_obj_set_pos(*bat_out, 4, 3);

	/* WiFi icon (right side, top). Starts in the disconnected color;
	 * wifi_mgmt_event_handler flips it to black on connect. The
	 * Reverse TFT panel reports BGR, so (0,0,255) renders as red. */
	*wifi_out = lv_label_create(screen);
	lv_label_set_text(*wifi_out, LV_SYMBOL_WIFI);
	lv_obj_set_style_text_font(*wifi_out, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(*wifi_out, lv_color_make(0, 0, 255), 0);
	lv_obj_set_pos(*wifi_out, 135 - 24, 3);
}

/**
 * @brief Build Screen 1: Weather data (temperature, humidity, pressure)
 *
 * Values use large font (Montserrat 32), units use smaller font (Montserrat 16).
 * Unit labels are right-aligned and baseline-aligned with values.
 */
static void build_weather_screen(void)
{
	lv_color_t text_color = lv_color_black();
	lv_color_t label_color = lv_color_make(100, 100, 100);
	lv_color_t unit_color = lv_color_make(80, 80, 80);
	const lv_font_t *font_value = &lv_font_montserrat_32;
	const lv_font_t *font_unit = &lv_font_montserrat_16;
	const lv_font_t *font_label = &lv_font_montserrat_14;

	screen_weather = lv_obj_create(NULL);
	setup_screen_style(screen_weather);
	build_status_bar(screen_weather, &battery_labels[0], &wifi_labels[0]);

	int32_t y = CONTENT_Y_START;

	/* Time HH:MM (centered, big font) */
	weather_time_label = create_label(screen_weather, font_value, text_color,
					  0, y, "00:00");
	lv_obj_set_width(weather_time_label, 135);
	lv_obj_set_style_text_align(weather_time_label, LV_TEXT_ALIGN_CENTER, 0);
	y += 42;

	/* TEMP label */
	create_label(screen_weather, font_label, label_color,
		     CONTENT_X_PAD, y, "TEMP");
	y += 16;

	/* Temperature value (big) + unit (small, baseline-aligned via negative Y offset) */
	weather_temp_value = create_label(screen_weather, font_value, text_color,
					  CONTENT_X_PAD, y, "--");
	weather_temp_unit = create_label(screen_weather, font_unit, unit_color,
					 0, 0, "\xC2\xB0""C");
	position_unit_label(weather_temp_unit, weather_temp_value);
	y += 42;

	/* HUM label */
	create_label(screen_weather, font_label, label_color,
		     CONTENT_X_PAD, y, "HUM");
	y += 16;

	/* Humidity value (big) + unit (small) */
	weather_hum_value = create_label(screen_weather, font_value, text_color,
					 CONTENT_X_PAD, y, "--");
	weather_hum_unit = create_label(screen_weather, font_unit, unit_color,
					0, 0, "%");
	position_unit_label(weather_hum_unit, weather_hum_value);
	y += 42;

	/* PRESS label */
	create_label(screen_weather, font_label, label_color,
		     CONTENT_X_PAD, y, "PRESS");
	y += 16;

	/* Pressure value (big) + unit (small) */
	weather_press_value = create_label(screen_weather, font_value, text_color,
					   CONTENT_X_PAD, y, "--");
	weather_press_unit = create_label(screen_weather, font_unit, unit_color,
					  0, 0, "hPa");
	position_unit_label(weather_press_unit, weather_press_value);
}

/**
 * @brief Build screen 2: SNTP drift history
 *
 * Shows the last DRIFT_HISTORY_SIZE drift values, one per row, oldest at
 * top. Rows are pre-allocated with placeholder text; update_drift_display()
 * fills them from the ring buffer.
 */
static void build_drift_screen(void)
{
	lv_color_t text_color = lv_color_black();
	lv_color_t label_color = lv_color_make(100, 100, 100);
	const lv_font_t *font_title = &lv_font_montserrat_14;
	const lv_font_t *font_row = &lv_font_montserrat_16;

	screen_drift = lv_obj_create(NULL);
	setup_screen_style(screen_drift);
	build_status_bar(screen_drift, &battery_labels[1], &wifi_labels[1]);

	int32_t y = CONTENT_Y_START;

	drift_title_label = create_label(screen_drift, font_title, label_color,
					 CONTENT_X_PAD, y, "DRIFT (ms)");
	y += 18;

	for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
		drift_row_labels[i] = create_label(screen_drift, font_row,
						   text_color,
						   CONTENT_X_PAD, y, "--");
		y += 22;
	}
}

/**
 * @brief Initialize the complete LVGL UI
 */
static void lvgl_ui_init(void)
{
	build_weather_screen();
	build_drift_screen();

	screens[0] = screen_weather;
	screens[1] = screen_drift;

	lv_screen_load(screen_weather);
	current_screen = 0;

	LOG_INF("LVGL UI initialized (%d screens + status bar)", NUM_SCREENS);
}

/* ========================================================================
 * Display update functions
 * ======================================================================== */

/**
 * @brief Update Weather screen labels with current sensor data
 */
static void update_weather_display(void)
{
	char buf[16];

	int temp_int = (int)temperature;
	int temp_frac = (int)((temperature - temp_int) * 10);
	if (temp_frac < 0) {
		temp_frac = -temp_frac;
	}

	int hum_int = (int)humidity;
	int hum_frac = (int)((humidity - hum_int) * 10);

	int press_int = (int)pressure;

	snprintf(buf, sizeof(buf), "%d.%d", temp_int, temp_frac);
	lv_label_set_text(weather_temp_value, buf);
	position_unit_label(weather_temp_unit, weather_temp_value);

	snprintf(buf, sizeof(buf), "%d.%d", hum_int, hum_frac);
	lv_label_set_text(weather_hum_value, buf);
	position_unit_label(weather_hum_unit, weather_hum_value);

	snprintf(buf, sizeof(buf), "%d", press_int);
	lv_label_set_text(weather_press_value, buf);
	position_unit_label(weather_press_unit, weather_press_value);
}

/**
 * @brief Update all sensor display data
 */
static void update_sensor_display(void)
{
	update_weather_display();

	int temp_int = (int)temperature;
	int temp_frac = (int)((temperature - temp_int) * 10);
	if (temp_frac < 0) {
		temp_frac = -temp_frac;
	}
	int hum_int = (int)humidity;
	int hum_frac = (int)((humidity - hum_int) * 10);
	int press_int = (int)pressure;

	LOG_INF("Display update - Temp=%d.%d C, Hum=%d.%d%%, Press=%d hPa",
		temp_int, temp_frac, hum_int, hum_frac, press_int);
}

/**
 * @brief Returns true at most once per wall-clock minute, on the boundary.
 *
 * After SNTP has set the clock we use tm_min from local time; before
 * that we fall back to an uptime-minute boundary so the display is
 * never frozen. Accepts tm_sec == 0 or 1 to absorb scheduler jitter
 * from the 1 s main-loop tick; the last-minute dedupe ensures only
 * one fire per minute regardless.
 */
static bool minute_boundary_fired(void)
{
	static int last_minute_fired = -1;
	int minute_now;

	if (time_synced) {
		struct timespec ts;
		struct tm local;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &local);
		if (local.tm_sec > 1) {
			return false;
		}
		minute_now = local.tm_min;
	} else {
		uint32_t up_s = (uint32_t)(k_uptime_get() / 1000);
		if (up_s % 60 != 0) {
			return false;
		}
		minute_now = (int)((up_s / 60) % 60);
	}

	if (minute_now == last_minute_fired) {
		return false;
	}
	last_minute_fired = minute_now;
	return true;
}

/**
 * @brief Update the on-screen time label.
 *
 * Once SNTP has synced the system clock at least once we render local
 * (Europe/Warsaw, DST-aware) HH:MM. Until then we fall back to an
 * uptime-based HH:MM so the display is never blank — but at least the
 * boot-time clock won't pretend to be a real wall time.
 */
static void update_time_display(void)
{
	char time_str[16];

	if (time_synced) {
		struct timespec ts;
		struct tm local;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &local);
		snprintf(time_str, sizeof(time_str),
			 "%02d:%02d", local.tm_hour, local.tm_min);
	} else {
		uint32_t up_s = (uint32_t)(k_uptime_get() / 1000);
		snprintf(time_str, sizeof(time_str),
			 "%02u:%02u", up_s / 3600, (up_s % 3600) / 60);
	}

	lv_label_set_text(weather_time_label, time_str);
}

/**
 * @brief Render the drift ring buffer onto the drift screen.
 *
 * Walks the ring oldest → newest so the most recent value is at the
 * bottom of the screen (visually matches a chat-log "scroll" feel).
 * Empty slots get a "--" placeholder.
 */
static void update_drift_display(void)
{
	for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
		int idx = (drift_head + i) % DRIFT_HISTORY_SIZE;
		const struct drift_entry *e = &drift_history[idx];
		char row[32];

		if (e->valid) {
			snprintf(row, sizeof(row),
				 "%02d:%02d  %+lld",
				 e->local_tm.tm_hour, e->local_tm.tm_min,
				 (long long)e->drift_ms);
		} else {
			snprintf(row, sizeof(row), "--");
		}
		lv_label_set_text(drift_row_labels[i], row);
	}
}

/* ========================================================================
 * Main application entry point
 * ======================================================================== */

int main(void)
{
	int ret;

	LOG_INF("WeatherSensor starting...");
	LOG_INF("Board: %s", CONFIG_BOARD);

	/* Pin local time to Europe/Warsaw with DST. POSIX TZ string:
	 *   CET-1CEST,M3.5.0,M10.5.0/3
	 * = standard CET (UTC+1), DST CEST (UTC+2), spring-forward last
	 * Sunday of March, fall-back last Sunday of October at 03:00.
	 * After this, picolibc's localtime_r() handles DST automatically. */
	setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
	tzset();

	/* CRITICAL INITIALIZATION SEQUENCE:
	 * 1. Enable display power (GPIO7) first
	 * 2. Wait for power rail to stabilize
	 * 3. Initialize display device
	 * 4. Enable backlight (GPIO45)
	 */
	ret = display_power_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display power: %d", ret);
		return ret;
	}

	ret = display_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display: %d", ret);
		return ret;
	}

	ret = backlight_init();
	if (ret < 0) {
		LOG_WRN("Backlight init failed (non-fatal): %d", ret);
	}

	/* Initialize sensors */
	LOG_INF("");
	LOG_INF("========================================");
	ret = sensors_init();
	static bool sensors_ok = false;
	sensors_ok = (ret == 0);
	if (!sensors_ok) {
		LOG_ERR("Sensor initialization failed: %d", ret);
		LOG_ERR("Continuing without sensors...");
	}
	LOG_INF("========================================");
	LOG_INF("");

	/* Initialize fuel gauge (after display power, since GPIO7 powers I2C bus) */
	ret = fuel_gauge_init_device();
	if (ret < 0) {
		LOG_WRN("Fuel gauge not available - using mocked battery");
	}

	/* Initialize LVGL multi-screen UI */
	lvgl_ui_init();

	/* Initial sensor read if sensors available */
	if (sensors_ok) {
		LOG_INF("Performing initial sensor read...");
		sensors_read();
		update_sensor_display();
	}

	/* Initial battery read */
	battery_read();
	update_battery_display();

	/* Register WiFi management event callback */
	net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT |
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
				     NET_EVENT_WIFI_AP_DISABLE_RESULT);
	net_mgmt_add_event_callback(&wifi_mgmt_cb);

	/* Initialize NVS and try to connect to WiFi.
	 * Priority: stored NVS credentials > Kconfig test credentials.
	 * If Kconfig credentials connect successfully, they're saved to NVS
	 * for next boot — so the next reflash won't need the Kconfig override. */
	if (nvs_init_storage() == 0) {
		char ssid[33] = {};
		char psk[65] = {};
		if (wifi_load_credentials(ssid, sizeof(ssid), psk, sizeof(psk))) {
			wifi_connect(ssid, psk);
		} else if (strlen(CONFIG_WEATHER_WIFI_TEST_SSID) > 0) {
			LOG_INF("Using Kconfig test credentials for WiFi");
			strncpy(pending_save_ssid, CONFIG_WEATHER_WIFI_TEST_SSID,
				sizeof(pending_save_ssid) - 1);
			strncpy(pending_save_psk, CONFIG_WEATHER_WIFI_TEST_PSK,
				sizeof(pending_save_psk) - 1);
			pending_save_credentials = true;
			wifi_connect(pending_save_ssid, pending_save_psk);
		} else {
			LOG_INF("No WiFi credentials stored — provisioning needed");
		}
	}

	LOG_INF("Initialization complete. Entering main loop...");

	/* Main loop */
	while (1) {
		/* Handle pending screen navigation from button presses */
		if (pending_navigate != 0) {
			screen_navigate(pending_navigate);
			pending_navigate = 0;
		}

		/* Handle pending provisioning request from D1 */
		if (pending_provisioning) {
			pending_provisioning = false;
			provisioning_start();
		}

		/* After AP_ENABLE_RESULT fires: assign IP and start DHCP */
		if (pending_ap_network_setup) {
			pending_ap_network_setup = false;
			provisioning_configure_ap_network();
		}

		/* Credentials received via HTTP form: persist to NVS and
		 * reboot. The new credentials take effect on the clean cold
		 * boot, avoiding fragile runtime AP→STA driver transitions. */
		if (pending_reboot) {
			pending_reboot = false;
			LOG_INF("Saving credentials and rebooting (SSID: %s)",
				pending_apply_ssid);
			wifi_save_credentials(pending_apply_ssid,
					      pending_apply_psk);
			/* Let the HTTP response flush to the browser. */
			k_msleep(1000);
			sys_reboot(SYS_REBOOT_COLD);
			/* unreachable */
		}

		/* WiFi status-bar icon: black = connected, red = disconnected.
		 * The Adafruit Reverse TFT panel reports BGR despite an RGB
		 * config, so we pass (0, 0, 255) to actually render red. */
		if (pending_wifi_icon_update) {
			pending_wifi_icon_update = false;
			lv_color_t c = wifi_connected
				? lv_color_black()
				: lv_color_make(0, 0, 255);
			for (int i = 0; i < NUM_SCREENS; i++) {
				lv_obj_set_style_text_color(wifi_labels[i],
							    c, 0);
			}
		}

		/* New SNTP drift sample arrived — repaint the drift screen.
		 * Cheap; LVGL only redraws the active screen. */
		if (pending_drift_screen_update) {
			pending_drift_screen_update = false;
			update_drift_display();
		}

		/* SNTP wall-clock sync: first sync as soon as WiFi is up,
		 * then every TIME_SYNC_INTERVAL_S after a successful sync.
		 * sntp_sync_time() blocks for up to SNTP_TIMEOUT_MS — fine
		 * at this cadence. */
		if (pending_initial_sync) {
			pending_initial_sync = false;
			sntp_sync_time();
		} else if (wifi_connected && time_synced &&
			   seconds_since_last_sync >= TIME_SYNC_INTERVAL_S) {
			sntp_sync_time();
		}

		/* Once per minute, aligned to the wall-clock minute start
		 * (or the uptime-minute start before SNTP has synced). One
		 * trigger keeps the sensor read and the visible HH:MM
		 * transition phase-locked. */
		if (minute_boundary_fired()) {
			update_time_display();
			if (sensors_ok) {
				LOG_INF("======== PERIODIC SENSOR READ (t=%us) ========",
					seconds_counter);
				sensors_read();
				update_sensor_display();
			}
		}

		/* Read battery every 60 seconds */
		if (seconds_counter % 60 == 0) {
			battery_read();
			update_battery_display();
		}

		/* Log status every 30 seconds */
		if (seconds_counter % 30 == 0) {
			LOG_INF("System running. Uptime: %u seconds, Screen: %d",
				seconds_counter, current_screen);
		}

		/* Process LVGL tasks */
		lv_timer_handler();

		/* Sleep for 1 second */
		k_msleep(1000);
		seconds_counter++;
		seconds_since_last_sync++;
	}

	return 0;
}
