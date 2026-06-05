idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        bt
        esp_wifi
        esp_event
        esp_netif
        mqtt
        esp_driver_uart
        example_init
)