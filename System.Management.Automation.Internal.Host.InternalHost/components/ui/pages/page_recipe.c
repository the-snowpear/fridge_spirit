// 冰箱小精灵 AI 菜谱页。
// 页面只读取本地 recipe_cache 并投递手动生成请求；真实 HTTPS 生成在 kitchen_ai 后台任务中执行，避免阻塞 LVGL。

#include "fridge_ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "fridge_kitchen_ai.h"
#include "fridge_storage.h"

#define RECIPE_CARD_COUNT 3
#define RECIPE_CACHE_REFRESH_MS 10000

typedef struct {
    char title[40];
    char tag[40];
    char reason[96];
    char use_inventory[96];
    char missing[80];
    char steps[160];
    int time_minutes;
} recipe_view_t;

static lv_obj_t *s_status_label;
static lv_obj_t *s_generate_btn;
static lv_obj_t *s_generate_label;
static lv_obj_t *s_summary_chips[3];
static lv_obj_t *s_cards[RECIPE_CARD_COUNT];
static lv_obj_t *s_card_tags[RECIPE_CARD_COUNT];
static lv_obj_t *s_card_titles[RECIPE_CARD_COUNT];
static lv_obj_t *s_card_descs[RECIPE_CARD_COUNT];
static lv_obj_t *s_card_steps[RECIPE_CARD_COUNT];
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

static int json_int_or(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static void join_string_array(const cJSON *array, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (cJSON_IsArray(array)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, array) {
            if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
                continue;
            }
            if (out[0] != '\0') {
                append_utf8_safe(out, out_size, "、");
            }
            append_utf8_safe(out, out_size, item->valuestring);
            if (strlen(out) + 8 >= out_size) {
                break;
            }
        }
    }
    if (out[0] == '\0') {
        copy_utf8_safe(out, out_size, fallback ? fallback : "");
    }
}

static void fill_fallback(recipe_view_t *recipes, size_t *count)
{
    *count = 1;
    strlcpy(recipes[0].title, "等待 AI 生成", sizeof(recipes[0].title));
    strlcpy(recipes[0].tag, "今日菜单", sizeof(recipes[0].tag));
    strlcpy(recipes[0].reason, "点击按钮后按库存生成真实菜单。", sizeof(recipes[0].reason));
    strlcpy(recipes[0].use_inventory, "当前库存", sizeof(recipes[0].use_inventory));
    strlcpy(recipes[0].missing, "无", sizeof(recipes[0].missing));
    strlcpy(recipes[0].steps, "生成完成后这里会显示简要步骤。", sizeof(recipes[0].steps));
    recipes[0].time_minutes = 0;
}

static bool load_cache_text(void)
{
    if (!s_cache_buf) {
        s_cache_buf = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
        if (!s_cache_buf) {
            return false;
        }
    }
    return fridge_storage_get_recipe_cache(s_cache_buf, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK;
}

static bool parse_cache_buffer(recipe_view_t *recipes, size_t *count, int64_t *generated_at)
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
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (*count >= RECIPE_CARD_COUNT || !cJSON_IsObject(item)) {
            break;
        }
        recipe_view_t *recipe = &recipes[*count];
        copy_utf8_safe(recipe->title, sizeof(recipe->title), json_string_or(item, "title", "AI 推荐菜"));
        copy_utf8_safe(recipe->tag, sizeof(recipe->tag), json_string_or(item, "tag", "AI 推荐"));
        copy_utf8_safe(recipe->reason, sizeof(recipe->reason), json_string_or(item, "reason", "根据当前库存生成"));
        join_string_array(cJSON_GetObjectItemCaseSensitive(item, "use_inventory"), recipe->use_inventory, sizeof(recipe->use_inventory), "当前库存");
        join_string_array(cJSON_GetObjectItemCaseSensitive(item, "missing"), recipe->missing, sizeof(recipe->missing), "无");
        join_string_array(cJSON_GetObjectItemCaseSensitive(item, "steps"), recipe->steps, sizeof(recipe->steps), "按日常做法完成烹饪");
        recipe->time_minutes = json_int_or(item, "time_minutes", 0);
        (*count)++;
    }
    cJSON_Delete(root);
    return *count > 0;
}

static bool parse_cache(recipe_view_t *recipes, size_t *count, int64_t *generated_at)
{
    if (!load_cache_text()) {
        return false;
    }
    return parse_cache_buffer(recipes, count, generated_at);
}

