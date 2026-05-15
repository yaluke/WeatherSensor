/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * InfluxDB uplink — per-minute sensor data to a LAN database.
 *
 * Hardcoded LAN-only assumptions: no TLS, no auth. Fine inside a
 * trusted home network.
 *
 * Architecture:
 *   - A fixed ring buffer (INFLUX_BUFFER_SIZE samples ≈ 6 h at one
 *     sample/min) absorbs offline gaps. Oldest-overwrite when full.
 *   - Capture pulls the latest readings from bme280_* and battery_*
 *     and pushes one sample with a wall-clock ns timestamp.
 *   - Drain walks the ring oldest-first, building Influx line-protocol
 *     and POSTing one line per HTTP request, stopping on the first
 *     error so a flapping NAS isn't hammered.
 *
 * Lifecycle:
 *   per minute, after sntp_is_synced()        -> influx_capture_sample()
 *   per second (every main-loop tick)         -> influx_drain(wifi_up)
 */

#ifndef WEATHER_SENSOR_INFLUX_H_
#define WEATHER_SENSOR_INFLUX_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot sensor + battery + wall-clock time into one ring slot.
 * Logs queue depth. Pre-condition: sntp_is_synced() — otherwise the
 * sample's timestamp would be the post-boot zero epoch. */
void influx_capture_sample(void);

/* Attempt to ship every queued sample, oldest first. Halts on the
 * first POST failure (and on wifi_up=false), leaving the rest in the
 * ring for the next call. Caller passes WiFi state explicitly so the
 * module stays decoupled from wifi_sta.h. */
void influx_drain(bool wifi_up);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_INFLUX_H_ */
