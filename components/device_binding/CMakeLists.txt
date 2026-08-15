idf_component_register(
    SRCS "fridge_device_binding.c"
    INCLUDE_DIRS "include"
    REQUIRES
        agent_storage
        ai_client
        cjson
        esp_http_client
        esp_timer
        freertos
        heap
        mbedtls
        mqtt_protocol
        network
        nvs_flash
)