static void generate_btn_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t err = fridge_kitchen_ai_generate_page_async(FRIDGE_KITCHEN_AI_TRIGGER_MANUAL, FRIDGE_KITCHEN_AI_PAGE_RECIPE);
    if (err == ESP_OK) {
        fridge_ui_toast("AI 正在生成菜谱");
    } else if (err == ESP_ERR_INVALID_STATE) {
        fridge_ui_toast("AI 已在生成中");
    } else {
        fridge_ui_toast("AI 生成任务启动失败");
    }
}

static lv_obj_t *create_summary_chip(lv_obj_t *parent, const char *text, int16_t x)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_pos(chip, x, 118);
    lv_obj_set_size(chip, 196, 34);
    lv_obj_set_style_bg_color(chip, theme->surface, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chip, theme->line, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_radius(chip, 17, 0);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, theme->accent, 0);
    lv_obj_set_style_text_font(label, fridge_ui_font_small(), 0);
    lv_obj_center(label);
    return label;
}

static void create_dish_art(lv_obj_t *parent, lv_color_t accent, uint8_t index)
{
    lv_obj_t *photo = lv_obj_create(parent);
    lv_obj_remove_style_all(photo);
    lv_obj_set_pos(photo, 468, 28);
    lv_obj_set_size(photo, 128, 106);
    lv_obj_set_style_bg_color(photo, lv_color_hex(0xFFFADF), 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(photo, 18, 0);

    lv_obj_t *plate = lv_obj_create(photo);
    lv_obj_remove_style_all(plate);
    lv_obj_set_size(plate, 86, 58);
    lv_obj_set_pos(plate, 21, 18);
    lv_obj_set_style_bg_color(plate, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(plate, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(plate, index == 1 ? 18 : 28, 0);

    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(photo);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 20, 20);
        lv_obj_set_pos(dot, 38 + i * 16, 38 + (i % 2) * 10);
        lv_obj_set_style_bg_color(dot, i % 2 ? accent : lv_color_hex(0xFFD058), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 10, 0);
    }
}

static void create_recipe_card(lv_obj_t *parent, uint8_t index)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    static const lv_color_t tints[] = {
        LV_COLOR_MAKE(0xF8, 0xEC, 0xD4),
        LV_COLOR_MAKE(0xE5, 0xF3, 0xDF),
        LV_COLOR_MAKE(0xE6, 0xF2, 0xF3),
    };
    static const lv_color_t accents[] = {
        LV_COLOR_MAKE(0xD9, 0x57, 0x45),
        LV_COLOR_MAKE(0x4F, 0x8F, 0x62),
        LV_COLOR_MAKE(0x42, 0x86, 0x82),
    };

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 660, 188);
    lv_obj_set_pos(card, 30, 166 + index * 208);
    lv_obj_set_style_bg_color(card, tints[index], 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_color(card, theme->line, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_cards[index] = card;

    s_card_tags[index] = lv_label_create(card);
    lv_obj_set_style_text_color(s_card_tags[index], accents[index], 0);
    lv_obj_set_style_text_font(s_card_tags[index], fridge_ui_font_small(), 0);
    lv_obj_set_pos(s_card_tags[index], 24, 20);
    lv_obj_set_width(s_card_tags[index], 410);
    lv_label_set_long_mode(s_card_tags[index], LV_LABEL_LONG_DOT);

    s_card_titles[index] = lv_label_create(card);
    lv_obj_set_style_text_color(s_card_titles[index], theme->text, 0);
    lv_obj_set_style_text_font(s_card_titles[index], fridge_ui_font_large(), 0);
    lv_obj_set_pos(s_card_titles[index], 24, 54);
    lv_obj_set_width(s_card_titles[index], 410);
    lv_label_set_long_mode(s_card_titles[index], LV_LABEL_LONG_DOT);

    s_card_descs[index] = lv_label_create(card);
    lv_obj_set_style_text_color(s_card_descs[index], theme->muted, 0);
    lv_obj_set_style_text_font(s_card_descs[index], fridge_ui_font_ai_body(), 0);
    lv_obj_set_pos(s_card_descs[index], 24, 96);
    lv_obj_set_width(s_card_descs[index], 410);
    lv_label_set_long_mode(s_card_descs[index], LV_LABEL_LONG_WRAP);

    s_card_steps[index] = lv_label_create(card);
    lv_obj_set_style_text_color(s_card_steps[index], theme->muted, 0);
    lv_obj_set_style_text_font(s_card_steps[index], fridge_ui_font_ai_body(), 0);
    lv_obj_set_pos(s_card_steps[index], 24, 128);
    lv_obj_set_width(s_card_steps[index], 410);
    lv_label_set_long_mode(s_card_steps[index], LV_LABEL_LONG_WRAP);

    create_dish_art(card, accents[index], index);
}

static void update_status_label(int64_t generated_at)
{
    fridge_kitchen_ai_status_t status = {0};
    (void)fridge_kitchen_ai_get_status(&status);
    if (status.running) {
        fridge_ui_label_set_text_if_changed(s_status_label, "AI 正在生成菜谱...");
        fridge_ui_label_set_text_if_changed(s_generate_label, "生成中");
        lv_obj_add_state(s_generate_btn, LV_STATE_DISABLED);
        return;
    }
    lv_obj_remove_state(s_generate_btn, LV_STATE_DISABLED);
    fridge_ui_label_set_text_if_changed(s_generate_label, "AI 现在生成");
    if (!status.last_ok && status.last_error[0]) {
        fridge_ui_label_set_text_if_changed(s_status_label, status.last_error);
    } else if (generated_at > 0) {
        fridge_ui_label_set_text_fmt_if_changed(s_status_label, "最近生成：%lld", (long long)generated_at);
    } else {
        fridge_ui_label_set_text_if_changed(s_status_label, "尚未生成今日 AI 菜谱。");
    }
}

void fridge_ui_page_recipe_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_bottom(parent, 112, 0);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "厨房 AI 助手");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "今晚可以这样做");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    s_generate_btn = lv_button_create(parent);
    lv_obj_set_size(s_generate_btn, 190, 48);
    lv_obj_set_pos(s_generate_btn, 500, 30);
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
    lv_obj_set_pos(s_status_label, 36, 82);
    lv_obj_set_width(s_status_label, 330);
    lv_obj_set_style_text_color(s_status_label, theme->muted, 0);
    lv_obj_set_style_text_font(s_status_label, fridge_ui_font_ai_body(), 0);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);

    s_summary_chips[0] = create_summary_chip(parent, "AI 菜谱", 30);
    s_summary_chips[1] = create_summary_chip(parent, "按库存", 262);
    s_summary_chips[2] = create_summary_chip(parent, "可离线查看", 494);
    for (uint8_t i = 0; i < RECIPE_CARD_COUNT; i++) {
        create_recipe_card(parent, i);
    }
}

