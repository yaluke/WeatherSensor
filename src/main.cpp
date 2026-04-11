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
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <stdio.h>

#include <zephyr/drivers/sensor/ens160.h>

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
static const struct device *ens160;

/* Fuel gauge device */
static const struct device *fuel_gauge_dev;

/* ========================================================================
 * Sensor data (global, updated by sensors_read())
 * ======================================================================== */

static float temperature = 0.0f;
static float humidity = 0.0f;
static float pressure = 0.0f;
static uint16_t eco2 = 0;
static uint16_t tvoc = 0;
static uint8_t aqi = 0;

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
static lv_obj_t *screen_airquality;

/* Screen array for indexed navigation */
static const int NUM_SCREENS = 2;
static int current_screen = 0;
static lv_obj_t *screens[2];

/* Status bar labels (duplicated on each screen) */
static lv_obj_t *weather_battery_label;
static lv_obj_t *weather_wifi_label;
static lv_obj_t *aq_battery_label;
static lv_obj_t *aq_wifi_label;

/* Screen 1 (Weather) labels */
static lv_obj_t *weather_time_label;
static lv_obj_t *weather_temp_value;
static lv_obj_t *weather_temp_unit;
static lv_obj_t *weather_hum_value;
static lv_obj_t *weather_hum_unit;
static lv_obj_t *weather_press_value;
static lv_obj_t *weather_press_unit;

/* Screen 2 (Air Quality) labels */
static lv_obj_t *aq_time_label;
static lv_obj_t *aq_aqi_value;
static lv_obj_t *aq_aqi_unit;
static lv_obj_t *aq_co2_value;
static lv_obj_t *aq_co2_unit;
static lv_obj_t *aq_tvoc_value;
static lv_obj_t *aq_tvoc_unit;

/* Time counter (seconds since boot) */
static uint32_t seconds_counter = 0;

/* ========================================================================
 * Button navigation
 * ======================================================================== */

/* Flag set by button callback, consumed by main loop.
 * +1 = next screen, -1 = previous screen.
 * Using volatile to ensure visibility between ISR context and main loop. */
static volatile int pending_navigate = 0;


/**
 * @brief Input event callback for hardware buttons
 *
 * Called by Zephyr input subsystem (synchronous mode).
 * We only set a flag here — LVGL calls happen in the main loop.
 */
