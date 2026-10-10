# The temperature sensor driver became its own component in ESP-IDF 5.3.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    set(tsens_component esp_driver_tsens)
else()
    set(tsens_component driver)
endif()

idf_component_register(
    SRCS "src/ps_core.c" "src/ps_port_esp.c"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp-tls mbedtls nvs_flash esp_event esp_netif esp_wifi esp_timer
                  esp_app_format app_update lwip ${tsens_component}
)

# Capture crashes by wrapping the panic handler (see ps_port_esp.c).
if(CONFIG_PINSCOPE_CRASH_CAPTURE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
endif()
