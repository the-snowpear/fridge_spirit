set(fridge_recovery_requires
    nvs_flash
    app_update
    esp_partition
    esp_https_ota
    esp_http_client
    mbedtls
    network
    diagnostics
    espcoredump
)

# IDF 6 在读取 sdkconfig 之前先展开 REQUIRES，组件 CMake 此时不能可靠判断 CONFIG_*。
# 当前 main 分支只承担 P4 第一阶段最小移植；S3 完整固件已由 s3-legacy/s3-final-20260702 存档。
if(FRIDGE_RECOVERY_BUILD)
    set(fridge_main_requires ${fridge_recovery_requires})
else()
    # 静态最小闭包保证首次 set-target（尚无 sdkconfig）也不会误拉入旧 UI/摄像头/ESP-SR。
    # display_test 体积小且用于上电前单独验屏，因此和常规最小启动一并保留。
    set(fridge_main_requires
        nvs_flash
        network
        diagnostics
        usb_protocol
        display_test
        display
        touch
        audio
        tfcard
        app_update
        freertos
        heap
        espcoredump
    )
endif()

# CMake profile 决定早期依赖闭包，Kconfig 决定 main.c 编译哪条启动路径；两者必须一致。
# 检查只在正常配置阶段执行，early-expansion 尚未加载 CONFIG_*，不能在该阶段判断。
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(FRIDGE_RECOVERY_BUILD AND NOT CONFIG_FRIDGE_RECOVERY_APP)
        message(FATAL_ERROR
            "FRIDGE_RECOVERY_BUILD=ON requires CONFIG_FRIDGE_RECOVERY_APP=y; "
            "use a recovery sdkconfig/defaults file before configuring the ota_1 image.")
    elseif(CONFIG_FRIDGE_RECOVERY_APP AND NOT FRIDGE_RECOVERY_BUILD)
        message(FATAL_ERROR
            "CONFIG_FRIDGE_RECOVERY_APP=y requires -DFRIDGE_RECOVERY_BUILD=ON; "
            "otherwise the early dependency closure is not the recovery profile.")
    endif()
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${fridge_main_requires}
)
