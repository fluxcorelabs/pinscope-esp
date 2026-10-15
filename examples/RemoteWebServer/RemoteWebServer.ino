/*
 * The ESP32's own web server, reachable from anywhere through Pinscope,
 * with its log, health, crash reports and firmware updates in the dashboard.
 *
 * Fill in your WiFi and the project key (dashboard: Connect a device), flash,
 * and open the device from the dashboard.
 *
 * For firmware updates from the dashboard, use a partition scheme with two
 * app slots (the default one has them). Lines passed to pinscope_log() reach
 * the dashboard's log; logLine() below sends each line to both places.
 */
#include <WebServer.h>
#include <WiFi.h>
#include <pinscope.h>

const char *WIFI_SSID = "your-network";
const char *WIFI_PASSWORD = "your-password";
const char *PINSCOPE_KEY = "psk_...";  // Connect a device, in the dashboard

WebServer server(80);

// A line to the serial port and to the dashboard's log.
void logLine(const char *fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
  va_end(args);
  if (n < 0) return;
  if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n';
  Serial.write(line, n);
  pinscope_log(line, n);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", [] {
    String page = "<!doctype html><meta name=viewport content='width=device-width'>"
                  "<h1>Hello from the ESP32</h1>"
                  "<p>Free heap: " + String(ESP.getFreeHeap()) + " bytes</p>"
                  "<p>Up for " + String(millis() / 1000) + " s</p>";
    server.send(200, "text/html", page);
  });
  server.begin();

  // Starts in the background; it waits for WiFi and reconnects on its own.
  pinscope_config_t config = {};
  config.project_key = PINSCOPE_KEY;
  config.firmware_version = "1.0.0";  // shown in the dashboard; raise it with each build you ship
  pinscope_start(&config);
}

void loop() {
  server.handleClient();

  static uint32_t last;
  if (millis() - last > 10000) {
    last = millis();
    logLine("still alive, %s", pinscope_url());
    pinscope_metric("loop_counter", millis() / 10000);
  }
}
