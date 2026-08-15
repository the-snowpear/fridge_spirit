// 冰箱小精灵定时器中心页（MORE-02）。
// 对齐 ui-reference/ver2：主计时面 + 快捷预设 + 其他运行中 + 已完成；实际计时在 kitchen_tools。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fridge_kitchen_tools.h"

#define TIMER_PRESET_COUNT 6
#define TIMER_OTHER_MAX (FRIDGE_KITCHEN_TIMER_MAX - 1)
#define TIMER_DONE_MAX FRIDGE_KITCHEN_TIMER_MAX

// 副行控件池：避免 update 时反复 create/delete 导致闪烁。
typedef struct {
    lv_obj_t *root;
    lv_obj_t *title;
    lv_obj_t *remain;
    lv_obj_t *meta;
    lv_obj_t *btn_pause;
    lv_obj_t *btn_pause_label;
    lv_obj_t *btn_plus;
    lv_obj_t *btn_cancel;
    uint8_t timer_id;
} timer_row_view_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *title;
    lv_obj_t *remain;
    lv_obj_t *meta;
} timer_done_view_t;

static lv_obj_t *s_scroll;
static lv_obj_t *s_hero;
static lv_obj_t *s_hero_name;
static lv_obj_t *s_hero_chip;
static lv_obj_t *s_hero_face;
static lv_obj_t *s_hero_sub;
static lv_obj_t *s_hero_ops;
static lv_obj_t *s_btn_pause;
static lv_obj_t *s_btn_pause_label;
static lv_obj_t *s_btn_plus;
static lv_obj_t *s_btn_cancel;
static lv_obj_t *s_other_title;
static lv_obj_t *s_done_title;
static lv_obj_t *s_other_wrap;
static lv_obj_t *s_done_wrap;

static timer_row_view_t s_other_rows[TIMER_OTHER_MAX];
static timer_done_view_t s_done_rows[TIMER_DONE_MAX];
static uint8_t s_primary_id;

static const uint8_t k_presets_min[TIMER_PRESET_COUNT] = {1, 3, 5, 10, 15, 30};

static void format_duration(uint32_t seconds, char *out, size_t out_size)
{
    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds / 60) % 60;
    uint32_t remain = seconds % 60;
    if (hours > 0) {
        snprintf(out, out_size, "%lu:%02lu:%02lu",
                 (unsigned long)hours, (unsigned long)minutes, (unsigned long)remain);
    } else {
        snprintf(out, out_size, "%02lu:%02lu", (unsigned long)minutes, (unsigned long)remain);
    }
}

static void style_card(lv_obj_t *obj, lv_color_t bg, lv_color_t border)
{
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static lv_obj_t *make_text_btn(lv_obj_t *parent,
                               const char *text,
                               lv_color_t bg,
                               lv_color_t fg,
                               lv_color_t border,
                               int16_t w,
                               int16_t h,
                               lv_event_cb_t cb,
                               void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, border, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
    lv_obj_center(label);
    return btn;
}

static void back_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_MORE);
}

static void new_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_TIMER_EDIT);
}

static void preset_cb(lv_event_t *event)
{
    uint8_t minutes = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (minutes == 0) {
        return;
    }
    char label[24];
    snprintf(label, sizeof(label), "%u 分钟", (unsigned)minutes);
    uint8_t id = 0;
    esp_err_t err = fridge_kitchen_tools_timer_start_ex((uint32_t)minutes * 60U,
                                                       label,
                                                       "手动",
                                                       FRIDGE_KITCHEN_TIMER_SOUND_VOICE,
                                                       &id);
    if (err == ESP_OK) {
        fridge_ui_toast("已启动倒计时");
    } else if (err == ESP_ERR_NO_MEM) {
        fridge_ui_toast("定时器已满");
    } else {
        fridge_ui_toast("启动失败");
    }
    fridge_ui_page_timer_update();
}

static void hero_pause_cb(lv_event_t *event)
{
    (void)event;
    if (s_primary_id == 0) {
        return;
    }
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < snap.timer_count; i++) {
        if (snap.timers[i].id != s_primary_id) {
            continue;
        }
        if (snap.timers[i].state == FRIDGE_KITCHEN_TIMER_PAUSED) {
            err = fridge_kitchen_tools_timer_resume_id(s_primary_id);
            fridge_ui_toast(err == ESP_OK ? "已继续" : "无法继续");
        } else {
            err = fridge_kitchen_tools_timer_pause_id(s_primary_id);
            fridge_ui_toast(err == ESP_OK ? "已暂停" : "无法暂停");
        }
        break;
    }
    fridge_ui_page_timer_update();
}

