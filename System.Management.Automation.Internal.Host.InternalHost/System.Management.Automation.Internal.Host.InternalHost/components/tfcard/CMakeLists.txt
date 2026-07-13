# 当前 main 分支固定为 P4 板载 TF：SDMMC Slot0 4-bit，LDO4 给 SDIO 引脚域供电。
# 不按 IDF_TARGET 条件选择源文件，避免首次 set-target 的早期依赖阶段错误编译非 P4 桩。
idf_component_register(
    SRCS "fridge_tfcard.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio freertos board
)
