idf_component_register(
    SRCS "fridge_diagnostics_log.c"
    INCLUDE_DIRS "include"
    REQUIRES tfcard log freertos esp_system esp_partition espcoredump
)