static void button_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_INF("Input event: type=%u code=%u value=%d sync=%u",
		evt->type, evt->code, evt->value, evt->sync);

	/* Only react to key press (value=1), ignore release (value=0) */
	if (evt->type != INPUT_EV_KEY || evt->value != 1) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_0: /* D0 button = UP = next screen */
		pending_navigate = 1;
		LOG_INF("Button D0 (UP) pressed");
		break;
	case INPUT_KEY_3: /* D2 button = DOWN = previous screen */
		pending_navigate = -1;
		LOG_INF("Button D2 (DOWN) pressed");
		break;
	case INPUT_KEY_1: /* D1 button = SET = reserved for future use */
		LOG_INF("Button D1 (SET) pressed");
		break;
	default:
		LOG_INF("Unknown button code: %u", evt->code);
		break;
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);

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

	/* ENS160 initialization */
	LOG_INF("Looking up ENS160 device...");
	ens160 = DEVICE_DT_GET_ONE(sciosense_ens160);

	if (!device_is_ready(ens160)) {
		LOG_ERR("ENS160: Device not ready!");
		return -ENODEV;
	}
	LOG_INF("ENS160: Device ready - %s", ens160->name);

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

	/* Read ENS160 */
	LOG_INF("ENS160: Fetching sample...");
	ret = sensor_sample_fetch(ens160);
	if (ret < 0) {
		LOG_ERR("ENS160: sample_fetch failed: %d", ret);
	} else {
		LOG_INF("ENS160: Sample fetch OK");

		sensor_channel_get(ens160, SENSOR_CHAN_CO2, &val);
		eco2 = val.val1;
		LOG_INF("ENS160: eCO2 = %u ppm (val1=%d val2=%d)", eco2, val.val1, val.val2);

		sensor_channel_get(ens160, SENSOR_CHAN_VOC, &val);
		tvoc = val.val1;
		LOG_INF("ENS160: TVOC = %u ppb (val1=%d val2=%d)", tvoc, val.val1, val.val2);

		sensor_channel_get(ens160, (enum sensor_channel)SENSOR_CHAN_ENS160_AQI, &val);
		aqi = val.val1;
		LOG_INF("ENS160: AQI = %u (val1=%d val2=%d)", aqi, val.val1, val.val2);
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
	lv_label_set_text(weather_battery_label, sym);
	lv_label_set_text(aq_battery_label, sym);
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

	/* WiFi icon (right side, top) */
	*wifi_out = lv_label_create(screen);
	lv_label_set_text(*wifi_out, LV_SYMBOL_WIFI);
	lv_obj_set_style_text_font(*wifi_out, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(*wifi_out, lv_color_make(180, 180, 180), 0);
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
	build_status_bar(screen_weather, &weather_battery_label, &weather_wifi_label);

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
 * @brief Build Screen 2: Air Quality data (AQI, CO2, TVOC)
 *
 * Values use large font (Montserrat 32), units use smaller font (Montserrat 16).
 */
static void build_airquality_screen(void)
{
	lv_color_t text_color = lv_color_black();
	lv_color_t label_color = lv_color_make(100, 100, 100);
	lv_color_t unit_color = lv_color_make(80, 80, 80);
	const lv_font_t *font_value = &lv_font_montserrat_32;
	const lv_font_t *font_unit = &lv_font_montserrat_16;
	const lv_font_t *font_label = &lv_font_montserrat_14;

	screen_airquality = lv_obj_create(NULL);
	setup_screen_style(screen_airquality);
	build_status_bar(screen_airquality, &aq_battery_label, &aq_wifi_label);

	int32_t y = CONTENT_Y_START;

	/* Time HH:MM (centered, big font) */
	aq_time_label = create_label(screen_airquality, font_value, text_color,
				     0, y, "00:00");
	lv_obj_set_width(aq_time_label, 135);
	lv_obj_set_style_text_align(aq_time_label, LV_TEXT_ALIGN_CENTER, 0);
	y += 42;

	/* AQ label */
	create_label(screen_airquality, font_label, label_color,
		     CONTENT_X_PAD, y, "AQ");
	y += 16;

	/* AQI value (big) + unit (small) */
	aq_aqi_value = create_label(screen_airquality, font_value, text_color,
				    CONTENT_X_PAD, y, "--");
	aq_aqi_unit = create_label(screen_airquality, font_unit, unit_color,
				   0, 0, "/5");
	position_unit_label(aq_aqi_unit, aq_aqi_value);
	y += 42;

	/* CO2 label */
	create_label(screen_airquality, font_label, label_color,
		     CONTENT_X_PAD, y, "CO2");
	y += 16;

	/* CO2 value (big) + unit (small) */
	aq_co2_value = create_label(screen_airquality, font_value, text_color,
				    CONTENT_X_PAD, y, "--");
	aq_co2_unit = create_label(screen_airquality, font_unit, unit_color,
				   0, 0, "ppm");
	position_unit_label(aq_co2_unit, aq_co2_value);
	y += 42;

	/* TVOC label */
	create_label(screen_airquality, font_label, label_color,
		     CONTENT_X_PAD, y, "TVOC");
	y += 16;

	/* TVOC value (big) + unit (small) */
	aq_tvoc_value = create_label(screen_airquality, font_value, text_color,
				     CONTENT_X_PAD, y, "--");
	aq_tvoc_unit = create_label(screen_airquality, font_unit, unit_color,
				    0, 0, "ppb");
	position_unit_label(aq_tvoc_unit, aq_tvoc_value);
}

/**
 * @brief Initialize the complete LVGL UI
 *
 * Creates two screens with a shared status bar on lv_layer_top.
 */
static void lvgl_ui_init(void)
{
	/* Build individual screens (each includes its own status bar) */
	build_weather_screen();
	build_airquality_screen();

	/* Set up screen array for indexed navigation */
	screens[0] = screen_weather;
	screens[1] = screen_airquality;

	/* Load the weather screen as the initial screen */
	lv_screen_load(screen_weather);
	current_screen = 0;

	LOG_INF("LVGL UI initialized (2 screens + status bar)");
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
 * @brief Update Air Quality screen labels with current sensor data
 */
static void update_airquality_display(void)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%u", aqi);
	lv_label_set_text(aq_aqi_value, buf);
	position_unit_label(aq_aqi_unit, aq_aqi_value);

	snprintf(buf, sizeof(buf), "%u", eco2);
	lv_label_set_text(aq_co2_value, buf);
	position_unit_label(aq_co2_unit, aq_co2_value);

	snprintf(buf, sizeof(buf), "%u", tvoc);
	lv_label_set_text(aq_tvoc_value, buf);
	position_unit_label(aq_tvoc_unit, aq_tvoc_value);
}

/**
 * @brief Update all sensor display data on both screens
 *
 * Both screens are updated even if only one is visible, so data is
 * current when the user navigates.
 */
static void update_sensor_display(void)
{
	update_weather_display();
	update_airquality_display();

	int temp_int = (int)temperature;
	int temp_frac = (int)((temperature - temp_int) * 10);
	if (temp_frac < 0) {
		temp_frac = -temp_frac;
	}
	int hum_int = (int)humidity;
	int hum_frac = (int)((humidity - hum_int) * 10);
	int press_int = (int)pressure;

	LOG_INF("Display update - Temp=%d.%d C, Hum=%d.%d%%, Press=%d hPa, "
		"AQI=%u, CO2=%u, TVOC=%u",
		temp_int, temp_frac, hum_int, hum_frac, press_int,
		aqi, eco2, tvoc);
}

/**
 * @brief Update time display on both screens
 *
 * @param seconds Number of seconds since boot
 */
static void update_time_display(uint32_t seconds)
{
	uint32_t hours = seconds / 3600;
	uint32_t minutes = (seconds % 3600) / 60;

	char time_str[16];
	snprintf(time_str, sizeof(time_str), "%02u:%02u", hours, minutes);

	lv_label_set_text(weather_time_label, time_str);
	lv_label_set_text(aq_time_label, time_str);
}

/* ========================================================================
 * Main application entry point
 * ======================================================================== */

int main(void)
{
	int ret;

	LOG_INF("WeatherSensor starting...");
	LOG_INF("Board: %s", CONFIG_BOARD);

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

	LOG_INF("Initialization complete. Entering main loop...");

	/* Main loop */
	while (1) {
		/* Handle pending screen navigation from button presses */
		if (pending_navigate != 0) {
			screen_navigate(pending_navigate);
			pending_navigate = 0;
		}

		/* Update time display every minute */
		if (seconds_counter % 60 == 0) {
			update_time_display(seconds_counter);
		}

		/* Read sensors every 10 seconds */
		if (sensors_ok && (seconds_counter % 10 == 0)) {
			LOG_INF("======== PERIODIC SENSOR READ (t=%us) ========",
				seconds_counter);
			sensors_read();
			update_sensor_display();
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
	}

	return 0;
}
