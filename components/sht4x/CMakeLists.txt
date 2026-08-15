idf_component_register(
    SRCS "fridge_sht4x.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_i2c esp_timer freertos log
)
