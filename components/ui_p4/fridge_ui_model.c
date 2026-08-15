// HOME-01 有界数据模型实现。
// JSON 是可替换示例内容，不参与页面结构和硬件调用；失败时完整回退到固件内置文本。

#include "fridge_ui_model.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "fridge_storage.h"

static void set_parse_error(char *error, size_t error_size, const char *text)
{
    if (error && error_size > 0) {
        strlcpy(error, text ? text : "", error_size);
    }
}

static bool copy_json_string(const cJSON *object, const char *key,
                             char *out, size_t out_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring ||
        strlen(item->valuestring) >= out_size) {
        return false;
    }
    strlcpy(out, item->valuestring, out_size);
    return true;
}

static bool parse_tone(const cJSON *object, fridge_ui_tone_t *out)
{
    char tone[12];
    if (!copy_json_string(object, "tone", tone, sizeof(tone))) {
        return false;
    }
    if (strcmp(tone, "normal") == 0) *out = FRIDGE_UI_TONE_NORMAL;
    else if (strcmp(tone, "ok") == 0) *out = FRIDGE_UI_TONE_OK;
    else if (strcmp(tone, "warn") == 0) *out = FRIDGE_UI_TONE_WARN;
    else if (strcmp(tone, "danger") == 0) *out = FRIDGE_UI_TONE_DANGER;
    else return false;
    return true;
}

static bool object_has_exact_keys(const cJSON *object, const char *const *keys,
                                  size_t key_count)
{
    if (!cJSON_IsObject(object)) return false;
    size_t actual_count = 0;
    bool matched[8] = {0};
    if (key_count > sizeof(matched) / sizeof(matched[0])) return false;
    for (const cJSON *item = object->child; item; item = item->next) {
        size_t match_index = key_count;
        for (size_t i = 0; i < key_count; i++) {
            if (item->string && strcmp(item->string, keys[i]) == 0) {
                match_index = i;
                break;
            }
        }
        if (match_index == key_count || matched[match_index]) return false;
        matched[match_index] = true;
        actual_count++;
    }
    return actual_count == key_count;
}

void fridge_ui_home_model_set_fallback(home_business_model_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->schema_version = FRIDGE_UI_HOME_SCHEMA_VERSION;
    out->stat_count = 4;
    out->stats[0] = (fridge_ui_home_stat_t){.value = 20, .tone = FRIDGE_UI_TONE_NORMAL};
    strlcpy(out->stats[0].key, "inventory", sizeof(out->stats[0].key));
    strlcpy(out->stats[0].label, "库存食材", sizeof(out->stats[0].label));
    strlcpy(out->stats[0].icon, "box", sizeof(out->stats[0].icon));
    out->stats[1] = (fridge_ui_home_stat_t){.value = 7, .tone = FRIDGE_UI_TONE_WARN};
    strlcpy(out->stats[1].key, "expiring", sizeof(out->stats[1].key));
    strlcpy(out->stats[1].label, "3天内临期", sizeof(out->stats[1].label));
    strlcpy(out->stats[1].icon, "clock", sizeof(out->stats[1].icon));
    out->stats[2] = (fridge_ui_home_stat_t){.value = 2, .tone = FRIDGE_UI_TONE_DANGER};
    strlcpy(out->stats[2].key, "expired", sizeof(out->stats[2].key));
    strlcpy(out->stats[2].label, "已过期", sizeof(out->stats[2].label));
    strlcpy(out->stats[2].icon, "warn", sizeof(out->stats[2].icon));
    out->stats[3] = (fridge_ui_home_stat_t){.value = 2, .tone = FRIDGE_UI_TONE_NORMAL};
    strlcpy(out->stats[3].key, "unlocated", sizeof(out->stats[3].key));
    strlcpy(out->stats[3].label, "待补充位置", sizeof(out->stats[3].label));
    strlcpy(out->stats[3].icon, "location", sizeof(out->stats[3].icon));

    out->expiring_count = 3;
    const char *names[] = {"原味酸奶", "卤牛肉", "菠菜"};
    const char *locations[] = {"右冷藏/第2层", "左冷藏/第2层", "果蔬抽屉/抽屉"};
    const char *quantities[] = {"4杯", "200g", "1把"};
    const char *badges[] = {"估算", "", ""};
    const char *expiry[] = {"今天到期", "今天到期", "明天到期"};
    for (size_t i = 0; i < out->expiring_count; i++) {
        strlcpy(out->expiring[i].name, names[i], sizeof(out->expiring[i].name));
        strlcpy(out->expiring[i].location, locations[i], sizeof(out->expiring[i].location));
        strlcpy(out->expiring[i].quantity, quantities[i], sizeof(out->expiring[i].quantity));
        strlcpy(out->expiring[i].badge, badges[i], sizeof(out->expiring[i].badge));
        strlcpy(out->expiring[i].expiry, expiry[i], sizeof(out->expiring[i].expiry));
        out->expiring[i].tone = i < 2 ? FRIDGE_UI_TONE_DANGER : FRIDGE_UI_TONE_WARN;
    }

    strlcpy(out->recommendation.title, "番茄炒蛋", sizeof(out->recommendation.title));
    strlcpy(out->recommendation.time, "15分钟", sizeof(out->recommendation.time));
    strlcpy(out->recommendation.difficulty, "简单", sizeof(out->recommendation.difficulty));
    out->recommendation.coverage = 90;
    strlcpy(out->recommendation.ready, "食材齐全", sizeof(out->recommendation.ready));
    strlcpy(out->recommendation.reason,
            "理由：番茄 3 天后到期、鸡蛋充足；15 分钟快手菜。",
            sizeof(out->recommendation.reason));
    strlcpy(out->recommendation.primary_action, "开始烹饪",
            sizeof(out->recommendation.primary_action));
    strlcpy(out->recommendation.secondary_action, "换一个",
            sizeof(out->recommendation.secondary_action));

    out->agent_tip_count = 2;
    strlcpy(out->agent_tips[0].text,
            "卤牛肉今天到期，晚餐推荐「卤牛肉拌面」，缺少面条和青菜，要加入购物清单吗？",
            sizeof(out->agent_tips[0].text));
    strlcpy(out->agent_tips[0].action, "加入购物清单",
            sizeof(out->agent_tips[0].action));
    strlcpy(out->agent_tips[1].text,
            "吐司面包已过期 3 天，需要我帮你从库存中移除吗？",
            sizeof(out->agent_tips[1].text));
    strlcpy(out->agent_tips[1].action, "去确认",
            sizeof(out->agent_tips[1].action));

    strlcpy(out->summaries.dinner, "晚：番茄炒蛋 + 菠菜鸡蛋汤",
            sizeof(out->summaries.dinner));
    strlcpy(out->summaries.shopping, "3 项待买 · 2 项建议",
            sizeof(out->summaries.shopping));
    strlcpy(out->summaries.next_title, "煮蛋", sizeof(out->summaries.next_title));
    strlcpy(out->summaries.next_remaining, "08:00", sizeof(out->summaries.next_remaining));
    strlcpy(out->summaries.next_source, "菜谱·番茄炒蛋",
            sizeof(out->summaries.next_source));
    strlcpy(out->last_sync,
            "14:31 库存变更已同步到云端 · 语音识别新增「内酯豆腐」待补充位置",
            sizeof(out->last_sync));
    out->from_tf = false;
}

