idf_component_register(
    SRCS "bmt_ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_config bmt_mesh bmt_node_table bmt_thingsboard bmt_types
                  app_update esp_http_client esp_https_ota bootloader_support
                  esp_wifi nvs_flash mbedtls bt
    # Same CA file as the shared components/bmt_ota (written by thingsboard/tls/gen_certs.sh)
    EMBED_TXTFILES "${CMAKE_CURRENT_LIST_DIR}/../../../../components/bmt_ota/ota_ca.pem"
)
