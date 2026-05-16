/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 */

#include "influx.h"

#include "battery.h"
#include "bme280.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
extern "C" {
#include <zephyr/net/socket.h>
}

LOG_MODULE_REGISTER(influx, LOG_LEVEL_INF);

#define INFLUX_BUFFER_SIZE  360   /* ~6 h at one sample/min */
#define INFLUX_TIMEOUT_MS   3000

struct env_sample {
	int64_t ts_unix_ns;
	float   temp_c;
	float   hum_pct;
	float   pres_hpa;
	int     battery_pct;  /* -1 if unavailable */
};

static struct env_sample influx_buf[INFLUX_BUFFER_SIZE];
static uint16_t influx_buf_head = 0;   /* next write slot */
static uint16_t influx_buf_count = 0;  /* current occupancy */

/* Push a sample into the ring. When full, overwrites the oldest. */
static void influx_buf_push(const struct env_sample *s)
{
	influx_buf[influx_buf_head] = *s;
	influx_buf_head = (influx_buf_head + 1) % INFLUX_BUFFER_SIZE;
	if (influx_buf_count < INFLUX_BUFFER_SIZE) {
		influx_buf_count++;
	} else {
		LOG_WRN("Influx ring full — overwriting oldest sample");
	}
}

/* Returns the oldest unsent sample (without popping). */
static const struct env_sample *influx_buf_peek(void)
{
	if (influx_buf_count == 0) {
		return NULL;
	}
	uint16_t oldest_idx = (influx_buf_head + INFLUX_BUFFER_SIZE -
			       influx_buf_count) % INFLUX_BUFFER_SIZE;
	return &influx_buf[oldest_idx];
}

/* Drop the oldest sample after a successful POST. */
static void influx_buf_pop(void)
{
	if (influx_buf_count > 0) {
		influx_buf_count--;
	}
}

/* Build the line-protocol body for one sample. Returns length, or
 * negative on overflow. */
static int influx_format_line(const struct env_sample *s,
			      char *buf, size_t buf_size)
{
	int n;
	if (s->battery_pct >= 0) {
		n = snprintf(buf, buf_size,
			"env,device=" CONFIG_WEATHER_DEVICE_ID
			" temp_c=%.2f,hum_pct=%.2f,pres_hpa=%.2f,"
			"battery_pct=%d %lld",
			(double)s->temp_c, (double)s->hum_pct,
			(double)s->pres_hpa, s->battery_pct,
			(long long)s->ts_unix_ns);
	} else {
		n = snprintf(buf, buf_size,
			"env,device=" CONFIG_WEATHER_DEVICE_ID
			" temp_c=%.2f,hum_pct=%.2f,pres_hpa=%.2f %lld",
			(double)s->temp_c, (double)s->hum_pct,
			(double)s->pres_hpa,
			(long long)s->ts_unix_ns);
	}
	if (n < 0 || (size_t)n >= buf_size) {
		return -ENOMEM;
	}
	return n;
}

/* POST one line to InfluxDB. Returns 0 on HTTP 204, negative otherwise.
 * Reuses the same zsock_* idiom as the SoftAP HTTP server. */
static int influx_post(const char *line, size_t line_len)
{
	int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		LOG_WRN("Influx: socket() failed: %d", errno);
		return -errno;
	}

	/* Best-effort connect timeout via SO_RCVTIMEO/SO_SNDTIMEO. */
	struct timeval tv = {
		.tv_sec  = INFLUX_TIMEOUT_MS / 1000,
		.tv_usec = (INFLUX_TIMEOUT_MS % 1000) * 1000,
	};
	zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(CONFIG_WEATHER_INFLUX_PORT);
	if (zsock_inet_pton(AF_INET, CONFIG_WEATHER_INFLUX_HOST,
			    &addr.sin_addr) != 1) {
		LOG_WRN("Influx: bad host '%s'", CONFIG_WEATHER_INFLUX_HOST);
		zsock_close(sock);
		return -EINVAL;
	}

	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_WRN("Influx: connect() failed: %d", errno);
		zsock_close(sock);
		return -errno;
	}

	char header[160];
	int hlen = snprintf(header, sizeof(header),
		"POST /write?db=" CONFIG_WEATHER_INFLUX_DB " HTTP/1.0\r\n"
		"Host: " CONFIG_WEATHER_INFLUX_HOST "\r\n"
		"Content-Type: application/octet-stream\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n"
		"\r\n",
		(unsigned)line_len);
	if (zsock_send(sock, header, hlen, 0) != hlen ||
	    zsock_send(sock, line, line_len, 0) != (ssize_t)line_len) {
		LOG_WRN("Influx: send() failed: %d", errno);
		zsock_close(sock);
		return -EIO;
	}

	/* Read enough of the response to see the status code. */
	char resp[64];
	int got = zsock_recv(sock, resp, sizeof(resp) - 1, 0);
	zsock_close(sock);
	if (got <= 0) {
		LOG_WRN("Influx: no response: %d", errno);
		return -EIO;
	}
	resp[got] = '\0';

	/* Status line looks like "HTTP/1.0 204 No Content\r\n..." */
	if (strncmp(resp, "HTTP/1.0 204", 12) == 0 ||
	    strncmp(resp, "HTTP/1.1 204", 12) == 0) {
		return 0;
	}
	LOG_WRN("Influx: unexpected status: %.40s", resp);
	return -EBADMSG;
}

void influx_drain(bool wifi_up)
{
	while (influx_buf_count > 0 && wifi_up) {
		const struct env_sample *s = influx_buf_peek();
		if (!s) {
			return;
		}
		char line[160];
		int line_len = influx_format_line(s, line, sizeof(line));
		if (line_len < 0) {
			/* Should never happen with our fixed-size buffer.
			 * Drop it so we don't loop forever. */
			LOG_ERR("Influx: format failed, dropping sample");
			influx_buf_pop();
			continue;
		}

		int ret = influx_post(line, line_len);
		if (ret == 0) {
			LOG_INF("Influx POST 204 OK (queue %u)",
				influx_buf_count - 1);
			influx_buf_pop();
		} else {
			LOG_WRN("Influx POST failed: %d (queue %u)",
				ret, influx_buf_count);
			break;  /* try again next tick */
		}
	}
}

void influx_capture_sample(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	struct env_sample s = {
		.ts_unix_ns  = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec,
		.temp_c      = bme280_get_temperature(),
		.hum_pct     = bme280_get_humidity(),
		.pres_hpa    = bme280_get_pressure(),
		.battery_pct = battery_is_available() ? (int)battery_get_soc() : -1,
	};
	influx_buf_push(&s);
	LOG_INF("Influx queued: %u/%u (temp=%.2f hum=%.2f pres=%.2f)",
		influx_buf_count, INFLUX_BUFFER_SIZE,
		(double)s.temp_c, (double)s.hum_pct, (double)s.pres_hpa);
}