void fridge_ui_more_model_set_fallback(more_business_model_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->schema_version = FRIDGE_UI_MORE_SCHEMA_VERSION;

    const char *ids[FRIDGE_UI_MORE_TOOL_COUNT] = {"timer", "alarm", "stopwatch"};
    const char *titles[FRIDGE_UI_MORE_TOOL_COUNT] = {"定时器", "闹钟", "秒表"};
    const char *descriptions[FRIDGE_UI_MORE_TOOL_COUNT] = {
        "厨房倒计时 · 快捷预设", "固定闹钟 · 每日提醒", "开始、暂停、复位",
    };
    const char *icons[FRIDGE_UI_MORE_TOOL_COUNT] = {"timer", "bell", "stopwatch"};
    for (size_t i = 0; i < FRIDGE_UI_MORE_TOOL_COUNT; i++) {
        strlcpy(out->cards[i].id, ids[i], sizeof(out->cards[i].id));
        strlcpy(out->cards[i].title, titles[i], sizeof(out->cards[i].title));
        strlcpy(out->cards[i].description, descriptions[i], sizeof(out->cards[i].description));
        strlcpy(out->cards[i].icon, icons[i], sizeof(out->cards[i].icon));
    }
    const uint16_t presets[FRIDGE_UI_MORE_TIMER_PRESETS_MAX] = {1, 3, 5, 10, 15, 30};
    memcpy(out->timer_presets_minutes, presets, sizeof(presets));
    strlcpy(out->timer_default_name, "厨房定时器", sizeof(out->timer_default_name));
    strlcpy(out->alarm_default_name, "闹钟", sizeof(out->alarm_default_name));
    out->from_tf = false;
}

static bool parse_stats(const cJSON *array, home_business_model_t *out)
{
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != FRIDGE_UI_HOME_STATS_MAX) {
        return false;
    }
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "value");
        fridge_ui_home_stat_t *stat = &out->stats[index++];
        if (!cJSON_IsObject(item) || !cJSON_IsNumber(value) || value->valuedouble < 0 ||
            value->valuedouble > 999 || value->valuedouble != value->valueint ||
            !copy_json_string(item, "key", stat->key, sizeof(stat->key)) ||
            !copy_json_string(item, "label", stat->label, sizeof(stat->label)) ||
            !copy_json_string(item, "icon", stat->icon, sizeof(stat->icon)) ||
            !parse_tone(item, &stat->tone)) {
            return false;
        }
        stat->value = value->valueint;
    }
    out->stat_count = index;
    return true;
}

static bool parse_expiring(const cJSON *array, home_business_model_t *out)
{
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : -1;
    if (count < 0 || count > FRIDGE_UI_HOME_EXPIRING_MAX) return false;
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        fridge_ui_home_expiring_t *entry = &out->expiring[index++];
        if (!cJSON_IsObject(item) ||
            !copy_json_string(item, "name", entry->name, sizeof(entry->name)) ||
            !copy_json_string(item, "location", entry->location, sizeof(entry->location)) ||
            !copy_json_string(item, "quantity", entry->quantity, sizeof(entry->quantity)) ||
            !copy_json_string(item, "badge", entry->badge, sizeof(entry->badge)) ||
            !copy_json_string(item, "expiry", entry->expiry, sizeof(entry->expiry)) ||
            !parse_tone(item, &entry->tone)) {
            return false;
        }
    }
    out->expiring_count = index;
    return true;
}

static bool parse_recommendation(const cJSON *object, home_business_model_t *out)
{
    fridge_ui_home_recommendation_t *rec = &out->recommendation;
    cJSON *coverage = cJSON_GetObjectItemCaseSensitive(object, "coverage");
    cJSON *ready = cJSON_GetObjectItemCaseSensitive(object, "ready");
    char reason[128];
    bool valid = cJSON_IsObject(object) && cJSON_IsNumber(coverage) &&
                 coverage->valueint >= 0 && coverage->valueint <= 100 &&
                 coverage->valuedouble == coverage->valueint && cJSON_IsBool(ready) &&
                 copy_json_string(object, "title", rec->title, sizeof(rec->title)) &&
                 copy_json_string(object, "time", rec->time, sizeof(rec->time)) &&
                 copy_json_string(object, "difficulty", rec->difficulty,
                                  sizeof(rec->difficulty)) &&
                 copy_json_string(object, "reason", reason, sizeof(reason)) &&
                 copy_json_string(object, "primary_action", rec->primary_action,
                                  sizeof(rec->primary_action)) &&
                 copy_json_string(object, "secondary_action", rec->secondary_action,
                                  sizeof(rec->secondary_action));
    if (!valid) return false;
    rec->coverage = coverage->valueint;
    strlcpy(rec->ready, cJSON_IsTrue(ready) ? "食材齐全" : "尚缺食材", sizeof(rec->ready));
    snprintf(rec->reason, sizeof(rec->reason), "理由：%s", reason);
    return true;
}

