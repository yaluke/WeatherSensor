/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "provisioning.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
extern "C" {
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
}

LOG_MODULE_REGISTER(provisioning, LOG_LEVEL_INF);

#define PROVISIONING_AP_SSID "WeatherSensor"

static volatile bool pending_provisioning = false;
static bool provisioning_active = false;
static bool ap_network_configured = false;

/* Forward declaration — defined in the HTTP server section below. */
static void http_server_start(void);

void provisioning_request(void)
{
	pending_provisioning = true;
}

bool provisioning_take_pending_request(void)
{
	if (pending_provisioning) {
		pending_provisioning = false;
		return true;
	}
	return false;
}

void provisioning_configure_ap_network(void)
{
	if (ap_network_configured) {
		return;
	}
	struct net_if *ap_iface = net_if_get_default();
	if (!ap_iface) {
		LOG_ERR("AP interface not available in event handler");
		return;
	}
	ap_network_configured = true;

	struct in_addr ap_addr, netmask, pool_start;
	net_addr_pton(AF_INET, "192.168.4.1", &ap_addr);
	net_addr_pton(AF_INET, "255.255.255.0", &netmask);
	net_addr_pton(AF_INET, "192.168.4.10", &pool_start);

	net_if_ipv4_set_gw(ap_iface, &ap_addr);
	if (!net_if_ipv4_addr_add(ap_iface, &ap_addr, NET_ADDR_MANUAL, 0)) {
		LOG_ERR("Failed to set AP IP address");
		return;
	}
	if (!net_if_ipv4_set_netmask_by_addr(ap_iface, &ap_addr, &netmask)) {
		LOG_ERR("Failed to set AP netmask");
	}

	int ret = net_dhcpv4_server_start(ap_iface, &pool_start);
	if (ret < 0) {
		LOG_ERR("DHCP server start failed: %d", ret);
	} else {
		LOG_INF("DHCP server started (pool: 192.168.4.10 - .13)");
	}

	http_server_start();

	LOG_INF("AP IP: 192.168.4.1 — connect to '%s' and open http://192.168.4.1/",
		PROVISIONING_AP_SSID);
}

/* ========================================================================
 * Minimal HTTP server for SoftAP provisioning
 * ========================================================================
 *
 * Serves a single HTML form at http://192.168.4.1/ while the AP is up.
 * GET /         → form with SSID + password fields
 * POST /connect → URL-decode the form body, persist credentials to NVS,
 *                 then sys_reboot(SYS_REBOOT_COLD). The new credentials
 *                 take effect on the clean cold boot rather than via a
 *                 fragile runtime AP→STA mode transition.
 */

#define HTTP_PORT      80
#define HTTP_STACK_SIZE 3072
#define HTTP_RECV_BUF  2048

K_THREAD_STACK_DEFINE(http_stack, HTTP_STACK_SIZE);
static struct k_thread http_thread_data;
static k_tid_t http_thread_tid = NULL;
static volatile bool http_server_run = false;

/* Credentials received via POST /connect, waiting for main loop to
 * persist them to NVS and reboot. We reboot rather than transition the
 * WiFi driver from AP→STA at runtime because the latter is fragile on
 * ESP32-S2 with Zephyr 4.4 — see plan velvet-zooming-hanrahan.md. */
static char pending_apply_ssid[33];
static char pending_apply_psk[65];
static volatile bool pending_reboot = false;

static const char PROVISIONING_HTML[] =
	"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>WeatherSensor setup</title>"
	"<style>body{font-family:sans-serif;margin:2em;max-width:420px}"
	"label{display:block;margin-top:1em}input{width:100%;padding:.5em;"
	"font-size:1em;box-sizing:border-box}button{margin-top:1.5em;padding:"
	".7em 1.5em;font-size:1em}</style></head><body>"
	"<h1>WeatherSensor WiFi setup</h1>"
	"<form method=\"POST\" action=\"/connect\">"
	"<label>SSID<input name=\"ssid\" required></label>"
	"<label>Password<input name=\"psk\" type=\"password\"></label>"
	"<button type=\"submit\">Connect</button></form></body></html>";

static void http_send_form(int client_fd)
{
	char header[128];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned)(sizeof(PROVISIONING_HTML) - 1));
	zsock_send(client_fd, header, hlen, 0);
	zsock_send(client_fd, PROVISIONING_HTML,
		   sizeof(PROVISIONING_HTML) - 1, 0);
}

static void http_send_simple(int client_fd, const char *status,
			      const char *body)
{
	char buf[256];
	int len = snprintf(buf, sizeof(buf),
		"HTTP/1.0 %s\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n%s",
		status, (unsigned)strlen(body), body);
	zsock_send(client_fd, buf, len, 0);
}

