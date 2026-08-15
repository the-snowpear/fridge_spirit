idf_component_register(
    SRCS "fridge_fy_sp703.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_i2s freertos log
)
