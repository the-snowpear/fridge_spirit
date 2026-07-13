if(IDF_TARGET STREQUAL "esp32p4")
    # P4 阶段：MIPI-CSI OV5647 方案尚未实装，先提供同 API 桩实现，
    # 避免 esp32-camera（不支持 esp32p4 目标）进入构建图导致整体编译失败。
    # usb_protocol/ui 等调用方无需改动，摄像头命令在运行期收到 ESP_ERR_NOT_SUPPORTED。
    idf_component_register(
        SRCS "fridge_camera_p4_stub.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_timer heap
    )
else()
    idf_component_register(
        SRCS "fridge_camera.c"
        INCLUDE_DIRS "include"
        REQUIRES esp32-camera esp_timer esp_psram heap driver sensors
    )
endif()
