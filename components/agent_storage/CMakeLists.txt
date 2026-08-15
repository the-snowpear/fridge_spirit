idf_component_register(
    SRCS "src/fridge_agent_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES tfcard cjson freertos heap
)
