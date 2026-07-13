# IDF 6 在加载 sdkconfig 前先展开依赖，不能用 CONFIG_FRIDGE_P4_MINIMAL 选择 REQUIRES。
# 当前 P4 主线固定编译最小串口协议；旧完整协议由 s3-legacy 分支保留。
idf_component_register(
    SRCS "fridge_usb_protocol_minimal.c"
    INCLUDE_DIRS "include"
    REQUIRES network diagnostics audio tfcard freertos heap log
)
