idf_component_register(
    SRCS
        "src/tool_registry.c"
        "src/tool_broker.c"
        "src/builtin_tools.c"
        "src/business_tools.c"
        "src/task_scheduler.c"
    INCLUDE_DIRS "include"
    REQUIRES agent_storage diagnostics sht4x tfcard trace_service kitchen_tools storage esp_timer cjson freertos heap nvs_flash
)
