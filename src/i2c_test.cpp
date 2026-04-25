/*
 * Minimal I2C diagnostic for ESP32-S2 Reverse TFT
 * - Powers GPIO7 (I2C power rail) HIGH
 * - Probes all 7-bit I2C addresses
 * - Specifically checks BME280 (0x76), ENS160 (0x53), MAX17048 (0x36)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(i2c_test, LOG_LEVEL_INF);

/* GPIO7 = I2C power rail enable on Reverse TFT */
static const struct gpio_dt_spec i2c_power =
	GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(i2c_reg), enable_gpios, 0);

/* I2C0 on GPIO3 (SDA) / GPIO4 (SCL) */
static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

int main(void)
{
	LOG_INF("=== I2C DIAGNOSTIC START ===");

	/* Step 1: power on I2C rail */
	if (!gpio_is_ready_dt(&i2c_power)) {
		LOG_ERR("I2C power GPIO not ready!");
		return -1;
	}
	gpio_pin_configure_dt(&i2c_power, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set_dt(&i2c_power, 1);
	LOG_INF("I2C power enabled (GPIO%d = HIGH)", i2c_power.pin);
	k_msleep(100); /* Give sensors time to boot */

	/* Step 2: check I2C device ready */
	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C device not ready!");
		return -1;
	}
	LOG_INF("I2C device ready: %s", i2c_dev->name);

	/* Step 3: scan all addresses */
	LOG_INF("");
	LOG_INF("Scanning I2C bus (0x03 - 0x77)...");
	LOG_INF("");

	int found_count = 0;
	uint8_t dummy;

	for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
		/* Attempt a zero-length write to check if device ACKs */
		int ret = i2c_read(i2c_dev, &dummy, 1, addr);

		if (ret == 0) {
			found_count++;
			const char *name = "";
			if (addr == 0x36) { name = " (MAX17048 fuel gauge)"; }
			else if (addr == 0x53) { name = " (ENS160 air quality)"; }
			else if (addr == 0x76) { name = " (BME280 temp/hum/press)"; }
			LOG_INF("  FOUND: 0x%02X%s", addr, name);
		}
	}

	LOG_INF("");
	LOG_INF("Scan complete: %d device(s) found", found_count);
	LOG_INF("");

	/* Step 4: explicit check of expected devices */
	LOG_INF("--- Expected devices ---");
	struct {
		uint8_t addr;
		const char *name;
	} expected[] = {
		{0x36, "MAX17048 (fuel gauge)"},
		{0x53, "ENS160 (air quality)"},
		{0x76, "BME280 (temp/hum/press)"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		int ret = i2c_read(i2c_dev, &dummy, 1, expected[i].addr);
		LOG_INF("  0x%02X %s: %s",
			expected[i].addr, expected[i].name,
			ret == 0 ? "RESPONDING" : "NO RESPONSE");
	}

	LOG_INF("");
	LOG_INF("=== I2C DIAGNOSTIC COMPLETE ===");

	/* Idle forever */
	while (1) {
		k_msleep(10000);
	}
	return 0;
}
