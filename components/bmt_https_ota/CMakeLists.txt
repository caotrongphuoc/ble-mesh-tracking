# HTTPS fetch + SHA / version guard + flash, shared by the gateway's own
# bmt_ota and the node-side components/bmt_ota. No mesh / WiFi logic.
idf_component_register(
    SRCS "bmt_https_ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update esp_http_client esp_https_ota
    # Written by thingsboard/tls/gen_certs.sh (not committed)
    EMBED_TXTFILES "ota_ca.pem"
)
