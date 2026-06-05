set(srcs 
    "main.c"
   
)

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "."
    REQUIRES 
        driver 
        nvs_flash
        bt
        esp_system
)