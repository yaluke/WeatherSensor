/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAX17048 fuel gauge driver wrapper.
 *
 * Owns the device handle and the last-known state-of-charge. Non-fatal
 * if the chip isn't present: battery_is_available() returns false and
 * the rest of the system continues with no battery telemetry.
 *
 * battery_init() must run after the display power rail is up (the fuel
 * gauge shares the GPIO7-gated 3V3 line on this board).
 */

#ifndef WEATHER_SENSOR_BATTERY_H_
#define WEATHER_SENSOR_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the fuel gauge and stash the device handle. Returns 0 if the
 * chip is ready, -ENODEV otherwise (logged at WARN, never fatal). */
int battery_init(void);

/* Read state-of-charge from the chip and cache it. No-op if the gauge
 * is unavailable. Logs the new value at INFO. */
void battery_read(void);

/* True once battery_init succeeded. Influx capture branches on this to
 * record -1 when no gauge was found. */
bool battery_is_available(void);

/* Last value cached by battery_read. Returns the mocked default
 * (100 %) until the first successful read. */
uint8_t battery_get_soc(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_BATTERY_H_ */
