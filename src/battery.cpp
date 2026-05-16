/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery.h"

#include <zephyr/device.h>
#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static const struct device *fuel_gauge_dev;
static bool battery_available = false;
static uint8_t battery_soc = 100; /* State of charge %, mocked at 100% */

int battery_init(void)
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

void battery_read(void)
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

bool battery_is_available(void)
{
	return battery_available;
}

uint8_t battery_get_soc(void)
{
	return battery_soc;
}
