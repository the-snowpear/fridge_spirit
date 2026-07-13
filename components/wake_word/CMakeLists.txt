idf_component_register(
    SRCS "fridge_wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES audio esp_timer freertos heap log nvs_flash
    PRIV_REQUIRES espressif__esp-sr
)
