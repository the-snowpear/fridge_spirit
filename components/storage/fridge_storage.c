// 冰箱小精灵本地存储抽象层。
// 负责给 AI 上下文提供库存、提醒、偏好、记忆摘要、短期会话历史和离线队列的统一读取接口。
// 硬件注意：结构化记忆摘要、业务快照和短期会话历史都放在 cache LittleFS，避免高频写 NVS 造成额外磨损。
#include "fridge_storage.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define STORAGE_NVS_NAMESPACE "fridge_store"
// 会话历史上行同步游标：记录"最近一条已成功推送到云端的消息 created_at（秒）"。
// 用于设备每轮对话后只增量上报新增消息，避免重连时重发整窗导致云端重复落库。
#define STORAGE_NVS_KEY_CHAT_SYNCED_AT "chat_sync_at"
#define STORAGE_CACHE_PARTITION_LABEL "cache"
#define STORAGE_CACHE_BASE_PATH "/cache"
#define STORAGE_REMINDER_PATH STORAGE_CACHE_BASE_PATH "/reminder_queue.json"
#define STORAGE_PREFERENCES_PATH STORAGE_CACHE_BASE_PATH "/user_preferences.json"
#define STORAGE_OFFLINE_QUEUE_PATH STORAGE_CACHE_BASE_PATH "/pending_queue.json"
#define STORAGE_CHAT_HISTORY_PATH STORAGE_CACHE_BASE_PATH "/ai_history.json"
#define STORAGE_UI_INVENTORY_PATH STORAGE_CACHE_BASE_PATH "/ui_inventory.json"
#define STORAGE_SHOPPING_LIST_PATH STORAGE_CACHE_BASE_PATH "/shopping_list.json"
#define STORAGE_RECIPE_CACHE_PATH STORAGE_CACHE_BASE_PATH "/recipe_cache.json"
#define STORAGE_NUTRITION_CACHE_PATH STORAGE_CACHE_BASE_PATH "/nutrition_cache.json"
#define STORAGE_KITCHEN_AI_META_PATH STORAGE_CACHE_BASE_PATH "/kitchen_ai_meta.json"
#define STORAGE_CHAT_HISTORY_SCHEMA_VERSION 1
#define STORAGE_CACHE_FORMAT_ON_FAIL true
#define STORAGE_HISTORY_FILE_BUFFER_SIZE 12288
#define STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE FRIDGE_STORAGE_MAX_JSON_LEN
#define STORAGE_EPOCH_READY_THRESHOLD 1735689600LL

static const char *TAG = "fridge_storage";

static bool s_initialized;
static uint32_t s_inventory_version = 1;

static esp_err_t open_storage_nvs(nvs_open_mode_t mode, nvs_handle_t *handle);

static const char *UI_INVENTORY_JSON =
    "{\"schema_version\":2,\"snapshot_version\":1,\"source\":\"ui_reference_seed\","
    "\"zones\":["
    "{\"id\":0,\"key\":\"freezer\",\"name\":\"冷冻区\",\"custom\":false},"
    "{\"id\":1,\"key\":\"left\",\"name\":\"左冷藏\",\"custom\":false},"
    "{\"id\":2,\"key\":\"right\",\"name\":\"右冷藏\",\"custom\":false},"
    "{\"id\":3,\"key\":\"door\",\"name\":\"门架\",\"custom\":false},"
    "{\"id\":4,\"key\":\"drawer\",\"name\":\"果蔬抽屉\",\"custom\":false}],"
    "\"layers\":["
    "{\"id\":\"f1\",\"zone_id\":\"freezer\",\"name\":\"第1层\",\"order\":0},"
    "{\"id\":\"f2\",\"zone_id\":\"freezer\",\"name\":\"第2层\",\"order\":1},"
    "{\"id\":\"l1\",\"zone_id\":\"left\",\"name\":\"第1层\",\"order\":0},"
    "{\"id\":\"l2\",\"zone_id\":\"left\",\"name\":\"第2层\",\"order\":1},"
    "{\"id\":\"l3\",\"zone_id\":\"left\",\"name\":\"第3层\",\"order\":2},"
    "{\"id\":\"r1\",\"zone_id\":\"right\",\"name\":\"第1层\",\"order\":0},"
    "{\"id\":\"r2\",\"zone_id\":\"right\",\"name\":\"第2层\",\"order\":1},"
    "{\"id\":\"d1\",\"zone_id\":\"door\",\"name\":\"上格\",\"order\":0},"
    "{\"id\":\"d2\",\"zone_id\":\"door\",\"name\":\"下格\",\"order\":1},"
    "{\"id\":\"v1\",\"zone_id\":\"drawer\",\"name\":\"抽屉\",\"order\":0}],"
    "\"items\":["
    "{\"id\":\"it01\",\"name\":\"鸡蛋\",\"quantity\":\"12个\",\"expire_date\":\"07-27\",\"days_left\":5,\"location\":\"右冷藏 / 第2层 / B2\",\"zone_id\":\"right\",\"layer_id\":\"r2\",\"cell\":4,\"note\":\"\"},"
    "{\"id\":\"it02\",\"name\":\"鲜牛奶\",\"quantity\":\"950ml\",\"expire_date\":\"07-24\",\"days_left\":2,\"location\":\"右冷藏 / 第2层 / B3\",\"zone_id\":\"right\",\"layer_id\":\"r2\",\"cell\":5,\"note\":\"全脂\"},"
    "{\"id\":\"it03\",\"name\":\"原味酸奶\",\"quantity\":\"4杯\",\"expire_date\":\"07-22\",\"days_left\":0,\"location\":\"右冷藏 / 第2层 / C1\",\"zone_id\":\"right\",\"layer_id\":\"r2\",\"cell\":6,\"note\":\"日期为估算\"},"
    "{\"id\":\"it04\",\"name\":\"番茄\",\"quantity\":\"3个\",\"expire_date\":\"07-25\",\"days_left\":3,\"location\":\"果蔬抽屉 / 抽屉 / A1\",\"zone_id\":\"drawer\",\"layer_id\":\"v1\",\"cell\":0,\"note\":\"\"},"
    "{\"id\":\"it05\",\"name\":\"菠菜\",\"quantity\":\"1把\",\"expire_date\":\"07-23\",\"days_left\":1,\"location\":\"果蔬抽屉 / 抽屉 / A2\",\"zone_id\":\"drawer\",\"layer_id\":\"v1\",\"cell\":1,\"note\":\"\"},"
    "{\"id\":\"it06\",\"name\":\"剩米饭\",\"quantity\":\"1盒\",\"expire_date\":\"07-21\",\"days_left\":-1,\"location\":\"左冷藏 / 第1层 / A1\",\"zone_id\":\"left\",\"layer_id\":\"l1\",\"cell\":0,\"note\":\"建议丢弃\"},"
    "{\"id\":\"it07\",\"name\":\"卤牛肉\",\"quantity\":\"200g\",\"expire_date\":\"07-22\",\"days_left\":0,\"location\":\"左冷藏 / 第2层 / A1\",\"zone_id\":\"left\",\"layer_id\":\"l2\",\"cell\":0,\"note\":\"今天优先吃\"},"
    "{\"id\":\"it08\",\"name\":\"速冻水饺\",\"quantity\":\"半袋(约20个)\",\"expire_date\":\"08-21\",\"days_left\":30,\"location\":\"冷冻区 / 第1层 / A1\",\"zone_id\":\"freezer\",\"layer_id\":\"f1\",\"cell\":0,\"note\":\"\"},"
    "{\"id\":\"it09\",\"name\":\"鸡胸肉\",\"quantity\":\"400g\",\"expire_date\":\"10-20\",\"days_left\":90,\"location\":\"冷冻区 / 第2层 / A1\",\"zone_id\":\"freezer\",\"layer_id\":\"f2\",\"cell\":0,\"note\":\"日期为估算\"},"
    "{\"id\":\"it10\",\"name\":\"黄油\",\"quantity\":\"1块\",\"expire_date\":\"08-05\",\"days_left\":14,\"location\":\"门架 / 上格 / A1\",\"zone_id\":\"door\",\"layer_id\":\"d1\",\"cell\":0,\"note\":\"227g装\"},"
    "{\"id\":\"it11\",\"name\":\"蚝油\",\"quantity\":\"半瓶\",\"expire_date\":\"09-20\",\"days_left\":60,\"location\":\"门架 / 下格 / A1\",\"zone_id\":\"door\",\"layer_id\":\"d2\",\"cell\":0,\"note\":\"\"},"
    "{\"id\":\"it12\",\"name\":\"可口可乐\",\"quantity\":\"3罐\",\"expire_date\":\"11-19\",\"days_left\":120,\"location\":\"门架 / 下格 / A2\",\"zone_id\":\"door\",\"layer_id\":\"d2\",\"cell\":1,\"note\":\"\"},"
    "{\"id\":\"it13\",\"name\":\"吐司面包\",\"quantity\":\"6片\",\"expire_date\":\"07-19\",\"days_left\":-3,\"location\":\"左冷藏 / 第1层 / A2\",\"zone_id\":\"left\",\"layer_id\":\"l1\",\"cell\":1,\"note\":\"已过期，待确认处理\"},"
    "{\"id\":\"it14\",\"name\":\"芝士片\",\"quantity\":\"8片\",\"expire_date\":\"07-29\",\"days_left\":7,\"location\":\"右冷藏 / 第1层 / A1\",\"zone_id\":\"right\",\"layer_id\":\"r1\",\"cell\":0,\"note\":\"\"},"
    "{\"id\":\"it15\",\"name\":\"胡萝卜\",\"quantity\":\"2根\",\"expire_date\":\"07-28\",\"days_left\":6,\"location\":\"果蔬抽屉 / 抽屉 / A3\",\"zone_id\":\"drawer\",\"layer_id\":\"v1\",\"cell\":2,\"note\":\"\"},"
    "{\"id\":\"it16\",\"name\":\"苹果\",\"quantity\":\"5个\",\"expire_date\":\"08-01\",\"days_left\":10,\"location\":\"左冷藏 / 第3层 / A1\",\"zone_id\":\"left\",\"layer_id\":\"l3\",\"cell\":0,\"note\":\"\"},"
    "{\"id\":\"it17\",\"name\":\"某绿叶菜\",\"quantity\":\"1把\",\"expire_date\":\"07-24\",\"days_left\":2,\"location\":\"\",\"zone_id\":\"\",\"layer_id\":\"\",\"note\":\"低置信度识别，名称待确认\"},"
    "{\"id\":\"it18\",\"name\":\"内酯豆腐\",\"quantity\":\"1盒\",\"expire_date\":\"07-23\",\"days_left\":1,\"location\":\"\",\"zone_id\":\"\",\"layer_id\":\"\",\"note\":\"位置待补充\"},"
    "{\"id\":\"it19\",\"name\":\"午餐肉罐头\",\"quantity\":\"1罐\",\"expire_date\":\"01-18\",\"days_left\":180,\"location\":\"右冷藏 / 第1层 / A2\",\"zone_id\":\"right\",\"layer_id\":\"r1\",\"cell\":1,\"note\":\"\"},"
    "{\"id\":\"it20\",\"name\":\"虾仁\",\"quantity\":\"250g\",\"expire_date\":\"09-05\",\"days_left\":45,\"location\":\"冷冻区 / 第2层 / A2\",\"zone_id\":\"freezer\",\"layer_id\":\"f2\",\"cell\":1,\"note\":\"\"}],"
    "\"layout_drafts\":[]}";