static bool parse_tips(const cJSON *array, home_business_model_t *out)
{
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : -1;
    if (count < 0 || count > FRIDGE_UI_HOME_TIPS_MAX) return false;
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        fridge_ui_home_tip_t *tip = &out->agent_tips[index++];
        if (!cJSON_IsObject(item) ||
            !copy_json_string(item, "text", tip->text, sizeof(tip->text)) ||
            !copy_json_string(item, "action", tip->action, sizeof(tip->action))) {
            return false;
        }
    }
    out->agent_tip_count = index;
    return true;
}

static bool parse_summaries(const cJSON *object, home_business_model_t *out)
{
    fridge_ui_home_summaries_t *summary = &out->summaries;
    return cJSON_IsObject(object) &&
           copy_json_string(object, "dinner", summary->dinner, sizeof(summary->dinner)) &&
           copy_json_string(object, "shopping", summary->shopping, sizeof(summary->shopping)) &&
           copy_json_string(object, "next_title", summary->next_title,
                            sizeof(summary->next_title)) &&
           copy_json_string(object, "next_remaining", summary->next_remaining,
                            sizeof(summary->next_remaining)) &&
           copy_json_string(object, "next_source", summary->next_source,
                            sizeof(summary->next_source));
}

esp_err_t fridge_ui_home_model_parse_json(const char *json, size_t json_len,
                                          home_business_model_t *out,
                                          char *error, size_t error_size)
{
    if (!json || !out || json_len == 0 || json_len > FRIDGE_UI_HOME_JSON_MAX) {
        set_parse_error(error, error_size, "home.json 大小越界");
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_parse_error(error, error_size, "home.json 不是有效 JSON 对象");
        return ESP_ERR_INVALID_ARG;
    }

    home_business_model_t parsed = {0};
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *stats = cJSON_GetObjectItemCaseSensitive(root, "stats");
    cJSON *expiring = cJSON_GetObjectItemCaseSensitive(root, "expiring");
    cJSON *recommendation = cJSON_GetObjectItemCaseSensitive(root, "recommendation");
    cJSON *tips = cJSON_GetObjectItemCaseSensitive(root, "agent_tips");
    cJSON *summaries = cJSON_GetObjectItemCaseSensitive(root, "summaries");
    bool ok = cJSON_IsNumber(schema) && schema->valueint == FRIDGE_UI_HOME_SCHEMA_VERSION &&
              schema->valuedouble == schema->valueint &&
              parse_stats(stats, &parsed) && parse_expiring(expiring, &parsed) &&
              parse_recommendation(recommendation, &parsed) &&
              parse_tips(tips, &parsed) && parse_summaries(summaries, &parsed) &&
              copy_json_string(root, "last_sync", parsed.last_sync, sizeof(parsed.last_sync));
    cJSON_Delete(root);
    if (!ok) {
        set_parse_error(error, error_size, "home.json 字段、数组数量或字符串长度不符合 v1");
        return ESP_ERR_INVALID_ARG;
    }
    parsed.schema_version = FRIDGE_UI_HOME_SCHEMA_VERSION;
    parsed.from_tf = true;
    *out = parsed;
    set_parse_error(error, error_size, "");
    return ESP_OK;
}

static int more_tool_index_from_id(const char *id)
{
    if (!id) return -1;
    if (strcmp(id, "timer") == 0) return FRIDGE_UI_MORE_TOOL_TIMER;
    if (strcmp(id, "alarm") == 0) return FRIDGE_UI_MORE_TOOL_ALARM;
    if (strcmp(id, "stopwatch") == 0) return FRIDGE_UI_MORE_TOOL_STOPWATCH;
    return -1;
}

static bool parse_more_tools(const cJSON *array, more_business_model_t *out)
{
    static const char *const tool_keys[] = {"id", "title", "description", "icon"};
    static const char *const expected_icons[] = {"timer", "bell", "stopwatch"};
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != FRIDGE_UI_MORE_TOOL_COUNT) {
        return false;
    }

    bool found[FRIDGE_UI_MORE_TOOL_COUNT] = {0};
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        char id[16];
        if (!object_has_exact_keys(item, tool_keys, sizeof(tool_keys) / sizeof(tool_keys[0])) ||
            !copy_json_string(item, "id", id, sizeof(id))) {
            return false;
        }
        int index = more_tool_index_from_id(id);
        if (index < 0 || found[index]) return false;

        fridge_ui_more_tool_t *tool = &out->cards[index];
        if (!copy_json_string(item, "title", tool->title, sizeof(tool->title)) ||
            !copy_json_string(item, "description", tool->description, sizeof(tool->description)) ||
            !copy_json_string(item, "icon", tool->icon, sizeof(tool->icon)) ||
            strcmp(tool->icon, expected_icons[index]) != 0) {
            return false;
        }
        strlcpy(tool->id, id, sizeof(tool->id));
        found[index] = true;
    }
    for (size_t i = 0; i < FRIDGE_UI_MORE_TOOL_COUNT; i++) {
        if (!found[i]) return false;
    }
    return true;
}

static bool parse_more_timer(const cJSON *object, more_business_model_t *out)
{
    static const char *const timer_keys[] = {"default_name", "presets_minutes"};
    const cJSON *presets = cJSON_GetObjectItemCaseSensitive(object, "presets_minutes");
    if (!object_has_exact_keys(object, timer_keys, sizeof(timer_keys) / sizeof(timer_keys[0])) ||
        !copy_json_string(object, "default_name", out->timer_default_name,
                          sizeof(out->timer_default_name)) ||
        !cJSON_IsArray(presets) ||
        cJSON_GetArraySize(presets) != FRIDGE_UI_MORE_TIMER_PRESETS_MAX) {
        return false;
    }

    int previous = 0;
    for (size_t i = 0; i < FRIDGE_UI_MORE_TIMER_PRESETS_MAX; i++) {
        const cJSON *minutes = cJSON_GetArrayItem(presets, (int)i);
        if (!cJSON_IsNumber(minutes) || minutes->valuedouble != minutes->valueint ||
            minutes->valueint < 1 || minutes->valueint > 180 ||
            minutes->valueint <= previous) {
            return false;
        }
        out->timer_presets_minutes[i] = (uint16_t)minutes->valueint;
        previous = minutes->valueint;
    }
    return true;
}

