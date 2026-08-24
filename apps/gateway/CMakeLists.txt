cmake_minimum_required(VERSION 3.22)

# [OTA] Version = MOST RECENT modification time of any source file (NOT
# the build wall-clock). Rebuilding without editing anything keeps the
# same version (no pointless OTA even though the .bin bytes may match);
# the version only bumps when a real .c / .h edit lands. bmt_ota.c
# strncmp's two YYYYMMDDHHMMSS strings to tell which side is NEWER - 
# blocks accidental downgrades.
set(PROJECT_VER "00000000000000")
file(GLOB_RECURSE BMT_VER_SRC_FILES
    "${CMAKE_CURRENT_LIST_DIR}/main/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/main/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/main/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/components/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/components/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/components/*/CMakeLists.txt"
)
foreach(_bmt_src_file ${BMT_VER_SRC_FILES})
    file(TIMESTAMP "${_bmt_src_file}" _bmt_src_ts "%Y%m%d%H%M%S" UTC)
    if(_bmt_src_ts STRGREATER PROJECT_VER)
        set(PROJECT_VER "${_bmt_src_ts}")
    endif()
endforeach()
message(STATUS "PROJECT_VER (newest source mtime) = ${PROJECT_VER}")

# Required: tell CMake "re-run configure if any listed file's mtime
# changes". Without this line, editing a .c / .h and rebuilding lets
# ninja recompile just that file but does NOT recompute PROJECT_VER
# (CMake configure does not re-run just because one file's contents
# changed).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BMT_VER_SRC_FILES})

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Gateway)

# Copy the .bin into the repo's firmware/ directory for the OTA server.
# Override with: idf.py -DBMT_OTA_DIR=/some/other/path build
if(NOT DEFINED BMT_OTA_DIR)
    set(BMT_OTA_DIR "${CMAKE_CURRENT_LIST_DIR}/../../firmware")
endif()
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BMT_OTA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_BINARY_DIR}/Gateway.bin"
        "${BMT_OTA_DIR}/Gateway.bin"
    COMMENT "Copied Gateway.bin -> ${BMT_OTA_DIR}"
)
