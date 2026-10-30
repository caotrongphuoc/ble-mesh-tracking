# [SECURITY] Copy of zephyr/share/sysbuild/template/CMakeLists.txt.
#
# MCUboot runs as its own Zephyr app (separate from the "beacon" app),
# so it does NOT automatically read boards/<board>.overlay from the
# beacon app. Pass the matching partition overlay into the MCUboot
# build explicitly via the CMake variable mcuboot_EXTRA_DTC_OVERLAY_FILE
# BEFORE find_package(Sysbuild) runs (that call creates the "mcuboot"
# target).
#
# Partitions differ per board (XIAO starts MCUboot at 0x27000, ProMicro
# at 0x26000) - see the board overlays for why.
if(BOARD MATCHES "^xiao_ble")
  set(BMT_MCUBOOT_OVERLAY mcuboot_xiao_ble.overlay)
elseif(BOARD MATCHES "^promicro_nrf52840/nrf52840/uf2$")
  set(BMT_MCUBOOT_OVERLAY mcuboot_promicro_nrf52840.overlay)
elseif(BOARD MATCHES "^promicro_nrf52840")
  # Only the /uf2 variant uses the Adafruit UF2 bootloader layout
  # (nrf52840_partition_uf2_sdv6.dtsi) the overlays are built around.
  message(FATAL_ERROR "ProMicro board target must be promicro_nrf52840/nrf52840/uf2 (got '${BOARD}').")
else()
  message(FATAL_ERROR "Unsupported board '${BOARD}'. Use xiao_ble/nrf52840 or promicro_nrf52840/nrf52840/uf2.")
endif()

set(mcuboot_EXTRA_DTC_OVERLAY_FILE
    ${CMAKE_CURRENT_LIST_DIR}/../boards/${BMT_MCUBOOT_OVERLAY}
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
