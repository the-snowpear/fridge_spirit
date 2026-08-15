if(IDF_TARGET STREQUAL "esp32p4")
    # P4：OV5647 MIPI-CSI + esp_video 高清预览与硬件 JPEG 抓拍。
    # 旧 stub 保留在 fridge_camera_p4_stub.c，默认不编译，便于紧急回退。
    idf_component_register(
        SRCS "fridge_camera_p4.c"
        INCLUDE_DIRS "include"
        LDFRAGMENTS "fridge_camera_iram.lf"
        REQUIRES
            esp_video
            esp_cam_sensor
            codec_es8311
            audio
            wake_word
            glm_realtime
            display_mipi
            board
            touch
            esp_driver_gpio
            esp_driver_jpeg
            esp_timer
            freertos
            heap
            log
    )
else()
    idf_component_register(
        SRCS "fridge_camera.c"
        INCLUDE_DIRS "include"
        REQUIRES esp32-camera esp_timer esp_psram heap driver sensors
    )
endif()