static const char *REMINDER_JSON =
    "{\"schema_version\":1,"
    "\"reminders\":["
    "{\"item_id\":\"seed_milk\",\"name\":\"牛奶\",\"days_left\":1,\"location\":\"门架中层\",\"suggestion\":\"今天或明天优先喝掉，若有异味请丢弃\"},"
    "{\"item_id\":\"seed_tomato\",\"name\":\"番茄\",\"days_left\":2,\"location\":\"冷藏主仓 B2\",\"suggestion\":\"可做番茄炒蛋或番茄汤\"}"
    "]}";

static const char *PREFERENCES_JSON =
    "{\"schema_version\":1,"
    "\"people\":2,"
    "\"taste\":[\"清淡\",\"少油\"],"
    "\"avoid\":[\"香菜\"],"
    "\"cooking_time_preference\":\"30分钟以内\","
    "\"kitchen_tools\":[\"电饭煲\",\"炒锅\",\"微波炉\"]}";

static const char *OFFLINE_QUEUE_JSON =
    "{\"schema_version\":1,"
    "\"pending_count\":0,"
    "\"policy\":\"离线时只保存业务事件和拍照任务状态，不长期保存图片原图\","
    "\"items\":[]}";

static const char *SHOPPING_LIST_JSON =
    "{\"schema_version\":1,"
    "\"source\":\"local_seed\","
    "\"items\":[]}";

static const char *RECIPE_CACHE_JSON =
    "{\"schema_version\":1,"
    "\"source\":\"local_seed\","
    "\"items\":[]}";

static const char *NUTRITION_CACHE_JSON =
    "{\"schema_version\":1,"
    "\"source\":\"local_seed\","
    "\"nutrition\":{\"score\":0,\"summary\":\"尚未生成 AI 营养建议\",\"focus\":[],\"suggestions\":[],\"restrict_note\":\"\"}}";

static const char *KITCHEN_AI_META_JSON =
    "{\"schema_version\":1,"
    "\"last_auto_day\":0,"
    "\"auto_attempt_day\":0,"
    "\"auto_attempt_count\":0,"
    "\"last_attempt_at\":0,"
    "\"last_generated_at\":0,"
    "\"last_error\":\"\"}";

static esp_err_t copy_json(const char *source, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(source && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid json copy args");
    size_t len = strlen(source);
    ESP_RETURN_ON_FALSE(len + 1 <= out_size, ESP_ERR_NO_MEM, TAG, "json output buffer too small");
    strlcpy(out, source, out_size);
    return ESP_OK;
}

static esp_err_t storage_read_text_file(const char *path, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(path && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid read file args");
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t bytes = fread(out, 1, out_size - 1, file);
    bool truncated = !feof(file);
    fclose(file);
    out[bytes] = '\0';
    return truncated ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t storage_write_text_file(const char *path, const char *text)
{
    ESP_RETURN_ON_FALSE(path && text, ESP_ERR_INVALID_ARG, TAG, "invalid write file args");
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, file);
    fclose(file);
    return written == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t storage_ensure_text_file_exists(const char *path, const char *default_text)
{
    ESP_RETURN_ON_FALSE(path && default_text, ESP_ERR_INVALID_ARG, TAG, "invalid ensure file args");
    FILE *file = fopen(path, "rb");
    if (file) {
        fclose(file);
        return ESP_OK;
    }
    // 首次启动或 cache 分区刚格式化时写入默认业务快照。
    // 已存在的生产数据只读取不覆盖，避免把用户确认过的库存退回测试种子。
    return storage_write_text_file(path, default_text);
}

static esp_err_t storage_read_or_create_text_file(const char *path,
                                                  const char *default_text,
                                                  char *out,
                                                  size_t out_size)
{
    ESP_RETURN_ON_FALSE(path && default_text && out && out_size > 0,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid read or create args");
    esp_err_t err = storage_read_text_file(path, out, out_size);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(storage_write_text_file(path, default_text), TAG, "create cache file failed");
        return copy_json(default_text, out, out_size);
    }
    return err;
}

static esp_err_t storage_write_json_document(const char *path, const char *json_text, size_t max_size)
{
    ESP_RETURN_ON_FALSE(path && json_text && json_text[0] == '{', ESP_ERR_INVALID_ARG, TAG, "sync document must be JSON object");
    ESP_RETURN_ON_FALSE(strlen(json_text) < max_size, ESP_ERR_INVALID_SIZE, TAG, "sync document too large");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    cJSON *root = cJSON_Parse(json_text);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "sync document JSON invalid");
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print sync document failed");
    esp_err_t err = strlen(printed) < max_size ? storage_write_text_file(path, printed) : ESP_ERR_INVALID_SIZE;
    cJSON_free(printed);
    return err;
}

// 购物清单由 UI 和 AI 都可能更新。写入先落到同分区临时文件，经 flush/读回校验后替换，
// 任一步失败恢复 .bak，避免掉电或空间不足留下半份清单。
static esp_err_t storage_write_shopping_json_atomic(const char *json_text)
{
    ESP_RETURN_ON_FALSE(json_text && json_text[0] == '{', ESP_ERR_INVALID_ARG, TAG,
                        "shopping document must be JSON object");
    ESP_RETURN_ON_FALSE(strlen(json_text) < FRIDGE_STORAGE_MAX_JSON_LEN, ESP_ERR_INVALID_SIZE,
                        TAG, "shopping document too large");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    cJSON *root = cJSON_Parse(json_text);
    ESP_RETURN_ON_FALSE(root && cJSON_IsObject(root), ESP_ERR_INVALID_ARG, TAG,
                        "shopping document JSON invalid");
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return ESP_ERR_NO_MEM;

    const char *part = STORAGE_CACHE_BASE_PATH "/shopping_list.json.part";
    const char *backup = STORAGE_CACHE_BASE_PATH "/shopping_list.json.bak";
    remove(part);
    FILE *file = fopen(part, "wb");
    if (!file) { cJSON_free(printed); return ESP_FAIL; }
    size_t len = strlen(printed);
    bool written = fwrite(printed, 1, len, file) == len && fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    if (!written) { remove(part); cJSON_free(printed); return ESP_FAIL; }

    remove(backup);
    bool had_old = rename(STORAGE_SHOPPING_LIST_PATH, backup) == 0;
    if (rename(part, STORAGE_SHOPPING_LIST_PATH) != 0) {
        if (had_old) (void)rename(backup, STORAGE_SHOPPING_LIST_PATH);
        remove(part);
        cJSON_free(printed);
        return ESP_FAIL;
    }
    char verify[FRIDGE_STORAGE_MAX_JSON_LEN] = {0};
    esp_err_t verify_err = storage_read_text_file(STORAGE_SHOPPING_LIST_PATH, verify, sizeof(verify));
    if (verify_err != ESP_OK || strcmp(verify, printed) != 0) {
        remove(STORAGE_SHOPPING_LIST_PATH);
        if (had_old) (void)rename(backup, STORAGE_SHOPPING_LIST_PATH);
        cJSON_free(printed);
        return ESP_FAIL;
    }
    remove(backup);
    cJSON_free(printed);
    return ESP_OK;
}

static esp_err_t open_storage_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NVS handle is NULL");
    return nvs_open(STORAGE_NVS_NAMESPACE, mode, handle);
}

static bool storage_time_ready(void)
{
    time_t now = time(NULL);
    return now >= STORAGE_EPOCH_READY_THRESHOLD;
}

static int64_t storage_now_seconds(void)
{
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 0;
}

static void storage_fill_default_history(fridge_storage_chat_history_t *history)
{
    if (!history) {
        return;
    }
    memset(history, 0, sizeof(*history));
    history->schema_version = STORAGE_CHAT_HISTORY_SCHEMA_VERSION;
    history->ttl_seconds = FRIDGE_STORAGE_CHAT_TTL_SECONDS;
    history->max_messages = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    history->time_ready = storage_time_ready();
    history->updated_at = history->time_ready ? (uint32_t)storage_now_seconds() : 0;
}

static void storage_reset_message(fridge_storage_chat_message_t *message)
{
    if (!message) {
        return;
    }
    memset(message, 0, sizeof(*message));
}

static bool storage_role_allowed(const char *role)
{
    return role && (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

static bool storage_content_allowed(const char *content)
{
    return content && content[0] != '\0';
}

static size_t storage_utf8_safe_prefix_len(const char *text, size_t max_bytes)
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

static bool storage_utf8_is_valid(const char *text)
{
    if (!text) {
        return false;
    }
    size_t len = strlen(text);
    return storage_utf8_safe_prefix_len(text, len) == len;
}

static void storage_copy_utf8_safe(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    size_t copy_len = storage_utf8_safe_prefix_len(text, out_size - 1);
    memcpy(out, text, copy_len);
    out[copy_len] = '\0';
}

static bool storage_replace_json_item(cJSON *root, const char *key, cJSON *item)
{
    if (!root || !key || !item) {
        cJSON_Delete(item);
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddItemToObject(root, key, item);
    return cJSON_GetObjectItemCaseSensitive(root, key) == item;
}

static int64_t storage_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t storage_json_u32(const cJSON *root, const char *camel_key, const char *snake_key, uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, camel_key);
    if (!cJSON_IsNumber(item) && snake_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, snake_key);
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return fallback;
    }
    return (uint32_t)item->valuedouble;
}

static int64_t storage_json_i64(const cJSON *root, const char *camel_key, const char *snake_key, int64_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, camel_key);
    if (!cJSON_IsNumber(item) && snake_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, snake_key);
    }
    if (!cJSON_IsNumber(item)) {
        return fallback;
    }
    return (int64_t)item->valuedouble;
}