/* Decode a URL-encoded substring (`+` → space, `%XX` → byte) from src[0..src_len)
 * into dst[0..dst_size). Always NUL-terminates. Returns decoded length. */
static size_t url_decode(const char *src, size_t src_len,
			 char *dst, size_t dst_size)
{
	size_t di = 0;
	for (size_t si = 0; si < src_len && di + 1 < dst_size; si++) {
		char c = src[si];
		if (c == '+') {
			dst[di++] = ' ';
		} else if (c == '%' && si + 2 < src_len) {
			char hex[3] = {src[si + 1], src[si + 2], '\0'};
			dst[di++] = (char)strtol(hex, NULL, 16);
			si += 2;
		} else {
			dst[di++] = c;
		}
	}
	dst[di] = '\0';
	return di;
}

/* Find `name=...` in a URL-encoded form body and URL-decode the value into
 * out[0..out_size). Returns true on success. */
static bool form_get_field(const char *body, const char *name,
			   char *out, size_t out_size)
{
	size_t name_len = strlen(name);
	const char *p = body;
	while (p && *p) {
		if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
			const char *val = p + name_len + 1;
			const char *end = strchr(val, '&');
			size_t vlen = end ? (size_t)(end - val) : strlen(val);
			url_decode(val, vlen, out, out_size);
			return true;
		}
		p = strchr(p, '&');
		if (p) {
			p++;
		}
	}
	return false;
}

/* Handle POST /connect: parse the form body, stash credentials for the
 * main loop to apply. On success, send an HTML page telling the user to
 * reconnect to their home WiFi. */
static void http_handle_post_connect(int client_fd, const char *request,
				     int received)
{
	/* Body starts after the blank line \r\n\r\n */
	const char *body = strstr(request, "\r\n\r\n");
	if (!body) {
		http_send_simple(client_fd, "400 Bad Request",
				 "Missing body");
		return;
	}
	body += 4;

	char ssid[33] = {};
	char psk[65] = {};
	if (!form_get_field(body, "ssid", ssid, sizeof(ssid)) ||
	    strlen(ssid) == 0) {
		http_send_simple(client_fd, "400 Bad Request",
				 "SSID required");
		return;
	}
	form_get_field(body, "psk", psk, sizeof(psk));

	LOG_INF("HTTP: received credentials for SSID '%s' (psk len=%u)",
		ssid, (unsigned)strlen(psk));

	strncpy(pending_apply_ssid, ssid, sizeof(pending_apply_ssid) - 1);
	pending_apply_ssid[sizeof(pending_apply_ssid) - 1] = '\0';
	strncpy(pending_apply_psk, psk, sizeof(pending_apply_psk) - 1);
	pending_apply_psk[sizeof(pending_apply_psk) - 1] = '\0';
	pending_reboot = true;

	static const char OK_HTML[] =
		"<!DOCTYPE html><html><body><h1>Saved</h1>"
		"<p>WeatherSensor will now reboot and connect to your WiFi. "
		"You can reconnect your phone to your normal network.</p>"
		"</body></html>";
	char header[128];
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned)(sizeof(OK_HTML) - 1));
	zsock_send(client_fd, header, hlen, 0);
	zsock_send(client_fd, OK_HTML, sizeof(OK_HTML) - 1, 0);
}

/* Single static recv buffer — accept loop is serial so no concurrency.
 * Sized to fit a typical browser POST (User-Agent and friends can be large). */
static char http_buf[HTTP_RECV_BUF];

/* Read until we have the full HTTP request (headers + Content-Length bytes
 * of body, if any) or the buffer fills / peer closes. Returns total bytes. */
static int http_recv_request(int client_fd)
{
	int total = 0;
	int header_end = -1;
	int content_length = 0;

	while (total < (int)sizeof(http_buf) - 1) {
		int n = zsock_recv(client_fd, http_buf + total,
				   sizeof(http_buf) - 1 - total, 0);
		if (n <= 0) {
			break;
		}
		total += n;
		http_buf[total] = '\0';

		if (header_end < 0) {
			char *hdr_end = strstr(http_buf, "\r\n\r\n");
			if (hdr_end) {
				header_end = (hdr_end - http_buf) + 4;
				/* Case-insensitive search for "content-length:". */
				static const char NEEDLE[] = "content-length:";
				for (char *p = http_buf; p + 15 < hdr_end; p++) {
					bool match = true;
					for (int i = 0; i < 15; i++) {
						char c = p[i];
						if (c >= 'A' && c <= 'Z') {
							c = c - 'A' + 'a';
						}
						if (c != NEEDLE[i]) {
							match = false;
							break;
						}
					}
					if (match) {
						content_length =
							(int)strtol(p + 15,
								    NULL, 10);
						break;
					}
				}
			}
		}
		if (header_end >= 0 &&
		    total - header_end >= content_length) {
			break;
		}
	}
	return total;
}

