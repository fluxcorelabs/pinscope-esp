# Basic example

A plain `esp_http_server` application (a page, a JSON API, a WebSocket echo)
made reachable from anywhere with one call to `pinscope_start()`.

```bash
idf.py set-target esp32s2          # or your chip
idf.py menuconfig                  # Example WiFi: SSID and password; Pinscope: project key
idf.py flash monitor
```

The partition table has two OTA slots, so firmware can be updated from the
Pinscope dashboard. `GET /crash` crashes the firmware on purpose so you can see
a crash report arrive.