static bool storage_json_bool(const cJSON *root, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

static void storage_json_copy_string(const cJSON *root, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(out, item->valuestring, out_size);
    }
}

// 库存是 UI 与云同步共同使用的主快照。写入必须保留上一版并在同一 LittleFS 分区改名，
// 这样断电、空间耗尽或写入失败都不会把可恢复的库存替换成半份 JSON。
static esp_err_t storage_write_ui_inventory_json_atomic(const char *json)
{
    ESP_RETURN_ON_FALSE(json && json[0] == '{', ESP_ERR_INVALID_ARG, TAG, "invalid ui inventory json");
    const char *part = STORAGE_CACHE_BASE_PATH "/ui_inventory.json.part";
    const char *backup = STORAGE_CACHE_BASE_PATH "/ui_inventory.json.bak";
    remove(part);
    FILE *file = fopen(part, "wb");
    if (!file) return ESP_FAIL;
    size_t len = strlen(json);
    bool written = fwrite(json, 1, len, file) == len && fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    if (!written) {
        remove(part);
        return ESP_FAIL;
    }
    remove(backup);
    bool had_old = rename(STORAGE_UI_INVENTORY_PATH, backup) == 0;
    if (rename(part, STORAGE_UI_INVENTORY_PATH) != 0) {
        if (had_old) (void)rename(backup, STORAGE_UI_INVENTORY_PATH);
        remove(part);
        return ESP_FAIL;
    }
    char verify[STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE] = {0};
    esp_err_t verify_err = storage_read_text_file(STORAGE_UI_INVENTORY_PATH, verify, sizeof(verify));
    if (verify_err != ESP_OK || strcmp(verify, json) != 0) {
        remove(STORAGE_UI_INVENTORY_PATH);
        if (had_old) (void)rename(backup, STORAGE_UI_INVENTORY_PATH);
        return ESP_FAIL;
    }
    remove(backup);
    return ESP_OK;
}

// 若掉电发生在“旧快照改名为 .bak”与“新快照改名为正式文件”之间，启动时恢复旧快照。
// 没有备份时返回 NOT_FOUND，由首次初始化路径写入种子数据。
static esp_err_t storage_recover_ui_inventory_backup(void)
{
    FILE *current = fopen(STORAGE_UI_INVENTORY_PATH, "rb");
    if (current) {
        fclose(current);
        return ESP_OK;
    }
    return rename(STORAGE_CACHE_BASE_PATH "/ui_inventory.json.bak", STORAGE_UI_INVENTORY_PATH) == 0
               ? ESP_OK
               : ESP_ERR_NOT_FOUND;
}

static esp_err_t storage_print_json_to_file(cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print ui inventory failed");
    esp_err_t err = strlen(printed) < STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE
                        ? storage_write_ui_inventory_json_atomic(printed)
                        : ESP_ERR_INVALID_SIZE;
    cJSON_free(printed);
    return err;
}

static esp_err_t storage_load_ui_inventory_root(cJSON **out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "inventory root output is NULL");
    *out = NULL;
    char *buffer = calloc(1, STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE);
    ESP_RETURN_ON_FALSE(buffer, ESP_ERR_NO_MEM, TAG, "allocate ui inventory load buffer failed");
    esp_err_t err = storage_read_or_create_text_file(STORAGE_UI_INVENTORY_PATH,
                                                     UI_INVENTORY_JSON,
                                                     buffer,
                                                     STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE);
    if (err == ESP_OK) {
        *out = cJSON_Parse(buffer);
        if (!*out) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    free(buffer);
    return err;
}

static void storage_fill_inventory_sync_status_from_root(cJSON *root, fridge_storage_inventory_sync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->snapshot_version = storage_json_u32(root, "snapshotVersion", "snapshot_version", storage_json_u32(root, "snapshot_version", NULL, 1));
    out->server_revision = storage_json_u32(root, "serverRevision", "server_revision", 0);
    out->updated_at_ms = storage_json_i64(root, "updatedAtMs", "updated_at_ms", 0);
    out->last_sync_at_ms = storage_json_i64(root, "lastSyncAtMs", "last_sync_at_ms", 0);
    out->dirty = storage_json_bool(root, "dirty", false);
    storage_json_copy_string(root, "homeId", out->home_id, sizeof(out->home_id));
    if (out->home_id[0] == '\0') {
        storage_json_copy_string(root, "home_id", out->home_id, sizeof(out->home_id));
    }
    storage_json_copy_string(root, "deviceId", out->device_id, sizeof(out->device_id));
    if (out->device_id[0] == '\0') {
        storage_json_copy_string(root, "device_id", out->device_id, sizeof(out->device_id));
    }
}

static const cJSON *storage_json_get_string_any(const cJSON *root, const char *key, const char *fallback_key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) && fallback_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, fallback_key);
    }
    return cJSON_IsString(item) ? item : NULL;
}

static const char *storage_json_string_value_any(const cJSON *root,
                                                 const char *key,
                                                 const char *fallback_key,
                                                 const char *fallback)
{
    const cJSON *item = storage_json_get_string_any(root, key, fallback_key);
    return (item && item->valuestring) ? item->valuestring : fallback;
}

static esp_err_t storage_update_inventory_sync_meta(cJSON *root,
                                                    uint32_t snapshot_version,
                                                    uint32_t server_revision,
                                                    int64_t updated_at_ms,
                                                    int64_t last_sync_at_ms,
                                                    bool dirty,
                                                    const char *home_id,
                                                    const char *device_id)
{
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "inventory root is NULL");
    ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "schema_version", cJSON_CreateNumber(1)), ESP_ERR_NO_MEM, TAG, "set schema failed");
    ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "snapshotVersion", cJSON_CreateNumber(snapshot_version)), ESP_ERR_NO_MEM, TAG, "set snapshotVersion failed");
    ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "updatedAtMs", cJSON_CreateNumber((double)updated_at_ms)), ESP_ERR_NO_MEM, TAG, "set updatedAtMs failed");
    ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "serverRevision", cJSON_CreateNumber(server_revision)), ESP_ERR_NO_MEM, TAG, "set serverRevision failed");
    ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "lastSyncAtMs", cJSON_CreateNumber((double)last_sync_at_ms)), ESP_ERR_NO_MEM, TAG, "set lastSyncAtMs failed");
    ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "dirty", cJSON_CreateBool(dirty)), ESP_ERR_NO_MEM, TAG, "set dirty failed");
    if (home_id && home_id[0]) {
        ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "homeId", cJSON_CreateString(home_id)), ESP_ERR_NO_MEM, TAG, "set homeId failed");
    }
    if (device_id && device_id[0]) {
        ESP_RETURN_ON_FALSE(storage_replace_json_item(root, "deviceId", cJSON_CreateString(device_id)), ESP_ERR_NO_MEM, TAG, "set deviceId failed");
    }
    return ESP_OK;
}

static cJSON *storage_find_zone_by_id(cJSON *zones, int id);
static bool storage_strip_cloud_prefix_one(char *quantity, size_t quantity_size);
static bool storage_clean_quantity_text(char *quantity, size_t quantity_size);

