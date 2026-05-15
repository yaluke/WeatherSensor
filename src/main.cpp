/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "battery.h"
#include "bme280.h"
#include "display.h"
#include "influx.h"
#include "provisioning.h"
#include "sntp_sync.h"
#include "wifi_sta.h"

LOG_MODULE_REGISTER(weather_sensor, LOG_LEVEL_INF);

/* ========================================================================
 * Hardware GPIO specs
 * ======================================================================== */

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

/* Drift screen labels — one row per ring slot, plus a header.
 * DRIFT_HISTORY_SIZE is shared with the SNTP module via app_state.h. */
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
		provisioning_request();
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
 * Battery display helper (moves to ui.cpp in a later commit)
 * ======================================================================== */

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
	const char *sym = battery_symbol(battery_get_soc());
	for (int i = 0; i < NUM_SCREENS; i++) {
		lv_label_set_text(battery_labels[i], sym);
	}
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
 * @brief Center a value+unit pair horizontally on the 135 px wide screen.
 *
 * Computes the combined width (value + 2 px gap + unit, matching
 * position_unit_label's spacing), slides the value's x so the group
 * is centered, then re-places the unit relative to the new value
 * position. Call after every text update to the value label since
 * its width can change (e.g. "999" vs "1010").
 */
static void center_value_unit(lv_obj_t *value, lv_obj_t *unit)
{
	lv_obj_update_layout(value);
	lv_obj_update_layout(unit);
	int32_t vw = lv_obj_get_width(value);
	int32_t uw = lv_obj_get_width(unit);
	int32_t total = vw + 2 + uw;
	int32_t new_vx = (135 - total) / 2;
	lv_obj_set_x(value, new_vx);
	position_unit_label(unit, value);
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
	 * the wifi_sta event handler raises a pending-icon flag the main
	 * loop picks up to flip this to black on connect. The Reverse TFT
	 * panel reports BGR, so (0,0,255) renders as red. */
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
	center_value_unit(weather_temp_value, weather_temp_unit);
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
	center_value_unit(weather_hum_value, weather_hum_unit);
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
	center_value_unit(weather_press_value, weather_press_unit);
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
	float temperature = bme280_get_temperature();
	float humidity = bme280_get_humidity();
	float pressure = bme280_get_pressure();

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
	center_value_unit(weather_temp_value, weather_temp_unit);

	snprintf(buf, sizeof(buf), "%d.%d", hum_int, hum_frac);
	lv_label_set_text(weather_hum_value, buf);
	position_unit_label(weather_hum_unit, weather_hum_value);
	center_value_unit(weather_hum_value, weather_hum_unit);

	snprintf(buf, sizeof(buf), "%d", press_int);
	lv_label_set_text(weather_press_value, buf);
	position_unit_label(weather_press_unit, weather_press_value);
	center_value_unit(weather_press_value, weather_press_unit);
}

/**
 * @brief Update all sensor display data
 */
static void update_sensor_display(void)
{
	update_weather_display();

	float temperature = bme280_get_temperature();
	float humidity = bme280_get_humidity();
	float pressure = bme280_get_pressure();

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

	if (sntp_is_synced()) {
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

	if (sntp_is_synced()) {
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
	drift_entry_t snapshot[DRIFT_HISTORY_SIZE];
	sntp_drift_snapshot(snapshot);

	for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
		const drift_entry_t *e = &snapshot[i];
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
	ret = bme280_init();
	static bool sensors_ok = false;
	sensors_ok = (ret == 0);
	if (!sensors_ok) {
		LOG_ERR("Sensor initialization failed: %d", ret);
		LOG_ERR("Continuing without sensors...");
	}
	LOG_INF("========================================");
	LOG_INF("");

	/* Hand the BME280 Vcc pin from boot-time gpio-hog control to
	 * runtime control. bme280_read now owns it and toggles it per
	 * minute-tick. */
	bme280_pin_release_to_runtime();

	/* Initialize fuel gauge (after display power, since GPIO7 powers I2C bus) */
	ret = battery_init();
	if (ret < 0) {
		LOG_WRN("Fuel gauge not available - using mocked battery");
	}

	/* Initialize LVGL multi-screen UI */
	lvgl_ui_init();

	/* Initial sensor read if sensors available */
	if (sensors_ok) {
		LOG_INF("Performing initial sensor read...");
		bme280_read();
		update_sensor_display();
	}

	/* Initial battery read */
	battery_read();
	update_battery_display();

	/* Mount NVS + register net_mgmt callback, then try to connect.
	 * Priority: stored NVS credentials > Kconfig test credentials.
	 * If Kconfig credentials connect successfully, they're saved to NVS
	 * for next boot — so the next reflash won't need the Kconfig override. */
	if (wifi_sta_init() == 0) {
		char ssid[33] = {};
		char psk[65] = {};
		if (wifi_load_credentials(ssid, sizeof(ssid), psk, sizeof(psk))) {
			wifi_connect(ssid, psk);
		} else if (strlen(CONFIG_WEATHER_WIFI_TEST_SSID) > 0) {
			LOG_INF("Using Kconfig test credentials for WiFi");
			wifi_arm_pending_save(CONFIG_WEATHER_WIFI_TEST_SSID,
					      CONFIG_WEATHER_WIFI_TEST_PSK);
			wifi_connect(CONFIG_WEATHER_WIFI_TEST_SSID,
				     CONFIG_WEATHER_WIFI_TEST_PSK);
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
		if (provisioning_take_pending_request()) {
			provisioning_start();
		}

		/* After AP_ENABLE_RESULT fires: assign IP and start DHCP */
		if (wifi_take_pending_ap_setup()) {
			provisioning_configure_ap_network();
		}

		/* Credentials received via HTTP form: persist to NVS and
		 * reboot. The new credentials take effect on the clean cold
		 * boot, avoiding fragile runtime AP→STA driver transitions. */
		char apply_ssid[33];
		char apply_psk[65];
		if (provisioning_take_pending_reboot(apply_ssid, sizeof(apply_ssid),
						     apply_psk, sizeof(apply_psk))) {
			LOG_INF("Saving credentials and rebooting (SSID: %s)",
				apply_ssid);
			wifi_save_credentials(apply_ssid, apply_psk);
			/* Let the HTTP response flush to the browser. */
			k_msleep(1000);
			sys_reboot(SYS_REBOOT_COLD);
			/* unreachable */
		}

		/* WiFi status-bar icon: black = connected, red = disconnected.
		 * The Adafruit Reverse TFT panel reports BGR despite an RGB
		 * config, so we pass (0, 0, 255) to actually render red. */
		if (wifi_take_pending_icon_update()) {
			lv_color_t c = wifi_is_connected()
				? lv_color_black()
				: lv_color_make(0, 0, 255);
			for (int i = 0; i < NUM_SCREENS; i++) {
				lv_obj_set_style_text_color(wifi_labels[i],
							    c, 0);
			}
		}

		/* New SNTP drift sample arrived — repaint the drift screen.
		 * Cheap; LVGL only redraws the active screen. */
		if (sntp_take_pending_drift_update()) {
			update_drift_display();
		}

		/* SNTP wall-clock sync: first sync as soon as WiFi is up,
		 * then every hour after a successful sync. sntp_sync_now()
		 * blocks for up to a few seconds — fine at this cadence. */
		if (sntp_take_pending_initial_sync()) {
			sntp_sync_now();
		} else if (wifi_is_connected() && sntp_should_resync()) {
			sntp_sync_now();
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
				bme280_read();
				update_sensor_display();
				/* Queue this reading for the InfluxDB uplink.
				 * Only when SNTP has set the wall clock — every
				 * row carries a real timestamp, no server-side
				 * "receive time" mess. */
				if (sntp_is_synced()) {
					influx_capture_sample();
				}
			}
		}

		/* Drain pending Influx samples — cheap when the buffer is
		 * empty, retries from the oldest unsent reading on failure. */
		influx_drain(wifi_is_connected());

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
		sntp_tick();
	}

	return 0;
}
