idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_eth
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
        driver
        lwip
)
