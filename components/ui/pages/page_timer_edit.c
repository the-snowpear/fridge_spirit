// 冰箱小精灵定时器编辑页（MORE-03 · 倒计时）。
// 对齐 ui-reference/ver2 的 timer 分支：时长步进 + 名称预设 + 提醒声音；不接菜谱关联/小程序同步。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "fridge_kitchen_tools.h"

#define NAME_PRESET_COUNT 4

static lv_obj_t *s_hour_label;
static lv_obj_t *s_min_label;
static lv_obj_t *s_name_label;
static lv_obj_t *s_sound_chime;
static lv_obj_t *s_sound_voice;
static lv_obj_t *s_name_chips[NAME_PRESET_COUNT];

static uint8_t s_hour;
static uint8_t s_minute = 10;
static uint8_t s_name_idx;
static fridge_kitchen_timer_sound_t s_sound = FRIDGE_KITCHEN_TIMER_SOUND_VOICE;

static const char *k_name_presets[NAME_PRESET_COUNT] = {
    "煮蛋",
    "焯水",
    "蒸鱼",
    "厨房计时",
};

static void style_card(lv_obj_t *obj, lv_color_t bg, lv_color_t border)
{
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static lv_obj_t *make_btn(lv_obj_t *parent,
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
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static void refresh_labels(void)
{
    if (s_hour_label) {
        fridge_ui_label_set_text_fmt_if_changed(s_hour_label, "%u", (unsigned)s_hour);
    }
    if (s_min_label) {
        fridge_ui_label_set_text_fmt_if_changed(s_min_label, "%02u", (unsigned)s_minute);
    }
    if (s_name_label) {
        fridge_ui_label_set_text_if_changed(s_name_label, k_name_presets[s_name_idx]);
    }

    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    for (uint8_t i = 0; i < NAME_PRESET_COUNT; i++) {
        if (!s_name_chips[i]) {
            continue;
        }
        bool on = (i == s_name_idx);
        lv_obj_set_style_bg_color(s_name_chips[i], on ? theme->accent : theme->surface, 0);
        lv_obj_t *lab = lv_obj_get_child(s_name_chips[i], 0);
        if (lab) {
            lv_obj_set_style_text_color(lab, on ? lv_color_white() : theme->text, 0);
        }
    }
    if (s_sound_chime && s_sound_voice) {
        bool chime = (s_sound == FRIDGE_KITCHEN_TIMER_SOUND_CHIME);
        lv_obj_set_style_bg_color(s_sound_chime, chime ? theme->accent : theme->surface, 0);
        lv_obj_set_style_bg_color(s_sound_voice, !chime ? theme->accent : theme->surface, 0);
        lv_obj_t *lc = lv_obj_get_child(s_sound_chime, 0);
        lv_obj_t *lv = lv_obj_get_child(s_sound_voice, 0);
        if (lc) {
            lv_obj_set_style_text_color(lc, chime ? lv_color_white() : theme->text, 0);
        }
        if (lv) {
            lv_obj_set_style_text_color(lv, !chime ? lv_color_white() : theme->text, 0);
        }
    }
}

static void back_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_TIMER);
}

static void hour_adj_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int next = (int)s_hour + delta;
    if (next < 0) {
        next = 0;
    } else if (next > 23) {
        next = 23;
    }
    s_hour = (uint8_t)next;
    refresh_labels();
}

static void min_adj_cb(lv_event_t *event)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int next = (int)s_minute + delta;
    if (next < 0) {
        next = 59;
    } else if (next > 59) {
        next = 0;
    }
    s_minute = (uint8_t)next;
    refresh_labels();
}

static void name_cb(lv_event_t *event)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (idx >= NAME_PRESET_COUNT) {
        return;
    }
    s_name_idx = idx;
    refresh_labels();
}

static void sound_cb(lv_event_t *event)
{
    s_sound = (fridge_kitchen_timer_sound_t)(uintptr_t)lv_event_get_user_data(event);
    refresh_labels();
}

static void start_cb(lv_event_t *event)
{
    (void)event;
    uint32_t seconds = ((uint32_t)s_hour * 3600U) + ((uint32_t)s_minute * 60U);
    if (seconds == 0) {
        fridge_ui_toast("时长至少 1 分钟");
        return;
    }
    uint8_t id = 0;
    esp_err_t err = fridge_kitchen_tools_timer_start_ex(seconds,
                                                       k_name_presets[s_name_idx],
                                                       "手动",
                                                       s_sound,
                                                       &id);
    if (err == ESP_OK) {
        fridge_ui_toast("定时器已开始");
        fridge_ui_show_page(FRIDGE_UI_PAGE_TIMER);
    } else if (err == ESP_ERR_NO_MEM) {
        fridge_ui_toast("定时器已满");
    } else {
        fridge_ui_toast("启动失败");
    }
}

static void cancel_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_TIMER);
}

