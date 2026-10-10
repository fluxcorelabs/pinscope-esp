/*
 * A plain esp_http_server application (page, JSON API, WebSocket echo) made
 * reachable from the internet with one call to pinscope_start().
 *
 * Set WiFi credentials and the Pinscope project key in
 * `idf.py menuconfig`.
 */
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "pinscope.h"

static const char *TAG = "example";

static const char INDEX_HTML[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP32</title><body style='font-family:system-ui;margin:2rem'>"
    "<h1>Hello from the ESP32</h1><p>Free heap: <b id=h>...</b> bytes</p>"
    "<p>WebSocket echo: <input id=m value=ping> <button onclick='s.send(m.value)'>send</button>"
    " <code id=o></code></p><script>"
    "setInterval(async()=>{h.textContent=(await (await fetch('/api/status')).json()).heap},2000);"
    "var s=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws');"
    "s.onmessage=e=>o.textContent=e.data</script>";

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"heap\":%u,\"uptime\":%u}", (unsigned)esp_get_free_heap_size(),
             (unsigned)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* Demo only: crash on purpose to see the backtrace arrive in the dashboard. */
static esp_err_t crash_get(httpd_req_t *req)
{
    volatile int *nowhere = NULL;
    ESP_LOGW(TAG, "crashing on request");
    *nowhere = 42;
    return ESP_OK;
}

static esp_err_t ws_echo(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK; /* handshake */
    }
    uint8_t buf[128];
    httpd_ws_frame_t frame = { .payload = buf };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    return httpd_ws_send_frame(req, &frame);
}

/* esp_http_server sends headers and body separately; without TCP_NODELAY the
 * body waits ~250 ms for a delayed ACK, on the LAN and through the tunnel. */
static esp_err_t on_session_open(httpd_handle_t server, int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    static const httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    static const httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET,
                                            .handler = status_get };
    static const httpd_uri_t crash_uri = { .uri = "/crash", .method = HTTP_GET, .handler = crash_get };
    static const httpd_uri_t ws_uri = { .uri = "/ws", .method = HTTP_GET, .handler = ws_echo,
                                        .is_websocket = true };

    config.lru_purge_enable = true;
    config.open_fn = on_session_open;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &ws_uri);
    httpd_register_uri_handler(server, &crash_uri);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

static void wifi_start(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi = { 0 };

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    strlcpy((char *)wifi.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD, sizeof(wifi.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_start();
    start_http_server();

    /* Everything Pinscope needs comes from menuconfig. */
    pinscope_start(NULL);

    for (int i = 0;; i++) {
        pinscope_metric("loop_counter", i);
        ESP_LOGI(TAG, "still alive, %s", pinscope_url());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
