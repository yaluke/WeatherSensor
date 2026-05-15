/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared application state.
 *
 * Cross-module data the orchestrator hands around. Each module owns
 * its own hardware handles and module-private bookkeeping as file-
 * scope statics; only state that's *actually* read or written across
 * module boundaries lives here.
 *
 * Single source of truth: main.cpp owns one `static app_state_t g_app`
 * and passes a pointer into module init/tick calls that need it.
 */

#ifndef WEATHER_SENSOR_APP_STATE_H_
#define WEATHER_SENSOR_APP_STATE_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DRIFT_HISTORY_SIZE 8

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sensor snapshot                                                    */
/* ------------------------------------------------------------------ */

/* Latest readings written by bme280_read() and battery_read(), read by
 * ui_update_* and influx_capture_sample. The temperature value here is
 * already offset-corrected (CONFIG_WEATHER_TEMP_OFFSET_CDEG applied);
 * raw chip output stays inside bme280.cpp. */
typedef struct {
	float   temperature;       /* deg C, after CDEG offset */
	float   humidity;          /* %RH */
	float   pressure;          /* hPa */
	uint8_t battery_soc;       /* state of charge, 0-100. Valid iff
	                              battery_available is true. */
	bool    battery_available; /* fuel gauge responded at last read */
} sensor_data_t;

/* ------------------------------------------------------------------ */
/* WiFi station / provisioning state                                  */
/* ------------------------------------------------------------------ */

/* connected: set/cleared by wifi_mgmt_event_handler.
 * pending_initial_sync: raised on first connect, consumed by main loop
 *     to kick the very first SNTP fetch.
 * pending_save_credentials + pending_save_ssid/psk: handoff from the
 *     provisioning HTTP thread to the main loop, which performs the
 *     NVS write (the HTTP thread's stack can't take a flash op).
 * pending_ap_network_setup: raised by provisioning_start so the main
 *     loop can configure the AP's IPv4 address + DHCP pool (net_mgmt
 *     calls must not run in a network event callback). */
typedef struct {
	bool connected;
	bool pending_initial_sync;
	bool pending_save_credentials;
	char pending_save_ssid[33];
	char pending_save_psk[65];
	bool pending_ap_network_setup;
} wifi_state_t;

/* ------------------------------------------------------------------ */
/* Time / SNTP drift state                                            */
/* ------------------------------------------------------------------ */

typedef struct {
	int64_t  drift_ms;     /* signed: corrected - pre-correction */
	uint32_t interval_s;   /* seconds since the previous sync */
	struct   tm local_tm;  /* local wall clock at sync moment */
	bool     valid;        /* false slots are skipped on render */
} drift_entry_t;

/* synced: set true on the first successful sntp_sync_time call.
 * seconds_since_last_sync: incremented by the main loop's 1 Hz tick.
 * pending_initial_sync: WiFi event raises this; main loop consumes.
 * history / head: ring buffer of recent drifts (drift_history_size
 *     entries) for the drift screen.
 * pending_drift_screen_update: raised when a new drift sample lands,
 *     consumed by the UI redraw. */
typedef struct {
	bool          synced;
	uint32_t      seconds_since_last_sync;
	bool          pending_initial_sync;
	drift_entry_t history[DRIFT_HISTORY_SIZE];
	int           head;
	bool          pending_drift_screen_update;
} time_state_t;

/* ------------------------------------------------------------------ */
/* Aggregate                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	sensor_data_t sensors;
	wifi_state_t  wifi;
	time_state_t  time;
} app_state_t;

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_APP_STATE_H_ */