static void storage_clean_quantity_copy(const cJSON *item, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *quantity = storage_json_string_value_any(item, "quantity", NULL, "");
    strlcpy(out, quantity, out_size);
    const cJSON *unit = storage_json_get_string_any(item, "unit", NULL);
    const char *unit_text = (unit && unit->valuestring) ? unit->valuestring : NULL;
    if (unit_text && unit_text[0] >= '0' && unit_text[0] <= '9') {
        strlcpy(out, unit_text, out_size);
    }
    if (unit_text && strcmp(unit_text, quantity) == 0) {
        (void)storage_strip_cloud_prefix_one(out, out_size);
    }
    (void)storage_clean_quantity_text(out, out_size);
}

static bool storage_inventory_zones_equivalent(const cJSON *local_root, const cJSON *incoming_root)
{
    const cJSON *local_zones = cJSON_GetObjectItemCaseSensitive(local_root, "zones");
    const cJSON *incoming_zones = cJSON_GetObjectItemCaseSensitive(incoming_root, "zones");
    if (!cJSON_IsArray(local_zones) || !cJSON_IsArray(incoming_zones)) {
        return cJSON_GetArraySize(local_zones) == cJSON_GetArraySize(incoming_zones);
    }

    const cJSON *local_zone = NULL;
    cJSON_ArrayForEach(local_zone, local_zones) {
        const cJSON *id_json = cJSON_GetObjectItemCaseSensitive(local_zone, "id");
        if (!cJSON_IsNumber(id_json)) {
            return false;
        }
        const cJSON *incoming_zone = storage_find_zone_by_id((cJSON *)incoming_zones, id_json->valueint);
        if (!cJSON_IsObject(incoming_zone)) {
            return false;
        }
        const char *local_name = storage_json_string_value_any(local_zone, "name", "label", "");
        const char *incoming_name = storage_json_string_value_any(incoming_zone, "name", "label", "");
        const char *local_note = storage_json_string_value_any(local_zone, "note", "hint", "");
        const char *incoming_note = storage_json_string_value_any(incoming_zone, "note", "hint", "");
        uint32_t local_width = storage_json_u32(local_zone, "width", NULL, 1);
        uint32_t incoming_width = storage_json_u32(incoming_zone, "width", NULL, 1);
        uint32_t local_height = storage_json_u32(local_zone, "height", NULL, 1);
        uint32_t incoming_height = storage_json_u32(incoming_zone, "height", NULL, 1);
        bool local_custom = storage_json_bool(local_zone, "custom", false);
        bool incoming_custom = storage_json_bool(incoming_zone, "custom", false);
        if (strcmp(local_name, incoming_name) != 0 ||
            strcmp(local_note, incoming_note) != 0 ||
            local_width != incoming_width ||
            local_height != incoming_height ||
            local_custom != incoming_custom) {
            return false;
        }
    }
    return cJSON_GetArraySize(local_zones) == cJSON_GetArraySize(incoming_zones);
}

static bool storage_inventory_item_equivalent(const cJSON *local_item, const cJSON *incoming_item)
{
    if (!cJSON_IsObject(local_item) || !cJSON_IsObject(incoming_item)) {
        return false;
    }
    const char *local_name = storage_json_string_value_any(local_item, "name", NULL, "");
    const char *incoming_name = storage_json_string_value_any(incoming_item, "name", NULL, "");
    const char *local_location = storage_json_string_value_any(local_item, "location", NULL, "");
    const char *incoming_location = storage_json_string_value_any(incoming_item, "location", NULL, "");
    uint32_t local_zone = storage_json_u32(local_item, "zone", NULL, UINT32_MAX);
    uint32_t incoming_zone = storage_json_u32(incoming_item, "zone", NULL, UINT32_MAX);
    uint32_t local_cell = storage_json_u32(local_item, "cell", NULL, UINT32_MAX);
    uint32_t incoming_cell = storage_json_u32(incoming_item, "cell", NULL, UINT32_MAX);
    uint32_t local_days = storage_json_u32(local_item, "days_left", "daysLeft", UINT32_MAX);
    uint32_t incoming_days = storage_json_u32(incoming_item, "days_left", "daysLeft", UINT32_MAX);
    char local_quantity[64] = {0};
    char incoming_quantity[64] = {0};
    storage_clean_quantity_copy(local_item, local_quantity, sizeof(local_quantity));
    storage_clean_quantity_copy(incoming_item, incoming_quantity, sizeof(incoming_quantity));
    if (strcmp(local_name, incoming_name) != 0 ||
        strcmp(local_quantity, incoming_quantity) != 0 ||
        strcmp(local_location, incoming_location) != 0 ||
        local_zone != incoming_zone ||
        local_cell != incoming_cell) {
        return false;
    }
    if (local_days != UINT32_MAX && incoming_days != UINT32_MAX) {
        return local_days == incoming_days;
    }
    const char *local_expire = storage_json_string_value_any(local_item, "expire_date", "expireDate", "");
    const char *incoming_expire = storage_json_string_value_any(incoming_item, "expire_date", "expireDate", "");
    return strcmp(local_expire, incoming_expire) == 0;
}

static bool storage_inventory_items_equivalent(const cJSON *local_root, const cJSON *incoming_root)
{
    const cJSON *local_items = cJSON_GetObjectItemCaseSensitive(local_root, "items");
    const cJSON *incoming_items = cJSON_GetObjectItemCaseSensitive(incoming_root, "items");
    if (!cJSON_IsArray(local_items) || !cJSON_IsArray(incoming_items)) {
        return cJSON_GetArraySize(local_items) == cJSON_GetArraySize(incoming_items);
    }
    int item_count = cJSON_GetArraySize(local_items);
    if (item_count != cJSON_GetArraySize(incoming_items)) {
        return false;
    }

    bool *matched = calloc((size_t)item_count, sizeof(bool));
    if (!matched) {
        return false;
    }
    for (int i = 0; i < item_count; i++) {
        const cJSON *local_item = cJSON_GetArrayItem(local_items, i);
        bool matched_item = false;
        for (int j = 0; j < item_count; j++) {
            if (matched[j]) {
                continue;
            }
            const cJSON *incoming_item = cJSON_GetArrayItem(incoming_items, j);
            if (storage_inventory_item_equivalent(local_item, incoming_item)) {
                matched[j] = true;
                matched_item = true;
                break;
            }
        }
        if (!matched_item) {
            free(matched);
            return false;
        }
    }
    free(matched);
    return true;
}

static bool storage_inventory_documents_equivalent(const cJSON *local_root, const cJSON *incoming_root)
{
    return storage_inventory_zones_equivalent(local_root, incoming_root) &&
           storage_inventory_items_equivalent(local_root, incoming_root);
}

static cJSON *storage_find_zone_by_id(cJSON *zones, int id)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, zones) {
        const cJSON *zone_id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsNumber(zone_id) && zone_id->valueint == id) {
            return item;
        }
    }
    return NULL;
}

