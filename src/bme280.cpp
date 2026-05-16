/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bme280.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bme280, LOG_LEVEL_INF);

/* BME280 module Vcc — switched per-read to eliminate residual self-heating.
 * Wired off-board: external DFRobot Gravity module's +V lead moved from the
 * always-on 3V3 rail to the Feather's A5 pin (silk label A5 = ESP32-S2 GPIO8,
 * port 0). ~12 mA source budget, BME280 peak < 1 mA, so direct GPIO drive
 * is fine without a load switch. */
static const struct gpio_dt_spec bme280_power = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
	.pin = 8,
	.dt_flags = GPIO_ACTIVE_HIGH,
};

/* I2C0 SDA/SCL pads (GPIO3 / GPIO4 on this board). Between minute-spaced
 * reads we re-purpose them as plain GPIO outputs pinned LOW so the BME280's
 * SDA/SCL→Vcc ESD clamp diodes cannot conduct from the bus into its
 * de-powered analog front-end. The first sensor_read() bring-up then calls
 * pinctrl_apply_state(DEFAULT) to mux them back to the I2C peripheral.
 *
 * Doing this through the GPIO API (rather than a pinctrl sleep state) is
 * deliberate: the ESP32 pinctrl driver rejects any pin entry whose pinmux
 * carries neither an input nor output signal (-ENOTSUP), and reusing the
 * I2C signal IDs in a sleep group leaves the matrix routing the bus to
 * the I2C controller, which overrides the GPIO output level. */
static const struct gpio_dt_spec i2c0_sda = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
	.pin = 3,
	.dt_flags = GPIO_ACTIVE_HIGH,
};
static const struct gpio_dt_spec i2c0_scl = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
	.pin = 4,
	.dt_flags = GPIO_ACTIVE_HIGH,
};

/* I2C0 pinctrl handle — used to mux SDA/SCL back to the I2C peripheral
 * after the GPIO sleep state. Requires CONFIG_PINCTRL_DYNAMIC=y (which
 * selects PINCTRL_NON_STATIC) so the I2C driver's per-instance pinctrl
 * config is reachable from application code. */
PINCTRL_DT_DEV_CONFIG_DECLARE(DT_NODELABEL(i2c0));
static const struct pinctrl_dev_config *i2c0_pinctrl =
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c0));

static const struct device *bme280;

/* I2C bus the BME280 sits on — cached so bme280_read() can re-write the
 * chip's CTRL_HUM register after each power-on (the chip loses all config
 * registers when we cut Vcc, and the upstream Zephyr driver doesn't expose
 * a hook to re-init humidity oversampling per-fetch). */
static const struct device *i2c_dev;

static float temperature = 0.0f;
static float humidity = 0.0f;
static float pressure = 0.0f;

int bme280_init(void)
{
	LOG_INF("=== SENSOR INITIALIZATION START ===");

	/* I2C bus reference — used in bme280_read() to re-arm CTRL_HUM
	 * after each Vcc cycle (see comment on i2c_dev declaration). */
	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C0: Device not ready!");
		return -ENODEV;
	}

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

void bme280_pin_release_to_runtime(void)
{
	/* BME280 module Vcc (GPIO8 / A5) was hogged HIGH in the DTS overlay
	 * so the driver's POST_KERNEL chip-init could talk to the sensor.
	 * Switch the pin to runtime control and drive it low — from now on
	 * bme280_read() owns the pin and power-cycles it per minute-tick. */
	gpio_pin_configure_dt(&bme280_power, GPIO_OUTPUT_INACTIVE);
}

