idf_component_register(
    SRCS "bmt_scan_core.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_auth bmt_config bmt_mesh bmt_ota bmt_scan bmt_scanner_id bmt_uart
                  nvs_flash esp_system example_init bt
)
