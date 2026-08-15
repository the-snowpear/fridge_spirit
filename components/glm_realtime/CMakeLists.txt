idf_component_register(
    SRCS "fridge_glm_realtime.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_websocket_client
        esp-tls
        mbedtls
        network
        audio
        wake_word
        tool_system
        freertos
        heap
        log
        cjson
        esp_timer
)
