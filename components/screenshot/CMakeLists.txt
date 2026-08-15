idf_component_register(
    SRCS "fridge_screenshot.c"
    INCLUDE_DIRS "include"
    REQUIRES display_mipi tfcard camera ui_p4 esp_driver_jpeg esp_timer heap log freertos
)