static bool storage_zone_is_custom_active(const cJSON *zone)
{
    if (!cJSON_IsObject(zone)) {
        return false;
    }
    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(zone, "custom");
    if (cJSON_IsBool(custom) && !cJSON_IsTrue(custom)) {
        return false;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(zone, "name");
    const char *text = cJSON_IsString(name) ? name->valuestring : "";
    return text && text[0] != '\0' && strcmp(text, "自定义区") != 0 && strcmp(text, "备用区") != 0;
}

static esp_err_t storage_preserve_local_custom_zones(cJSON *incoming_root)
{
    cJSON *incoming_zones = cJSON_GetObjectItemCaseSensitive(incoming_root, "zones");
    if (!cJSON_IsArray(incoming_zones)) {
        return ESP_OK;
    }

    cJSON *local_root = NULL;
    if (storage_load_ui_inventory_root(&local_root) != ESP_OK || !local_root) {
        return ESP_OK;
    }
    cJSON *local_zones = cJSON_GetObjectItemCaseSensitive(local_root, "zones");
    if (!cJSON_IsArray(local_zones)) {
        cJSON_Delete(local_root);
        return ESP_OK;
    }

    for (int id = 4; id < 6; id++) {
        if (storage_find_zone_by_id(incoming_zones, id)) {
            continue;
        }
        cJSON *local_zone = storage_find_zone_by_id(local_zones, id);
        if (!storage_zone_is_custom_active(local_zone)) {
            continue;
        }
        cJSON *copy = cJSON_Duplicate(local_zone, true);
        if (!copy || !cJSON_AddItemToArray(incoming_zones, copy)) {
            cJSON_Delete(copy);
            cJSON_Delete(local_root);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(local_root);
    return ESP_OK;
}

static bool storage_unit_is_repeated_ascii(const char *unit, char ch)
{
    if (!unit || unit[0] == '\0' || unit[1] == '\0') {
        return false;
    }
    for (const char *p = unit; *p; p++) {
        if (*p != ch) {
            return false;
        }
    }
    return true;
}

static bool storage_strip_cloud_prefix_one(char *quantity, size_t quantity_size)
{
    if (!quantity || quantity_size == 0) {
        return false;
    }
    size_t len = strlen(quantity);
    if (len < 3 || quantity[0] != '1') {
        return false;
    }
    char *unit = quantity;
    while (*unit && ((*unit >= '0' && *unit <= '9') || *unit == '.')) {
        unit++;
    }
    if (!(strcmp(unit, "g") == 0 || strcmp(unit, "克") == 0 || strcmp(unit, "ml") == 0 || strcmp(unit, "毫升") == 0)) {
        return false;
    }
    memmove(quantity, quantity + 1, len);
    quantity[quantity_size - 1] = '\0';
    return true;
}

static bool storage_clean_quantity_text(char *quantity, size_t quantity_size)
{
    if (!quantity || quantity_size == 0 || quantity[0] == '\0') {
        return false;
    }
    char original[64] = {0};
    strlcpy(original, quantity, sizeof(original));

    char number[32] = {0};
    char unit[32] = {0};
    size_t number_len = 0;
    while (quantity[number_len] &&
           ((quantity[number_len] >= '0' && quantity[number_len] <= '9') || quantity[number_len] == '.')) {
        number_len++;
    }
    if (number_len == 0 || number_len >= sizeof(number)) {
        return false;
    }
    memcpy(number, quantity, number_len);
    number[number_len] = '\0';
    strlcpy(unit, quantity + number_len, sizeof(unit));
    if (storage_unit_is_repeated_ascii(unit, 'g')) {
        strlcpy(unit, "g", sizeof(unit));
    } else if (strcmp(unit, "克克") == 0) {
        strlcpy(unit, "克", sizeof(unit));
    } else if (strcmp(unit, "个个") == 0) {
        strlcpy(unit, "个", sizeof(unit));
    } else if (strcmp(unit, "份份") == 0) {
        strlcpy(unit, "份", sizeof(unit));
    } else if (strcmp(unit, "盒盒") == 0) {
        strlcpy(unit, "盒", sizeof(unit));
    } else if (strcmp(unit, "瓶瓶") == 0) {
        strlcpy(unit, "瓶", sizeof(unit));
    } else if (strcmp(unit, "袋袋") == 0) {
        strlcpy(unit, "袋", sizeof(unit));
    }

    bool integer_number = true;
    for (size_t i = 0; number[i]; i++) {
        if (number[i] < '0' || number[i] > '9') {
            integer_number = false;
            break;
        }
    }
    if (integer_number) {
        bool gram_like = strcmp(unit, "g") == 0 || strcmp(unit, "克") == 0 || strcmp(unit, "ml") == 0 || strcmp(unit, "毫升") == 0;
        size_t max_digits = gram_like ? 3 : 2;
        size_t len = strlen(number);
        bool polluted_prefix = len > max_digits && number[0] == '1' && number[1] == '1';
        while (polluted_prefix && len > max_digits && number[0] == '1') {
            memmove(number, number + 1, len);
            len--;
        }
        bool all_one = len > 1;
        for (size_t i = 0; i < len; i++) {
            if (number[i] != '1') {
                all_one = false;
                break;
            }
        }
        if (all_one) {
            strlcpy(number, "1", sizeof(number));
        }
    }

    char cleaned[64] = {0};
    snprintf(cleaned, sizeof(cleaned), "%s%s", number, unit);
    if (strcmp(cleaned, original) == 0) {
        return false;
    }
    strlcpy(quantity, cleaned, quantity_size);
    return true;
}

static size_t storage_repair_inventory_root_quantities(cJSON *root)
{
    if (!root) {
        return 0;
    }
    size_t count = 0;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *quantity = cJSON_GetObjectItemCaseSensitive(item, "quantity");
        if (!cJSON_IsString(quantity) || !quantity->valuestring) {
            continue;
        }
        char cleaned[64] = {0};
        const cJSON *unit = cJSON_GetObjectItemCaseSensitive(item, "unit");
        const char *quantity_text = quantity->valuestring;
        const char *unit_text = cJSON_IsString(unit) ? unit->valuestring : NULL;
        if (unit_text && unit_text[0] >= '0' && unit_text[0] <= '9') {
            // 兼容旧后端污染：云端曾把设备 "200g" 写成 quantity=1, unit="200g"，
            // 再下发时会拼成 "1200g"。此时 unit 本身才是可信的设备显示数量。
            quantity_text = unit_text;
        }
        strlcpy(cleaned, quantity_text, sizeof(cleaned));
        if (unit_text && strcmp(unit_text, quantity->valuestring) == 0) {
            (void)storage_strip_cloud_prefix_one(cleaned, sizeof(cleaned));
        }
        if (storage_clean_quantity_text(cleaned, sizeof(cleaned))) {
            cJSON_SetValuestring(quantity, cleaned);
            count++;
        } else if (quantity_text != quantity->valuestring && strcmp(quantity->valuestring, cleaned) != 0) {
            cJSON_SetValuestring(quantity, cleaned);
            count++;
        }
    }
    return count;
}

static size_t storage_prune_history_inplace(fridge_storage_chat_history_t *history, bool time_ready)
{
    if (!history) {
        return 0;
    }

    size_t write_index = 0;
    size_t pruned = 0;
    int64_t now = time_ready ? storage_now_seconds() : 0;
    int64_t cutoff = time_ready ? (now - FRIDGE_STORAGE_CHAT_TTL_SECONDS) : 0;

    for (size_t i = 0; i < history->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        fridge_storage_chat_message_t *message = &history->messages[i];
        bool expired = time_ready && message->created_at > 0 && message->created_at < cutoff;
        bool invalid = !storage_role_allowed(message->role) ||
                       !storage_content_allowed(message->content) ||
                       !storage_utf8_is_valid(message->content);
        if (expired || invalid) {
            pruned++;
            continue;
        }
        if (write_index != i) {
            history->messages[write_index] = *message;
        }
        write_index++;
    }

    if (write_index > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        write_index = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }

    if (write_index > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        write_index = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }

    if (write_index > history->max_messages && history->max_messages > 0) {
        size_t drop_count = write_index - history->max_messages;
        memmove(history->messages,
                history->messages + drop_count,
                (write_index - drop_count) * sizeof(history->messages[0]));
        for (size_t i = write_index - drop_count; i < write_index; i++) {
            storage_reset_message(&history->messages[i]);
        }
        pruned += drop_count;
        write_index -= drop_count;
    }

    for (size_t i = write_index; i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        storage_reset_message(&history->messages[i]);
    }

    history->count = write_index;
    history->time_ready = time_ready;
    history->updated_at = time_ready ? (uint32_t)now : 0;
    history->schema_version = STORAGE_CHAT_HISTORY_SCHEMA_VERSION;
    history->ttl_seconds = FRIDGE_STORAGE_CHAT_TTL_SECONDS;
    history->max_messages = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    return pruned;
}

static esp_err_t storage_mount_cache_fs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_CACHE_BASE_PATH,
        .partition_label = STORAGE_CACHE_PARTITION_LABEL,
        .format_if_mount_failed = STORAGE_CACHE_FORMAT_ON_FAIL,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t storage_history_from_json(const char *json, fridge_storage_chat_history_t *history)
{
    ESP_RETURN_ON_FALSE(json && history, ESP_ERR_INVALID_ARG, TAG, "invalid history json args");
    storage_fill_default_history(history);

    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "parse history json failed");

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *updated_at = cJSON_GetObjectItemCaseSensitive(root, "updated_at");
    const cJSON *ttl_seconds = cJSON_GetObjectItemCaseSensitive(root, "ttl_seconds");
    const cJSON *max_messages = cJSON_GetObjectItemCaseSensitive(root, "max_messages");
    const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");

    if (cJSON_IsNumber(schema) && schema->valueint > 0) {
        history->schema_version = (uint32_t)schema->valueint;
    }
    if (cJSON_IsNumber(updated_at) && updated_at->valuedouble >= 0) {
        history->updated_at = (uint32_t)updated_at->valuedouble;
    }
    if (cJSON_IsNumber(ttl_seconds) && ttl_seconds->valueint > 0) {
        history->ttl_seconds = (uint32_t)ttl_seconds->valueint;
    }
    if (cJSON_IsNumber(max_messages) && max_messages->valueint > 0 && max_messages->valueint <= FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        history->max_messages = (uint32_t)max_messages->valueint;
    }

    if (cJSON_IsArray(messages)) {
        size_t count = 0;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, messages) {
            if (count >= FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
                break;
            }
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
            const cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
            const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
            const cJSON *task_type = cJSON_GetObjectItemCaseSensitive(item, "task_type");
            const cJSON *created_at = cJSON_GetObjectItemCaseSensitive(item, "created_at");
            if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
                continue;
            }

            fridge_storage_chat_message_t *message = &history->messages[count];
            storage_reset_message(message);
            if (cJSON_IsString(id) && id->valuestring) {
                strlcpy(message->id, id->valuestring, sizeof(message->id));
            }
            strlcpy(message->role, role->valuestring, sizeof(message->role));
            storage_copy_utf8_safe(message->content, sizeof(message->content), content->valuestring);
            if (cJSON_IsString(task_type) && task_type->valuestring) {
                strlcpy(message->task_type, task_type->valuestring, sizeof(message->task_type));
            }
            if (cJSON_IsNumber(created_at)) {
                message->created_at = (int64_t)created_at->valuedouble;
            }
            count++;
        }
        history->count = count;
    }

    history->time_ready = storage_time_ready();
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t storage_history_to_json(const fridge_storage_chat_history_t *history, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(history && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid history output args");

    esp_err_t ret = ESP_OK;
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create history root failed");

    cJSON_AddNumberToObject(root, "schema_version", history->schema_version);
    cJSON_AddNumberToObject(root, "updated_at", history->updated_at);
    cJSON_AddNumberToObject(root, "ttl_seconds", history->ttl_seconds);
    cJSON_AddNumberToObject(root, "max_messages", history->max_messages);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    ESP_GOTO_ON_FALSE(messages, ESP_ERR_NO_MEM, cleanup, TAG, "create history messages failed");

    for (size_t i = 0; i < history->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        const fridge_storage_chat_message_t *message = &history->messages[i];
        cJSON *item = cJSON_CreateObject();
        ESP_GOTO_ON_FALSE(item, ESP_ERR_NO_MEM, cleanup, TAG, "create history item failed");
        cJSON_AddStringToObject(item, "id", message->id);
        cJSON_AddStringToObject(item, "role", message->role);
        cJSON_AddStringToObject(item, "content", message->content);
        cJSON_AddStringToObject(item, "task_type", message->task_type);
        cJSON_AddNumberToObject(item, "created_at", (double)message->created_at);
        cJSON_AddItemToArray(messages, item);
    }

    char *printed = cJSON_PrintUnformatted(root);
    ESP_GOTO_ON_FALSE(printed, ESP_ERR_NO_MEM, cleanup, TAG, "print history json failed");
    ESP_GOTO_ON_FALSE(strlen(printed) + 1 <= out_size, ESP_ERR_NO_MEM, cleanup_printed, TAG, "history json buffer too small");
    strlcpy(out, printed, out_size);
    cJSON_free(printed);
    cJSON_Delete(root);
    return ESP_OK;

cleanup_printed:
    cJSON_free(printed);
cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t storage_read_history_file(fridge_storage_chat_history_t *history)
{
    ESP_RETURN_ON_FALSE(history, ESP_ERR_INVALID_ARG, TAG, "history is NULL");
    storage_fill_default_history(history);

    FILE *file = fopen(STORAGE_CHAT_HISTORY_PATH, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    char *buffer = calloc(1, STORAGE_HISTORY_FILE_BUFFER_SIZE);
    if (!buffer) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t bytes = fread(buffer, 1, STORAGE_HISTORY_FILE_BUFFER_SIZE - 1, file);
    fclose(file);
    buffer[bytes] = '\0';

    esp_err_t err = storage_history_from_json(buffer, history);
    free(buffer);
    return err;
}

static esp_err_t storage_write_history_file(const fridge_storage_chat_history_t *history)
{
    ESP_RETURN_ON_FALSE(history, ESP_ERR_INVALID_ARG, TAG, "history is NULL");

    char *buffer = calloc(1, STORAGE_HISTORY_FILE_BUFFER_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_history_to_json(history, buffer, STORAGE_HISTORY_FILE_BUFFER_SIZE);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    FILE *file = fopen(STORAGE_CHAT_HISTORY_PATH, "wb");
    if (!file) {
        free(buffer);
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer, 1, strlen(buffer), file);
    fclose(file);
    free(buffer);
    return (written > 0 || history->count == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t storage_generate_message_id(char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid message id args");
    int64_t now_us = esp_timer_get_time();
    snprintf(out, out_size, "local-%" PRId64, now_us);
    return ESP_OK;
}

static esp_err_t storage_prepare_message(const fridge_storage_chat_message_t *input, fridge_storage_chat_message_t *out)
{
    ESP_RETURN_ON_FALSE(input && out, ESP_ERR_INVALID_ARG, TAG, "invalid message args");
    ESP_RETURN_ON_FALSE(storage_role_allowed(input->role), ESP_ERR_INVALID_ARG, TAG, "chat message role invalid");
    ESP_RETURN_ON_FALSE(storage_content_allowed(input->content), ESP_ERR_INVALID_ARG, TAG, "chat message content invalid");

    *out = *input;
    storage_copy_utf8_safe(out->content, sizeof(out->content), input->content);
    if (out->id[0] == '\0') {
        ESP_RETURN_ON_ERROR(storage_generate_message_id(out->id, sizeof(out->id)), TAG, "generate chat message id failed");
    }
    if (out->created_at <= 0) {
        out->created_at = storage_now_seconds();
    }
    return ESP_OK;
}

esp_err_t fridge_storage_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = storage_mount_cache_fs();
    ESP_RETURN_ON_ERROR(err, TAG, "mount cache littlefs failed");

    // 生产运行时的业务数据统一从 cache LittleFS 读取；这些默认内容只在文件缺失时落盘。
    // 库存以 UI 快照作为单一来源，避免屏幕显示和 AI 上下文读到两份不同库存。
    // 原子替换掉电后可能只留下 .bak。必须在“缺失时写种子”之前恢复，
    // 否则 ensure 会先创建新种子并掩盖用户库存备份。
    (void)storage_recover_ui_inventory_backup();
    ESP_RETURN_ON_ERROR(storage_ensure_text_file_exists(STORAGE_UI_INVENTORY_PATH, UI_INVENTORY_JSON),
                        TAG,
                        "init ui inventory file failed");
    ESP_RETURN_ON_ERROR(storage_ensure_text_file_exists(STORAGE_REMINDER_PATH, REMINDER_JSON),
                        TAG,
                        "init reminder queue file failed");
    ESP_RETURN_ON_ERROR(storage_ensure_text_file_exists(STORAGE_PREFERENCES_PATH, PREFERENCES_JSON),
                        TAG,
                        "init preferences file failed");
    ESP_RETURN_ON_ERROR(storage_ensure_text_file_exists(STORAGE_OFFLINE_QUEUE_PATH, OFFLINE_QUEUE_JSON),
                        TAG,
                        "init offline queue file failed");

    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate init history failed");

    err = storage_read_history_file(history);
    if (err == ESP_ERR_NOT_FOUND) {
        storage_fill_default_history(history);
        err = storage_write_history_file(history);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "chat history file invalid, recreating: %s", esp_err_to_name(err));
        storage_fill_default_history(history);
        err = storage_write_history_file(history);
    }
    free(history);
    ESP_RETURN_ON_ERROR(err, TAG, "init history file failed");

    char *ui_inventory = calloc(1, STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE);
    ESP_RETURN_ON_FALSE(ui_inventory, ESP_ERR_NO_MEM, TAG, "allocate ui inventory buffer failed");
    err = storage_read_text_file(STORAGE_UI_INVENTORY_PATH, ui_inventory, STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE);
    if (err == ESP_ERR_NOT_FOUND) {
        err = storage_write_text_file(STORAGE_UI_INVENTORY_PATH, UI_INVENTORY_JSON);
    } else if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(ui_inventory);
        if (!root) {
            ESP_LOGW(TAG, "ui inventory invalid, recreating");
            err = storage_write_text_file(STORAGE_UI_INVENTORY_PATH, UI_INVENTORY_JSON);
            s_inventory_version = 1;
        } else {
            const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
            const cJSON *source = cJSON_GetObjectItemCaseSensitive(root, "source");
            // 只升级仓库自带的旧参考种子；任何已经由 UI 写成 ui_p4_local 的真实库存
            // 都原样保留，避免固件升级覆盖用户的增删改结果。
            const bool old_reference_seed =
                cJSON_IsNumber(schema) && schema->valueint < 2 &&
                cJSON_IsString(source) && source->valuestring &&
                strcmp(source->valuestring, "ui_reference_seed") == 0;
            if (old_reference_seed) {
                err = storage_write_text_file(STORAGE_UI_INVENTORY_PATH,
                                              UI_INVENTORY_JSON);
                s_inventory_version = 1;
            } else {
                fridge_storage_inventory_sync_status_t status = {0};
                storage_fill_inventory_sync_status_from_root(root, &status);
                s_inventory_version =
                    status.snapshot_version > 0 ? status.snapshot_version : 1;
            }
        }
        cJSON_Delete(root);
    }
    free(ui_inventory);
    ESP_RETURN_ON_ERROR(err, TAG, "init ui inventory file failed");

    s_initialized = true;
    ESP_LOGI(TAG, "storage facade initialized, inventory_version=%lu", (unsigned long)s_inventory_version);
    return ESP_OK;
}

esp_err_t fridge_storage_get_inventory_snapshot(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_UI_INVENTORY_PATH, UI_INVENTORY_JSON, out, out_size);
}

esp_err_t fridge_storage_get_ui_inventory_snapshot(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    (void)storage_recover_ui_inventory_backup();
    esp_err_t err = storage_read_text_file(STORAGE_UI_INVENTORY_PATH, out, out_size);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(storage_write_text_file(STORAGE_UI_INVENTORY_PATH, UI_INVENTORY_JSON), TAG, "create ui inventory failed");
        return copy_json(UI_INVENTORY_JSON, out, out_size);
    }
    return err;
}

esp_err_t fridge_storage_set_ui_inventory_snapshot(const char *inventory_json)
{
    ESP_RETURN_ON_FALSE(inventory_json && inventory_json[0] == '{', ESP_ERR_INVALID_ARG, TAG, "ui inventory must be JSON object");
    ESP_RETURN_ON_FALSE(strlen(inventory_json) < STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE, ESP_ERR_INVALID_SIZE, TAG, "ui inventory too large");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    cJSON *root = cJSON_Parse(inventory_json);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "ui inventory json invalid");
    (void)storage_repair_inventory_root_quantities(root);
    fridge_storage_inventory_sync_status_t old_status = {0};
    cJSON *old_root = NULL;
    if (storage_load_ui_inventory_root(&old_root) == ESP_OK && old_root) {
        storage_fill_inventory_sync_status_from_root(old_root, &old_status);
    }
    cJSON_Delete(old_root);
    uint32_t next_snapshot = old_status.snapshot_version > 0 ? old_status.snapshot_version + 1 : s_inventory_version + 1;
    esp_err_t err = storage_update_inventory_sync_meta(root,
                                                       next_snapshot,
                                                       old_status.server_revision,
                                                       storage_now_ms(),
                                                       old_status.last_sync_at_ms,
                                                       true,
                                                       old_status.home_id,
                                                       old_status.device_id);
    if (err == ESP_OK) {
        err = storage_print_json_to_file(root);
    }
    cJSON_Delete(root);
    ESP_RETURN_ON_ERROR(err, TAG, "write ui inventory with sync meta failed");
    s_inventory_version = next_snapshot;
    return ESP_OK;
}

