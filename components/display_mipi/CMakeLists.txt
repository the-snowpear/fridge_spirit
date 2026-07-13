idf_component_register(
    SRCS "fridge_display_mipi.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_hw_support esp_lcd freertos log
)

# 测试关闭时 Kconfig 不定义 HOLD_MS，且正常固件也不应链接循环测试任务。
if(CONFIG_FRIDGE_MIPI_SCREEN_TEST)
    target_sources(${COMPONENT_LIB} PRIVATE "fridge_display_mipi_test.c")
endif()
