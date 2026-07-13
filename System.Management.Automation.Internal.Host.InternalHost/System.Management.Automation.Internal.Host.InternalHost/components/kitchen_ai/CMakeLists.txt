idf_component_register(
    SRCS "fridge_kitchen_ai.c"
    INCLUDE_DIRS "include"
    REQUIRES ai_client ai_context storage network freertos esp_timer heap log cjson
)
