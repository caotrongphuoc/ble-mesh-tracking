set(srcs 
    "main.c"
   
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "."
    REQUIRES 
        driver 
        esp_system
        nvs_flash
        bt
        esp_driver_uart
)