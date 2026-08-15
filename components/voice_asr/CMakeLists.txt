idf_component_register(
    SRCS "fridge_voice_asr.c"
    INCLUDE_DIRS "include"
    REQUIRES audio asr agent_runtime wake_word freertos heap log
)