static bool parse_more_alarm(const cJSON *object, more_business_model_t *out)
{
    static const char *const alarm_keys[] = {"default_name"};
    return object_has_exact_keys(object, alarm_keys, sizeof(alarm_keys) / sizeof(alarm_keys[0])) &&
           copy_json_string(object, "default_name", out->alarm_default_name,
                            sizeof(out->alarm_default_name));
}

esp_err_t fridge_ui_more_model_parse_json(const char *json, size_t json_len,
                                          more_business_model_t *out,
                                          char *error, size_t error_size)
{
    static const char *const root_keys[] = {"schema_version", "tools", "timer", "alarm"};
    if (!json || !out || json_len == 0 || json_len > FRIDGE_UI_MORE_JSON_MAX) {
        set_parse_error(error, error_size, "more.json 大小越界");
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_parse_error(error, error_size, "more.json 不是有效 JSON 对象");
        return ESP_ERR_INVALID_ARG;
    }

    more_business_model_t parsed = {0};
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
    const cJSON *timer = cJSON_GetObjectItemCaseSensitive(root, "timer");
    const cJSON *alarm = cJSON_GetObjectItemCaseSensitive(root, "alarm");
    bool ok = object_has_exact_keys(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0])) &&
              cJSON_IsNumber(schema) && schema->valueint == FRIDGE_UI_MORE_SCHEMA_VERSION &&
              schema->valuedouble == schema->valueint && parse_more_tools(tools, &parsed) &&
              parse_more_timer(timer, &parsed) && parse_more_alarm(alarm, &parsed);
    cJSON_Delete(root);
    if (!ok) {
        set_parse_error(error, error_size, "more.json 字段、工具或预设不符合 v1");
        return ESP_ERR_INVALID_ARG;
    }
    parsed.schema_version = FRIDGE_UI_MORE_SCHEMA_VERSION;
    parsed.from_tf = true;
    *out = parsed;
    set_parse_error(error, error_size, "");
    return ESP_OK;
}

void fridge_ui_plan_model_set_fallback(plan_business_model_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->schema_version = FRIDGE_UI_PLAN_SCHEMA_VERSION;
    strlcpy(out->week_range, "7月21日 – 7月27日", sizeof(out->week_range));

    const char *slots[FRIDGE_UI_PLAN_MEAL_SLOTS] = {"早餐", "午餐", "晚餐", "加餐"};
    const char *dishes[FRIDGE_UI_PLAN_MEAL_SLOTS] = {
        "苹果酸奶杯", "虾仁滑蛋", "番茄炒蛋 + 菠菜鸡蛋汤", "酸奶",
    };
    for (size_t i = 0; i < FRIDGE_UI_PLAN_MEAL_SLOTS; i++) {
        strlcpy(out->today_meals[i].slot, slots[i], sizeof(out->today_meals[i].slot));
        strlcpy(out->today_meals[i].dish, dishes[i], sizeof(out->today_meals[i].dish));
    }

    out->expiring_count = 3;
    const char *ex_names[3] = {"原味酸奶", "卤牛肉", "菠菜"};
    const char *ex_badges[3] = {"今天到期", "今天到期", "明天到期"};
    const fridge_ui_tone_t ex_tones[3] = {
        FRIDGE_UI_TONE_DANGER, FRIDGE_UI_TONE_DANGER, FRIDGE_UI_TONE_WARN,
    };
    for (size_t i = 0; i < out->expiring_count; i++) {
        strlcpy(out->expiring[i].name, ex_names[i], sizeof(out->expiring[i].name));
        strlcpy(out->expiring[i].badge, ex_badges[i], sizeof(out->expiring[i].badge));
        out->expiring[i].tone = ex_tones[i];
    }

    out->shopping_count = 2;
    const char *sh_names[2] = {"面条", "黑胡椒"};
    for (size_t i = 0; i < out->shopping_count; i++) {
        strlcpy(out->shopping[i].name, sh_names[i], sizeof(out->shopping[i].name));
        strlcpy(out->shopping[i].group, "待购买", sizeof(out->shopping[i].group));
    }

    const char *wk_dates[FRIDGE_UI_PLAN_WEEK_DAYS] = {
        "周一 21", "周二 22", "周三 23", "周四 24", "周五 25", "周六 26", "周日 27",
    };
    const char *wk_sum[FRIDGE_UI_PLAN_WEEK_DAYS] = {
        "红烧肉 / 清炒时蔬", "番茄炒蛋 / 菠菜汤", "冲突待处理", "咖喱鸡 / 米饭",
        "冲突待处理", "火锅", "饺子 / 小米粥",
    };
    for (size_t i = 0; i < FRIDGE_UI_PLAN_WEEK_DAYS; i++) {
        strlcpy(out->week[i].date, wk_dates[i], sizeof(out->week[i].date));
        strlcpy(out->week[i].summary, wk_sum[i], sizeof(out->week[i].summary));
        out->week[i].today = (i == 1);
        out->week[i].conflict = (i == 2 || i == 4);
    }

    strlcpy(out->agent_tip.text,
            "卤牛肉今天到期，晚餐推荐「卤牛肉拌面」，缺少面条和青菜，要加入购物清单吗？",
            sizeof(out->agent_tip.text));
    strlcpy(out->agent_tip.action, "去确认", sizeof(out->agent_tip.action));

    strlcpy(out->ctx_dinner, "番茄炒蛋+菠菜汤", sizeof(out->ctx_dinner));
    strlcpy(out->ctx_shopping_count, "2 项", sizeof(out->ctx_shopping_count));
    strlcpy(out->ctx_expiring_count, "2 项", sizeof(out->ctx_expiring_count));
    strlcpy(out->ctx_cooking_recipe, "番茄炒蛋", sizeof(out->ctx_cooking_recipe));
    strlcpy(out->ctx_cooking_step, "下番茄块，中火炒出汁", sizeof(out->ctx_cooking_step));
    out->ctx_cooking_step_cur = 3;
    out->ctx_cooking_step_total = 6;
    out->from_tf = false;
}

