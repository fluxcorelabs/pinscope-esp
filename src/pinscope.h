/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 FluxCore Labs. See LICENSE: for use with the Pinscope service only. */
/*
 * Pinscope - remote access and telemetry for embedded devices.
 *
 * One outbound connection, encrypted with the Noise protocol, carries:
 *   - TCP streams from the cloud to servers listening on this device
 *     (HTTP, WebSocket, anything TCP on localhost)
 *   - metrics and log lines from the device to the cloud
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINSCOPE_VERSION   "0.2.0"
#define PINSCOPE_MAX_PORTS 4

typedef enum {
    PINSCOPE_STOPPED = 0,
    PINSCOPE_CONNECTING,
    PINSCOPE_ONLINE,
    PINSCOPE_REJECTED, /* server refused the project key or device credential */
} pinscope_state_t;

typedef enum {
    PINSCOPE_DEFAULT = 0, /* use the build-time default */
    PINSCOPE_ON,
    PINSCOPE_OFF,
} pinscope_opt_t;

/* Built-in metrics, for pinscope_config_t.metrics. */
#define PINSCOPE_M_HEAP   0x01 /* heap_free, heap_min, heap_largest */
#define PINSCOPE_M_UPTIME 0x02 /* uptime_s */
#define PINSCOPE_M_RSSI   0x04 /* rssi */
#define PINSCOPE_M_WIFI   0x08 /* wifi_disconnects, wifi_last_reason */
#define PINSCOPE_M_STACK  0x10 /* stack_min: least free stack of any task */
#define PINSCOPE_M_TEMP   0x20 /* chip_temp_c, where the chip has a sensor */
#define PINSCOPE_M_TASKS  0x40 /* stack/<task>: free stack of every task */
#define PINSCOPE_M_CPU    0x80 /* cpu/<task> and cpu_load, needs FreeRTOS run-time stats */

/* Every field is optional: zero means "use the Kconfig / built-in default". */
typedef struct {
    const char *project_key; /* from the dashboard, same for every device in a project */
    const char *device_name; /* label shown in the dashboard */

    /* Local TCP ports the cloud may open. ports[0] is served on the device URL,
     * the others on <port>-<device>.<domain>. Default: { 80 }. */
    uint16_t ports[PINSCOPE_MAX_PORTS];

    pinscope_opt_t log_capture;    /* forward the platform log output */
    pinscope_opt_t system_metrics; /* master switch for the built-in metrics */
    uint16_t metrics;            /* PINSCOPE_M_* bits to send, 0 = build-time selection */
    uint16_t metrics_interval_s; /* default 60 */

    uint8_t  task_priority;  /* default 5 */
} pinscope_config_t;

/* Start the tunnel task. Pass NULL to use build-time defaults only.
 * Safe to call before the network is up; it keeps retrying in the background.
 * Returns 0 on success. */
int pinscope_start(const pinscope_config_t *config);

void pinscope_stop(void);

pinscope_state_t pinscope_state(void);

/* Public URL of this device, "" until the first successful connection. */
const char *pinscope_url(void);

/* Queue one metric sample. Thread-safe, never blocks on the network. */
void pinscope_metric(const char *name, double value);

/* Queue raw log text. Thread-safe, never blocks on the network. */
void pinscope_log(const char *text, size_t len);

/* Skip the reconnect backoff, e.g. right after the network came up. */
void pinscope_kick(void);

#ifdef __cplusplus
}
#endif