esp_err_t fridge_storage_restore_seed_inventory(void)
{
    // 演示恢复入口：只在串口调试/比赛恢复时调用，恢复后按本地用户修改处理并标记 dirty。
    return fridge_storage_set_ui_inventory_snapshot(UI_INVENTORY_JSON);
}

esp_err_t fridge_storage_repair_inventory_quantities(size_t *repaired_count)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    cJSON *root = NULL;
    ESP_RETURN_ON_ERROR(storage_load_ui_inventory_root(&root), TAG, "load inventory before repair failed");
    size_t count = storage_repair_inventory_root_quantities(root);
    esp_err_t err = ESP_OK;
    if (count > 0) {
        fridge_storage_inventory_sync_status_t status = {0};
        storage_fill_inventory_sync_status_from_root(root, &status);
        uint32_t next_snapshot = status.snapshot_version > 0 ? status.snapshot_version + 1 : s_inventory_version + 1;
        err = storage_update_inventory_sync_meta(root,
                                                 next_snapshot,
                                                 status.server_revision,
                                                 storage_now_ms(),
                                                 status.last_sync_at_ms,
                                                 true,
                                                 status.home_id,
                                                 status.device_id);
        if (err == ESP_OK) {
            err = storage_print_json_to_file(root);
        }
        if (err == ESP_OK) {
            s_inventory_version = next_snapshot;
        }
    }
    cJSON_Delete(root);
    if (repaired_count) {
        *repaired_count = count;
    }
    return err;
}