static bool parse_plan_meals(const cJSON *array, plan_business_model_t *out)
{
    if (!cJSON_IsArray(array) ||
        cJSON_GetArraySize(array) != FRIDGE_UI_PLAN_MEAL_SLOTS) {
        return false;
    }
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        fridge_ui_plan_meal_t *meal = &out->today_meals[index++];
        if (!cJSON_IsObject(item) ||
            !copy_json_string(item, "slot", meal->slot, sizeof(meal->slot)) ||
            !copy_json_string(item, "dish", meal->dish, sizeof(meal->dish))) {
            return false;
        }
    }
    return true;
}

static bool parse_plan_expiring(const cJSON *array, plan_business_model_t *out)
{
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : -1;
    if (count < 0 || count > FRIDGE_UI_PLAN_EXPIRING_MAX) return false;
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        fridge_ui_plan_expiring_t *entry = &out->expiring[index++];
        if (!cJSON_IsObject(item) ||
            !copy_json_string(item, "name", entry->name, sizeof(entry->name)) ||
            !copy_json_string(item, "badge", entry->badge, sizeof(entry->badge)) ||
            !parse_tone(item, &entry->tone)) {
            return false;
        }
    }
    out->expiring_count = index;
    return true;
}

static bool parse_plan_shopping(const cJSON *array, plan_business_model_t *out)
{
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : -1;
    if (count < 0 || count > FRIDGE_UI_PLAN_SHOPPING_MAX) return false;
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        fridge_ui_plan_shopping_t *entry = &out->shopping[index++];
        if (!cJSON_IsObject(item) ||
            !copy_json_string(item, "name", entry->name, sizeof(entry->name)) ||
            !copy_json_string(item, "group", entry->group, sizeof(entry->group))) {
            return false;
        }
    }
    out->shopping_count = index;
    return true;
}

static bool parse_plan_week(const cJSON *array, plan_business_model_t *out)
{
    if (!cJSON_IsArray(array) ||
        cJSON_GetArraySize(array) != FRIDGE_UI_PLAN_WEEK_DAYS) {
        return false;
    }
    cJSON *item = NULL;
    size_t index = 0;
    cJSON_ArrayForEach(item, array) {
        fridge_ui_plan_week_day_t *day = &out->week[index++];
        const cJSON *today = cJSON_GetObjectItemCaseSensitive(item, "today");
        const cJSON *conflict = cJSON_GetObjectItemCaseSensitive(item, "conflict");
        if (!cJSON_IsObject(item) || !cJSON_IsBool(today) || !cJSON_IsBool(conflict) ||
            !copy_json_string(item, "date", day->date, sizeof(day->date)) ||
            !copy_json_string(item, "summary", day->summary, sizeof(day->summary))) {
            return false;
        }
        day->today = cJSON_IsTrue(today);
        day->conflict = cJSON_IsTrue(conflict);
    }
    return true;
}

static bool parse_plan_tip(const cJSON *object, plan_business_model_t *out)
{
    return cJSON_IsObject(object) &&
           copy_json_string(object, "text", out->agent_tip.text,
                            sizeof(out->agent_tip.text)) &&
           copy_json_string(object, "action", out->agent_tip.action,
                            sizeof(out->agent_tip.action));
}

static bool parse_plan_ctx(const cJSON *object, plan_business_model_t *out)
{
    const cJSON *step_cur = cJSON_GetObjectItemCaseSensitive(object, "cooking_step_cur");
    const cJSON *step_total = cJSON_GetObjectItemCaseSensitive(object, "cooking_step_total");
    if (!cJSON_IsObject(object) ||
        !cJSON_IsNumber(step_cur) || step_cur->valuedouble != step_cur->valueint ||
        step_cur->valueint < 0 || step_cur->valueint > 99 ||
        !cJSON_IsNumber(step_total) || step_total->valuedouble != step_total->valueint ||
        step_total->valueint < 0 || step_total->valueint > 99 ||
        !copy_json_string(object, "dinner", out->ctx_dinner, sizeof(out->ctx_dinner)) ||
        !copy_json_string(object, "shopping_count", out->ctx_shopping_count,
                          sizeof(out->ctx_shopping_count)) ||
        !copy_json_string(object, "expiring_count", out->ctx_expiring_count,
                          sizeof(out->ctx_expiring_count)) ||
        !copy_json_string(object, "cooking_recipe", out->ctx_cooking_recipe,
                          sizeof(out->ctx_cooking_recipe)) ||
        !copy_json_string(object, "cooking_step", out->ctx_cooking_step,
                          sizeof(out->ctx_cooking_step))) {
        return false;
    }
    out->ctx_cooking_step_cur = step_cur->valueint;
    out->ctx_cooking_step_total = step_total->valueint;
    return true;
}

