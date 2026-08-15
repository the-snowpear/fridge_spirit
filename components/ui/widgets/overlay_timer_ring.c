// 定时器到时浮层（OV-09）。
// 非阻塞：挂在 g_ui_root 上，不强制切页；多 RINGING 时展示最早到时的一条，结束后再展示下一条。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "fridge_kitchen_tools.h"

static lv_obj_t *s_root;
static lv_obj_t *s_panel;
static lv_obj_t *s_face;
static lv_obj_t *s_name;
static lv_obj_t *s_src;
static uint8_t s_active_id;
static bool s_visible;

static lv_obj_t *make_btn(lv_obj_t *parent,
                          const char *text,
                          lv_color_t bg,
                          lv_color_t fg,
                          lv_color_t border,
                          int16_t w,
                          lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 52);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, border, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_body(), 0);
    lv_obj_center(label);
    return btn;
}

static void hide_overlay(void)
{
    if (s_root) {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    s_visible = false;
    s_active_id = 0;
}

static void end_cb(lv_event_t *event)
{
    (void)event;
    if (s_active_id != 0) {
        (void)fridge_kitchen_tools_timer_dismiss_id(s_active_id);
    }
    hide_overlay();
    fridge_ui_page_timer_update();
}

static void plus_cb(lv_event_t *event)
{
    (void)event;
    if (s_active_id != 0) {
        (void)fridge_kitchen_tools_timer_add_seconds(s_active_id, 60);
        fridge_ui_toast("已延长 1 分钟");
    }
    hide_overlay();
    fridge_ui_page_timer_update();
}

static void later_cb(lv_event_t *event)
{
    // 参考 ghost「稍后提醒」：关闭面板并 +1 分钟继续计时。
    plus_cb(event);
}

static const fridge_kitchen_timer_snapshot_t *pick_ringing(const fridge_kitchen_tools_snapshot_t *snap)
{
    const fridge_kitchen_timer_snapshot_t *best = NULL;
    for (size_t i = 0; i < snap->timer_count; i++) {
        const fridge_kitchen_timer_snapshot_t *t = &snap->timers[i];
        if (t->state != FRIDGE_KITCHEN_TIMER_RINGING) {
            continue;
        }
        if (!best || t->target_at_ms < best->target_at_ms) {
            best = t;
        }
    }
    return best;
}

void fridge_ui_overlay_timer_ring_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    if (!parent) {
        return;
    }

    // 半透明遮罩：不抢 Dock 区域外的全部触摸，但面板本身可点。
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, FRIDGE_DISPLAY_WIDTH, FRIDGE_DISPLAY_HEIGHT);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x24342D), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_50, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    // 点击遮罩空白不关闭（避免误触 dismiss）；只通过按钮操作。
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

    s_panel = lv_obj_create(s_root);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_panel, 560, 320);
    lv_obj_center(s_panel);
    lv_obj_set_style_bg_color(s_panel, theme->surface, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_panel, theme->line, 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_radius(s_panel, 8, 0);
    lv_obj_set_style_shadow_width(s_panel, 0, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *kicker = lv_label_create(s_panel);
    lv_label_set_text(kicker, "定时器到时");
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_MID, 0, 16);

    s_face = lv_label_create(s_panel);
    lv_label_set_text(s_face, "00:00");
    lv_obj_set_style_text_font(s_face, fridge_ui_font_number(), 0);
    lv_obj_set_style_text_color(s_face, theme->accent, 0);
    lv_obj_align(s_face, LV_ALIGN_TOP_MID, 0, 44);

    s_name = lv_label_create(s_panel);
    lv_label_set_text(s_name, "时间到！");
    lv_obj_set_style_text_font(s_name, fridge_ui_font_body(), 0);
    lv_obj_set_style_text_color(s_name, theme->text, 0);
    lv_obj_set_width(s_name, 500);
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, 150);

    s_src = lv_label_create(s_panel);
    lv_label_set_text(s_src, "来源：手动");
    lv_obj_set_style_text_font(s_src, fridge_ui_font_small(), 0);
    lv_obj_set_style_text_color(s_src, theme->muted, 0);
    lv_obj_align(s_src, LV_ALIGN_TOP_MID, 0, 186);

    lv_obj_t *end_btn = make_btn(s_panel, "结束", theme->accent, lv_color_white(), theme->accent, 150, end_cb);
    lv_obj_set_pos(end_btn, 30, 240);

    lv_obj_t *plus_btn = make_btn(s_panel, "延长1分钟", theme->surface, theme->text, theme->line, 170, plus_cb);
    lv_obj_set_pos(plus_btn, 195, 240);

    lv_obj_t *later_btn = make_btn(s_panel, "稍后提醒", theme->surface, theme->text, theme->line, 150, later_cb);
    lv_obj_set_pos(later_btn, 380, 240);

    s_visible = false;
    s_active_id = 0;
}

void fridge_ui_overlay_timer_ring_update(void)
{
    if (!s_root) {
        return;
    }
    fridge_kitchen_tools_snapshot_t snap = {0};
    if (fridge_kitchen_tools_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    const fridge_kitchen_timer_snapshot_t *ring = pick_ringing(&snap);
    if (!ring) {
        if (s_visible) {
            hide_overlay();
        }
        return;
    }

    s_active_id = ring->id;
    fridge_ui_label_set_text_if_changed(s_face, "00:00");
    fridge_ui_label_set_text_fmt_if_changed(s_name, "%s 时间到！",
                                            ring->label[0] ? ring->label : "厨房计时");
    fridge_ui_label_set_text_fmt_if_changed(s_src, "来源：%s",
                                            ring->source[0] ? ring->source : "手动");
    if (!s_visible) {
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        // 保证浮层在最上层。
        lv_obj_move_foreground(s_root);
        s_visible = true;
    }
}
