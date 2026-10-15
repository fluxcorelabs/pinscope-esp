/*
 * A plain esp_http_server application (page, JSON API, WebSocket echo) made
 * reachable from the internet with one call to pinscope_start().
 *
 * Set WiFi credentials and the Pinscope project key in
 * `idf.py menuconfig`.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
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
    "<!doctype html><html lang=en><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">\n"
    "<title>ESP32</title><style>\n"
    ":root{--bg:#F4F4F5;--card:#fff;--line:#E4E4E7;--text:#18181B;--mute:#71717A;--acc:#4B4DC4;--ok:#16A34A}\n"
    "@media(prefers-color-scheme:dark){:root{--bg:#0F1014;--card:#17181D;--line:#2A2B33;--text:#F4F4F5;--mute:#A1A1AA;--acc:#9B9CF7;--ok:#4ADE80}}\n"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}\n"
    "main{max-width:640px;margin:0 auto;padding:24px 16px}\n"
    "h1{font-size:22px;margin:0}.sub{color:var(--mute);margin:2px 0 20px}\n"
    ".dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--ok);margin-right:8px;vertical-align:middle}\n"
    ".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}\n"
    ".t{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px}\n"
    ".t span{display:block;color:var(--mute);font-size:13px}.t b{font:600 22px ui-monospace,Menlo,monospace}\n"
    ".wide{grid-column:1/-1}button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:8px;padding:8px 14px;cursor:pointer}\n"
    ".row{display:flex;justify-content:space-between;align-items:center;gap:12px}.foot{color:var(--mute);font-size:13px;margin-top:18px}\n"
    "code{font:13px ui-monospace,Menlo,monospace}\n"
    "</style><main>\n"
    "<h1><span class=dot id=d></span><span id=chip>ESP32</span></h1>\n"
    "<p class=sub>This page is served by the web server on the device.</p>\n"
    "<div class=grid>\n"
    "<div class=t><span>Uptime</span><b id=up>-</b></div>\n"
    "<div class=t><span>Free heap</span><b id=heap>-</b></div>\n"
    "<div class=t><span>Lowest free heap</span><b id=min>-</b></div>\n"
    "<div class=t><span>WiFi signal</span><b id=rssi>-</b></div>\n"
    "<div class=\"t wide row\"><div><span>Round trip to the device (WebSocket)</span><b id=rtt>-</b></div><button onclick=\"if(confirm('Crash the firmware on purpose? It restarts and the crash report appears in Pinscope.'))fetch('/crash')\">Crash it</button></div>\n"
    "</div>\n"
    "<p class=foot>Firmware <code id=app>-</code> &middot; ESP-IDF <code id=idf>-</code></p>\n"
    "</main><script>\n"
    "const $=id=>document.getElementById(id),kb=n=>(n/1024).toFixed(1)+' KB';\n"
    "const dur=s=>s<3600?Math.floor(s/60)+' min '+s%60+' s':Math.floor(s/3600)+' h '+Math.floor(s%3600/60)+' min';\n"
    "async function poll(){try{const j=await(await fetch('/api/status')).json();\n"
    "$('chip').textContent=j.chip.toUpperCase().replace(/^ESP32(?=.)/,'ESP32-');$('up').textContent=dur(j.uptime);$('heap').textContent=kb(j.heap);$('min').textContent=kb(j.min_heap);\n"
    "$('rssi').textContent=j.rssi+' dBm';$('app').textContent=j.app;$('idf').textContent=j.idf;$('d').style.opacity=1}catch(e){$('d').style.opacity=.2}}\n"
    "poll();setInterval(poll,2000);\n"
    "let ws;function open(){ws=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws');\n"
    "ws.onmessage=e=>$('rtt').textContent=Math.round(performance.now()-Number(e.data))+' ms';ws.onclose=()=>setTimeout(open,2000)}\n"
    "open();setInterval(()=>ws.readyState==1&&ws.send(String(performance.now())),2000);\n"
    "</script>\n";

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char json[224];
    wifi_ap_record_t ap = { 0 };
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(json, sizeof(json),
             "{\"chip\":\"%s\",\"heap\":%u,\"min_heap\":%u,\"uptime\":%u,\"rssi\":%d,"
             "\"app\":\"%s\",\"idf\":\"%s\"}",
             CONFIG_IDF_TARGET, (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)(esp_timer_get_time() / 1000000), rssi, app->version, app->idf_ver);
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
