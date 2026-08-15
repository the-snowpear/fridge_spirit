idf_component_register(
    SRCS
        "fridge_font_manager.c"
        "../ui/fonts/fridge_font_cn_16.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl freertos log heap tfcard cjson mbedtls
)

# LVGL 9.2.2 的 ESP-IDF 适配层没有把 FreeType 的头文件目录传给
# LVGL 自身源码；这里在组件图生成后补齐目录和链接依赖，避免修改
# managed_components 中会被重新下载的第三方文件。
idf_component_get_property(FREETYPE_DIR espressif__freetype COMPONENT_DIR)
idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
idf_component_get_property(FREETYPE_LIB espressif__freetype COMPONENT_LIB)
target_include_directories(${LVGL_LIB} PRIVATE
    "${FREETYPE_DIR}/freetype/include"
    "${FREETYPE_DIR}/freetype/devel"
)
# FreeType 的公共头在非 FT2_BUILD_LIBRARY 模式下会清掉 FT_ERR_PREFIX，
# 而 LVGL 的 FT_THROW 宏稍后才展开；显式恢复标准前缀，兼容该组件的裁剪配置。
target_compile_definitions(${LVGL_LIB} PRIVATE FT2_BUILD_LIBRARY FT_ERR_PREFIX=FT_Err_)
target_link_libraries(${LVGL_LIB} PUBLIC ${FREETYPE_LIB})
