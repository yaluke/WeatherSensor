/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * SNTP wall-clock sync + drift telemetry ring buffer.
 *
 * sntp_sync_now() runs synchronously (sntp_simple blocks for up to a
 * few seconds including DNS) — the main loop calls it at most once per
 * hour, never during user interaction, so the stall is acceptable. On
 * every sync after the first, the difference between the pre-sync and
 * post-sync wall clock is logged as a "drift" sample and pushed into a
 * fixed-size ring the drift screen renders from.
 *
 * The module owns:
 *   - the synced flag (until the first successful sync, timestamps are
 *     uptime-relative and nothing wall-clock-dependent runs);
 *   - the seconds-since-last-sync counter the main loop increments via
 *     sntp_tick() and which sntp_should_resync() compares against the
 *     1-hour threshold;
 *   - two volatile "pending" flags handed off from the WiFi event
 *     handler (initial sync) and from sntp_sync_now itself (drift
 *     screen needs redraw).
 */

#ifndef WEATHER_SENSOR_SNTP_SYNC_H_
#define WEATHER_SENSOR_SNTP_SYNC_H_

#include <stdbool.h>

#include "app_state.h"   /* drift_entry_t, DRIFT_HISTORY_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* Fetch wall-clock from pool.ntp.org and clock_settime() it. On any
 * sync after the first, logs and records the drift sample. Returns 0
 * on success, negative errno on SNTP/DNS/settime failure (in which
 * case the previous clock is kept). */
int sntp_sync_now(void);

/* Bump the seconds-since-last-sync counter. Call from the 1 Hz main
 * loop. Cheap; just an integer increment. */
void sntp_tick(void);

/* True once any sync has succeeded. Influx capture and the time-of-
 * day display both gate on this. */
bool sntp_is_synced(void);

/* True iff we are already synced AND the configured re-sync interval
 * has elapsed since the last successful sync. Main loop polls this
 * each tick to decide whether to call sntp_sync_now again. */
bool sntp_should_resync(void);

/* Raised by the WiFi mgmt event handler the moment we first connect.
 * Consumed (atomically read + cleared) by the main loop. */
void sntp_request_initial_sync(void);
bool sntp_take_pending_initial_sync(void);

/* Raised inside sntp_sync_now when a new drift sample lands. Consumed
 * by the UI redraw path. */
bool sntp_take_pending_drift_update(void);

/* Copy the drift ring into the caller's buffer in chronological order
 * (oldest first). The caller's array must have DRIFT_HISTORY_SIZE
 * slots. Used by the drift screen to render without exposing the
 * underlying ring layout. */
void sntp_drift_snapshot(drift_entry_t out[DRIFT_HISTORY_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_SNTP_SYNC_H_ */