esp_err_t fridge_ui_plan_model_parse_json(const char *json, size_t json_len,
                                          plan_business_model_t *out,
                                          char *error, size_t error_size)
{
    if (!json || !out || json_len == 0 || json_len > FRIDGE_UI_PLAN_JSON_MAX) {
        set_parse_error(error, error_size, "plan.json 大小越界");
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_parse_error(error, error_size, "plan.json 不是有效 JSON 对象");
        return ESP_ERR_INVALID_ARG;
    }

    plan_business_model_t parsed = {0};
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *meals = cJSON_GetObjectItemCaseSensitive(root, "today_meals");
    const cJSON *expiring = cJSON_GetObjectItemCaseSensitive(root, "expiring");
    const cJSON *shopping = cJSON_GetObjectItemCaseSensitive(root, "shopping");
    const cJSON *week = cJSON_GetObjectItemCaseSensitive(root, "week");
    const cJSON *tip = cJSON_GetObjectItemCaseSensitive(root, "agent_tip");
    const cJSON *ctx = cJSON_GetObjectItemCaseSensitive(root, "ctx");
    bool ok = cJSON_IsNumber(schema) &&
              schema->valueint == FRIDGE_UI_PLAN_SCHEMA_VERSION &&
              schema->valuedouble == schema->valueint &&
              copy_json_string(root, "week_range", parsed.week_range,
                               sizeof(parsed.week_range)) &&
              parse_plan_meals(meals, &parsed) &&
              parse_plan_expiring(expiring, &parsed) &&
              parse_plan_shopping(shopping, &parsed) &&
              parse_plan_week(week, &parsed) &&
              parse_plan_tip(tip, &parsed) &&
              parse_plan_ctx(ctx, &parsed);
    cJSON_Delete(root);
    if (!ok) {
        set_parse_error(error, error_size, "plan.json 字段、数组数量或字符串长度不符合 v1");
        return ESP_ERR_INVALID_ARG;
    }
    parsed.schema_version = FRIDGE_UI_PLAN_SCHEMA_VERSION;
    parsed.from_tf = true;
    *out = parsed;
    set_parse_error(error, error_size, "");
    return ESP_OK;
}

// ============ PLAN-06 购物清单模型实现 ============

static bool shopping_copy_optional_string(const cJSON *object, const char *key,
                                          char *out, size_t out_size,
                                          const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) {
        strlcpy(out, fallback, out_size);
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= out_size) {
        return false;
    }
    strlcpy(out, item->valuestring, out_size);
    return true;
}

static bool shopping_source_is_ai(const char *source)
{
    return source && (strcmp(source, "device_ai") == 0 || strcmp(source, "AI建议") == 0 ||
                      strcmp(source, "Agent建议") == 0 || strcmp(source, "小精灵建议") == 0);
}

static bool shopping_parse_json(const char *json, shopping_business_model_t *out)
{
    if (!json || !out) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    shopping_business_model_t parsed = {0};
    bool ok = cJSON_IsNumber(schema) && schema->valueint == FRIDGE_UI_SHOPPING_SCHEMA_VERSION &&
              schema->valuedouble == schema->valueint && cJSON_IsArray(items) &&
              cJSON_GetArraySize(items) <= FRIDGE_UI_SHOPPING_ITEMS_MAX &&
              shopping_copy_optional_string(root, "source", parsed.source,
                                            sizeof(parsed.source), "local_seed");
    if (ok) {
        size_t index = 0;
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, items) {
            fridge_ui_shopping_item_t *item = &parsed.items[index];
            char generated_id[24];
            snprintf(generated_id, sizeof(generated_id), "item-%u", (unsigned)(index + 1U));
            ok = cJSON_IsObject(entry) &&
                 shopping_copy_optional_string(entry, "id", item->id, sizeof(item->id), generated_id) &&
                 copy_json_string(entry, "name", item->name, sizeof(item->name)) &&
                 shopping_copy_optional_string(entry, "quantity", item->quantity, sizeof(item->quantity), "1份") &&
                 shopping_copy_optional_string(entry, "reason", item->reason, sizeof(item->reason), "") &&
                 shopping_copy_optional_string(entry, "priority", item->priority, sizeof(item->priority), "normal") &&
                 shopping_copy_optional_string(entry, "source", item->source, sizeof(item->source), parsed.source) &&
                 shopping_copy_optional_string(entry, "relation", item->relation, sizeof(item->relation), "");
            const cJSON *checked = cJSON_GetObjectItemCaseSensitive(entry, "checked");
            if (!ok || (checked && !cJSON_IsBool(checked))) break;
            item->checked = checked ? cJSON_IsTrue(checked) : false;
            const char *group_fallback = item->checked ? "已购买" :
                (shopping_source_is_ai(item->source) ? "建议购买" : "待购买");
            if (!shopping_copy_optional_string(entry, "group", item->group,
                                                sizeof(item->group), group_fallback)) break;
            // 已购买分组才是完成状态；checked 兼容旧 JSON，不再承载页面勾选态。
            item->checked = strcmp(item->group, "已购买") == 0;
            index++;
        }
        parsed.item_count = ok ? index : 0;
    }
    cJSON_Delete(root);
    if (!ok) return false;
    parsed.schema_version = FRIDGE_UI_SHOPPING_SCHEMA_VERSION;
    parsed.from_local = true;
    *out = parsed;
    return true;
}

static char *shopping_serialize_json(const shopping_business_model_t *model)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    if (!root || !items) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "schema_version", FRIDGE_UI_SHOPPING_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "source", model->source[0] ? model->source : "local_seed");
    for (size_t i = 0; i < model->item_count; i++) {
        const fridge_ui_shopping_item_t *src = &model->items[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) { cJSON_Delete(root); return NULL; }
        cJSON_AddStringToObject(item, "id", src->id);
        cJSON_AddStringToObject(item, "name", src->name);
        cJSON_AddStringToObject(item, "quantity", src->quantity);
        cJSON_AddStringToObject(item, "reason", src->reason);
        cJSON_AddStringToObject(item, "priority", src->priority);
        cJSON_AddStringToObject(item, "group", src->group);
        cJSON_AddStringToObject(item, "source", src->source);
        cJSON_AddStringToObject(item, "relation", src->relation);
        cJSON_AddBoolToObject(item, "checked", src->checked);
        cJSON_AddItemToArray(items, item);
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return printed;
}

