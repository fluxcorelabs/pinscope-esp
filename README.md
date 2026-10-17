# Pinscope for ESP32

[![ESP Component Registry](https://components.espressif.com/components/pinscope/pinscope/badge.svg)](https://components.espressif.com/components/pinscope/pinscope)
[![PlatformIO Registry](https://badges.registry.platformio.org/packages/pinscope/library/pinscope.svg)](https://registry.platformio.org/libraries/pinscope/pinscope)
[![Arduino Library Manager](https://www.ardu-badge.com/badge/Pinscope.svg)](https://www.ardu-badge.com/Pinscope)
[![Docs](https://img.shields.io/badge/docs-pinscope.dev-9B9CF7)](https://pinscope.dev/docs)
[![License](https://img.shields.io/badge/license-see%20LICENSE-lightgrey)](LICENSE)

The device side of [Pinscope](https://pinscope.dev): it gives the web server
on an ESP32 its own HTTPS address, sends logs, metrics and crash reports, and
receives firmware updates, over one outbound connection that needs 9.4 KB of
RAM in total.

For ESP-IDF 5.x and 6.x, Arduino (Arduino-ESP32 2.x and 3.x) and
PlatformIO, on every ESP32 with WiFi (ESP32, S2, S3, C2, C3, C5, C6, C61).
You need a Pinscope account for the project key.

## Add it to a project

From your project's folder:

```sh
idf.py add-dependency "pinscope/pinscope^0.2.10"
```

The build downloads it from the
[ESP Component Registry](https://components.espressif.com/components/pinscope/pinscope)
the next time it runs.

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

## Arduino

Install the library: in the Arduino IDE, **Tools → Manage Libraries**,
search for **Pinscope**. Or **Sketch → Include Library → Add .ZIP Library**
with
[pinscope-esp-0.2.10.zip](https://github.com/pinscope-dev/pinscope-esp/archive/refs/tags/v0.2.10.zip).
Then **File → Examples → Pinscope → RemoteWebServer** is a complete sketch:

```cpp
#include <WiFi.h>
#include <pinscope.h>

void setup() {
  WiFi.begin("your-network", "your-password");
  // your web server, on port 80

  pinscope_config_t config = {};
  config.project_key = "psk_...";      // Connect a device, in the dashboard
  config.firmware_version = "1.0.0";   // shown in the dashboard
  pinscope_start(&config);
}
```

Crash reports need Arduino-ESP32 3.x; everything else also works on 2.x.
Firmware updates need a partition scheme with two app slots (the default has
them). Lines passed to `pinscope_log()` reach the dashboard's log; the
example's `logLine()` sends each line to the serial port and the dashboard.

## PlatformIO

In `platformio.ini`, with the Arduino or the ESP-IDF framework:

```ini
lib_deps = pinscope/pinscope@^0.2.10
```

(from the [PlatformIO Registry](https://registry.platformio.org/libraries/pinscope/pinscope))

and start it with a `pinscope_config_t` as in the Arduino example. Under
PlatformIO, crash reports work with both frameworks (with Arduino, on
Arduino-ESP32 3.x).

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
| `cpu/<task>` for every task, and `cpu_load` | `PINSCOPE_METRIC_TASK_CPU` | off |
| `chip_temp_c` | `PINSCOPE_METRIC_TEMPERATURE` | on, where the chip has a sensor |

`PINSCOPE_METRICS_INTERVAL` sets how often the built-in metrics are sent
(60 s). At run time, `pinscope_config_t.metrics`, `.metrics_interval_s`,
`.system_metrics` and `.log_capture` override the build-time choice.

Notes:

- CPU usage is off by default: it turns on FreeRTOS run-time statistics
  (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` and
  `CONFIG_FREERTOS_USE_TRACE_FACILITY`) for the whole firmware, which costs a
  timer read on every context switch and some RAM per task. Switch
  `PINSCOPE_METRIC_TASK_CPU` on if you want it.
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

- RAM: 9.4 KB in total while connected (measured on an ESP32-S2): 4.9 KB
  static (frame buffers of 1 KiB out and 512 B in, a 2 KiB outbox for logs
  and metrics, the rest of the state) and a 3 KiB task stack. The sizes are
  fixed; there is nothing to tune.
- Flash: 18.2 KB for the component.
- No TLS library and no DNS cache: the connection is a Noise handshake
  (X25519) and ChaCha20-Poly1305 implemented in the component, and the
  service's name is looked up with one query per connection.
- Network: one outbound TCP connection to the service on port 443, so it
  passes networks that only let HTTPS out. Only firewalls that inspect
  traffic and allow nothing but TLS on that port block it.
- Sockets: two of its own (the connection to the service, and a loopback UDP
  socket that wakes its task) and, per open stream, the one your server
  accepts. The 2 streams fit the stock `CONFIG_LWIP_MAX_SOCKETS=10`.
- Nothing in your lwIP configuration has to change. The component reaches
  your servers through lwIP's callback API, so the per-socket limit on
  received packets (`CONFIG_LWIP_TCP_RECVMBOX_SIZE`) does not apply to it.
- If your HTTP server answers slowly through the tunnel, set `TCP_NODELAY` on
  its connections (see `examples/esp-idf-basic`): `esp_http_server` sends
  headers and body separately, which otherwise costs about 250 ms per request.

## Licence

The source is here for you to read, audit and change. The
[Pinscope Device Component License](LICENSE) lets you build it into your
products and ship them, for use with the Pinscope service only.
