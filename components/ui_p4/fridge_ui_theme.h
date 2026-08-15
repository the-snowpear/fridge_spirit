// 冰箱小精灵 ver2 主题令牌。
// 颜色、圆角和六档字体角色集中在这里，页面控件只引用主题，不持有字体生命周期。

#pragma once

#include "fridge_font_manager.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 设计稿使用的 RGB888 纯色；LVGL 会按显示的 RGB565 格式完成量化。
#define FRIDGE_UI_COLOR_PAPER       0xFFFDF8
#define FRIDGE_UI_COLOR_SURFACE     0xFFFFFF
#define FRIDGE_UI_COLOR_CREAM       0xFFF5E5
#define FRIDGE_UI_COLOR_SAGE        0x5F8F72
#define FRIDGE_UI_COLOR_SAGE_DARK   0x2F6047
#define FRIDGE_UI_COLOR_MINT        0xDCEFDC
#define FRIDGE_UI_COLOR_TOMATO      0xD95745
#define FRIDGE_UI_COLOR_YOLK        0xF6BF4F
#define FRIDGE_UI_COLOR_INK         0x24342D
#define FRIDGE_UI_COLOR_MUTED       0x758078
#define FRIDGE_UI_COLOR_LINE        0xD9E2DC
#define FRIDGE_UI_COLOR_LINE_STRONG 0xAFC2B6
#define FRIDGE_UI_COLOR_WARN_INK    0x6B4E00
#define FRIDGE_UI_COLOR_OFFLINE     0x9AA39D
#define FRIDGE_UI_COLOR_INFO        0x4A7FA5
#define FRIDGE_UI_COLOR_WARN_BG     0xFDEECD
#define FRIDGE_UI_COLOR_DANGER_BG   0xF9E2DE
#define FRIDGE_UI_COLOR_OFFLINE_BG  0xECEEED

void fridge_ui_theme_init(void);

// TF 字体延迟加载成功后更新全部角色 style，并通知 LVGL 只重算引用这些 style 的对象。
void fridge_ui_theme_refresh_fonts(void);

// 返回只包含 text_font 属性的角色 style；可与控件自身颜色/布局 style 叠加。
lv_style_t *fridge_ui_theme_font_style(fridge_font_role_t role);

#ifdef __cplusplus
}
#endif
