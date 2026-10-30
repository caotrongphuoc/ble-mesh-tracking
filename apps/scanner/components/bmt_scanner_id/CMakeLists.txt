idf_component_register(
    SRCS "bmt_scanner_id.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_config nvs_flash
)