void fridge_ui_page_recipe_update(void)
{
    recipe_view_t recipes[RECIPE_CARD_COUNT] = {0};
    size_t count = 0;
    int64_t generated_at = 0;
    fridge_kitchen_ai_status_t status = {0};
    (void)fridge_kitchen_ai_get_status(&status);
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool should_read_cache = !s_cache_buf ||
                             (status.last_generated_at > 0 && status.last_generated_at != s_last_seen_generated_at) ||
                             (now_ms - s_last_cache_check_ms >= RECIPE_CACHE_REFRESH_MS);
    bool ok = false;
    if (should_read_cache) {
        ok = parse_cache(recipes, &count, &generated_at);
        s_last_cache_check_ms = now_ms;
        if (ok) {
            s_last_seen_generated_at = generated_at;
        }
    } else {
        ok = parse_cache_buffer(recipes, &count, &generated_at);
    }
    uint32_t next_hash = hash_text(s_cache_buf);
    if (next_hash != s_last_hash || !ok) {
        if (!ok) {
            fill_fallback(recipes, &count);
        }
        for (uint8_t i = 0; i < RECIPE_CARD_COUNT; i++) {
            if (i < count) {
                lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
                fridge_ui_label_set_text_fmt_if_changed(s_card_tags[i],
                                                        recipes[i].time_minutes > 0 ? "%s · %d 分钟" : "%s",
                                                        recipes[i].tag,
                                                        recipes[i].time_minutes);
                fridge_ui_label_set_text_if_changed(s_card_titles[i], recipes[i].title);
                fridge_ui_label_set_text_fmt_if_changed(s_card_descs[i], "库存：%s  缺：%s", recipes[i].use_inventory, recipes[i].missing);
                fridge_ui_label_set_text_if_changed(s_card_steps[i], recipes[i].steps[0] ? recipes[i].steps : recipes[i].reason);
            } else {
                lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        fridge_ui_label_set_text_fmt_if_changed(s_summary_chips[0], "%u 道菜", (unsigned)count);
        fridge_ui_label_set_text_if_changed(s_summary_chips[1], ok ? "AI 已生成" : "等待生成");
        fridge_ui_label_set_text_if_changed(s_summary_chips[2], generated_at > 0 ? "已缓存" : "未缓存");
        s_last_hash = next_hash;
    }
    update_status_label(generated_at);
}