int bme280_read(void (*while_bus_up)(void))
{
	struct sensor_value val;
	int ret;

	LOG_INF("=== SENSOR READ START ===");

	/* Power the BME280 module's Vcc rail. Datasheet specifies ~2 ms
	 * start-up time; 5 ms gives margin for the module's onboard parts
	 * (the DFRobot Gravity adds a 3V3 LDO between our GPIO and the
	 * chip). The chip lost all config registers while Vcc was off and
	 * comes up in sleep mode with everything zeroed.
	 *
	 * Order matters: bring Vcc up BEFORE re-muxing SDA/SCL to the I2C
	 * peripheral. Once the chip is at 3.3 V its ESD clamps are inert,
	 * so engaging the bus signals (and their pull-ups) is clean. */
	gpio_pin_set_dt(&bme280_power, 1);
	k_msleep(5);
	ret = pinctrl_apply_state(i2c0_pinctrl, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_WRN("BME280: pinctrl default apply failed: %d", ret);
	}

	/* Reset the I2C controller's FSM. While SDA/SCL were pinned LOW
	 * the peripheral saw "bus busy / mid-transfer" and latches that
	 * state internally. i2c_recover_bus drives 9 clock pulses + STOP
	 * and re-initialises the controller, so the next transfer doesn't
	 * fail with -EBUSY at the i2c_ll_is_bus_busy() check. */
	ret = i2c_recover_bus(i2c_dev);
	if (ret < 0) {
		LOG_WRN("BME280: i2c_recover_bus failed: %d", ret);
	}

	/* Re-arm humidity oversampling. The upstream driver writes
	 * CTRL_HUM (0xF2) only at init; in forced mode sample_fetch
	 * touches CTRL_MEAS but not CTRL_HUM. After our Vcc cycle the
	 * chip's CTRL_HUM has reverted to 0 (humidity disabled), so we
	 * write 0x01 (oversampling x1) directly via the I2C API. */
	ret = i2c_reg_write_byte(i2c_dev, 0x77, 0xF2, 0x01);
	if (ret < 0) {
		LOG_WRN("BME280: CTRL_HUM re-arm failed: %d", ret);
	}

	/* Read BME280 */
	LOG_INF("BME280: Fetching sample...");
	ret = sensor_sample_fetch(bme280);
	if (ret < 0) {
		LOG_ERR("BME280: sample_fetch failed: %d", ret);
	} else {
		LOG_INF("BME280: Sample fetch OK");

		sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &val);
		/* Subtract the build-time offset (centi-deg C) to compensate
		 * for residual board-conduction self-heating left over after
		 * switching the BME280 to forced mode. */
		temperature = sensor_value_to_float(&val)
			- (float)CONFIG_WEATHER_TEMP_OFFSET_CDEG / 100.0f;
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

	/* Piggy-back any other I2C0 traffic the caller wants to do while
	 * the bus is still alive (typically the fuel-gauge poll). Runs
	 * even on sample_fetch failure — those failures are confined to
	 * the BME280 chip, the bus itself is fine. */
	if (while_bus_up) {
		while_bus_up();
	}

	/* Park SDA/SCL at GND first, then cut Vcc. With Vcc still applied,
	 * dragging the bus lines low is at worst a malformed I2C condition
	 * the chip ignores. With the lines pinned LOW, removing Vcc leaves
	 * no path through the chip's SDA/SCL→Vcc ESD clamps, so the analog
	 * front-end can't be back-powered. Reversing the order would let
	 * the bus pull-ups keep the chip warm via diode conduction — the
	 * exact failure mode this change exists to kill.
	 *
	 * GPIO_OUTPUT_LOW reconfigures the pad as a push-pull output and
	 * latches it at 0. This re-routes the GPIO matrix away from the
	 * I2C peripheral (esp32_gpio driver calls esp_rom_gpio_matrix_out
	 * with SIG_GPIO_OUT_IDX), and the pad's internal pull-up is
	 * implicitly disabled by the GPIO_OUTPUT_LOW flag set. */
	ret = gpio_pin_configure_dt(&i2c0_sda, GPIO_OUTPUT_LOW);
	if (ret < 0) {
		LOG_WRN("BME280: SDA park-low failed: %d", ret);
	}
	ret = gpio_pin_configure_dt(&i2c0_scl, GPIO_OUTPUT_LOW);
	if (ret < 0) {
		LOG_WRN("BME280: SCL park-low failed: %d", ret);
	}
	gpio_pin_set_dt(&bme280_power, 0);

	LOG_INF("=== SENSOR READ COMPLETE ===");
	return 0;
}

float bme280_get_temperature(void)
{
	return temperature;
}

float bme280_get_humidity(void)
{
	return humidity;
}

float bme280_get_pressure(void)
{
	return pressure;
}
