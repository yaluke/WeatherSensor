/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <stdio.h>

LOG_MODULE_REGISTER(weather_sensor, LOG_LEVEL_INF);

/* LED device (for debugging without UART console) */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Display power control (GPIO7 on Reverse board) - MUST be HIGH before display init */
static const struct gpio_dt_spec display_power =
	GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(i2c_reg), enable_gpios, 0);

/* Backlight control (GPIO45 on Reverse board) - uses LED subsystem alias */
static const struct gpio_dt_spec backlight =
	GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);

/* Display device */
static const struct device *display_dev;

/* LVGL objects */
static lv_obj_t *time_label;
static lv_obj_t *status_label;

/* Time counter (seconds since boot) */
static uint32_t seconds_counter = 0;

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

/**
 * @brief Initialize LVGL UI elements
 */
static void lvgl_ui_init(void)
{
	/* Set screen background color to blue for visibility */
	lv_obj_t *screen = lv_scr_act();
	lv_obj_set_style_bg_color(screen, lv_color_hex(0x0000FF), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

	/* Create a label for status */
	status_label = lv_label_create(screen);
	lv_label_set_text(status_label, "WeatherSensor v1.0");
	lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 10);
	lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);

	/* Create a label for time display */
	time_label = lv_label_create(screen);
	lv_label_set_text(time_label, "00:00:00");
	lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);

	LOG_INF("LVGL UI initialized");
}

/**
 * @brief Update time display
 *
 * @param seconds Number of seconds since boot
 */
static void update_time_display(uint32_t seconds)
{
	uint32_t hours = seconds / 3600;
	uint32_t minutes = (seconds % 3600) / 60;
	uint32_t secs = seconds % 60;

	char time_str[16];
	snprintf(time_str, sizeof(time_str), "%02u:%02u:%02u", hours, minutes, secs);

	lv_label_set_text(time_label, time_str);
}

/**
 * @brief Main application entry point
 */
int main(void)
{
	int ret;
	bool led_state = false;

	LOG_INF("WeatherSensor starting...");
	LOG_INF("Board: %s", CONFIG_BOARD);

	/* Initialize LED for visual debugging */
	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
	} else {
		ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure LED: %d", ret);
		} else {
			LOG_INF("LED initialized successfully");
			/* Blink LED 3 times at startup to show code is running */
			for (int i = 0; i < 3; i++) {
				gpio_pin_set_dt(&led, 1);
				k_msleep(200);
				gpio_pin_set_dt(&led, 0);
				k_msleep(200);
			}
		}
	}

	/* CRITICAL INITIALIZATION SEQUENCE:
	 * 1. Enable display power (GPIO7) first
	 * 2. Wait for power rail to stabilize
	 * 3. Initialize display device
	 * 4. Enable backlight (GPIO45)
	 */
	ret = display_power_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display power: %d", ret);
		/* Blink LED rapidly to indicate error */
		while (1) {
			gpio_pin_toggle_dt(&led);
			k_msleep(100);
		}
	}

	ret = display_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display: %d", ret);
		/* Blink LED rapidly to indicate error */
		while (1) {
			gpio_pin_toggle_dt(&led);
			k_msleep(100);
		}
	}

	ret = backlight_init();
	if (ret < 0) {
		LOG_WRN("Backlight init failed (non-fatal): %d", ret);
	}

	/* Initialize LVGL UI */
	lvgl_ui_init();

	LOG_INF("Initialization complete. Entering main loop...");

	/* Main loop */
	while (1) {
		/* Update time display every second */
		update_time_display(seconds_counter);

		/* Toggle LED every second to show system is running */
		led_state = !led_state;
		gpio_pin_set_dt(&led, led_state);

		/* Log status every 10 seconds */
		if (seconds_counter % 10 == 0) {
			LOG_INF("System running. Uptime: %u seconds", seconds_counter);
		}

		/* Process LVGL tasks (v8+ API) */
		lv_timer_handler();

		/* Sleep for 1 second */
		k_msleep(1000);
		seconds_counter++;
	}

	return 0;
}
