idf_component_register(
    SRCS "bmt_mesh.c"
    INCLUDE_DIRS "."
    REQUIRES bt
    PRIV_REQUIRES bmt_config bmt_mac_cache bmt_mqtt bmt_node_table
                  bmt_scan_list bmt_thingsboard bmt_types
                  nvs_flash esp_timer
)
