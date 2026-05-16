/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sntp_sync.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
extern "C" {
#include <zephyr/net/sntp.h>
}

LOG_MODULE_REGISTER(sntp_sync, LOG_LEVEL_INF);

#define SNTP_SERVER             "pool.ntp.org"
#define SNTP_TIMEOUT_MS         5000
#define TIME_SYNC_INTERVAL_S    3600  /* 1 hour */

static bool time_synced = false;
static uint32_t seconds_since_last_sync = 0;
static volatile bool pending_initial_sync = false;
static volatile bool pending_drift_screen_update = false;

/* Ring buffer of recent SNTP drift measurements (last DRIFT_HISTORY_SIZE
 * comparisons; the very first sync is excluded — there's nothing to
 * compare it against). */
static drift_entry_t drift_history[DRIFT_HISTORY_SIZE];
static int drift_head = 0;  /* next slot to overwrite */

/* Convert an SNTP fractional-second value (Q32.32 fixed-point) to nanoseconds. */
static inline uint32_t sntp_fraction_to_ns(uint64_t fraction)
{
	return (uint32_t)((fraction * NSEC_PER_SEC) >> 32);
}

int sntp_sync_now(void)
{
	struct sntp_time st;
	int ret = sntp_simple(SNTP_SERVER, SNTP_TIMEOUT_MS, &st);
	if (ret < 0) {
		LOG_WRN("SNTP sync failed: %d (keeping previous time)", ret);
		return ret;
	}

	struct timespec new_ts = {
		.tv_sec  = (time_t)st.seconds,
		.tv_nsec = sntp_fraction_to_ns(st.fraction),
	};

	if (time_synced) {
		struct timespec before;
		clock_gettime(CLOCK_REALTIME, &before);
		int64_t before_ms = (int64_t)before.tv_sec * 1000 +
				    before.tv_nsec / 1000000;
		int64_t new_ms    = (int64_t)new_ts.tv_sec * 1000 +
				    new_ts.tv_nsec / 1000000;
		int64_t drift_ms  = before_ms - new_ms;
		LOG_INF("SNTP synced: drift = %+lld ms over %u s",
			(long long)drift_ms, seconds_since_last_sync);

		drift_entry_t *slot = &drift_history[drift_head];
		slot->drift_ms = drift_ms;
		slot->interval_s = seconds_since_last_sync;
		localtime_r(&new_ts.tv_sec, &slot->local_tm);
		slot->valid = true;
		drift_head = (drift_head + 1) % DRIFT_HISTORY_SIZE;
		pending_drift_screen_update = true;
	} else {
		struct tm utc;
		gmtime_r(&new_ts.tv_sec, &utc);
		LOG_INF("SNTP synced: initial value %04d-%02d-%02d %02d:%02d:%02d UTC",
			utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
			utc.tm_hour, utc.tm_min, utc.tm_sec);
	}

	if (clock_settime(CLOCK_REALTIME, &new_ts) < 0) {
		LOG_ERR("clock_settime failed: %d", errno);
		return -errno;
	}

	time_synced = true;
	seconds_since_last_sync = 0;
	return 0;
}

void sntp_tick(void)
{
	seconds_since_last_sync++;
}

bool sntp_is_synced(void)
{
	return time_synced;
}

bool sntp_should_resync(void)
{
	return time_synced && seconds_since_last_sync >= TIME_SYNC_INTERVAL_S;
}

void sntp_request_initial_sync(void)
{
	pending_initial_sync = true;
}

bool sntp_take_pending_initial_sync(void)
{
	if (pending_initial_sync) {
		pending_initial_sync = false;
		return true;
	}
	return false;
}

bool sntp_take_pending_drift_update(void)
{
	if (pending_drift_screen_update) {
		pending_drift_screen_update = false;
		return true;
	}
	return false;
}

void sntp_drift_snapshot(drift_entry_t out[DRIFT_HISTORY_SIZE])
{
	/* Caller wants chronological order, oldest first. drift_head
	 * points at the next slot to overwrite, so the oldest sample is
	 * at drift_head itself (or the slot is invalid if we haven't
	 * wrapped yet — render code handles the valid flag). */
	for (int i = 0; i < DRIFT_HISTORY_SIZE; i++) {
		int idx = (drift_head + i) % DRIFT_HISTORY_SIZE;
		out[i] = drift_history[idx];
	}
}
