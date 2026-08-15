idf_component_register(
    SRCS "src/workflow_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES tool_system trace_service agent_storage tfcard esp_timer cjson heap
)
