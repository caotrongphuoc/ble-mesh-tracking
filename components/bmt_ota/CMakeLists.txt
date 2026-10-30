idf_component_register(
    SRCS "bmt_ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bmt_https_ota bmt_mesh
                  esp_wifi esp_event esp_netif nvs_flash
)
