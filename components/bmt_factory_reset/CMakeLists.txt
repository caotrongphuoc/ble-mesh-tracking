idf_component_register(
    SRCS "bmt_factory_reset.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio nvs_flash
)
