idf_component_register(
    SRCS "fridge_c6_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_hosted mbedtls freertos esp_system
)
