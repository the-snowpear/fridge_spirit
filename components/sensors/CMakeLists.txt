idf_component_register(
    SRCS "fridge_mpu6050_p4.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_adc esp_driver_i2c esp_timer freertos log nvs_flash
)
