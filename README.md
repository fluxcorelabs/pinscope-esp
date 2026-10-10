# Pinscope for ESP32

The device side of [Pinscope](https://pinscope.fluxcorelabs.io): it gives the
web server on an ESP32 a secure public URL and sends logs, metrics and crash
reports, and receives firmware updates, over one outbound connection.

For ESP-IDF 5.x and 6.x. You need a Pinscope account for the project key.

## Add it to a project

In your project's `main/idf_component.yml`:

```yaml
dependencies:
  pinscope:
    git: https://github.com/fluxcorelabs/pinscope-esp.git
```

The build downloads it the next time it runs. To pin a version, add
`version: "<tag or commit>"`.

## Use it

1. `idf.py menuconfig` → **Pinscope**: enter the project key from the
   dashboard.
2. Call it once, any time after `esp_netif_init()` and
   `esp_event_loop_create_default()`:

```c
#include "pinscope.h"

pinscope_start(NULL);
```

That is all. It does not need WiFi to be up yet and reconnects on its own.
Whatever listens on port 80 of the device is now reachable at the device's
URL; change the port under **Pinscope → Local port to expose**.

Optional calls:

```c
pinscope_metric("temperature", 23.5);   // your own metrics, from any task
pinscope_url();                         // public URL once online
pinscope_state();                       // PINSCOPE_ONLINE, ...
```

Settings can also be given in code instead of menuconfig; every field is
optional:

```c
pinscope_config_t cfg = {
    .project_key = "psk_...",
    .ports = { 80, 8080 },
    .metrics = PINSCOPE_M_HEAP | PINSCOPE_M_RSSI,
    .log_capture = PINSCOPE_OFF,
};
pinscope_start(&cfg);
```

## What is sent, and how to turn each off

Everything is under **Pinscope → Reported data** in menuconfig.

| Data | Option | Default |
|---|---|---|
| Chip, firmware version, ELF hash, last reset reason | `PINSCOPE_DEVICE_INFO` | on |
| `ESP_LOG` output | `PINSCOPE_LOG_CAPTURE` | on |
| Crash reason, task and backtrace after a panic | `PINSCOPE_CRASH_CAPTURE` | on |
| All built-in metrics (master switch) | `PINSCOPE_SYSTEM_METRICS` | on |
| `heap_free`, `heap_min`, `heap_largest` | `PINSCOPE_METRIC_HEAP` | on |
| `uptime_s` | `PINSCOPE_METRIC_UPTIME` | on |
| `rssi` | `PINSCOPE_METRIC_RSSI` | on |
| `wifi_disconnects`, `wifi_last_reason` | `PINSCOPE_METRIC_WIFI_DISCONNECTS` | on |
| `stack_min`, plus a log warning when a task runs low | `PINSCOPE_METRIC_TASK_STACK` | on |
| `stack/<task>` for every task | `PINSCOPE_METRIC_TASK_STACK_EACH` | on |
| `cpu/<task>` for every task, and `cpu_load` | `PINSCOPE_METRIC_TASK_CPU` | on |
| `chip_temp_c` | `PINSCOPE_METRIC_TEMPERATURE` | on, where the chip has a sensor |

`PINSCOPE_METRICS_INTERVAL` sets how often the built-in metrics are sent
(60 s). At run time, `pinscope_config_t.metrics`, `.metrics_interval_s`,
`.system_metrics` and `.log_capture` override the build-time choice.

Notes:

- CPU usage turns on FreeRTOS run-time statistics
  (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` and
  `CONFIG_FREERTOS_USE_TRACE_FACILITY`), which costs a timer read on every
  context switch. Switch `PINSCOPE_METRIC_TASK_CPU` off if you do not want
  that.
- With CPU usage off, stack data covers every task only on single-core chips;
  on dual-core chips it then needs `CONFIG_FREERTOS_USE_TRACE_FACILITY`,
  otherwise only the Pinscope task is measured.
- Crash capture wraps `esp_panic_handler`. Turn it off if your application
  wraps that function itself.
- On RISC-V chips the crash report lists code addresses found on the stack
  instead of an exact backtrace.

## Firmware updates

With `PINSCOPE_OTA` on (the default) and two OTA app slots in the partition
table, the project owner can push a new application image from the dashboard.
It goes through the standard `esp_ota` path: written to the spare slot,
verified, set as the boot partition, restart. Enable
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` to have the bootloader return to the
previous image when the new one never manages to reach the server; Pinscope
confirms the image on its first successful connection, and restarts the
device if that has not happened within five minutes
(`PINSCOPE_OTA_TRIAL_S`), so a bad update undoes itself. While an image is being
written the tunnel is busy, so requests to the device wait.

## Resources

- RAM: two frame buffers (1 KiB each), the outbox (3 KiB), an 8 KiB task stack,
  plus the TLS session. All sizes are in menuconfig.
- Sockets: one for the tunnel and two per open stream. The default of 2
  streams fits the stock `CONFIG_LWIP_MAX_SOCKETS=10`.
- If your HTTP server answers slowly through the tunnel, set `TCP_NODELAY` on
  its connections (see `examples/esp-idf-basic`): `esp_http_server` sends
  headers and body separately, which otherwise costs about 250 ms per request.

## Licence

The source is here for you to read, audit and change. The
[Pinscope Device Component License](LICENSE) lets you build it into your
products and ship them, for use with the Pinscope service only.
