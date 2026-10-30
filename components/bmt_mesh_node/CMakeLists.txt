# Mesh node side shared by relay and scanner: provisioning state, config
# server callback, RESET_CMD, OTA result report. Each app keeps its own
# UUID, composition and app-specific vendor opcodes.
idf_component_register(
    SRCS "bmt_mesh_node.c"
    INCLUDE_DIRS "."
    REQUIRES bt bmt_protocol
    PRIV_REQUIRES esp_system
)