static void hero_plus_cb(lv_event_t *event)
{
    (void)event;
    if (s_primary_id == 0) {
        return;
    }
    esp_err_t err = fridge_kitchen_tools_timer_add_seconds(s_primary_id, 60);
    fridge_ui_toast(err == ESP_OK ? "已延长 1 分钟" : "延长失败");
    fridge_ui_page_timer_update();
}

static void hero_cancel_cb(lv_event_t *event)
{
    (void)event;
    if (s_primary_id == 0) {
        return;
    }
    esp_err_t err = fridge_kitchen_tools_timer_cancel_id(s_primary_id);
    fridge_ui_toast(err == ESP_OK ? "已取消定时器" : "取消失败");
    fridge_ui_page_timer_update();
}

static uint8_t row_id_from_event(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    for (uint8_t i = 0; i < TIMER_OTHER_MAX; i++) {
        if (target == s_other_rows[i].btn_pause ||
            target == s_other_rows[i].btn_plus ||
            target == s_other_rows[i].btn_cancel) {
            return s_other_rows[i].timer_id;
        }
    }
    return 0;
}

static void row_pause_cb(lv_event_t *event)
{
    uint8_t id = row_id_from_event(event);
    if (id == 0) {
        return;
    }
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < snap.timer_count; i++) {
        if (snap.timers[i].id != id) {
            continue;
        }
        if (snap.timers[i].state == FRIDGE_KITCHEN_TIMER_PAUSED) {
            (void)fridge_kitchen_tools_timer_resume_id(id);
        } else {
            (void)fridge_kitchen_tools_timer_pause_id(id);
        }
        break;
    }
    fridge_ui_page_timer_update();
}

static void row_plus_cb(lv_event_t *event)
{
    uint8_t id = row_id_from_event(event);
    if (id == 0) {
        return;
    }
    (void)fridge_kitchen_tools_timer_add_seconds(id, 60);
    fridge_ui_page_timer_update();
}

static void row_cancel_cb(lv_event_t *event)
{
    uint8_t id = row_id_from_event(event);
    if (id == 0) {
        return;
    }
    (void)fridge_kitchen_tools_timer_cancel_id(id);
    fridge_ui_page_timer_update();
}

