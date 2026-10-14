# The temperature sensor driver became its own component in ESP-IDF 5.3.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
    set(tsens_component esp_driver_tsens)
else()
    set(tsens_component driver)
endif()

idf_component_register(
    SRCS "src/ps_core.c" "src/ps_crypto.c" "src/ps_port_esp.c"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_timer
                  esp_app_format app_update lwip ${tsens_component}
)

# A mistyped key would only show as a device that never appears, so stop the
# build instead. (Empty is fine: the key can also be passed to pinscope_start.)
set(ps_key "${CONFIG_PINSCOPE_PROJECT_KEY}")
if(ps_key AND NOT ps_key MATCHES "^psk_[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "Pinscope: the project key in menuconfig is not one: it must start with psk_ "
        "and hold only letters, digits, - and _. Copy it again from the dashboard.")
endif()
string(LENGTH "${ps_key}" ps_key_len)
if(ps_key AND NOT ps_key_len EQUAL 28)
    string(SUBSTRING "${ps_key}" 0 28 ps_key_head)
    if(ps_key_len EQUAL 56 AND ps_key STREQUAL "${ps_key_head}${ps_key_head}")
        message(FATAL_ERROR "Pinscope: the project key in menuconfig was pasted twice. "
            "Keep one copy (28 characters).")
    endif()
    message(FATAL_ERROR "Pinscope: the project key in menuconfig has ${ps_key_len} characters; "
        "a key has 28. Copy it again from the dashboard.")
endif()

# Capture crashes by wrapping the panic handler (see ps_port_esp.c).
if(CONFIG_PINSCOPE_CRASH_CAPTURE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
endif()
