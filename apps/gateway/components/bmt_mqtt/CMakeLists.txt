idf_component_register(
    SRCS "bmt_mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt bmt_types
    PRIV_REQUIRES bmt_config esp-tls mbedtls
    EMBED_TXTFILES "ca.pem"
)
