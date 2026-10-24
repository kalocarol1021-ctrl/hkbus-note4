idf_component_register(
    SRCS
        "app_main.cc"
        "net.cc"
        "dns_server.cc"
        "http_fetch.cc"
        "web_server.cc"
        "ota.cc"
    INCLUDE_DIRS "."
    REQUIRES
        hkbus_core
        zectrix_board
        zectrix_epd
        esp_driver_gpio
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        app_update
        json
        lwip
        mbedtls
        nvs_flash
    EMBED_FILES
        "fonts/cjk20.bin"
        "fonts/cjk26.bin"
        "fonts/cjk32.bin"
        "fonts/num44.bin"
        "fonts/num58.bin"
    EMBED_TXTFILES
        "web/index.html"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
