// 冰箱小精灵 ver2 主题实现。
// 六档正文角色外加符号角色都通过全局 lv_style_t 绑定，TF 字体切换时无需递归改控件。

#include "fridge_ui_theme.h"

#include <stdbool.h>

static lv_style_t s_font_styles[FRIDGE_FONT_ROLE_COUNT];
static bool s_initialized;

void fridge_ui_theme_init(void)
{
    if (s_initialized) {
        return;
    }
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        lv_style_init(&s_font_styles[i]);
        lv_style_set_text_font(&s_font_styles[i],
                               fridge_font_manager_get((fridge_font_role_t)i));
    }
    s_initialized = true;
}

void fridge_ui_theme_refresh_fonts(void)
{
    if (!s_initialized) {
        fridge_ui_theme_init();
    }
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        lv_style_set_text_font(&s_font_styles[i],
                               fridge_font_manager_get((fridge_font_role_t)i));
        lv_obj_report_style_change(&s_font_styles[i]);
    }
}

lv_style_t *fridge_ui_theme_font_style(fridge_font_role_t role)
{
    if (!s_initialized) {
        fridge_ui_theme_init();
    }
    if (role < 0 || role >= FRIDGE_FONT_ROLE_COUNT) {
        role = FRIDGE_FONT_BODY;
    }
    return &s_font_styles[role];
}
