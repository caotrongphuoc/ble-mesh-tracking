# Header-only. bmt_protocol.h is plain C and is also included by the
# Zephyr nRF52840 beacons via target_include_directories().
idf_component_register(
    INCLUDE_DIRS "."
    REQUIRES bt
)
