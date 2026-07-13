# 当前 main 分支固定为 P4 新 I2C 驱动；S3 legacy 驱动保留源码但不进入本分支构建。
idf_component_register(
    SRCS "fridge_touch_p4.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_gpio esp_driver_i2c freertos log
)
