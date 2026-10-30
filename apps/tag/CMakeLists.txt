cmake_minimum_required(VERSION 3.22)

# Only the header-only protocol component - the rest of components/ needs mesh.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/bmt_protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(Tag)