esp_err_t fridge_storage_get_inventory_sync_status(fridge_storage_inventory_sync_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "inventory sync status output is NULL");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    cJSON *root = NULL;
    ESP_RETURN_ON_ERROR(storage_load_ui_inventory_root(&root), TAG, "load inventory status root failed");
    storage_fill_inventory_sync_status_from_root(root, out);
    if (out->snapshot_version == 0) {
        out->snapshot_version = s_inventory_version;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t fridge_storage_get_inventory_snapshot_version(uint32_t *out_version)
{
    ESP_RETURN_ON_FALSE(out_version, ESP_ERR_INVALID_ARG, TAG, "inventory version output is NULL");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    *out_version = s_inventory_version;
    return ESP_OK;
}

esp_err_t fridge_storage_get_inventory_sync_payload(char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid inventory sync payload output");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_UI_INVENTORY_PATH, UI_INVENTORY_JSON, out, out_size);
}

esp_err_t fridge_storage_apply_inventory_cloud_snapshot(const char *inventory_json,
                                                        uint32_t server_revision,
                                                        const char *home_id,
                                                        const char *device_id,
                                                        bool *applied)
{
    ESP_RETURN_ON_FALSE(inventory_json && inventory_json[0] == '{', ESP_ERR_INVALID_ARG, TAG, "cloud inventory must be JSON object");
    ESP_RETURN_ON_FALSE(strlen(inventory_json) < STORAGE_UI_INVENTORY_FILE_BUFFER_SIZE, ESP_ERR_INVALID_SIZE, TAG, "cloud inventory too large");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    if (applied) {
        *applied = false;
    }

    cJSON *root = cJSON_Parse(inventory_json);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "cloud inventory json invalid");
    fridge_storage_inventory_sync_status_t local = {0};
    (void)fridge_storage_get_inventory_sync_status(&local);
    if (server_revision == 0 && local.updated_at_ms > 0) {
        // 云端还没有真实同步修订（server_revision==0）时，绝不允许默认/空快照覆盖设备本地数据。
        // 注意：这里不再要求 !local.dirty。即使本地是刚 restore_seed 的 dirty 演示数据，
        // 也必须保留，避免被仍运行旧版本、推送空/默认云端快照的后端把本地 20 件库存清空。
        cJSON_Delete(root);
        return ESP_OK;
    }
    if (!local.dirty && server_revision > 0 && server_revision <= local.server_revision) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    esp_err_t err = storage_preserve_local_custom_zones(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "preserve custom zones failed: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }
    (void)storage_repair_inventory_root_quantities(root);
    if (local.dirty) {
        // 设备本地 updatedAtMs 是开机相对毫秒，云端 updatedAtMs 是 Unix epoch 毫秒，二者不能比较。
        // 本地存在未确认修改时，只在云端回写与 UI 可见内容等价时清 dirty；不同内容一律保留设备快照继续上报。
        cJSON *local_root = NULL;
        err = storage_load_ui_inventory_root(&local_root);
        if (err != ESP_OK || !local_root) {
            cJSON_Delete(root);
            cJSON_Delete(local_root);
            return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
        }
        (void)storage_repair_inventory_root_quantities(local_root);
        if (!storage_inventory_documents_equivalent(local_root, root)) {
            ESP_LOGW(TAG,
                     "keep local dirty inventory; reject cloud overwrite server_rev=%" PRIu32 " local_rev=%" PRIu32,
                     server_revision,
                     local.server_revision);
            cJSON_Delete(local_root);
            cJSON_Delete(root);
            return ESP_OK;
        }
        uint32_t synced_revision = server_revision > local.server_revision ? server_revision : local.server_revision;
        err = storage_update_inventory_sync_meta(local_root,
                                                 local.snapshot_version > 0 ? local.snapshot_version : s_inventory_version,
                                                 synced_revision,
                                                 local.updated_at_ms,
                                                 storage_now_ms(),
                                                 false,
                                                 (home_id && home_id[0]) ? home_id : local.home_id,
                                                 (device_id && device_id[0]) ? device_id : local.device_id);
        if (err == ESP_OK) {
            err = storage_print_json_to_file(local_root);
        }
        cJSON_Delete(local_root);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mark equivalent dirty inventory synced failed: %s", esp_err_to_name(err));
            return err;
        }
        if (applied) {
            *applied = false;
        }
        return ESP_OK;
    }
    uint32_t next_snapshot = local.snapshot_version > 0 ? local.snapshot_version + 1 : s_inventory_version + 1;
    err = storage_update_inventory_sync_meta(root,
                                             next_snapshot,
                                             server_revision,
                                             storage_now_ms(),
                                             storage_now_ms(),
                                             false,
                                             (home_id && home_id[0]) ? home_id : local.home_id,
                                             (device_id && device_id[0]) ? device_id : local.device_id);
    if (err == ESP_OK) {
        err = storage_print_json_to_file(root);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply cloud inventory failed: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }
    s_inventory_version = next_snapshot;
    if (applied) {
        *applied = true;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t fridge_storage_mark_inventory_synced(uint32_t server_revision)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    cJSON *root = NULL;
    ESP_RETURN_ON_ERROR(storage_load_ui_inventory_root(&root), TAG, "load inventory before mark synced failed");
    fridge_storage_inventory_sync_status_t status = {0};
    storage_fill_inventory_sync_status_from_root(root, &status);
    uint32_t next_revision = server_revision > status.server_revision ? server_revision : status.server_revision;
    esp_err_t err = storage_update_inventory_sync_meta(root,
                                                       status.snapshot_version > 0 ? status.snapshot_version : s_inventory_version,
                                                       next_revision,
                                                       status.updated_at_ms,
                                                       storage_now_ms(),
                                                       false,
                                                       status.home_id,
                                                       status.device_id);
    if (err == ESP_OK) {
        err = storage_print_json_to_file(root);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t fridge_storage_get_reminder_queue(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_REMINDER_PATH, REMINDER_JSON, out, out_size);
}

esp_err_t fridge_storage_set_reminder_queue(const char *reminder_json)
{
    // 云端同步提醒确认状态时写入 LittleFS；仅由 MQTT/用户动作触发，避免高频磨损 Flash。
    return storage_write_json_document(STORAGE_REMINDER_PATH, reminder_json, FRIDGE_STORAGE_MAX_JSON_LEN);
}

esp_err_t fridge_storage_get_user_preferences(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_PREFERENCES_PATH, PREFERENCES_JSON, out, out_size);
}

esp_err_t fridge_storage_set_user_preferences(const char *preferences_json)
{
    // 用户偏好是 AI 菜谱/购物清单的重要上下文，允许云端手动同步覆盖本地缓存。
    return storage_write_json_document(STORAGE_PREFERENCES_PATH, preferences_json, FRIDGE_STORAGE_MAX_JSON_LEN);
}

esp_err_t fridge_storage_get_shopping_list(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    // 上次替换若在断电时停在“旧文件已改名”阶段，优先恢复可读的备份。
    FILE *current = fopen(STORAGE_SHOPPING_LIST_PATH, "rb");
    if (current) {
        fclose(current);
    } else {
        (void)rename(STORAGE_CACHE_BASE_PATH "/shopping_list.json.bak",
                     STORAGE_SHOPPING_LIST_PATH);
    }
    return storage_read_or_create_text_file(STORAGE_SHOPPING_LIST_PATH, SHOPPING_LIST_JSON, out, out_size);
}

esp_err_t fridge_storage_set_shopping_list(const char *shopping_json)
{
    // 购物清单可离线查看，云端恢复后通过 MQTT 写入本地 JSON 文档。
    return storage_write_shopping_json_atomic(shopping_json);
}

esp_err_t fridge_storage_get_recipe_cache(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_RECIPE_CACHE_PATH, RECIPE_CACHE_JSON, out, out_size);
}

