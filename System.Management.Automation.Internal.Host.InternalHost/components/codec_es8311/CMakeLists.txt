idf_component_register(
    SRCS "fridge_codec_es8311.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        freertos
        log
)
