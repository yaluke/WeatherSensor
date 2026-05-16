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

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boot sequence:
 *   display_power_init()  -- GPIO7 HIGH, then 10 ms settle
 *   display_init()        -- bind to zephyr,display, drop blanking
 *   backlight_init()      -- GPIO45 HIGH (non-fatal if it fails)
 *
 * Each returns 0 on success or a negative errno.
 */
int display_power_init(void);
int display_init(void);
int backlight_init(void);

/*
 * Runtime power gating, for the button-wake/30s-timeout UX:
 *   display_sleep()       -- GPIO45 LOW, then GPIO7 LOW. Backlight
 *                            first so we don't briefly show a fading
 *                            frame while the controller loses power.
 *   display_resume()      -- GPIO7 HIGH, 10 ms settle, re-issue
 *                            display_blanking_off (the chip lost
 *                            state with Vcc), GPIO45 HIGH.
 *
 * Both idempotent. Track is_on internally so calling sleep when
 * already off (or resume when already on) is a no-op.
 *
 * After display_resume() the caller should invalidate the active
 * LVGL screen to force a redraw (the chip's internal frame buffer
 * came back as garbage).
 */
void display_sleep(void);
int display_resume(void);

/* Snapshot of the on/off state. */
bool display_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_DISPLAY_H_ */
