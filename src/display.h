/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display power, backlight, and ST7789V bring-up.
 *
 * The Reverse TFT board needs GPIO7 driven HIGH before any SPI talk
 * to the display controller, and GPIO45 controls the backlight. PM
 * domain control crashes the SoC on this board, so we toggle both
 * GPIOs by hand.
 */

#ifndef WEATHER_SENSOR_DISPLAY_H_
#define WEATHER_SENSOR_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Required call order:
 *   display_power_init()  -- GPIO7 HIGH, then 10 ms settle
 *   display_init()        -- bind to zephyr,display, drop blanking
 *   backlight_init()      -- GPIO45 HIGH (non-fatal if it fails)
 *
 * Each returns 0 on success or a negative errno.
 */
int display_power_init(void);
int display_init(void);
int backlight_init(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_DISPLAY_H_ */
