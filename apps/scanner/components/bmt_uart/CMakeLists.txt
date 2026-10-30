idf_component_register(
    SRCS "bmt_uart.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_mesh bmt_ota bmt_scanner_id bmt_types
                  esp_driver_uart esp_system app_update
)
