/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display.h"

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* Display power control (GPIO7 on Reverse board) - MUST be HIGH before display init */
static const struct gpio_dt_spec display_power =
	GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(i2c_reg), enable_gpios, 0);

/* Backlight control (GPIO45 on Reverse board) - uses LED subsystem alias */
static const struct gpio_dt_spec backlight =
	GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);

/* Bound in display_init(). Kept module-private; LVGL's Zephyr port
 * discovers the chosen display itself, so app code never needs this
 * pointer once init is done. */
static const struct device *display_dev;

int display_power_init(void)
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

int backlight_init(void)
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

int display_init(void)
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
