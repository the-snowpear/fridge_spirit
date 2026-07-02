idf_component_register(
    SRCS "fridge_usb_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES network diagnostics mqtt_protocol ai_client ai_context ai_actions storage sensors radar state_machine audio asr voice_session speaker kitchen_tools kitchen_ai camera wake_word touch ui freertos
)
