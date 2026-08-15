// 冰箱小精灵 P4 字体资源管理接口。
// 字体包放在 TF 卡，BIN 字体加载到 PSRAM；FreeType 负责缺字和任意字号回退。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRIDGE_FONT_BODY_SMALL = 0,
    FRIDGE_FONT_BODY_SMALL_EMPHASIS,
    FRIDGE_FONT_BODY,
    FRIDGE_FONT_BODY_EMPHASIS,
    FRIDGE_FONT_TITLE,
    FRIDGE_FONT_SECTION,
    FRIDGE_FONT_LARGE,
    FRIDGE_FONT_HUGE,
    FRIDGE_FONT_SYMBOL,
    // 700 字重只用于首页标题、统计数字和状态舱时钟，避免正文整体过黑。
    FRIDGE_FONT_TITLE_STRONG,
    FRIDGE_FONT_SECTION_STRONG,
    FRIDGE_FONT_LARGE_STRONG,
    // 竖屏状态舱专用：匹配参考稿的 16px 加重说明字与 40px 粗体时钟。
    FRIDGE_FONT_CAPTION_EMPHASIS,
    FRIDGE_FONT_STATUS_TIME_STRONG,
    FRIDGE_FONT_ROLE_COUNT,
} fridge_font_role_t;

typedef struct {
    bool initialized;
    bool tf_pack_loaded;
    bool manifest_verified;
    bool freetype_ready;
    uint32_t loaded_roles;
    uint32_t missing_codepoints;
    char package_version[32];
    char last_error[96];
} fridge_font_status_t;

// 快速初始化：Flash 内置字 + 可选 TF BIN；不加载 FreeType，保证 UI 首帧可出。
esp_err_t fridge_font_manager_init(void);
// 延后加载 TF 上的 Noto CJK FreeType 回退。必须在 lv_init + 首帧之后调用，避免启动黑屏。
// 若首次调用时 TF 尚未挂载，可在 TF 就绪后再次调用；会重新校验 manifest 并加载 BIN/FreeType。
esp_err_t fridge_font_manager_load_freetype(void);
// TF 挂载后重试加载 BIN 包（不要求 FreeType）。可安全重复调用。
esp_err_t fridge_font_manager_try_load_tf_pack(void);
const lv_font_t *fridge_font_manager_get(fridge_font_role_t role);
esp_err_t fridge_font_manager_reload(void);
void fridge_font_manager_get_status(fridge_font_status_t *out);
bool fridge_font_manager_has_codepoint(uint32_t codepoint);

#ifdef __cplusplus
}
#endif
