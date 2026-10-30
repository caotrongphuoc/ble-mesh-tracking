idf_component_register(
    SRCS "bmt_ota.c" "bmt_ota_key.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_config bmt_https_ota bmt_mesh bmt_node_table bmt_thingsboard bmt_types
                  nvs_flash mbedtls bt
)
