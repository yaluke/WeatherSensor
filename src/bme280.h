/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * BME280 environmental sensor — power-cycled per minute-tick read.
 *
 * Wraps the upstream Zephyr bosch,bme280 driver plus the surrounding
 * dance needed to keep the chip cold between reads:
 *
 *   1. Vcc (A5 / GPIO8) goes low between samples.
 *   2. SDA / SCL get re-muxed off the I2C peripheral and pinned to
 *      GND so the chip's ESD clamps cannot back-feed its internal
 *      rail through the bus pull-ups.
 *   3. On the next read, the bus is re-muxed to the I2C peripheral,
 *      i2c_recover_bus clears the controller's stale "bus busy"
 *      latch, CTRL_HUM is re-armed (the chip lost it with Vcc), and
 *      forced-mode sample_fetch runs.
 *
 * Why CONFIG_BME280_MODE_FORCED + 1× oversampling + IIR off (the
 * "weather-station" preset) and why all of the above lives here — see
 * the original commits that introduced this code. The lifecycle is:
 *
 *   bme280_init()                  -- in main(), early
 *   bme280_pin_release_to_runtime() -- in main(), after init done
 *   bme280_read()                  -- per minute-tick, forever
 */

#ifndef WEATHER_SENSOR_BME280_H_
#define WEATHER_SENSOR_BME280_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Probe the BME280 and the underlying I2C bus. Vcc is assumed already
 * applied by the gpio-hog in the DTS overlay so the bosch_bme280
 * driver's POST_KERNEL init could read the chip's calibration NVM. */
int bme280_init(void);

/* Hand the Vcc pin from boot-time gpio-hog control to runtime control,
 * leaving it driven LOW. Call once after bme280_init returns, before
 * the main loop starts ticking. From here on, bme280_read owns the pin
 * and toggles it per read. */
void bme280_pin_release_to_runtime(void);

/* Full minute-tick read: power up, recover bus, re-arm CTRL_HUM,
 * forced-mode sample_fetch, optionally run a piggy-back callback
 * while the bus is still alive, then park lines LOW, power down.
 *
 * The `while_bus_up` hook exists for other I2C0 devices (battery
 * fuel gauge etc.) so they get a brief window with a non-parked bus
 * without each owning their own wake/park sequence. Pass NULL when
 * there's no piggy-back work.
 *
 * Returns 0 even if the sample_fetch errored (so the main loop keeps
 * ticking); sub-step failures are LOG_WRN/LOG_ERR. */
int bme280_read(void (*while_bus_up)(void));

/* Most-recent readings cached by bme280_read. Temperature has the
 * CONFIG_WEATHER_TEMP_OFFSET_CDEG offset already applied; pressure is
 * in hPa (the driver returns kPa and bme280_read scales x10). */
float bme280_get_temperature(void);
float bme280_get_humidity(void);
float bme280_get_pressure(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_BME280_H_ */