void fridge_ui_page_timer_edit_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 24, 0);

    // 默认值：10 分钟煮蛋，贴近参考 mock。
    s_hour = 0;
    s_minute = 10;
    s_name_idx = 0;
    s_sound = FRIDGE_KITCHEN_TIMER_SOUND_VOICE;

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "编辑定时器");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_set_pos(title, 28, 12);

    lv_obj_t *id_chip = lv_label_create(parent);
    lv_label_set_text(id_chip, "MORE-03");
    lv_obj_set_style_text_font(id_chip, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(id_chip, theme->muted, 0);
    lv_obj_set_pos(id_chip, 220, 22);

    lv_obj_t *sub = lv_label_create(parent);
    lv_label_set_text(sub, "新建或修改倒计时");
    lv_obj_set_style_text_font(sub, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(sub, theme->muted, 0);
    lv_obj_set_pos(sub, 28, 52);

    lv_obj_t *back = make_btn(parent, "返回", theme->surface, theme->text, theme->line,
                              96, 48, back_cb, NULL);
    lv_obj_set_pos(back, 600, 18);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, 664, 360);
    lv_obj_set_pos(card, 28, 96);
    style_card(card, theme->surface, theme->line);

    lv_obj_t *type_l = lv_label_create(card);
    lv_label_set_text(type_l, "类型");
    lv_obj_set_style_text_font(type_l, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(type_l, theme->muted, 0);
    lv_obj_set_pos(type_l, 20, 16);

    lv_obj_t *type_v = lv_label_create(card);
    lv_label_set_text(type_v, "倒计时");
    lv_obj_set_style_text_font(type_v, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(type_v, theme->accent, 0);
    lv_obj_set_pos(type_v, 20, 40);

    lv_obj_t *dur_l = lv_label_create(card);
    lv_label_set_text(dur_l, "时长（时 / 分）");
    lv_obj_set_style_text_font(dur_l, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(dur_l, theme->muted, 0);
    lv_obj_set_pos(dur_l, 20, 84);

    lv_obj_t *h_dec = make_btn(card, "时-", theme->surface, theme->text, theme->line,
                               72, 48, hour_adj_cb, (void *)(intptr_t)-1);
    lv_obj_set_pos(h_dec, 20, 112);

    s_hour_label = lv_label_create(card);
    lv_label_set_text(s_hour_label, "0");
    lv_obj_set_style_text_font(s_hour_label, fridge_ui_font_large(), 0);
    lv_obj_set_style_text_color(s_hour_label, theme->text, 0);
    lv_obj_set_pos(s_hour_label, 110, 118);

    lv_obj_t *h_inc = make_btn(card, "时+", theme->accent, lv_color_white(), theme->accent,
                               72, 48, hour_adj_cb, (void *)(intptr_t)1);
    lv_obj_set_pos(h_inc, 160, 112);

    lv_obj_t *sep = lv_label_create(card);
    lv_label_set_text(sep, ":");
    lv_obj_set_style_text_font(sep, fridge_ui_font_large(), 0);
    lv_obj_set_pos(sep, 250, 118);

    lv_obj_t *m_dec = make_btn(card, "分-", theme->surface, theme->text, theme->line,
                               72, 48, min_adj_cb, (void *)(intptr_t)-1);
    lv_obj_set_pos(m_dec, 280, 112);

    s_min_label = lv_label_create(card);
    lv_label_set_text(s_min_label, "10");
    lv_obj_set_style_text_font(s_min_label, fridge_ui_font_large(), 0);
    lv_obj_set_style_text_color(s_min_label, theme->text, 0);
    lv_obj_set_pos(s_min_label, 370, 118);

    lv_obj_t *m_inc = make_btn(card, "分+", theme->accent, lv_color_white(), theme->accent,
                               72, 48, min_adj_cb, (void *)(intptr_t)1);
    lv_obj_set_pos(m_inc, 430, 112);

    lv_obj_t *name_l = lv_label_create(card);
    lv_label_set_text(name_l, "名称");
    lv_obj_set_style_text_font(name_l, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(name_l, theme->muted, 0);
    lv_obj_set_pos(name_l, 20, 180);

    s_name_label = lv_label_create(card);
    lv_label_set_text(s_name_label, "煮蛋");
    lv_obj_set_style_text_font(s_name_label, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(s_name_label, theme->text, 0);
    lv_obj_set_pos(s_name_label, 20, 204);

    const int16_t chip_w = 148;
    for (uint8_t i = 0; i < NAME_PRESET_COUNT; i++) {
        s_name_chips[i] = make_btn(card,
                                   k_name_presets[i],
                                   theme->surface,
                                   theme->text,
                                   theme->line,
                                   chip_w,
                                   44,
                                   name_cb,
                                   (void *)(uintptr_t)i);
        lv_obj_set_pos(s_name_chips[i], 20 + (int16_t)i * (chip_w + 8), 240);
        lv_obj_t *lab = lv_obj_get_child(s_name_chips[i], 0);
        if (lab) {
            lv_obj_set_style_text_font(lab, fridge_ui_font_small(), 0);
        }
    }

    lv_obj_t *sound_l = lv_label_create(card);
    lv_label_set_text(sound_l, "提醒声音");
    lv_obj_set_style_text_font(sound_l, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(sound_l, theme->muted, 0);
    lv_obj_set_pos(sound_l, 20, 300);

    s_sound_chime = make_btn(card, "铃声", theme->surface, theme->text, theme->line,
                             160, 44, sound_cb, (void *)(uintptr_t)FRIDGE_KITCHEN_TIMER_SOUND_CHIME);
    lv_obj_set_pos(s_sound_chime, 20, 324);

    s_sound_voice = make_btn(card, "语音播报", theme->accent, lv_color_white(), theme->accent,
                             160, 44, sound_cb, (void *)(uintptr_t)FRIDGE_KITCHEN_TIMER_SOUND_VOICE);
    lv_obj_set_pos(s_sound_voice, 196, 324);

    lv_obj_t *cancel = make_btn(parent, "取消", theme->surface, theme->text, theme->line,
                                200, 56, cancel_cb, NULL);
    lv_obj_set_pos(cancel, 120, 480);

    lv_obj_t *start = make_btn(parent, "立即开始", theme->accent, lv_color_white(), theme->accent,
                               280, 56, start_cb, NULL);
    lv_obj_set_pos(start, 360, 480);

    refresh_labels();
}

void fridge_ui_page_timer_edit_update(void)
{
    // 编辑页无运行态轮询需求；保留接口供 UI 任务统一调用。
}
