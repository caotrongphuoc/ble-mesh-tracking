# [SECURITY] Copy of zephyr/share/sysbuild/template/CMakeLists.txt.
#
# MCUboot runs as its own Zephyr app (separate from the "beacon" app),
# so it does NOT automatically read boards/promicro_nrf52840_nrf52840_uf2.overlay
# from the beacon app. Pass the MCUboot-specific partition overlay
# explicitly via the CMake variable mcuboot_EXTRA_DTC_OVERLAY_FILE
# BEFORE find_package(Sysbuild) runs (that call creates the "mcuboot"
# target).
#
# NOTE: partitions on the ProMicro variant start at 0x26000 (different
# from XIAO at 0x27000) - see the explanation in
# boards/promicro_nrf52840_nrf52840_uf2.overlay.
set(mcuboot_EXTRA_DTC_OVERLAY_FILE
    ${CMAKE_CURRENT_LIST_DIR}/../boards/mcuboot_partitions.overlay
    CACHE INTERNAL ""
)

# The board's default serial backend is USB CDC-ACM, but MCUboot does
# not enable the USB stack -> link error "undefined reference to
# __device_dts_ord_N". MCUboot has no need for a console (just verify
# and boot), see mcuboot.conf.
set(mcuboot_EXTRA_CONF_FILE
    ${CMAKE_CURRENT_LIST_DIR}/../mcuboot.conf
    CACHE INTERNAL ""
)

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)
