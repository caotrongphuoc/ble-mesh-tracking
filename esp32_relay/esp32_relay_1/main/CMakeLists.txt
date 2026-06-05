if(NOT DEFINED RELAY_NUM)
    set(RELAY_NUM 1)
endif()

idf_component_register(SRCS "esp32_relay_1.c"
                       INCLUDE_DIRS ".")
