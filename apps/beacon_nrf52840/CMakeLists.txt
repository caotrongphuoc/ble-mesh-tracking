#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(beacon)

# Board-specific battery path. Devicetree + Kconfig per board come from
# boards/<board>.overlay and boards/<board>.conf (picked up by Zephyr).
if(BOARD STREQUAL "xiao_ble" OR BOARD STREQUAL "promicro_nrf52840")
  set(BMT_BATTERY_SRC src/bmt_battery_${BOARD}.c)
else()
  message(FATAL_ERROR "Unsupported board '${BOARD}'. Use xiao_ble/nrf52840 or promicro_nrf52840/nrf52840/uf2.")
endif()

target_sources(app PRIVATE
    src/main.c
    src/bmt_auth.c
    src/bmt_beacon.c
    src/bmt_battery.c
    ${BMT_BATTERY_SRC}
)

# Wire protocol shared with the ESP-IDF apps (plain C header).
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../components/bmt_protocol)
