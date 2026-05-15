/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi station-mode connection + NVS credential storage.
 *
 * Lifecycle at boot:
 *   wifi_sta_init()                       NVS mount + net_mgmt callback
 *   wifi_load_credentials(...)            try the persisted SSID/PSK
 *   wifi_connect(ssid, psk)               if any found, kick a connect
 *
 * Runtime:
 *   wifi_is_connected()                   gate predicate for uplink
 *   wifi_take_pending_icon_update()       UI consumes when icon needs
 *                                         a colour flip on (re)connect
 *
 * Provisioning re-save path (test-SSID fast path): when we're about
 * to connect with credentials that came from Kconfig (not NVS),
 * wifi_arm_pending_save() stashes them. The net_mgmt event handler
 * will write them to NVS the moment the association succeeds, so a
 * power cycle without re-flashing keeps the connection. After a
 * provisioning-form submit the main loop calls wifi_save_credentials
 * directly (and then reboots).
 */

#ifndef WEATHER_SENSOR_WIFI_STA_H_
#define WEATHER_SENSOR_WIFI_STA_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount NVS, register the net_mgmt event callback. Returns 0 on
 * success or a negative errno (NVS mount can fail; net_mgmt register
 * does not). */
int wifi_sta_init(void);

/* Read SSID + PSK from NVS into the caller's buffers. Returns true
 * when both keys were present; false otherwise (and the buffers are
 * left untouched). */
bool wifi_load_credentials(char *ssid, size_t ssid_size,
			   char *psk, size_t psk_size);

/* Persist SSID + PSK to NVS. Returns 0 on success or a negative
 * errno from the underlying nvs_write. Each value is stored as a
 * separate key including the NUL terminator. */
int wifi_save_credentials(const char *ssid, const char *psk);

/* Request an STA association. Synchronous net_mgmt(NET_REQUEST_
 * WIFI_CONNECT). Logs the SSID; the actual connect happens
 * asynchronously and reports back through the event callback. */
int wifi_connect(const char *ssid, const char *psk);

/* Stash credentials to write to NVS *after* the next successful
 * association. Used for the test-SSID Kconfig path so the first
 * boot's Kconfig credentials become the persistent set without
 * reflashing. */
void wifi_arm_pending_save(const char *ssid, const char *psk);

/* Snapshot of the connect state set/cleared by the net_mgmt event
 * handler. */
bool wifi_is_connected(void);

/* Raised inside the event handler on every connect/disconnect so the
 * UI can repaint the WiFi icon colour. Read+clear atomically. */
bool wifi_take_pending_icon_update(void);

/* Raised inside the event handler when the WiFi driver finishes
 * bringing up SoftAP mode. The provisioning module consumes this
 * from the main loop and then configures the AP's IPv4 address and
 * DHCP pool — net_mgmt calls that must not run inside an event
 * callback. */
bool wifi_take_pending_ap_setup(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_SENSOR_WIFI_STA_H_ */