void fridge_ui_shopping_model_set_fallback(shopping_business_model_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->schema_version = FRIDGE_UI_SHOPPING_SCHEMA_VERSION;
    strlcpy(out->source, "local_seed", sizeof(out->source));
    static const char *const names[] = {"面条", "黑胡椒", "吐司面包", "鲜牛奶", "鸡蛋", "青菜"};
    static const char *const quantities[] = {"1把", "1瓶", "1袋", "2盒", "10个", "1把"};
    static const char *const groups[] = {"待购买", "待购买", "待购买", "建议购买", "建议购买", "已购买"};
    static const char *const sources[] = {"菜谱缺失", "菜谱缺失", "菜单计划", "低库存", "小精灵建议", "手动添加"};
    static const char *const relations[] = {"周三菜单", "周五菜单", "本周菜单", "库存不足", "小精灵建议", ""};
    out->item_count = sizeof(names) / sizeof(names[0]);
    for (size_t i = 0; i < out->item_count; i++) {
        fridge_ui_shopping_item_t *item = &out->items[i];
        snprintf(item->id, sizeof(item->id), "sp%u", (unsigned)(i + 1U));
        strlcpy(item->name, names[i], sizeof(item->name));
        strlcpy(item->quantity, quantities[i], sizeof(item->quantity));
        strlcpy(item->priority, i < 2 ? "high" : "normal", sizeof(item->priority));
        strlcpy(item->group, groups[i], sizeof(item->group));
        strlcpy(item->source, sources[i], sizeof(item->source));
        strlcpy(item->relation, relations[i], sizeof(item->relation));
        item->checked = i == out->item_count - 1U;
    }
}

esp_err_t fridge_ui_shopping_model_load(shopping_business_model_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    char json[FRIDGE_UI_SHOPPING_JSON_MAX] = {0};
    esp_err_t read_err = fridge_storage_get_shopping_list(json, sizeof(json));
    if (read_err != ESP_OK) return read_err;
    shopping_business_model_t loaded = {0};
    if (!shopping_parse_json(json, &loaded)) return ESP_ERR_INVALID_RESPONSE;
    if (loaded.item_count == 0 && strcmp(loaded.source, "local_seed") == 0) {
        fridge_ui_shopping_model_set_fallback(&loaded);
        char *seed = shopping_serialize_json(&loaded);
        if (!seed) return ESP_ERR_NO_MEM;
        esp_err_t err = fridge_storage_set_shopping_list(seed);
        cJSON_free(seed);
        if (err != ESP_OK) return err;
    }
    *out = loaded;
    return ESP_OK;
}

esp_err_t fridge_ui_shopping_model_mutate(shopping_business_model_t *out,
                                          fridge_ui_shopping_mutation_t mutation,
                                          const char *item_id, const char *name)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    (void)item_id;
    shopping_business_model_t next = *out;
    bool changed = false;
    if (mutation == FRIDGE_UI_SHOPPING_MUTATE_ADD_MANUAL) {
        if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
        if (next.item_count >= FRIDGE_UI_SHOPPING_ITEMS_MAX) return ESP_ERR_NO_MEM;
        fridge_ui_shopping_item_t *item = &next.items[next.item_count++];
        memset(item, 0, sizeof(*item));
        snprintf(item->id, sizeof(item->id), "manual-%u", (unsigned)next.item_count);
        strlcpy(item->name, name, sizeof(item->name));
        strlcpy(item->quantity, "1份", sizeof(item->quantity));
        strlcpy(item->priority, "normal", sizeof(item->priority));
        strlcpy(item->group, "待购买", sizeof(item->group));
        strlcpy(item->source, "手动添加", sizeof(item->source));
        changed = true;
    } else if (mutation == FRIDGE_UI_SHOPPING_MUTATE_CLEAR_DONE) {
        size_t write = 0;
        for (size_t i = 0; i < next.item_count; i++) {
            if (next.items[i].checked || strcmp(next.items[i].group, "已购买") == 0) {
                changed = true;
                continue;
            }
            if (write != i) next.items[write] = next.items[i];
            write++;
        }
        next.item_count = write;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (!changed) return ESP_ERR_NOT_FOUND;
    char *json = shopping_serialize_json(&next);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = fridge_storage_set_shopping_list(json);
    cJSON_free(json);
    if (err != ESP_OK) return err;
    *out = next;
    return ESP_OK;
}

