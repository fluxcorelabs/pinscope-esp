idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES esp_http_server esp_wifi esp_event esp_netif nvs_flash esp_timer esp_app_format
)
