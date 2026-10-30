idf_component_register(
    SRCS "bmt_auth.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_config mbedtls nvs_flash esp_timer
)
