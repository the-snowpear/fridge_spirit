// 冰箱小精灵购物清单页。
// 页面读取 AI 写入的 shopping_list 缓存；checkbox 只做本地视觉反馈，不在触摸路径写 Flash。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "fridge_kitchen_ai.h"
#include "fridge_storage.h"

#define SHOPPING_VIEW_MAX 6
#define SHOPPING_CACHE_REFRESH_MS 10000

typedef struct {
    char name[32];
    char quantity[24];
    char reason[72];
    char priority[16];
    bool checked;
} shopping_view_t;

static lv_obj_t *s_status_label;
static lv_obj_t *s_generate_btn;
static lv_obj_t *s_generate_label;
static lv_obj_t *s_items[SHOPPING_VIEW_MAX];
static char *s_cache_buf;
static uint32_t s_last_hash;
static int64_t s_last_cache_check_ms;
static int64_t s_last_seen_generated_at;

static uint32_t hash_text(const char *text)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

static const char *json_string_or(const cJSON *root, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0] ? item->valuestring : fallback;
}

static size_t utf8_safe_prefix_len(const char *text, size_t max_bytes)
{
    if (!text || max_bytes == 0) {
        return 0;
    }
    size_t i = 0;
    size_t last_good = 0;
    while (i < max_bytes && text[i] != '\0') {
        unsigned char ch = (unsigned char)text[i];
        size_t need = 0;
        if (ch < 0x80) {
            need = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            need = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            need = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            need = 4;
        } else {
            break;
        }
        if (i + need > max_bytes) {
            break;
        }
        for (size_t j = 1; j < need; j++) {
            if (((unsigned char)text[i + j] & 0xC0) != 0x80) {
                return last_good;
            }
        }
        i += need;
        last_good = i;
    }
    return last_good;
}

static void copy_utf8_safe(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    size_t copy_len = utf8_safe_prefix_len(text, out_size - 1);
    memcpy(out, text, copy_len);
    out[copy_len] = '\0';
}

static void append_utf8_safe(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0 || !text || text[0] == '\0') {
        return;
    }
    size_t used = strlen(out);
    if (used >= out_size - 1) {
        return;
    }
    size_t copy_len = utf8_safe_prefix_len(text, out_size - used - 1);
    memcpy(out + used, text, copy_len);
    out[used + copy_len] = '\0';
}

static bool load_cache_text(void)
{
    if (!s_cache_buf) {
        s_cache_buf = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
        if (!s_cache_buf) {
            return false;
        }
    }
    return fridge_storage_get_shopping_list(s_cache_buf, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK;
}

static bool parse_cache_buffer(shopping_view_t *items, size_t *count, int64_t *generated_at)
{
    *count = 0;
    if (generated_at) {
        *generated_at = 0;
    }
    if (!s_cache_buf || s_cache_buf[0] == '\0') {
        return false;
    }
    cJSON *root = cJSON_Parse(s_cache_buf);
    if (!root) {
        return false;
    }
    const cJSON *generated = cJSON_GetObjectItemCaseSensitive(root, "generated_at");
    if (generated_at && cJSON_IsNumber(generated)) {
        *generated_at = (int64_t)generated->valuedouble;
    }
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *src = NULL;
    cJSON_ArrayForEach(src, array) {
        if (*count >= SHOPPING_VIEW_MAX || !cJSON_IsObject(src)) {
            break;
        }
        shopping_view_t *item = &items[*count];
        copy_utf8_safe(item->name, sizeof(item->name), json_string_or(src, "name", "待补货"));
        copy_utf8_safe(item->quantity, sizeof(item->quantity), json_string_or(src, "quantity", "1份"));
        copy_utf8_safe(item->reason, sizeof(item->reason), json_string_or(src, "reason", "AI 采购建议"));
        copy_utf8_safe(item->priority, sizeof(item->priority), json_string_or(src, "priority", "normal"));
        const cJSON *checked = cJSON_GetObjectItemCaseSensitive(src, "checked");
        item->checked = cJSON_IsBool(checked) && cJSON_IsTrue(checked);
        (*count)++;
    }
    cJSON_Delete(root);
    return *count > 0;
}

static bool parse_cache(shopping_view_t *items, size_t *count, int64_t *generated_at)
{
    if (!load_cache_text()) {
        return false;
    }
    return parse_cache_buffer(items, count, generated_at);
}

static void generate_btn_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = fridge_kitchen_ai_generate_page_async(FRIDGE_KITCHEN_AI_TRIGGER_MANUAL, FRIDGE_KITCHEN_AI_PAGE_SHOPPING);
    if (err == ESP_OK) {
        fridge_ui_toast("AI 正在生成采购建议");
    } else if (err == ESP_ERR_INVALID_STATE) {
        fridge_ui_toast("AI 已在生成中");
    } else {
        fridge_ui_toast("AI 生成任务启动失败");
    }
}

