idf_component_register(
    SRCS "fridge_voice_asr.c"
    INCLUDE_DIRS "include"
    REQUIRES audio asr wake_word freertos heap log
)