esp_err_t fridge_ui_shopping_model_move_selected(shopping_business_model_t *out,
                                                 const char item_ids[][24],
                                                 size_t item_id_count,
                                                 bool purchased)
{
    if (!out || !item_ids || item_id_count == 0 ||
        item_id_count > FRIDGE_UI_SHOPPING_ITEMS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    shopping_business_model_t next = *out;
    bool changed = false;
    for (size_t i = 0; i < next.item_count; i++) {
        fridge_ui_shopping_item_t *item = &next.items[i];
        bool selected = false;
        for (size_t j = 0; j < item_id_count; j++) {
            if (item_ids[j][0] && strcmp(item->id, item_ids[j]) == 0) {
                selected = true;
                break;
            }
        }
        if (!selected) continue;
        const char *target_group = purchased ? "已购买" : "待购买";
        if (strcmp(item->group, target_group) != 0 || item->checked != purchased) {
            strlcpy(item->group, target_group, sizeof(item->group));
            item->checked = purchased;
            changed = true;
        }
    }
    if (!changed) return ESP_ERR_NOT_FOUND;
    char *json = shopping_serialize_json(&next);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = fridge_storage_set_shopping_list(json);
    cJSON_free(json);
    if (err != ESP_OK) return err;
    *out = next;
    return ESP_OK;
}

// ============ CHAT 对话页数据模型实现 ============

void fridge_ui_chat_model_init_foreground(chat_business_model_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->schema_version = FRIDGE_UI_CHAT_SCHEMA_VERSION;
    out->from_tf = false;
    strlcpy(out->permission_mode, "标准模式", sizeof(out->permission_mode));
    out->online = false;
    strlcpy(out->agent_state, "等待小精灵就绪", sizeof(out->agent_state));
    strlcpy(out->tts_state, "播报待命", sizeof(out->tts_state));
}

void fridge_ui_chat_model_reset_foreground_session(chat_business_model_t *out)
{
    if (!out) return;

    // Agent Runtime 会在 SESSION_RESET 后逐条回放 TF 会话记录。这里仅清空旧消息和
    // 运行态，不能填入参考页演示内容，也不能覆盖网络/权限这类页面级状态。
    memset(out->messages, 0, sizeof(out->messages));
    out->message_count = 0;
    out->agent_working = false;
    out->from_tf = false;
    out->schema_version = FRIDGE_UI_CHAT_SCHEMA_VERSION;
    strlcpy(out->tts_state, "播报待命", sizeof(out->tts_state));
}

void fridge_ui_chat_model_set_fallback(chat_business_model_t *out)
{
    // 旧入口只保留给配置加载兼容路径；CHAT-01 始终从唯一的空 foreground 开始。
    fridge_ui_chat_model_init_foreground(out);
}

static bool parse_chat_message_type(const char *type_str, fridge_ui_chat_message_type_t *out)
{
    if (strcmp(type_str, "user") == 0) {
        *out = FRIDGE_UI_CHAT_MSG_USER;
        return true;
    } else if (strcmp(type_str, "agent") == 0) {
        *out = FRIDGE_UI_CHAT_MSG_AGENT;
        return true;
    } else if (strcmp(type_str, "tool") == 0) {
        *out = FRIDGE_UI_CHAT_MSG_TOOL;
        return true;
    } else if (strcmp(type_str, "result") == 0) {
        *out = FRIDGE_UI_CHAT_MSG_RESULT;
        return true;
    }
    return false;
}

static bool parse_chat_risk(const char *risk_str, fridge_ui_chat_risk_t *out)
{
    if (strcmp(risk_str, "低") == 0 || strcmp(risk_str, "low") == 0) {
        *out = FRIDGE_UI_CHAT_RISK_LOW;
        return true;
    } else if (strcmp(risk_str, "中") == 0 || strcmp(risk_str, "medium") == 0) {
        *out = FRIDGE_UI_CHAT_RISK_MEDIUM;
        return true;
    } else if (strcmp(risk_str, "高") == 0 || strcmp(risk_str, "high") == 0) {
        *out = FRIDGE_UI_CHAT_RISK_HIGH;
        return true;
    }
    return false;
}

static bool parse_chat_tool_status(const char *status_str, fridge_ui_chat_tool_status_t *out)
{
    if (strcmp(status_str, "pending") == 0) {
        *out = FRIDGE_UI_CHAT_TOOL_STATUS_PENDING;
        return true;
    } else if (strcmp(status_str, "running") == 0) {
        *out = FRIDGE_UI_CHAT_TOOL_STATUS_RUNNING;
        return true;
    } else if (strcmp(status_str, "ok") == 0) {
        *out = FRIDGE_UI_CHAT_TOOL_STATUS_OK;
        return true;
    } else if (strcmp(status_str, "error") == 0) {
        *out = FRIDGE_UI_CHAT_TOOL_STATUS_ERROR;
        return true;
    }
    return false;
}

esp_err_t fridge_ui_chat_model_parse_json(const char *json, size_t json_len,
                                          chat_business_model_t *out,
                                          char *error, size_t error_size)
{
    if (!json || json_len == 0 || json_len > FRIDGE_UI_CHAT_JSON_MAX || !out) {
        set_parse_error(error, error_size, "invalid input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        set_parse_error(error, error_size, "JSON parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    chat_business_model_t parsed;
    memset(&parsed, 0, sizeof(parsed));

    // 解析 schema_version
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(version) || version->valueint != FRIDGE_UI_CHAT_SCHEMA_VERSION) {
        cJSON_Delete(root);
        set_parse_error(error, error_size, "invalid schema_version");
        return ESP_ERR_INVALID_ARG;
    }

    // 解析权限模式
    if (!copy_json_string(root, "permission_mode", parsed.permission_mode,
                         sizeof(parsed.permission_mode))) {
        strlcpy(parsed.permission_mode, "标准模式", sizeof(parsed.permission_mode));
    }

    // 解析在线状态
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(root, "online");
    parsed.online = cJSON_IsTrue(online);

    // 解析消息列表
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (cJSON_IsArray(messages)) {
        size_t msg_count = cJSON_GetArraySize(messages);
        if (msg_count > FRIDGE_UI_CHAT_MESSAGES_MAX) {
            msg_count = FRIDGE_UI_CHAT_MESSAGES_MAX;
        }

        for (size_t i = 0; i < msg_count; i++) {
            const cJSON *msg = cJSON_GetArrayItem(messages, i);
            if (!cJSON_IsObject(msg)) continue;

            fridge_ui_chat_message_t *m = &parsed.messages[parsed.message_count];

            // 解析消息类型
            char type_str[16];
            if (!copy_json_string(msg, "type", type_str, sizeof(type_str)) ||
                !parse_chat_message_type(type_str, &m->type)) {
                continue;
            }

            // 通用字段
            copy_json_string(msg, "text", m->text, sizeof(m->text));
            copy_json_string(msg, "time", m->time, sizeof(m->time));

            // tool专用字段
            if (m->type == FRIDGE_UI_CHAT_MSG_TOOL) {
                copy_json_string(msg, "name", m->tool_name, sizeof(m->tool_name));
                copy_json_string(msg, "label", m->tool_label, sizeof(m->tool_label));
                copy_json_string(msg, "detail", m->tool_detail, sizeof(m->tool_detail));
                copy_json_string(msg, "latency", m->tool_latency, sizeof(m->tool_latency));

                char risk_str[16], status_str[16];
                if (copy_json_string(msg, "risk", risk_str, sizeof(risk_str))) {
                    parse_chat_risk(risk_str, &m->risk);
                }
                if (copy_json_string(msg, "status", status_str, sizeof(status_str))) {
                    parse_chat_tool_status(status_str, &m->tool_status);
                }
            }

            parsed.message_count++;
        }
    }

    cJSON_Delete(root);
    parsed.schema_version = FRIDGE_UI_CHAT_SCHEMA_VERSION;
    parsed.agent_working = false;
    strlcpy(parsed.agent_state, "已从 TF 加载", sizeof(parsed.agent_state));
    strlcpy(parsed.tts_state, "播报待命", sizeof(parsed.tts_state));
    parsed.from_tf = true;
    *out = parsed;
    set_parse_error(error, error_size, "");
    return ESP_OK;
}
