/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui.h"

#include "app_state.h"      /* DRIFT_HISTORY_SIZE, drift_entry_t */
#include "battery.h"
#include "bme280.h"
#include "provisioning.h"
#include "sntp_sync.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

/* ========================================================================
 * UI constants
 * ======================================================================== */

#define STATUS_BAR_H    22
#define CONTENT_Y_START (STATUS_BAR_H + 4)
#define CONTENT_X_PAD   5

/* ========================================================================
 * LVGL objects
 * ======================================================================== */

static lv_obj_t *screen_weather;
static lv_obj_t *screen_drift;

static const int NUM_SCREENS = 2;
static int current_screen = 0;
static lv_obj_t *screens[NUM_SCREENS];

static lv_obj_t *battery_labels[NUM_SCREENS];
static lv_obj_t *wifi_labels[NUM_SCREENS];

static lv_obj_t *weather_time_label;
static lv_obj_t *weather_temp_value;
static lv_obj_t *weather_temp_unit;
static lv_obj_t *weather_hum_value;
static lv_obj_t *weather_hum_unit;
static lv_obj_t *weather_press_value;
static lv_obj_t *weather_press_unit;

static lv_obj_t *drift_title_label;
static lv_obj_t *drift_row_labels[DRIFT_HISTORY_SIZE];

/* ========================================================================
 * Button navigation
 * ======================================================================== */

static volatile int pending_navigate = 0;

/* Software-side hold filter — additional defense against phantom presses
 * that sneak through the gpio-keys debounce. button_input_cb queues a
 * delayable work for BUTTON_HOLD_MS; if a release arrives before then,
 * we cancel the work and the press is dropped. Real human presses last
 * far longer than BUTTON_HOLD_MS so the work fires and the press is
 * dispatched as if it had arrived directly. */
#define BUTTON_HOLD_MS 80
static int button_pending_code = 0;
static void button_hold_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(button_hold_work, button_hold_handler);

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
		/* Release: if dispatch hasn't fired yet, the press was too
		 * short — cancel and drop. */
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

static void screen_navigate(int direction)
{
	current_screen = (current_screen + direction + NUM_SCREENS) % NUM_SCREENS;
	lv_screen_load(screens[current_screen]);
	LOG_INF("Switched to screen %d", current_screen);
}

/* ========================================================================
 * Status bar helpers
 * ======================================================================== */

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

/* ========================================================================
 * LVGL UI construction helpers
 * ======================================================================== */

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

/* Position a unit label to the right of a value label, baseline-aligned.
 * Manual layout instead of lv_obj_align_to to avoid live dependency
 * issues across screen switches. */
static void position_unit_label(lv_obj_t *unit, lv_obj_t *value)
{
	lv_obj_update_layout(value);
	int32_t vx = lv_obj_get_x(value);
	int32_t vy = lv_obj_get_y(value);
	int32_t vw = lv_obj_get_width(value);
	int32_t vh = lv_obj_get_height(value);
	int32_t uh = lv_obj_get_height(unit);

	lv_obj_set_pos(unit, vx + vw + 2, vy + vh - uh - 3);
}

/* Center a value+unit pair horizontally on the 135 px wide screen.
 * Call after every text update to the value label since its width can
 * change (e.g. "999" vs "1010"). */
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

static void setup_screen_style(lv_obj_t *scr)
{
	lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

static void build_status_bar(lv_obj_t *screen, lv_obj_t **bat_out, lv_obj_t **wifi_out)
{
	*bat_out = lv_label_create(screen);
	lv_label_set_text(*bat_out, LV_SYMBOL_BATTERY_FULL);
	lv_obj_set_style_text_font(*bat_out, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(*bat_out, lv_color_black(), 0);
	lv_obj_set_pos(*bat_out, 4, 3);

	/* WiFi icon (right side, top). Starts in the disconnected color;
	 * the wifi_sta event handler raises a pending-icon flag that main
	 * forwards to ui_update_wifi_icon() to flip this to black on
	 * connect. The Reverse TFT panel reports BGR, so (0,0,255) renders
	 * as red. */
	*wifi_out = lv_label_create(screen);
	lv_label_set_text(*wifi_out, LV_SYMBOL_WIFI);
	lv_obj_set_style_text_font(*wifi_out, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(*wifi_out, lv_color_make(0, 0, 255), 0);
	lv_obj_set_pos(*wifi_out, 135 - 24, 3);
}

/* Build Screen 1: Weather data (temperature, humidity, pressure).
 * Values use large font (Montserrat 32), units use smaller font
 * (Montserrat 16). Unit labels are right-aligned and baseline-aligned
 * with values. */
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

	/* Temperature value (big) + unit (small, baseline-aligned) */
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

/* Build screen 2: SNTP drift history. Shows the last DRIFT_HISTORY_SIZE
 * drift values, one per row, oldest at top. Rows are pre-allocated with
 * placeholder text; ui_update_drift_display() fills them from the SNTP
 * module's ring buffer. */
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

void ui_init(void)
{
	build_weather_screen();
	build_drift_screen();

	screens[0] = screen_weather;
	screens[1] = screen_drift;

	lv_screen_load(screen_weather);
	current_screen = 0;

	LOG_INF("LVGL UI initialized (%d screens + status bar)", NUM_SCREENS);
}

int ui_take_pending_navigate(void)
{
	int dir = pending_navigate;
	if (dir != 0) {
		pending_navigate = 0;
		screen_navigate(dir);
	}
	return dir;
}

int ui_current_screen(void)
{
	return current_screen;
}

/* ========================================================================
 * Display update functions
 * ======================================================================== */

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

void ui_update_sensor_display(void)
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

/* Once SNTP has synced the system clock at least once we render local
 * (Europe/Warsaw, DST-aware) HH:MM. Until then we fall back to an
 * uptime-based HH:MM so the display is never blank — but at least the
 * boot-time clock won't pretend to be a real wall time. */
void ui_update_time_display(void)
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

/* Walks the ring oldest → newest so the most recent value is at the
 * bottom of the screen (visually matches a chat-log "scroll" feel).
 * Empty slots get a "--" placeholder. */
void ui_update_drift_display(void)
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

void ui_update_battery(uint8_t soc)
{
	const char *sym = battery_symbol(soc);
	for (int i = 0; i < NUM_SCREENS; i++) {
		lv_label_set_text(battery_labels[i], sym);
	}
}

void ui_update_wifi_icon(bool connected)
{
	lv_color_t c = connected
		? lv_color_black()
		: lv_color_make(0, 0, 255);
	for (int i = 0; i < NUM_SCREENS; i++) {
		lv_obj_set_style_text_color(wifi_labels[i], c, 0);
	}
}

void ui_tick(void)
{
	lv_timer_handler();
}