esp_err_t fridge_storage_set_recipe_cache(const char *recipe_json)
{
    // 菜谱缓存来自 AI 推荐结果，保存为 JSON 文档供离线页和后续 UI 页面读取。
    return storage_write_json_document(STORAGE_RECIPE_CACHE_PATH, recipe_json, FRIDGE_STORAGE_MAX_JSON_LEN);
}

esp_err_t fridge_storage_get_nutrition_cache(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_NUTRITION_CACHE_PATH, NUTRITION_CACHE_JSON, out, out_size);
}

esp_err_t fridge_storage_set_nutrition_cache(const char *nutrition_json)
{
    // 营养助手缓存仅用于屏幕展示生活化建议，不写入库存，也不替代专业营养判断。
    return storage_write_json_document(STORAGE_NUTRITION_CACHE_PATH, nutrition_json, FRIDGE_STORAGE_MAX_JSON_LEN);
}

esp_err_t fridge_storage_get_kitchen_ai_meta(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_KITCHEN_AI_META_PATH, KITCHEN_AI_META_JSON, out, out_size);
}

esp_err_t fridge_storage_set_kitchen_ai_meta(const char *meta_json)
{
    // 每日生成节流状态写入 Flash，后台任务只在自动尝试或成功生成时更新，避免高频磨损。
    return storage_write_json_document(STORAGE_KITCHEN_AI_META_PATH, meta_json, FRIDGE_STORAGE_MAX_JSON_LEN);
}

esp_err_t fridge_storage_get_chat_history(fridge_storage_chat_history_t *out, size_t *pruned_count)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "chat history output is NULL");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate chat history failed");

    esp_err_t err = storage_read_history_file(history);
    if (err == ESP_ERR_NOT_FOUND) {
        storage_fill_default_history(history);
        err = storage_write_history_file(history);
    }
    if (err != ESP_OK) {
        free(history);
        ESP_RETURN_ON_ERROR(err, TAG, "read chat history failed");
    }

    size_t pruned = storage_prune_history_inplace(history, storage_time_ready());
    if (pruned > 0) {
        err = storage_write_history_file(history);
        if (err != ESP_OK) {
            free(history);
            ESP_RETURN_ON_ERROR(err, TAG, "persist pruned chat history failed");
        }
    }

    *out = *history;
    free(history);
    if (pruned_count) {
        *pruned_count = pruned;
    }
    return ESP_OK;
}

esp_err_t fridge_storage_append_chat_messages(const fridge_storage_chat_message_t *messages,
                                              size_t message_count,
                                              size_t *pruned_count)
{
    ESP_RETURN_ON_FALSE(messages && message_count > 0 && message_count <= FRIDGE_STORAGE_MAX_CHAT_MESSAGES,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "chat messages are invalid");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    if (!storage_time_ready()) {
        if (pruned_count) {
            *pruned_count = 0;
        }
        return ESP_ERR_INVALID_STATE;
    }

    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate append history failed");

    size_t pruned = 0;
    esp_err_t err = fridge_storage_get_chat_history(history, &pruned);
    if (err != ESP_OK) {
        free(history);
        ESP_RETURN_ON_ERROR(err, TAG, "get chat history before append failed");
    }

    // 单条消息接近 1KB，最多 30 条时本地数组会吃掉 20KB+ 任务栈；
    // USB/语音 AI 返回路径会马上写历史，必须放到堆上，避免栈破坏在 LittleFS flash yield 时暴露。
    fridge_storage_chat_message_t *prepared = calloc(message_count, sizeof(*prepared));
    if (!prepared) {
        free(history);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "allocate prepared messages failed");
    }
    for (size_t i = 0; i < message_count; i++) {
        fridge_storage_chat_message_t next = {0};
        err = storage_prepare_message(&messages[i], &next);
        if (err != ESP_OK) {
            free(prepared);
            free(history);
            ESP_RETURN_ON_ERROR(err, TAG, "prepare chat message failed");
        }

        prepared[i] = next;
        if (history->count < FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
            history->messages[history->count++] = next;
        } else {
            memmove(history->messages,
                    history->messages + 1,
                    (FRIDGE_STORAGE_MAX_CHAT_MESSAGES - 1) * sizeof(history->messages[0]));
            history->messages[FRIDGE_STORAGE_MAX_CHAT_MESSAGES - 1] = next;
            history->count = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
        }
    }

    pruned += storage_prune_history_inplace(history, true);
    history->updated_at = (uint32_t)storage_now_seconds();
    history->time_ready = true;
    err = storage_write_history_file(history);
    free(history);
    if (err != ESP_OK) {
        free(prepared);
        ESP_RETURN_ON_ERROR(err, TAG, "write chat history failed");
    }

    free(prepared);
    if (pruned_count) {
        *pruned_count = pruned;
    }
    return ESP_OK;
}

esp_err_t fridge_storage_append_chat_message(const fridge_storage_chat_message_t *message, size_t *pruned_count)
{
    return fridge_storage_append_chat_messages(message, 1, pruned_count);
}

esp_err_t fridge_storage_clear_chat_history(void)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    ESP_RETURN_ON_FALSE(history, ESP_ERR_NO_MEM, TAG, "allocate clear history failed");
    storage_fill_default_history(history);
    esp_err_t err = storage_write_history_file(history);
    free(history);
    // 清空本地历史时同步把上行游标归零，避免清空后残留游标把后续新对话误判为"已上报"而漏发。
    if (err == ESP_OK) {
        (void)fridge_storage_set_chat_synced_at(0);
    }
    return err;
}

esp_err_t fridge_storage_get_chat_synced_at(int64_t *out_ms)
{
    ESP_RETURN_ON_FALSE(out_ms, ESP_ERR_INVALID_ARG, TAG, "chat synced cursor output is NULL");
    *out_ms = 0;
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    nvs_handle_t handle;
    esp_err_t err = open_storage_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 命名空间尚未创建时视为"还没上报过"，返回 0 让首轮对话全量补发。
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open storage NVS for chat cursor failed");
    int64_t synced_at = 0;
    if (nvs_get_i64(handle, STORAGE_NVS_KEY_CHAT_SYNCED_AT, &synced_at) == ESP_OK && synced_at > 0) {
        *out_ms = synced_at;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t fridge_storage_set_chat_synced_at(int64_t ms)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_storage_nvs(NVS_READWRITE, &handle), TAG, "open storage NVS for chat cursor failed");
    esp_err_t err = nvs_set_i64(handle, STORAGE_NVS_KEY_CHAT_SYNCED_AT, ms > 0 ? ms : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t fridge_storage_get_chat_history_since(int64_t after_ms, fridge_storage_chat_history_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "chat history since output is NULL");

    // 复用全量读取（含过期/超量裁剪），再按 created_at 过滤出尚未上报的增量，保持原有顺序。
    fridge_storage_chat_history_t *full = calloc(1, sizeof(*full));
    ESP_RETURN_ON_FALSE(full, ESP_ERR_NO_MEM, TAG, "allocate chat history since buffer failed");
    esp_err_t err = fridge_storage_get_chat_history(full, NULL);
    if (err != ESP_OK) {
        free(full);
        ESP_RETURN_ON_ERROR(err, TAG, "read chat history for delta failed");
    }

    memset(out, 0, sizeof(*out));
    out->schema_version = full->schema_version;
    out->updated_at = full->updated_at;
    out->ttl_seconds = full->ttl_seconds;
    out->max_messages = full->max_messages;
    out->time_ready = full->time_ready;
    out->count = 0;
    for (size_t i = 0; i < full->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        if (full->messages[i].created_at > after_ms && full->messages[i].content[0] != '\0') {
            out->messages[out->count++] = full->messages[i];
        }
    }
    free(full);
    return ESP_OK;
}

esp_err_t fridge_storage_get_offline_queue_summary(char *out, size_t out_size)
{
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");
    return storage_read_or_create_text_file(STORAGE_OFFLINE_QUEUE_PATH, OFFLINE_QUEUE_JSON, out, out_size);
}

esp_err_t fridge_storage_get_status(fridge_storage_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "storage status output is NULL");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init failed");

    memset(out, 0, sizeof(*out));
    out->cache_ready = true;
    out->assets_ready = false;
    out->inventory_version = s_inventory_version;
    fridge_storage_inventory_sync_status_t inventory_sync = {0};
    if (fridge_storage_get_inventory_sync_status(&inventory_sync) == ESP_OK) {
        out->inventory_version = inventory_sync.snapshot_version;
        out->inventory_server_revision = inventory_sync.server_revision;
        out->inventory_last_sync_at_ms = inventory_sync.last_sync_at_ms;
        out->inventory_dirty = inventory_sync.dirty;
    }
    strlcpy(out->cache_note,
            "当前 storage facade：cache LittleFS 保存结构化记忆、库存、提醒、偏好、离线队列、UI 快照和 48 小时短期会话历史。",
            sizeof(out->cache_note));
    return ESP_OK;
}
