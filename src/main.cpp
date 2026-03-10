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

/* Display device */
static const struct device *display_dev;

/* LVGL objects */
static lv_obj_t *time_label;
static lv_obj_t *status_label;

/* Time counter (seconds since boot) */
static uint32_t seconds_counter = 0;

/**
 * @brief Initialize the display subsystem
 *
 * @return 0 on success, negative errno on failure
 */
static int display_init(void)
{
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	LOG_INF("Display device initialized: %s", display_dev->name);
	return 0;
}

/**
 * @brief Initialize LVGL UI elements
 */
static void lvgl_ui_init(void)
{
	/* Create a label for status */
	status_label = lv_label_create(lv_scr_act());
	lv_label_set_text(status_label, "WeatherSensor v1.0");
	lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 10);
	lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);

	/* Create a label for time display */
	time_label = lv_label_create(lv_scr_act());
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

	/* Initialize display */
	ret = display_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display: %d", ret);
		/* Blink LED rapidly to indicate error */
		while (1) {
			gpio_pin_toggle_dt(&led);
			k_msleep(100);
		}
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
