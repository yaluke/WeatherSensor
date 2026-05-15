/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "battery.h"
#include "bme280.h"
#include "display.h"
#include "influx.h"
#include "provisioning.h"
#include "sntp_sync.h"
#include "ui.h"
#include "wifi_sta.h"

LOG_MODULE_REGISTER(weather_sensor, LOG_LEVEL_INF);

static uint32_t seconds_counter = 0;

/* Piggy-back hook for bme280_read: refresh the battery state-of-charge
 * and its status-bar symbol while the I2C0 bus is still alive. Keeps
 * the MAX17048 off the bus during the windows when bme280_read parks
 * SDA/SCL low. */
static void refresh_battery(void)
{
	battery_read();
	ui_update_battery(battery_get_soc());
}

/* Returns true at most once per wall-clock minute, on the boundary.
 *
 * After SNTP has set the clock we use tm_min from local time; before
 * that we fall back to an uptime-minute boundary so the display is
 * never frozen. Accepts tm_sec == 0 or 1 to absorb scheduler jitter
 * from the 1 s main-loop tick; the last-minute dedupe ensures only
 * one fire per minute regardless. */
static bool minute_boundary_fired(void)
{
	static int last_minute_fired = -1;
	int minute_now;

	if (sntp_is_synced()) {
		struct timespec ts;
		struct tm local;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &local);
		if (local.tm_sec > 1) {
			return false;
		}
		minute_now = local.tm_min;
	} else {
		uint32_t up_s = (uint32_t)(k_uptime_get() / 1000);
		if (up_s % 60 != 0) {
			return false;
		}
		minute_now = (int)((up_s / 60) % 60);
	}

	if (minute_now == last_minute_fired) {
		return false;
	}
	last_minute_fired = minute_now;
	return true;
}

