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

# profile 由项目根 CMake 传入 early-expansion；组件必须从 IDF build property 取值。
idf_build_get_property(FRIDGE_RECOVERY_BUILD FRIDGE_RECOVERY_BUILD)
idf_build_get_property(FRIDGE_MIPI_TEST_BUILD FRIDGE_MIPI_TEST_BUILD)

# IDF 6 在读取 sdkconfig 之前先展开 REQUIRES，组件 CMake 此时不能可靠判断 CONFIG_*。
# 当前 main 分支只承担 P4 第一阶段最小移植；S3 完整固件已由 s3-legacy/s3-final-20260702 存档。
if(FRIDGE_RECOVERY_BUILD)
    set(fridge_main_requires ${fridge_recovery_requires})
elseif(FRIDGE_MIPI_TEST_BUILD)
    # 首次 DSI 点屏必须与厂商例程一样保持最小闭包，避免 esp_hosted 等组件在 app_main 前自动启动。
    # esp_psram 显式注册 P4 32MB PSRAM Kconfig；DPI 双帧缓冲由 esp_lcd 从 PSRAM 分配。
    set(fridge_main_requires
        nvs_flash
        display_mipi
        esp_psram
    )
else()
    # 静态最小闭包保证首次 set-target（尚无 sdkconfig）也不会误拉入旧 S3 UI/SPI 屏/ESP-SR 全量业务。
    # SPI/TR230S display 与 display_test 已移除；正常联调只用 MIPI-DSI（display_mipi）。
    set(fridge_main_requires
        nvs_flash
        network
        diagnostics
        diagnostics_log
        usb_protocol
        display_mipi
        touch
        sht4x
        sensors
        ui_p4
        font_manager
        ime
        camera
        screenshot
        audio
        asr
        wake_word
        voice_asr
        tfcard
        agent_storage
        mqtt_protocol
        device_binding
        tool_system
        agent_runtime
        trace_service
        workflow_engine
        skill_manager
        glm_realtime
        app_update
        freertos
        heap
        # ESP-IDF 5.5.1 用显式 COMPONENTS=main 最小闭包时，esp_psram 属于 esp_system 的
        # 可选依赖不会被自动纳入，导致 CONFIG_SPIRAM 等符号未注册、32MB 片内 PSRAM 关闭。
        # 显式 REQUIRES 以注册 PSRAM Kconfig 并编译 esp_psram（IDF6 时由其它闭包间接带入）。
        esp_psram
        espcoredump
    )
endif()

# CMake profile 决定早期依赖闭包，Kconfig 决定 main.c 编译哪条启动路径；两者必须一致。
# 检查只在正常配置阶段执行，early-expansion 尚未加载 CONFIG_*，不能在该阶段判断。
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(FRIDGE_RECOVERY_BUILD AND FRIDGE_MIPI_TEST_BUILD)
        message(FATAL_ERROR "FRIDGE_RECOVERY_BUILD and FRIDGE_MIPI_TEST_BUILD are mutually exclusive")
    elseif(FRIDGE_RECOVERY_BUILD AND NOT CONFIG_FRIDGE_RECOVERY_APP)
        message(FATAL_ERROR
            "FRIDGE_RECOVERY_BUILD=ON requires CONFIG_FRIDGE_RECOVERY_APP=y; "
            "use a recovery sdkconfig/defaults file before configuring the ota_1 image.")
    elseif(CONFIG_FRIDGE_RECOVERY_APP AND NOT FRIDGE_RECOVERY_BUILD)
        message(FATAL_ERROR
            "CONFIG_FRIDGE_RECOVERY_APP=y requires -DFRIDGE_RECOVERY_BUILD=ON; "
            "otherwise the early dependency closure is not the recovery profile.")
    elseif(FRIDGE_MIPI_TEST_BUILD AND NOT CONFIG_FRIDGE_MIPI_SCREEN_TEST)
        message(FATAL_ERROR
            "FRIDGE_MIPI_TEST_BUILD=ON requires CONFIG_FRIDGE_MIPI_SCREEN_TEST=y in sdkconfig.")
    elseif(CONFIG_FRIDGE_MIPI_SCREEN_TEST AND NOT FRIDGE_MIPI_TEST_BUILD)
        message(FATAL_ERROR
            "CONFIG_FRIDGE_MIPI_SCREEN_TEST=y requires -DFRIDGE_MIPI_TEST_BUILD=ON; "
            "otherwise background components may auto-start before the isolated display test.")
    endif()
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ${fridge_main_requires}
)
