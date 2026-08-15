# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# S3 的 OV3660/esp32-camera 已由 s3-legacy 分支存档；P4 主线不再扫描旧相机组件。

# 只把 main 及其 REQUIRES 闭包纳入构建图，避免未被引用的组件（尤其是不支持当前
# 芯片目标的第三方组件）在切换 esp32s3/esp32p4 目标时拖垮编译。
set(COMPONENTS main)

# recovery 依赖闭包必须在读取 sdkconfig 之前确定，因此把 CMake profile 作为
# IDF build property 传入 early-expansion 子进程，不能只在组件内声明普通 option。
option(FRIDGE_RECOVERY_BUILD "Build the small ota_1 local recovery firmware profile" OFF)
option(FRIDGE_MIPI_TEST_BUILD "Build the isolated WKS70WSV114-WCT MIPI-DSI test profile" OFF)

# recovery 在组件发现阶段就排除 UI/字体/输入法，避免组件管理器解析并下载
# LVGL/FreeType，也保证 ota_1 的构建图不会意外带入大 UI 链路。
if(FRIDGE_RECOVERY_BUILD)
    set(EXCLUDE_COMPONENTS ui ui_p4 font_manager ime display_mipi touch)
endif()

# MIPI 独立点屏不使用任何 managed component。必须在加载 project.cmake 前关闭管理器，
# 否则现有 dependencies.lock 会把 esp_hosted/ESP-SR whole-archive 链入测试镜像并在 app_main 前启动。
if(FRIDGE_MIPI_TEST_BUILD)
    set(ENV{IDF_COMPONENT_MANAGER} 0)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(FRIDGE_RECOVERY_BUILD "${FRIDGE_RECOVERY_BUILD}")
idf_build_set_property(FRIDGE_MIPI_TEST_BUILD "${FRIDGE_MIPI_TEST_BUILD}")
# 当前 main 已进入 P4 迁移阶段，显式版本避免固件描述继续继承最近的 s3-final Git 标签。
set(PROJECT_VER "p4-minimal-20260711")
project(fridge_spirit)
