/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * SoftAP provisioning — bring up an open AP named "WeatherSensor",
 * configure 192.168.4.1/24 with a 4-slot DHCP pool, and serve a tiny
 * HTTP form at http://192.168.4.1/ that accepts SSID + password.
 *
 * Triggered by a long-press on D2 (button input → main loop →
 * provisioning_request → provisioning_take_pending_request →
 * provisioning_start). The AP enable is asynchronous; the net_mgmt
 * callback in wifi_sta raises a flag the main loop consumes to call
 * provisioning_configure_ap_network — the post-enable IP/DHCP/HTTP
 * bring-up that net_mgmt forbids inside a callback.
 *
 * Form submit stashes the new credentials and arms a reboot. The main
 * loop drains via provisioning_take_pending_reboot, persists the creds
 * through wifi_sta, and calls sys_reboot. We reboot rather than
 * runtime-transition AP→STA because that path is fragile on ESP32-S2
 * with Zephyr 4.4.
 */

#ifndef WEATHER_SENSOR_PROVISIONING_H_
#define WEATHER_SENSOR_PROVISIONING_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the "user wants provisioning" flag. Cheap; safe from a button
 * input handler context. */
void provisioning_request(void);

/* Atomically read+clear the request flag. Main loop polls this and
 * calls provisioning_start when it returns true. */
bool provisioning_take_pending_request(void);

/* Disconnect STA, enable AP. Synchronous, blocks for ~500 ms while
 * the driver settles. Returns 0 on net_mgmt success, negative errno
 * otherwise. Idempotent: returns 0 immediately if already active. */
int provisioning_start(void);

/* Assign 192.168.4.1 to the AP interface, start the DHCP server,
 * and kick the HTTP listener. Must run from a thread context (not a
 * net_mgmt event callback). Idempotent: returns immediately if
 * already configured. */
void provisioning_configure_ap_network(void);

/* Atomically read+clear the "POST /connect just landed" flag. On
 * true, the caller-supplied buffers are filled with the SSID and PSK
 * from the form. Caller is then expected to persist via
 * wifi_save_credentials and reboot. */
bool provisioning_take_pending_reboot(char *ssid_out, size_t ssid_sz,
				      char *psk_out, size_t psk_sz);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_PROVISIONING_H_ */