static void create_other_row(lv_obj_t *parent, timer_row_view_t *row, int16_t y)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    row->root = lv_obj_create(parent);
    lv_obj_remove_flag(row->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row->root, 656, 118);
    lv_obj_set_pos(row->root, 0, y);
    style_card(row->root, theme->surface, theme->line);
    lv_obj_add_flag(row->root, LV_OBJ_FLAG_HIDDEN);

    row->title = lv_label_create(row->root);
    lv_obj_set_pos(row->title, 16, 12);
    lv_obj_set_width(row->title, 420);
    lv_obj_set_style_text_font(row->title, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(row->title, theme->text, 0);
    lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);

    row->remain = lv_label_create(row->root);
    lv_obj_set_pos(row->remain, 460, 12);
    lv_obj_set_width(row->remain, 180);
    lv_obj_set_style_text_align(row->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(row->remain, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(row->remain, theme->accent, 0);

    row->meta = lv_label_create(row->root);
    lv_obj_set_pos(row->meta, 16, 44);
    lv_obj_set_width(row->meta, 620);
    lv_obj_set_style_text_font(row->meta, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(row->meta, theme->muted, 0);
    lv_label_set_long_mode(row->meta, LV_LABEL_LONG_DOT);

    row->btn_pause = make_text_btn(row->root, "暂停", theme->surface, theme->accent, theme->line,
                                   120, 40, row_pause_cb, NULL);
    lv_obj_set_pos(row->btn_pause, 16, 70);
    row->btn_pause_label = lv_obj_get_child(row->btn_pause, 0);

    row->btn_plus = make_text_btn(row->root, "+1分钟", theme->surface, theme->accent, theme->line,
                                  120, 40, row_plus_cb, NULL);
    lv_obj_set_pos(row->btn_plus, 148, 70);

    row->btn_cancel = make_text_btn(row->root, "取消", theme->surface, theme->danger, theme->danger,
                                    100, 40, row_cancel_cb, NULL);
    lv_obj_set_pos(row->btn_cancel, 280, 70);
    row->timer_id = 0;
}

static void create_done_row(lv_obj_t *parent, timer_done_view_t *row, int16_t y)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    row->root = lv_obj_create(parent);
    lv_obj_remove_flag(row->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row->root, 656, 72);
    lv_obj_set_pos(row->root, 0, y);
    style_card(row->root, theme->surface, theme->line);
    lv_obj_set_style_opa(row->root, LV_OPA_60, 0);
    lv_obj_add_flag(row->root, LV_OBJ_FLAG_HIDDEN);

    row->title = lv_label_create(row->root);
    lv_obj_set_pos(row->title, 16, 12);
    lv_obj_set_width(row->title, 420);
    lv_obj_set_style_text_font(row->title, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(row->title, theme->text, 0);
    lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);

    row->remain = lv_label_create(row->root);
    lv_obj_set_pos(row->remain, 460, 12);
    lv_obj_set_width(row->remain, 180);
    lv_obj_set_style_text_align(row->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(row->remain, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(row->remain, theme->muted, 0);

    row->meta = lv_label_create(row->root);
    lv_obj_set_pos(row->meta, 16, 42);
    lv_obj_set_width(row->meta, 620);
    lv_obj_set_style_text_font(row->meta, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(row->meta, theme->muted, 0);
}

void fridge_ui_page_timer_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 24, 0);
    s_scroll = parent;

    // ---- 页头 ----
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "定时器");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_set_style_text_color(title, theme->text, 0);
    lv_obj_set_pos(title, 28, 12);

    lv_obj_t *id_chip = lv_label_create(parent);
    lv_label_set_text(id_chip, "MORE-02");
    lv_obj_set_style_text_font(id_chip, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(id_chip, theme->muted, 0);
    lv_obj_set_pos(id_chip, 148, 22);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "倒计时 · 厨房计时");
    lv_obj_set_style_text_font(sub, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(sub, theme->muted, 0);
    lv_obj_set_pos(sub, 28, 52);

    lv_obj_t *new_btn = make_text_btn(parent, "新建", theme->accent, lv_color_white(), theme->accent,
                                      96, 48, new_cb, NULL);
    lv_obj_set_pos(new_btn, 492, 18);
    lv_obj_t *new_label = lv_obj_get_child(new_btn, 0);
    if (new_label) {
        lv_obj_set_style_text_font(new_label, fridge_ui_font_body(), 0);
    }

    lv_obj_t *back_btn = make_text_btn(parent, "返回", theme->surface, theme->text, theme->line,
                                       96, 48, back_cb, NULL);
    lv_obj_set_pos(back_btn, 600, 18);
    lv_obj_t *back_label = lv_obj_get_child(back_btn, 0);
    if (back_label) {
        lv_obj_set_style_text_font(back_label, fridge_ui_font_body(), 0);
    }

    // ---- 主计时面 ----
    s_hero = lv_obj_create(parent);
    lv_obj_remove_flag(s_hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_hero, 664, 250);
    lv_obj_set_pos(s_hero, 28, 96);
    style_card(s_hero, lv_color_hex(0xFFF5E5), theme->line);

    s_hero_name = lv_label_create(s_hero);
    lv_label_set_text(s_hero_name, "厨房倒计时");
    lv_obj_set_style_text_font(s_hero_name, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(s_hero_name, theme->accent, 0);
    lv_obj_align(s_hero_name, LV_ALIGN_TOP_MID, -40, 18);

    s_hero_chip = lv_label_create(s_hero);
    lv_label_set_text(s_hero_chip, "");
    lv_obj_set_style_text_font(s_hero_chip, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(s_hero_chip, theme->muted, 0);
    lv_obj_align(s_hero_chip, LV_ALIGN_TOP_MID, 90, 22);

    s_hero_face = lv_label_create(s_hero);
    lv_label_set_text(s_hero_face, "00:00");
    lv_obj_set_style_text_font(s_hero_face, fridge_ui_font_number(), 0);
    lv_obj_set_style_text_color(s_hero_face, theme->muted, 0);
    lv_obj_set_style_text_opa(s_hero_face, LV_OPA_60, 0);
    lv_obj_align(s_hero_face, LV_ALIGN_TOP_MID, 0, 56);

    s_hero_sub = lv_label_create(s_hero);
    lv_label_set_text(s_hero_sub, "点下方预设立即开始，或新建自定义时长");
    lv_obj_set_style_text_font(s_hero_sub, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(s_hero_sub, theme->muted, 0);
    lv_obj_set_width(s_hero_sub, 600);
    lv_obj_set_style_text_align(s_hero_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hero_sub, LV_ALIGN_TOP_MID, 0, 160);

    s_hero_ops = lv_obj_create(s_hero);
    lv_obj_remove_style_all(s_hero_ops);
    lv_obj_set_size(s_hero_ops, 600, 52);
    lv_obj_align(s_hero_ops, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(s_hero_ops, LV_OBJ_FLAG_HIDDEN);

    s_btn_pause = make_text_btn(s_hero_ops, "暂停", theme->surface, theme->accent, theme->line,
                                170, 48, hero_pause_cb, NULL);
    lv_obj_set_pos(s_btn_pause, 20, 2);
    s_btn_pause_label = lv_obj_get_child(s_btn_pause, 0);

    s_btn_plus = make_text_btn(s_hero_ops, "+1 分钟", theme->surface, theme->accent, theme->line,
                               170, 48, hero_plus_cb, NULL);
    lv_obj_set_pos(s_btn_plus, 215, 2);

    s_btn_cancel = make_text_btn(s_hero_ops, "取消", theme->surface, theme->danger, theme->danger,
                                 170, 48, hero_cancel_cb, NULL);
    lv_obj_set_pos(s_btn_cancel, 410, 2);

    // ---- 快捷开始 ----
    lv_obj_t *preset_label = lv_label_create(parent);
    lv_label_set_text(preset_label, "快捷开始");
    lv_obj_set_style_text_font(preset_label, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(preset_label, theme->muted, 0);
    lv_obj_set_pos(preset_label, 28, 362);

    const int16_t preset_y = 392;
    const int16_t preset_w = 100;
    const int16_t preset_h = 72;
    const int16_t preset_gap = 8;
    for (uint8_t i = 0; i < TIMER_PRESET_COUNT; i++) {
        int16_t x = 28 + (int16_t)i * (preset_w + preset_gap);
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_size(btn, preset_w, preset_h);
        lv_obj_set_pos(btn, x, preset_y);
        style_card(btn, theme->surface, theme->line);
        lv_obj_set_style_bg_color(btn, theme->surface, 0);
        lv_obj_add_event_cb(btn, preset_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)k_presets_min[i]);

        lv_obj_t *num = lv_label_create(btn);
        char num_text[8];
        snprintf(num_text, sizeof(num_text), "%u", (unsigned)k_presets_min[i]);
        lv_label_set_text(num, num_text);
        lv_obj_set_style_text_font(num, fridge_ui_font_large(), 0);
        lv_obj_set_style_text_color(num, theme->accent, 0);
        lv_obj_align(num, LV_ALIGN_TOP_MID, 0, 8);

        lv_obj_t *unit = lv_label_create(btn);
        lv_label_set_text(unit, "分钟");
        lv_obj_set_style_text_font(unit, fridge_ui_font_small(), 0);
        lv_obj_set_style_text_color(unit, theme->muted, 0);
        lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    // ---- 其他运行中 ----
    s_other_title = lv_label_create(parent);
    lv_label_set_text(s_other_title, "其他运行中");
    lv_obj_set_style_text_font(s_other_title, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(s_other_title, theme->muted, 0);
    lv_obj_set_pos(s_other_title, 28, 480);
    lv_obj_add_flag(s_other_title, LV_OBJ_FLAG_HIDDEN);

    s_other_wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(s_other_wrap);
    lv_obj_set_pos(s_other_wrap, 28, 508);
    lv_obj_set_size(s_other_wrap, 664, TIMER_OTHER_MAX * 126);
    lv_obj_add_flag(s_other_wrap, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < TIMER_OTHER_MAX; i++) {
        create_other_row(s_other_wrap, &s_other_rows[i], (int16_t)(i * 126));
    }

    // ---- 已完成 ----
    s_done_title = lv_label_create(parent);
    lv_label_set_text(s_done_title, "已完成");
    lv_obj_set_style_text_font(s_done_title, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(s_done_title, theme->muted, 0);
    lv_obj_set_pos(s_done_title, 28, 520);
    lv_obj_add_flag(s_done_title, LV_OBJ_FLAG_HIDDEN);

    s_done_wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(s_done_wrap);
    lv_obj_set_pos(s_done_wrap, 28, 548);
    lv_obj_set_size(s_done_wrap, 664, TIMER_DONE_MAX * 80);
    lv_obj_add_flag(s_done_wrap, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < TIMER_DONE_MAX; i++) {
        create_done_row(s_done_wrap, &s_done_rows[i], (int16_t)(i * 80));
    }

    fridge_ui_page_timer_update();
}

void fridge_ui_page_timer_update(void)
{
    if (!s_hero_face) {
        return;
    }
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }

    // 分类：active 按 remaining 升序；primary 取 active[0] 或 ringing。
    const fridge_kitchen_timer_snapshot_t *active[FRIDGE_KITCHEN_TIMER_MAX];
    const fridge_kitchen_timer_snapshot_t *done[FRIDGE_KITCHEN_TIMER_MAX];
    size_t active_n = 0;
    size_t done_n = 0;
    const fridge_kitchen_timer_snapshot_t *ringing = NULL;

    for (size_t i = 0; i < snap.timer_count; i++) {
        const fridge_kitchen_timer_snapshot_t *t = &snap.timers[i];
        if (t->state == FRIDGE_KITCHEN_TIMER_RINGING) {
            if (!ringing || t->target_at_ms < ringing->target_at_ms) {
                ringing = t;
            }
            active[active_n++] = t;
        } else if (t->state == FRIDGE_KITCHEN_TIMER_RUNNING || t->state == FRIDGE_KITCHEN_TIMER_PAUSED) {
            active[active_n++] = t;
        } else if (t->state == FRIDGE_KITCHEN_TIMER_DONE) {
            done[done_n++] = t;
        }
    }

    // 简单插入排序：remaining 升序，RINGING 视为 0 且优先。
    for (size_t i = 1; i < active_n; i++) {
        const fridge_kitchen_timer_snapshot_t *key = active[i];
        size_t j = i;
        while (j > 0) {
            uint32_t ra = active[j - 1]->state == FRIDGE_KITCHEN_TIMER_RINGING ? 0 : active[j - 1]->remaining_seconds;
            uint32_t rb = key->state == FRIDGE_KITCHEN_TIMER_RINGING ? 0 : key->remaining_seconds;
            if (ra <= rb) {
                break;
            }
            active[j] = active[j - 1];
            j--;
        }
        active[j] = key;
    }

    const fridge_kitchen_timer_snapshot_t *primary = ringing ? ringing : (active_n ? active[0] : NULL);
    s_primary_id = primary ? primary->id : 0;

    if (primary) {
        style_card(s_hero, theme->surface, theme->line);
        fridge_ui_label_set_text_if_changed(s_hero_name, primary->label[0] ? primary->label : "厨房计时");
        if (primary->state == FRIDGE_KITCHEN_TIMER_PAUSED) {
            fridge_ui_label_set_text_if_changed(s_hero_chip, "已暂停");
            lv_obj_set_style_text_color(s_hero_chip, lv_color_hex(0x6B4E00), 0);
        } else if (primary->state == FRIDGE_KITCHEN_TIMER_RINGING) {
            fridge_ui_label_set_text_if_changed(s_hero_chip, "时间到");
            lv_obj_set_style_text_color(s_hero_chip, theme->danger, 0);
        } else {
            fridge_ui_label_set_text_if_changed(s_hero_chip, "运行中");
            lv_obj_set_style_text_color(s_hero_chip, theme->accent, 0);
        }

        char face[16];
        format_duration(primary->remaining_seconds, face, sizeof(face));
        fridge_ui_label_set_text_if_changed(s_hero_face, face);
        lv_obj_set_style_text_color(s_hero_face, theme->text, 0);
        lv_obj_set_style_text_opa(s_hero_face, LV_OPA_COVER, 0);

        char total[16];
        format_duration(primary->duration_seconds, total, sizeof(total));
        fridge_ui_label_set_text_fmt_if_changed(s_hero_sub,
                                                "总时长 %s · 来源 %s",
                                                total,
                                                primary->source[0] ? primary->source : "手动");

        lv_obj_clear_flag(s_hero_ops, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_pause_label) {
            fridge_ui_label_set_text_if_changed(s_btn_pause_label,
                                                primary->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "继续" : "暂停");
        }
        // RINGING 时暂停无意义，隐藏暂停按钮语义改由到时层处理，但仍可 +1 / 取消。
        if (primary->state == FRIDGE_KITCHEN_TIMER_RINGING) {
            lv_obj_add_flag(s_btn_pause, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_btn_pause, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        style_card(s_hero, lv_color_hex(0xFFF5E5), theme->line);
        fridge_ui_label_set_text_if_changed(s_hero_name, "厨房倒计时");
        fridge_ui_label_set_text_if_changed(s_hero_chip, "");
        fridge_ui_label_set_text_if_changed(s_hero_face, "00:00");
        lv_obj_set_style_text_color(s_hero_face, theme->muted, 0);
        lv_obj_set_style_text_opa(s_hero_face, LV_OPA_60, 0);
        fridge_ui_label_set_text_if_changed(s_hero_sub, "点下方预设立即开始，或新建自定义时长");
        lv_obj_add_flag(s_hero_ops, LV_OBJ_FLAG_HIDDEN);
    }

    // 其他运行中：跳过 primary
    size_t other_n = 0;
    for (size_t i = 0; i < active_n && other_n < TIMER_OTHER_MAX; i++) {
        if (primary && active[i]->id == primary->id) {
            continue;
        }
        timer_row_view_t *row = &s_other_rows[other_n];
        row->timer_id = active[i]->id;
        lv_obj_clear_flag(row->root, LV_OBJ_FLAG_HIDDEN);
        fridge_ui_label_set_text_if_changed(row->title, active[i]->label[0] ? active[i]->label : "厨房计时");
        char remain[16];
        format_duration(active[i]->remaining_seconds, remain, sizeof(remain));
        fridge_ui_label_set_text_if_changed(row->remain, remain);
        char total[16];
        format_duration(active[i]->duration_seconds, total, sizeof(total));
        fridge_ui_label_set_text_fmt_if_changed(row->meta,
                                                "总时长 %s · 来源 %s · %s",
                                                total,
                                                active[i]->source[0] ? active[i]->source : "手动",
                                                active[i]->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "已暂停" : "运行中");
        if (row->btn_pause_label) {
            fridge_ui_label_set_text_if_changed(row->btn_pause_label,
                                                active[i]->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "继续" : "暂停");
        }
        other_n++;
    }
    for (size_t i = other_n; i < TIMER_OTHER_MAX; i++) {
        s_other_rows[i].timer_id = 0;
        lv_obj_add_flag(s_other_rows[i].root, LV_OBJ_FLAG_HIDDEN);
    }

    if (other_n > 0) {
        lv_obj_clear_flag(s_other_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_other_wrap, LV_OBJ_FLAG_HIDDEN);
        fridge_ui_label_set_text_fmt_if_changed(s_other_title, "其他运行中（%u）", (unsigned)other_n);
        lv_obj_set_height(s_other_wrap, (int32_t)(other_n * 126));
    } else {
        lv_obj_add_flag(s_other_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_other_wrap, LV_OBJ_FLAG_HIDDEN);
    }

    // 已完成列表
    size_t show_done = done_n > TIMER_DONE_MAX ? TIMER_DONE_MAX : done_n;
    for (size_t i = 0; i < show_done; i++) {
        timer_done_view_t *row = &s_done_rows[i];
        lv_obj_clear_flag(row->root, LV_OBJ_FLAG_HIDDEN);
        fridge_ui_label_set_text_if_changed(row->title, done[i]->label[0] ? done[i]->label : "厨房计时");
        fridge_ui_label_set_text_if_changed(row->remain, "00:00");
        char total[16];
        format_duration(done[i]->duration_seconds, total, sizeof(total));
        fridge_ui_label_set_text_fmt_if_changed(row->meta,
                                                "总时长 %s · 来源 %s",
                                                total,
                                                done[i]->source[0] ? done[i]->source : "手动");
    }
    for (size_t i = show_done; i < TIMER_DONE_MAX; i++) {
        lv_obj_add_flag(s_done_rows[i].root, LV_OBJ_FLAG_HIDDEN);
    }

    // 动态布局：其他运行中与已完成纵向排布
    int16_t y = 480;
    if (other_n > 0) {
        lv_obj_set_pos(s_other_title, 28, y);
        y += 28;
        lv_obj_set_pos(s_other_wrap, 28, y);
        y += (int16_t)(other_n * 126) + 12;
    }
    if (show_done > 0) {
        lv_obj_clear_flag(s_done_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_done_wrap, LV_OBJ_FLAG_HIDDEN);
        fridge_ui_label_set_text_fmt_if_changed(s_done_title, "已完成（%u）", (unsigned)show_done);
        lv_obj_set_pos(s_done_title, 28, y);
        y += 28;
        lv_obj_set_pos(s_done_wrap, 28, y);
        lv_obj_set_height(s_done_wrap, (int32_t)(show_done * 80));
    } else {
        lv_obj_add_flag(s_done_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_done_wrap, LV_OBJ_FLAG_HIDDEN);
    }

    (void)s_scroll;
}
