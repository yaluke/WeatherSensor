/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL UI: two-screen multi-display (weather + drift), shared status
 * bar, button input handling, and screen navigation.
 *
 * The module is self-contained: it owns every lv_obj_t in the
 * application, the screens array, the LVGL fonts, and the gpio-keys
 * input callback. Main only orchestrates — it calls ui_init() once,
 * then per-tick calls ui_update_sensor_display() / ui_update_time_
 * display() / ui_update_drift_display() / ui_update_battery() /
 * ui_update_wifi_icon() / ui_tick() and consumes ui_take_pending_
 * navigate() to drive the screen flip.
 */

#ifndef WEATHER_SENSOR_UI_H_
#define WEATHER_SENSOR_UI_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build both screens, the status bar, the drift rows, and switch to
 * screen 0. Idempotent in practice — call exactly once after
 * display_init() succeeds. */
void ui_init(void);

/* Atomically read+clear the screen-navigation request raised by the
 * button input handler. Returns +1 for next, -1 for previous, 0 for
 * "no pending navigation". Main loop polls each tick. */
int ui_take_pending_navigate(void);

/* Index of the currently-shown screen (0 = weather, 1 = drift). Read
 * from main loop only for status logging. */
int ui_current_screen(void);

/* Render the latest BME280 readings into the weather screen labels.
 * Pulls values via bme280_get_*() — no parameters needed. */
void ui_update_sensor_display(void);

/* Render the wall-clock (or uptime fallback) into the weather time
 * label. */
void ui_update_time_display(void);

/* Repaint the drift screen rows from the SNTP history ring. */
void ui_update_drift_display(void);

/* Update the status-bar battery symbol on every screen. */
void ui_update_battery(uint8_t soc);

/* Update the status-bar WiFi icon colour on every screen. Black on
 * true, red on false (the Adafruit Reverse panel reports BGR, so the
 * "red" colour is constructed accordingly). */
void ui_update_wifi_icon(bool connected);

/* Pump LVGL's task scheduler. Main loop calls this each second. */
void ui_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_UI_H_ */