static void create_row(lv_obj_t *parent, uint8_t index)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, "等待 AI 生成");
    lv_obj_set_size(cb, 620, 58);
    lv_obj_set_pos(cb, 54, 122 + index * 70);
    lv_obj_set_style_text_color(cb, theme->text, 0);
    lv_obj_set_style_text_font(cb, fridge_ui_font_ai_body(), 0);
    s_items[index] = cb;
}

static void update_status_label(int64_t generated_at)
{
    fridge_kitchen_ai_status_t status = {0};
    (void)fridge_kitchen_ai_get_status(&status);
    if (status.running) {
        fridge_ui_label_set_text_if_changed(s_status_label, "AI 正在根据库存和菜谱生成采购清单...");
        fridge_ui_label_set_text_if_changed(s_generate_label, "生成中");
        lv_obj_add_state(s_generate_btn, LV_STATE_DISABLED);
        return;
    }
    lv_obj_remove_state(s_generate_btn, LV_STATE_DISABLED);
    fridge_ui_label_set_text_if_changed(s_generate_label, "AI 现在生成");
    if (!status.last_ok && status.last_error[0]) {
        fridge_ui_label_set_text_if_changed(s_status_label, status.last_error);
    } else if (generated_at > 0) {
        fridge_ui_label_set_text_fmt_if_changed(s_status_label, "最近生成：%lld；勾选仅作本地提示", (long long)generated_at);
    } else {
        fridge_ui_label_set_text_if_changed(s_status_label, "尚未生成 AI 购物清单，点击按钮立即生成。");
    }
}

void fridge_ui_page_shopping_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "自动购物清单");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "下次采购建议");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    s_generate_btn = lv_button_create(parent);
    lv_obj_set_size(s_generate_btn, 190, 48);
    lv_obj_set_pos(s_generate_btn, 500, 28);
    lv_obj_set_style_bg_color(s_generate_btn, theme->accent_2, 0);
    lv_obj_set_style_radius(s_generate_btn, 20, 0);
    lv_obj_set_style_shadow_width(s_generate_btn, 0, 0);
    lv_obj_add_event_cb(s_generate_btn, generate_btn_cb, LV_EVENT_CLICKED, NULL);
    s_generate_label = lv_label_create(s_generate_btn);
    lv_label_set_text(s_generate_label, "AI 现在生成");
    lv_obj_set_style_text_color(s_generate_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_generate_label, fridge_ui_font_small(), 0);
    lv_obj_center(s_generate_label);

    s_status_label = lv_label_create(parent);
    lv_obj_set_pos(s_status_label, 38, 84);
    lv_obj_set_width(s_status_label, 640);
    lv_obj_set_style_text_color(s_status_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_status_label, fridge_ui_font_ai_body(), 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);

    for (uint8_t i = 0; i < SHOPPING_VIEW_MAX; i++) {
        create_row(parent, i);
    }
}

void fridge_ui_page_shopping_update(void)
{
    shopping_view_t items[SHOPPING_VIEW_MAX] = {0};
    size_t count = 0;
    int64_t generated_at = 0;
    fridge_kitchen_ai_status_t status = {0};
    (void)fridge_kitchen_ai_get_status(&status);
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool should_read_cache = !s_cache_buf ||
                             (status.last_generated_at > 0 && status.last_generated_at != s_last_seen_generated_at) ||
                             (now_ms - s_last_cache_check_ms >= SHOPPING_CACHE_REFRESH_MS);
    bool ok = false;
    if (should_read_cache) {
        ok = parse_cache(items, &count, &generated_at);
        s_last_cache_check_ms = now_ms;
        if (ok) {
            s_last_seen_generated_at = generated_at;
        }
    } else {
        ok = parse_cache_buffer(items, &count, &generated_at);
    }
    uint32_t next_hash = hash_text(s_cache_buf);
    if (next_hash != s_last_hash || !ok) {
        for (uint8_t i = 0; i < SHOPPING_VIEW_MAX; i++) {
            if (ok && i < count) {
                lv_obj_clear_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
                char text[160] = {0};
                append_utf8_safe(text, sizeof(text), items[i].name);
                append_utf8_safe(text, sizeof(text), " ");
                append_utf8_safe(text, sizeof(text), items[i].quantity);
                append_utf8_safe(text, sizeof(text), " · ");
                append_utf8_safe(text, sizeof(text), strcmp(items[i].priority, "high") == 0 ? "优先" : "可选");
                append_utf8_safe(text, sizeof(text), " · ");
                append_utf8_safe(text, sizeof(text), items[i].reason);
                lv_checkbox_set_text(s_items[i], text);
                if (items[i].checked) {
                    lv_obj_add_state(s_items[i], LV_STATE_CHECKED);
                } else {
                    lv_obj_remove_state(s_items[i], LV_STATE_CHECKED);
                }
            } else if (!ok && i == 0) {
                lv_obj_clear_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
                lv_checkbox_set_text(s_items[i], "等待 AI 生成采购清单");
                lv_obj_remove_state(s_items[i], LV_STATE_CHECKED);
            } else {
                lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_last_hash = next_hash;
    }
    update_status_label(generated_at);
}
