# 板级引脚定义组件：仅头文件，无源码。
# 所有硬件驱动组件的 GPIO 分配统一从 include/fridge_board_pins.h 取宏，禁止再在驱动内散落硬编码。
idf_component_register(INCLUDE_DIRS "include")
