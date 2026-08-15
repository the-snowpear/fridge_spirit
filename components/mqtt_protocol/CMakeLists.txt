idf_component_register(
    SRCS "fridge_mqtt_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES
        agent_storage
        ai_client
        cjson
        esp_timer
        freertos
        heap
        mbedtls
        mqtt
        network
        nvs_flash
        tool_system
)