static void http_handle_client(int client_fd)
{
	int received = http_recv_request(client_fd);
	if (received <= 0) {
		zsock_close(client_fd);
		return;
	}

	/* Log the request line (first line up to \r or \n) for debugging. */
	char req_line[80];
	size_t n = 0;
	while (n < sizeof(req_line) - 1 && n < (size_t)received &&
	       http_buf[n] != '\r' && http_buf[n] != '\n') {
		req_line[n] = http_buf[n];
		n++;
	}
	req_line[n] = '\0';
	LOG_INF("HTTP: %s", req_line);

	if (strncmp(http_buf, "GET / ", 6) == 0 ||
	    strncmp(http_buf, "GET /index", 10) == 0) {
		http_send_form(client_fd);
	} else if (strncmp(http_buf, "POST /connect", 13) == 0) {
		http_handle_post_connect(client_fd, http_buf, received);
	} else if (strncmp(http_buf, "GET ", 4) == 0) {
		http_send_simple(client_fd, "404 Not Found", "Not found");
	} else {
		http_send_simple(client_fd, "400 Bad Request", "Bad request");
	}

	zsock_close(client_fd);
}

static void http_server_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	int server_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("HTTP: socket() failed: %d", errno);
		return;
	}

	int opt = 1;
	zsock_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
			 &opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(HTTP_PORT);

	if (zsock_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("HTTP: bind() failed: %d", errno);
		zsock_close(server_fd);
		return;
	}

	if (zsock_listen(server_fd, 2) < 0) {
		LOG_ERR("HTTP: listen() failed: %d", errno);
		zsock_close(server_fd);
		return;
	}

	LOG_INF("HTTP server listening on 192.168.4.1:%d", HTTP_PORT);

	while (http_server_run) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = zsock_accept(server_fd,
					     (struct sockaddr *)&client_addr,
					     &client_len);
		if (client_fd < 0) {
			LOG_WRN("HTTP: accept() failed: %d", errno);
			break;
		}
		LOG_INF("HTTP: client connected");
		http_handle_client(client_fd);
	}

	zsock_close(server_fd);
	LOG_INF("HTTP server stopped");
}

static void http_server_start(void)
{
	if (http_thread_tid) {
		return;
	}
	http_server_run = true;
	http_thread_tid = k_thread_create(
		&http_thread_data, http_stack,
		K_THREAD_STACK_SIZEOF(http_stack),
		http_server_thread, NULL, NULL, NULL,
		5, 0, K_NO_WAIT);
	k_thread_name_set(http_thread_tid, "http_srv");
}

int provisioning_start(void)
{
	if (provisioning_active) {
		LOG_WRN("Provisioning already active");
		return 0;
	}

	struct net_if *iface = net_if_get_default();

	/* Disconnect STA first — the esp32 driver switches between STA and AP
	 * via esp_wifi_set_mode() internally when ap_enable is called. The
	 * disconnect is needed so the driver sees STA as idle. */
	LOG_INF("Disconnecting STA before enabling AP...");
	net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
	k_msleep(500);

	struct wifi_connect_req_params ap_config = {};
	ap_config.ssid = (const uint8_t *)PROVISIONING_AP_SSID;
	ap_config.ssid_length = strlen(PROVISIONING_AP_SSID);
	ap_config.security = WIFI_SECURITY_TYPE_NONE;
	ap_config.channel = WIFI_CHANNEL_ANY;
	ap_config.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("Starting SoftAP: SSID='%s' (open)", PROVISIONING_AP_SSID);
	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface,
			   &ap_config, sizeof(ap_config));
	if (ret < 0) {
		LOG_ERR("AP enable failed: %d", ret);
		return ret;
	}

	provisioning_active = true;
	/* Network config (IP + DHCP server) happens in the AP_ENABLE_RESULT
	 * event handler once the interface is actually up. */
	return 0;
}

bool provisioning_take_pending_reboot(char *ssid_out, size_t ssid_sz,
				      char *psk_out, size_t psk_sz)
{
	if (!pending_reboot) {
		return false;
	}
	pending_reboot = false;
	strncpy(ssid_out, pending_apply_ssid, ssid_sz - 1);
	ssid_out[ssid_sz - 1] = '\0';
	strncpy(psk_out, pending_apply_psk, psk_sz - 1);
	psk_out[psk_sz - 1] = '\0';
	return true;
}
