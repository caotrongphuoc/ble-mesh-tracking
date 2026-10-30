idf_component_register(
    SRCS "bmt_mesh.c"
    INCLUDE_DIRS "."
    REQUIRES bt bmt_mesh_node
    PRIV_REQUIRES bmt_config bmt_ota bmt_types
)