int main(void)
{
	int ret;

	LOG_INF("WeatherSensor starting...");
	LOG_INF("Board: %s", CONFIG_BOARD);

	/* Pin local time to Europe/Warsaw with DST. POSIX TZ string:
	 *   CET-1CEST,M3.5.0,M10.5.0/3
	 * = standard CET (UTC+1), DST CEST (UTC+2), spring-forward last
	 * Sunday of March, fall-back last Sunday of October at 03:00.
	 * After this, picolibc's localtime_r() handles DST automatically. */
	setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
	tzset();

	/* CRITICAL INITIALIZATION SEQUENCE:
	 * 1. Enable display power (GPIO7) first
	 * 2. Wait for power rail to stabilize
	 * 3. Initialize display device
	 * 4. Enable backlight (GPIO45)
	 */
	ret = display_power_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display power: %d", ret);
		return ret;
	}

	ret = display_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize display: %d", ret);
		return ret;
	}

	ret = backlight_init();
	if (ret < 0) {
		LOG_WRN("Backlight init failed (non-fatal): %d", ret);
	}

	/* Initialize sensors */
	LOG_INF("");
	LOG_INF("========================================");
	ret = bme280_init();
	static bool sensors_ok = false;
	sensors_ok = (ret == 0);
	if (!sensors_ok) {
		LOG_ERR("Sensor initialization failed: %d", ret);
		LOG_ERR("Continuing without sensors...");
	}
	LOG_INF("========================================");
	LOG_INF("");

	/* Hand the BME280 Vcc pin from boot-time gpio-hog control to
	 * runtime control. bme280_read now owns it and toggles it per
	 * minute-tick. */
	bme280_pin_release_to_runtime();

	/* Initialize fuel gauge (after display power, since GPIO7 powers I2C bus) */
	ret = battery_init();
	if (ret < 0) {
		LOG_WRN("Fuel gauge not available - using mocked battery");
	}

	/* Initialize LVGL multi-screen UI */
	ui_init();

	/* Initial sensor read if sensors available. Piggy-back the
	 * battery poll so it gets the I2C bus while it's still up. */
	if (sensors_ok) {
		LOG_INF("Performing initial sensor read...");
		bme280_read(refresh_battery);
		ui_update_sensor_display();
	} else {
		/* No BME280 — battery still wants to be read. The bus is
		 * idle (no park-low cycle without bme280_read), so it's
		 * safe to talk to MAX17048 directly. */
		refresh_battery();
	}

	/* Mount NVS + register net_mgmt callback, then try to connect.
	 * Priority: stored NVS credentials > Kconfig test credentials.
	 * If Kconfig credentials connect successfully, they're saved to NVS
	 * for next boot — so the next reflash won't need the Kconfig override. */
	if (wifi_sta_init() == 0) {
		char ssid[33] = {};
		char psk[65] = {};
		if (wifi_load_credentials(ssid, sizeof(ssid), psk, sizeof(psk))) {
			wifi_connect(ssid, psk);
		} else if (strlen(CONFIG_WEATHER_WIFI_TEST_SSID) > 0) {
			LOG_INF("Using Kconfig test credentials for WiFi");
			wifi_arm_pending_save(CONFIG_WEATHER_WIFI_TEST_SSID,
					      CONFIG_WEATHER_WIFI_TEST_PSK);
			wifi_connect(CONFIG_WEATHER_WIFI_TEST_SSID,
				     CONFIG_WEATHER_WIFI_TEST_PSK);
		} else {
			LOG_INF("No WiFi credentials stored — provisioning needed");
		}
	}

	LOG_INF("Initialization complete. Entering main loop...");

	while (1) {
		/* Screen navigation from button presses. Module handles the
		 * LVGL screen swap internally; we ignore the return. */
		(void)ui_take_pending_navigate();

		/* Handle pending provisioning request from D1 */
		if (provisioning_take_pending_request()) {
			provisioning_start();
		}

		/* After AP_ENABLE_RESULT fires: assign IP and start DHCP */
		if (wifi_take_pending_ap_setup()) {
			provisioning_configure_ap_network();
		}

		/* Credentials received via HTTP form: persist to NVS and
		 * reboot. The new credentials take effect on the clean cold
		 * boot, avoiding fragile runtime AP→STA driver transitions. */
		char apply_ssid[33];
		char apply_psk[65];
		if (provisioning_take_pending_reboot(apply_ssid, sizeof(apply_ssid),
						     apply_psk, sizeof(apply_psk))) {
			LOG_INF("Saving credentials and rebooting (SSID: %s)",
				apply_ssid);
			wifi_save_credentials(apply_ssid, apply_psk);
			/* Let the HTTP response flush to the browser. */
			k_msleep(1000);
			sys_reboot(SYS_REBOOT_COLD);
			/* unreachable */
		}

		/* WiFi status-bar icon repaint */
		if (wifi_take_pending_icon_update()) {
			ui_update_wifi_icon(wifi_is_connected());
		}

		/* New SNTP drift sample arrived — repaint the drift screen.
		 * Cheap; LVGL only redraws the active screen. */
		if (sntp_take_pending_drift_update()) {
			ui_update_drift_display();
		}

		/* SNTP wall-clock sync: first sync as soon as WiFi is up,
		 * then every hour after a successful sync. sntp_sync_now()
		 * blocks for up to a few seconds — fine at this cadence. */
		if (sntp_take_pending_initial_sync()) {
			sntp_sync_now();
		} else if (wifi_is_connected() && sntp_should_resync()) {
			sntp_sync_now();
		}

		/* Once per minute, aligned to the wall-clock minute start
		 * (or the uptime-minute start before SNTP has synced). One
		 * trigger keeps the sensor read and the visible HH:MM
		 * transition phase-locked. */
		if (minute_boundary_fired()) {
			ui_update_time_display();
			if (sensors_ok) {
				LOG_INF("======== PERIODIC SENSOR READ (t=%us) ========",
					seconds_counter);
				/* Piggy-back the battery read inside the
				 * bme280 bus-up window so it doesn't try to
				 * talk to MAX17048 while SDA/SCL are parked. */
				bme280_read(refresh_battery);
				ui_update_sensor_display();
				/* Queue this reading for the InfluxDB uplink.
				 * Only when SNTP has set the wall clock — every
				 * row carries a real timestamp, no server-side
				 * "receive time" mess. */
				if (sntp_is_synced()) {
					influx_capture_sample();
				}
			}
		}

		/* Drain pending Influx samples — cheap when the buffer is
		 * empty, retries from the oldest unsent reading on failure. */
		influx_drain(wifi_is_connected());

		/* Log status every 30 seconds */
		if (seconds_counter % 30 == 0) {
			LOG_INF("System running. Uptime: %u seconds, Screen: %d",
				seconds_counter, ui_current_screen());
		}

		/* Process LVGL tasks */
		ui_tick();

		/* Sleep for 1 second */
		k_msleep(1000);
		seconds_counter++;
		sntp_tick();
	}

	return 0;
}
