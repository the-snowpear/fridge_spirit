// 冰箱小精灵厨房 AI 后台生成组件。
// 负责每天或手动触发一次真实 AI 生成，把菜谱、营养建议和购物清单拆成三个本地 JSON 缓存。
// 硬件注意：本组件不配置 GPIO、不改变屏幕/摄像头/音频时钟；HTTPS 期间 Wi-Fi 峰值仍要求稳定 5V/2A 供电。

#include "fridge_kitchen_ai.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_network.h"
#include "fridge_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define KITCHEN_AI_TASK_STACK 6144
#define KITCHEN_AI_TASK_PRIORITY 3
#define KITCHEN_AI_QUEUE_LEN 2
#define KITCHEN_AI_AUTO_TARGET_HOUR 8
#define KITCHEN_AI_AUTO_CHECK_MS (30 * 60 * 1000)
#define KITCHEN_AI_AUTO_LATE_DELAY_MS (2 * 60 * 1000)
#define KITCHEN_AI_AUTO_RETRY_MS (30 * 60 * 1000)
#define KITCHEN_AI_AUTO_MAX_ATTEMPTS 3
#define KITCHEN_AI_EPOCH_READY_THRESHOLD 1735689600LL
#define KITCHEN_AI_FREE_INTERNAL_MIN_BYTES (8 * 1024)
#define KITCHEN_AI_LARGEST_INTERNAL_MIN_BYTES (4 * 1024)

static const char *TAG = "fridge_kitchen_ai";

typedef struct {
    fridge_kitchen_ai_trigger_t trigger;
    fridge_kitchen_ai_page_t page;
} kitchen_ai_job_t;

typedef struct {
    int64_t last_auto_day;
    int64_t auto_attempt_day;
    uint8_t auto_attempt_count;
    int64_t last_attempt_at;
    int64_t last_generated_at;
    char last_error[FRIDGE_KITCHEN_AI_MAX_ERROR_LEN + 1];
} kitchen_ai_meta_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static fridge_kitchen_ai_status_t s_status;
static int64_t s_next_auto_check_ms;
static int64_t s_first_ready_ms;
static bool s_initialized;

static void *kitchen_ai_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    }
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool wall_time_ready(void)
{
    return time(NULL) >= KITCHEN_AI_EPOCH_READY_THRESHOLD;
}

static int64_t today_key(void)
{
    time_t now = time(NULL);
    if (now < KITCHEN_AI_EPOCH_READY_THRESHOLD) {
        return 0;
    }
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    return ((int64_t)(tm_now.tm_year + 1900) * 10000) + ((int64_t)(tm_now.tm_mon + 1) * 100) + tm_now.tm_mday;
}

static int today_hour(void)
{
    time_t now = time(NULL);
    if (now < KITCHEN_AI_EPOCH_READY_THRESHOLD) {
        return -1;
    }
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    return tm_now.tm_hour;
}

static bool network_ready(void)
{
    fridge_network_status_t net = {0};
    return fridge_network_get_status(&net) == ESP_OK && net.connected && net.internet_ready;
}

static const char *trigger_text(fridge_kitchen_ai_trigger_t trigger)
{
    return trigger == FRIDGE_KITCHEN_AI_TRIGGER_MANUAL ? "manual" : "auto";
}

const char *fridge_kitchen_ai_page_text(fridge_kitchen_ai_page_t page)
{
    switch (page) {
    case FRIDGE_KITCHEN_AI_PAGE_RECIPE:
        return "recipe";
    case FRIDGE_KITCHEN_AI_PAGE_NUTRITION:
        return "nutrition";
    case FRIDGE_KITCHEN_AI_PAGE_SHOPPING:
        return "shopping";
    case FRIDGE_KITCHEN_AI_PAGE_ALL:
    default:
        return "all";
    }
}

static void set_status_running(fridge_kitchen_ai_trigger_t trigger, fridge_kitchen_ai_page_t page)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_status.running = true;
        s_status.state = FRIDGE_KITCHEN_AI_STATE_RUNNING;
        s_status.last_trigger = trigger;
        s_status.current_page = page;
        s_status.last_error[0] = '\0';
        s_status.http_status = 0;
        s_status.latency_ms = 0;
        xSemaphoreGive(s_lock);
    }
}

static void set_status_done(bool ok,
                            fridge_kitchen_ai_trigger_t trigger,
                            fridge_kitchen_ai_page_t page,
                            int64_t generated_at,
                            int http_status,
                            uint32_t latency_ms,
                            const char *error)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_status.running = false;
        s_status.last_ok = ok;
        s_status.state = ok ? FRIDGE_KITCHEN_AI_STATE_OK : FRIDGE_KITCHEN_AI_STATE_ERROR;
        s_status.last_trigger = trigger;
        s_status.current_page = FRIDGE_KITCHEN_AI_PAGE_ALL;
        s_status.last_page = page;
        if (ok && generated_at > 0) {
            s_status.last_generated_at = generated_at;
        }
        s_status.http_status = http_status;
        s_status.latency_ms = latency_ms;
        strlcpy(s_status.last_error, error ? error : "", sizeof(s_status.last_error));
        xSemaphoreGive(s_lock);
    }
}

static void set_status_auto_day(int64_t day)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_status.last_auto_day = day;
        xSemaphoreGive(s_lock);
    }
}

static void set_status_reply_preview(const char *reply)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_status.last_reply_len = reply ? strlen(reply) : 0;
        if (reply && reply[0]) {
            strlcpy(s_status.last_reply_preview, reply, sizeof(s_status.last_reply_preview));
        } else {
            s_status.last_reply_preview[0] = '\0';
        }
        xSemaphoreGive(s_lock);
    }
}

static int64_t json_i64(const cJSON *root, const char *key, int64_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : fallback;
}

static void json_copy_string(const cJSON *root, const char *key, char *out, size_t out_size)
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

static char *safe_json_string_alloc(const char *text, const char *fallback, size_t max_bytes)
{
    const char *src = (text && text[0]) ? text : (fallback ? fallback : "");
    size_t len = utf8_safe_prefix_len(src, max_bytes);
    char *out = calloc(1, len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, src, len);
    return out;
}

static void load_meta(kitchen_ai_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    char *json = kitchen_ai_calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (!json) {
        return;
    }
    if (fridge_storage_get_kitchen_ai_meta(json, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        cJSON *root = cJSON_Parse(json);
        if (root) {
            meta->last_auto_day = json_i64(root, "last_auto_day", 0);
            meta->auto_attempt_day = json_i64(root, "auto_attempt_day", 0);
            meta->auto_attempt_count = (uint8_t)json_i64(root, "auto_attempt_count", 0);
            meta->last_attempt_at = json_i64(root, "last_attempt_at", 0);
            meta->last_generated_at = json_i64(root, "last_generated_at", 0);
            json_copy_string(root, "last_error", meta->last_error, sizeof(meta->last_error));
            cJSON_Delete(root);
        }
    }
    free(json);
}

static esp_err_t save_meta(const kitchen_ai_meta_t *meta)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create meta root failed");
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddNumberToObject(root, "last_auto_day", (double)meta->last_auto_day);
    cJSON_AddNumberToObject(root, "auto_attempt_day", (double)meta->auto_attempt_day);
    cJSON_AddNumberToObject(root, "auto_attempt_count", meta->auto_attempt_count);
    cJSON_AddNumberToObject(root, "last_attempt_at", (double)meta->last_attempt_at);
    cJSON_AddNumberToObject(root, "last_generated_at", (double)meta->last_generated_at);
    cJSON_AddStringToObject(root, "last_error", meta->last_error);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print meta failed");
    esp_err_t err = fridge_storage_set_kitchen_ai_meta(printed);
    cJSON_free(printed);
    return err;
}

static bool root_has_kitchen_schema(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    return cJSON_GetObjectItemCaseSensitive(root, "recipes") &&
           cJSON_GetObjectItemCaseSensitive(root, "nutrition") &&
           cJSON_GetObjectItemCaseSensitive(root, "shopping");
}

static bool root_has_page_schema(const cJSON *root, fridge_kitchen_ai_page_t page)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    if (page == FRIDGE_KITCHEN_AI_PAGE_RECIPE) {
        return cJSON_GetObjectItemCaseSensitive(root, "items") || cJSON_GetObjectItemCaseSensitive(root, "recipes");
    }
    if (page == FRIDGE_KITCHEN_AI_PAGE_NUTRITION) {
        return cJSON_GetObjectItemCaseSensitive(root, "nutrition");
    }
    if (page == FRIDGE_KITCHEN_AI_PAGE_SHOPPING) {
        return cJSON_GetObjectItemCaseSensitive(root, "items") || cJSON_GetObjectItemCaseSensitive(root, "shopping");
    }
    return root_has_kitchen_schema(root);
}

static void log_reply_preview(const char *reply)
{
    char preview[241];
    size_t written = 0;
    if (!reply) {
        ESP_LOGW(TAG, "AI kitchen reply preview: (null)");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)reply; *p && written + 1 < sizeof(preview); p++) {
        unsigned char ch = *p;
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
        preview[written++] = (char)ch;
    }
    preview[written] = '\0';
    ESP_LOGW(TAG, "AI kitchen reply preview len=%u: %s", (unsigned)strlen(reply), preview);
}

static cJSON *parse_kitchen_json_from_reply(const char *reply)
{
    if (!reply || !reply[0]) {
        return NULL;
    }

    // 先尝试整段解析：JSON mode 正常时这里最快成功。
    cJSON *root = cJSON_Parse(reply);
    if (root) {
        if (root_has_kitchen_schema(root)) {
            return root;
        }
        cJSON_Delete(root);
    }

    // 有些兼容服务会把 JSON object 当作 JSON string 再包一层："{\"schema_version\":1,...}"。
    // 这里先解开外层字符串，再按同一套逻辑解析内部文本。
    root = cJSON_Parse(reply);
    if (cJSON_IsString(root) && root->valuestring) {
        cJSON *inner = parse_kitchen_json_from_reply(root->valuestring);
        cJSON_Delete(root);
        if (inner) {
            ESP_LOGW(TAG, "accepted kitchen JSON from quoted AI reply");
            return inner;
        }
    } else if (root) {
        cJSON_Delete(root);
    }

    // 再兼容模型输出 Markdown 代码块、前后解释文字、或混入其它 JSON 片段的情况。
    const char *scan = reply;
    while ((scan = strchr(scan, '{')) != NULL) {
        const char *end = NULL;
        root = cJSON_ParseWithLengthOpts(scan, strlen(scan), &end, false);
        if (root) {
            if (root_has_kitchen_schema(root)) {
                ESP_LOGW(TAG, "accepted kitchen JSON embedded in AI reply, offset=%u", (unsigned)(scan - reply));
                return root;
            }
            cJSON_Delete(root);
        }
        scan++;
    }
    return NULL;
}

static cJSON *parse_page_json_from_reply(const char *reply, fridge_kitchen_ai_page_t page)
{
    if (page == FRIDGE_KITCHEN_AI_PAGE_ALL) {
        return parse_kitchen_json_from_reply(reply);
    }
    if (!reply || !reply[0]) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(reply);
    if (root) {
        if (root_has_page_schema(root, page)) {
            return root;
        }
        if (cJSON_IsString(root) && root->valuestring) {
            cJSON *inner = parse_page_json_from_reply(root->valuestring, page);
            cJSON_Delete(root);
            if (inner) {
                return inner;
            }
        } else {
            cJSON_Delete(root);
        }
    }

    const char *scan = reply;
    while ((scan = strchr(scan, '{')) != NULL) {
        const char *end = NULL;
        root = cJSON_ParseWithLengthOpts(scan, strlen(scan), &end, false);
        if (root) {
            if (root_has_page_schema(root, page)) {
                return root;
            }
            cJSON_Delete(root);
        }
        scan++;
    }
    return NULL;
}

static cJSON *dup_string_array(const cJSON *source, size_t max_items, const char *fallback)
{
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }
    size_t count = 0;
    if (cJSON_IsArray(source)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, source) {
            if (count >= max_items) {
                break;
            }
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
                char *safe = safe_json_string_alloc(item->valuestring, "", 96);
                if (!safe) {
                    cJSON_Delete(array);
                    return NULL;
                }
                cJSON_AddItemToArray(array, cJSON_CreateString(safe));
                free(safe);
                count++;
            }
        }
    }
    if (count == 0 && fallback && fallback[0]) {
        cJSON_AddItemToArray(array, cJSON_CreateString(fallback));
    }
    return array;
}

static const char *json_string_or(const cJSON *root, const char *key, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0] ? item->valuestring : fallback;
}

static int json_int_or(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool required_string_present(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0';
}

static esp_err_t validate_generated_root(const cJSON *root)
{
    ESP_RETURN_ON_FALSE(cJSON_IsObject(root), ESP_ERR_INVALID_RESPONSE, TAG, "AI root is not object");
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (schema) {
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(schema) && schema->valueint == 1,
                            ESP_ERR_INVALID_RESPONSE,
                            TAG,
                            "AI schema_version invalid");
    }

    const cJSON *recipes = cJSON_GetObjectItemCaseSensitive(root, "recipes");
    const cJSON *nutrition = cJSON_GetObjectItemCaseSensitive(root, "nutrition");
    const cJSON *shopping = cJSON_GetObjectItemCaseSensitive(root, "shopping");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(recipes) && cJSON_GetArraySize(recipes) > 0,
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "AI recipes missing");
    ESP_RETURN_ON_FALSE(cJSON_IsObject(nutrition), ESP_ERR_INVALID_RESPONSE, TAG, "AI nutrition missing");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(shopping), ESP_ERR_INVALID_RESPONSE, TAG, "AI shopping missing");

    const cJSON *recipe = NULL;
    cJSON_ArrayForEach(recipe, recipes) {
        // 本地构建缓存时会对 tag/reason/use_inventory/missing 等非核心字段补默认值；
        // 这里只校验页面必须有的最小信息，避免模型少一个展示字段就丢弃整次生成。
        ESP_RETURN_ON_FALSE(cJSON_IsObject(recipe) && required_string_present(recipe, "title"),
                            ESP_ERR_INVALID_RESPONSE,
                            TAG,
                            "AI recipe item invalid");
    }
    ESP_RETURN_ON_FALSE(required_string_present(nutrition, "summary"),
                        ESP_ERR_INVALID_RESPONSE,
                        TAG,
                        "AI nutrition fields invalid");

    const cJSON *shopping_item = NULL;
    cJSON_ArrayForEach(shopping_item, shopping) {
        ESP_RETURN_ON_FALSE(cJSON_IsObject(shopping_item) && required_string_present(shopping_item, "name"),
                            ESP_ERR_INVALID_RESPONSE,
                            TAG,
                            "AI shopping item invalid");
    }
    return ESP_OK;
}

static esp_err_t add_safe_string(cJSON *object, const char *key, const char *value, const char *fallback, size_t max_bytes)
{
    char *safe = safe_json_string_alloc(value, fallback, max_bytes);
    ESP_RETURN_ON_FALSE(safe, ESP_ERR_NO_MEM, TAG, "alloc safe string failed");
    cJSON *item = cJSON_CreateString(safe);
    free(safe);
    ESP_RETURN_ON_FALSE(item, ESP_ERR_NO_MEM, TAG, "create safe string failed");
    cJSON_AddItemToObject(object, key, item);
    return ESP_OK;
}

static cJSON *build_recipe_doc(const cJSON *root, int64_t generated_at)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(doc, "items");
    if (!doc || !items) {
        cJSON_Delete(doc);
        return NULL;
    }
    cJSON_AddNumberToObject(doc, "schema_version", 1);
    cJSON_AddStringToObject(doc, "source", "device_ai");
    cJSON_AddNumberToObject(doc, "generated_at", (double)generated_at);

    const cJSON *recipes = cJSON_GetObjectItemCaseSensitive(root, "recipes");
    if (!cJSON_IsArray(recipes)) {
        recipes = cJSON_GetObjectItemCaseSensitive(root, "items");
    }
    const cJSON *recipe = NULL;
    size_t count = 0;
    cJSON_ArrayForEach(recipe, recipes) {
        if (count >= 3 || !cJSON_IsObject(recipe)) {
            break;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(doc);
            return NULL;
        }
        if (add_safe_string(item, "title", json_string_or(recipe, "title", "AI 推荐菜"), "AI 推荐菜", 72) != ESP_OK ||
            add_safe_string(item, "tag", json_string_or(recipe, "tag", "AI 推荐"), "AI 推荐", 48) != ESP_OK ||
            add_safe_string(item, "reason", json_string_or(recipe, "reason", "根据当前库存生成"), "根据当前库存生成", 160) != ESP_OK) {
            cJSON_Delete(item);
            cJSON_Delete(doc);
            return NULL;
        }
        cJSON_AddNumberToObject(item, "time_minutes", json_int_or(recipe, "time_minutes", 20));
        cJSON_AddItemToObject(item, "use_inventory", dup_string_array(cJSON_GetObjectItemCaseSensitive(recipe, "use_inventory"), 6, "按当前库存选择"));
        cJSON_AddItemToObject(item, "missing", dup_string_array(cJSON_GetObjectItemCaseSensitive(recipe, "missing"), 5, ""));
        cJSON_AddItemToObject(item, "steps", dup_string_array(cJSON_GetObjectItemCaseSensitive(recipe, "steps"), 5, "按日常做法完成烹饪"));
        if (!cJSON_GetObjectItemCaseSensitive(item, "use_inventory") ||
            !cJSON_GetObjectItemCaseSensitive(item, "missing") ||
            !cJSON_GetObjectItemCaseSensitive(item, "steps")) {
            cJSON_Delete(item);
            cJSON_Delete(doc);
            return NULL;
        }
        cJSON_AddItemToArray(items, item);
        count++;
    }
    return doc;
}

static cJSON *build_nutrition_doc(const cJSON *root, int64_t generated_at)
{
    const cJSON *nutrition = cJSON_GetObjectItemCaseSensitive(root, "nutrition");
    cJSON *doc = cJSON_CreateObject();
    if (!doc) {
        return NULL;
    }
    cJSON_AddNumberToObject(doc, "schema_version", 1);
    cJSON_AddStringToObject(doc, "source", "device_ai");
    cJSON_AddNumberToObject(doc, "generated_at", (double)generated_at);
    cJSON *body = cJSON_AddObjectToObject(doc, "nutrition");
    if (!body) {
        cJSON_Delete(doc);
        return NULL;
    }
    cJSON_AddNumberToObject(body, "score", json_int_or(nutrition, "score", 0));
    if (add_safe_string(body, "summary", json_string_or(nutrition, "summary", "尚未生成有效营养建议"), "尚未生成有效营养建议", 220) != ESP_OK ||
        add_safe_string(body, "restrict_note", json_string_or(nutrition, "restrict_note", "仅供日常膳食参考"), "仅供日常膳食参考", 180) != ESP_OK) {
        cJSON_Delete(doc);
        return NULL;
    }
    cJSON_AddItemToObject(body, "focus", dup_string_array(cJSON_GetObjectItemCaseSensitive(nutrition, "focus"), 4, "按库存补齐膳食结构"));
    cJSON_AddItemToObject(body, "suggestions", dup_string_array(cJSON_GetObjectItemCaseSensitive(nutrition, "suggestions"), 5, "优先消耗临期食材"));
    if (!cJSON_GetObjectItemCaseSensitive(body, "focus") || !cJSON_GetObjectItemCaseSensitive(body, "suggestions")) {
        cJSON_Delete(doc);
        return NULL;
    }
    return doc;
}

static cJSON *build_shopping_doc(const cJSON *root, int64_t generated_at)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(doc, "items");
    if (!doc || !items) {
        cJSON_Delete(doc);
        return NULL;
    }
    cJSON_AddNumberToObject(doc, "schema_version", 1);
    cJSON_AddStringToObject(doc, "source", "device_ai");
    cJSON_AddNumberToObject(doc, "generated_at", (double)generated_at);

    const cJSON *shopping = cJSON_GetObjectItemCaseSensitive(root, "shopping");
    if (!cJSON_IsArray(shopping)) {
        shopping = cJSON_GetObjectItemCaseSensitive(root, "items");
    }
    const cJSON *src_item = NULL;
    size_t count = 0;
    cJSON_ArrayForEach(src_item, shopping) {
        if (count >= 8 || !cJSON_IsObject(src_item)) {
            break;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(doc);
            return NULL;
        }
        if (add_safe_string(item, "name", json_string_or(src_item, "name", "待补货"), "待补货", 48) != ESP_OK ||
            add_safe_string(item, "quantity", json_string_or(src_item, "quantity", "1份"), "1份", 32) != ESP_OK ||
            add_safe_string(item, "reason", json_string_or(src_item, "reason", "AI 采购建议"), "AI 采购建议", 120) != ESP_OK ||
            add_safe_string(item, "priority", json_string_or(src_item, "priority", count < 3 ? "high" : "normal"), "normal", 16) != ESP_OK) {
            cJSON_Delete(item);
            cJSON_Delete(doc);
            return NULL;
        }
        const cJSON *checked = cJSON_GetObjectItemCaseSensitive(src_item, "checked");
        cJSON_AddBoolToObject(item, "checked", cJSON_IsBool(checked) ? cJSON_IsTrue(checked) : false);
        // PLAN-06 兼容 schema v1：AI 未显式分组的条目固定进入“建议购买”，
        // 不把 AI 输出误当作用户已经确认的待买或已购买项目。
        char id[24];
        snprintf(id, sizeof(id), "ai-%lld-%u", (long long)generated_at, (unsigned)(count + 1U));
        cJSON_AddStringToObject(item, "id", id);
        cJSON_AddStringToObject(item, "group", "建议购买");
        cJSON_AddStringToObject(item, "source", "device_ai");
        cJSON_AddStringToObject(item, "relation", "");
        cJSON_AddItemToArray(items, item);
        count++;
    }
    return doc;
}

static esp_err_t print_and_store(cJSON *doc, esp_err_t (*store_fn)(const char *));

// AI 只追加或更新建议项，不能覆盖用户手动添加、待购买和已购买记录。
// 存储层负责对最终文档做原子替换；这里先在内存 DOM 中完成有界合并。
static esp_err_t merge_and_store_generated_shopping(cJSON *generated)
{
    if (!generated) return ESP_ERR_INVALID_ARG;
    char stored[FRIDGE_STORAGE_MAX_JSON_LEN] = {0};
    esp_err_t read_err = fridge_storage_get_shopping_list(stored, sizeof(stored));
    if (read_err != ESP_OK) return read_err;
    cJSON *current = cJSON_Parse(stored);
    cJSON *current_items = current ? cJSON_GetObjectItemCaseSensitive(current, "items") : NULL;
    cJSON *generated_items = cJSON_GetObjectItemCaseSensitive(generated, "items");
    if (!current || !cJSON_IsObject(current) || !cJSON_IsArray(current_items) ||
        !cJSON_IsArray(generated_items)) {
        cJSON_Delete(current);
        return print_and_store(generated, fridge_storage_set_shopping_list);
    }
    cJSON *source = cJSON_GetObjectItemCaseSensitive(current, "source");
    if (!cJSON_IsString(source)) cJSON_AddStringToObject(current, "source", "local");
    cJSON *candidate = NULL;
    cJSON_ArrayForEach(candidate, generated_items) {
        if (!cJSON_IsObject(candidate) || cJSON_GetArraySize(current_items) >= 8) break;
        const cJSON *candidate_name = cJSON_GetObjectItemCaseSensitive(candidate, "name");
        if (!cJSON_IsString(candidate_name) || !candidate_name->valuestring) continue;
        cJSON *existing = NULL;
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, current_items) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            if (cJSON_IsString(name) && name->valuestring &&
                strcmp(name->valuestring, candidate_name->valuestring) == 0) {
                existing = entry;
                break;
            }
        }
        if (existing) {
            const cJSON *group = cJSON_GetObjectItemCaseSensitive(existing, "group");
            // 用户已经采纳或完成的同名项目不被 AI 重置。
            if (cJSON_IsString(group) && strcmp(group->valuestring, "建议购买") != 0) continue;
            cJSON_DeleteItemFromObject(existing, "reason");
            cJSON_AddItemToObject(existing, "reason", cJSON_Duplicate(
                cJSON_GetObjectItemCaseSensitive(candidate, "reason"), true));
            continue;
        }
        cJSON *copy = cJSON_Duplicate(candidate, true);
        if (!copy) { cJSON_Delete(current); return ESP_ERR_NO_MEM; }
        cJSON_AddItemToArray(current_items, copy);
    }
    esp_err_t err = print_and_store(current, fridge_storage_set_shopping_list);
    cJSON_Delete(current);
    return err;
}

static esp_err_t print_and_store(cJSON *doc, esp_err_t (*store_fn)(const char *))
{
    ESP_RETURN_ON_FALSE(doc && store_fn, ESP_ERR_INVALID_ARG, TAG, "invalid store args");
    char *printed = cJSON_PrintUnformatted(doc);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print generated doc failed");
    esp_err_t err = store_fn(printed);
    cJSON_free(printed);
    return err;
}

static esp_err_t store_generated_payload(const char *reply, int64_t generated_at)
{
    cJSON *root = parse_kitchen_json_from_reply(reply);
    if (!root) {
        log_reply_preview(reply);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG, "AI reply has no valid kitchen JSON object");
    }
    esp_err_t validate_err = validate_generated_root(root);
    if (validate_err != ESP_OK) {
        log_reply_preview(reply);
        cJSON_Delete(root);
        return validate_err;
    }

    cJSON *recipe_doc = build_recipe_doc(root, generated_at);
    cJSON *nutrition_doc = build_nutrition_doc(root, generated_at);
    cJSON *shopping_doc = build_shopping_doc(root, generated_at);
    cJSON_Delete(root);
    if (!recipe_doc || !nutrition_doc || !shopping_doc) {
        cJSON_Delete(recipe_doc);
        cJSON_Delete(nutrition_doc);
        cJSON_Delete(shopping_doc);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = print_and_store(recipe_doc, fridge_storage_set_recipe_cache);
    if (err == ESP_OK) {
        err = print_and_store(nutrition_doc, fridge_storage_set_nutrition_cache);
    }
    if (err == ESP_OK) {
        err = merge_and_store_generated_shopping(shopping_doc);
    }
    cJSON_Delete(recipe_doc);
    cJSON_Delete(nutrition_doc);
    cJSON_Delete(shopping_doc);
    return err;
}

static esp_err_t store_generated_page_payload(const char *reply, int64_t generated_at, fridge_kitchen_ai_page_t page)
{
    if (page == FRIDGE_KITCHEN_AI_PAGE_ALL) {
        return store_generated_payload(reply, generated_at);
    }

    cJSON *root = parse_page_json_from_reply(reply, page);
    if (!root) {
        log_reply_preview(reply);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_RESPONSE, TAG, "AI reply has no valid page JSON object");
    }

    cJSON *doc = NULL;
    esp_err_t (*store_fn)(const char *) = NULL;
    if (page == FRIDGE_KITCHEN_AI_PAGE_RECIPE) {
        doc = build_recipe_doc(root, generated_at);
        store_fn = fridge_storage_set_recipe_cache;
    } else if (page == FRIDGE_KITCHEN_AI_PAGE_NUTRITION) {
        doc = build_nutrition_doc(root, generated_at);
        store_fn = fridge_storage_set_nutrition_cache;
    } else if (page == FRIDGE_KITCHEN_AI_PAGE_SHOPPING) {
        doc = build_shopping_doc(root, generated_at);
        store_fn = fridge_storage_set_shopping_list;
    }
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(doc && store_fn, ESP_ERR_NO_MEM, TAG, "build page doc failed");
    esp_err_t err = page == FRIDGE_KITCHEN_AI_PAGE_SHOPPING ?
                        merge_and_store_generated_shopping(doc) : print_and_store(doc, store_fn);
    cJSON_Delete(doc);
    return err;
}

static void update_auto_attempt(kitchen_ai_meta_t *meta, int64_t day, int64_t now_sec, const char *error)
{
    if (meta->auto_attempt_day != day) {
        meta->auto_attempt_day = day;
        meta->auto_attempt_count = 0;
    }
    if (meta->auto_attempt_count < UINT8_MAX) {
        meta->auto_attempt_count++;
    }
    meta->last_attempt_at = now_sec;
    strlcpy(meta->last_error, error ? error : "", sizeof(meta->last_error));
    (void)save_meta(meta);
}

static const char *page_task_type(fridge_kitchen_ai_page_t page)
{
    switch (page) {
    case FRIDGE_KITCHEN_AI_PAGE_RECIPE:
        return "kitchen_recipe";
    case FRIDGE_KITCHEN_AI_PAGE_NUTRITION:
        return "kitchen_nutrition";
    case FRIDGE_KITCHEN_AI_PAGE_SHOPPING:
        return "kitchen_shopping";
    case FRIDGE_KITCHEN_AI_PAGE_ALL:
    default:
        return "recipe_generate";
    }
}

static const char *page_user_text(fridge_kitchen_ai_page_t page)
{
    switch (page) {
    case FRIDGE_KITCHEN_AI_PAGE_RECIPE:
        return "generate recipe page cache";
    case FRIDGE_KITCHEN_AI_PAGE_NUTRITION:
        return "generate nutrition page cache";
    case FRIDGE_KITCHEN_AI_PAGE_SHOPPING:
        return "generate shopping page cache";
    case FRIDGE_KITCHEN_AI_PAGE_ALL:
    default:
        return "generate recipe nutrition shopping pages";
    }
}

static const char *page_prompt(fridge_kitchen_ai_page_t page)
{
    switch (page) {
    case FRIDGE_KITCHEN_AI_PAGE_RECIPE:
        return "只输出菜谱页 JSON，禁止 Markdown/解释/代码块。根对象必须是：{\"schema_version\":1,\"generated_at\":0,\"items\":[{\"title\":\"番茄蛋汤\",\"tag\":\"快手\",\"time_minutes\":15,\"use_inventory\":[\"番茄\"],\"missing\":[],\"steps\":[\"洗切食材\",\"煮熟调味\"],\"reason\":\"优先消耗库存\"}]}。严格只生成 1 道菜；title 10字内；tag 6字内；use_inventory 最多3项；missing 最多2项；steps 固定2条且每条10字内；reason 16字内。只基于库存/临期/偏好，缺少食材写 missing，不编造库存。";
    case FRIDGE_KITCHEN_AI_PAGE_NUTRITION:
        return "只输出营养助手页 JSON，禁止 Markdown/解释/代码块。根对象：{\"schema_version\":1,\"generated_at\":0,\"nutrition\":{\"score\":80,\"summary\":\"库存蔬菜偏少\",\"focus\":[\"补蔬菜\"],\"suggestions\":[\"晚餐加绿叶菜\"],\"restrict_note\":\"仅供日常参考\"}}。summary 32字内；focus 1-3条；suggestions 1-3条；restrict_note 必须说明非医疗建议；只基于库存、偏好、忌口和临期提醒。";
    case FRIDGE_KITCHEN_AI_PAGE_SHOPPING:
        return "只输出购物清单页 JSON，禁止 Markdown/解释/代码块。根对象：{\"schema_version\":1,\"generated_at\":0,\"items\":[{\"name\":\"青菜\",\"quantity\":\"1把\",\"reason\":\"补充蔬菜\",\"priority\":\"high\",\"checked\":false}]}。items 可以为空数组，最多 8 项；priority 只能 high/normal/low；checked 必须 false；只采购补齐菜谱或营养缺口所需，不重复库存已有且充足的食材。";
    case FRIDGE_KITCHEN_AI_PAGE_ALL:
    default:
        return "任务：为 ESP32 冰箱屏幕一次性生成菜谱页、营养助手页、购物清单页的缓存内容。只输出 UTF-8 JSON object，禁止 Markdown、解释、代码块、前后缀。必须完全使用这些字段名：schema_version,generated_at,recipes,nutrition,shopping,title,tag,time_minutes,use_inventory,missing,steps,reason,score,summary,focus,suggestions,restrict_note,name,quantity,priority,checked。根对象固定：{\"schema_version\":1,\"generated_at\":0,\"recipes\":[{\"title\":\"番茄蛋汤\",\"tag\":\"快手\",\"time_minutes\":15,\"use_inventory\":[\"番茄\"],\"missing\":[],\"steps\":[\"洗切食材\",\"加热烹饪\"],\"reason\":\"优先消耗库存\"}],\"nutrition\":{\"score\":80,\"summary\":\"库存蔬菜偏少\",\"focus\":[\"补蔬菜\"],\"suggestions\":[\"晚餐加绿叶菜\"],\"restrict_note\":\"仅供日常参考\"},\"shopping\":[{\"name\":\"青菜\",\"quantity\":\"1把\",\"reason\":\"补充蔬菜\",\"priority\":\"high\",\"checked\":false}]}。recipes 必须 1-2 项；steps 至少 2 条；nutrition.focus 和 nutrition.suggestions 各 1-3 条；shopping 可以为空数组，最多 8 项；priority 只能 high、normal 或 low；checked 必须是 JSON boolean。内容必须基于项目上下文里的库存、临期、偏好和记忆；库存缺失时写 missing，不要编造已经有的库存。";
    }
}

static esp_err_t generate_now(fridge_kitchen_ai_trigger_t trigger, fridge_kitchen_ai_page_t page)
{
    set_status_running(trigger, page);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_internal < KITCHEN_AI_FREE_INTERNAL_MIN_BYTES || largest_internal < KITCHEN_AI_LARGEST_INTERNAL_MIN_BYTES) {
        ESP_LOGW(TAG,
                 "skip kitchen AI due to low internal heap: free=%u largest=%u",
                 (unsigned)free_internal,
                 (unsigned)largest_internal);
        set_status_done(false, trigger, page, 0, 0, 0, "内部 SRAM 不足，已取消 AI 生成");
        return ESP_ERR_NO_MEM;
    }
    kitchen_ai_meta_t meta = {0};
    load_meta(&meta);

    const int64_t day = today_key();
    const int64_t now_sec = (int64_t)time(NULL);
    if (trigger == FRIDGE_KITCHEN_AI_TRIGGER_AUTO) {
        update_auto_attempt(&meta, day, now_sec, "");
    }

    fridge_ai_config_update_t *config = kitchen_ai_calloc(1, sizeof(*config));
    fridge_ai_context_preview_t *preview = kitchen_ai_calloc(1, sizeof(*preview));
    fridge_ai_assistant_result_t *result = kitchen_ai_calloc(1, sizeof(*result));
    fridge_ai_task_request_t *context_req = kitchen_ai_calloc(1, sizeof(*context_req));
    fridge_ai_assistant_request_t *ai_req = kitchen_ai_calloc(1, sizeof(*ai_req));
    if (!config || !preview || !result || !context_req || !ai_req) {
        free(config);
        free(preview);
        free(result);
        free(context_req);
        free(ai_req);
        set_status_done(false, trigger, page, 0, 0, 0, "厨房 AI 内存不足");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err == ESP_OK) {
        err = fridge_ai_client_ensure_network_ready(result->chat.error, sizeof(result->chat.error));
    }
    if (err != ESP_OK) {
        const char *error = result->chat.error[0] ? result->chat.error : esp_err_to_name(err);
        strlcpy(meta.last_error, error, sizeof(meta.last_error));
        (void)save_meta(&meta);
        set_status_done(false, trigger, page, 0, result->chat.http_status, result->chat.latency_ms, error);
        free(config);
        free(preview);
        free(result);
        free(context_req);
        free(ai_req);
        return err;
    }

    strlcpy(context_req->task_type, page_task_type(page), sizeof(context_req->task_type));
    strlcpy(context_req->user_text, page_user_text(page), sizeof(context_req->user_text));
    context_req->include_inventory = true;
    context_req->include_reminders = true;
    context_req->include_preferences = true;
    // 单页生成优先保证 JSON 完整性：菜谱/营养/购物按钮只需要库存、提醒和偏好。
    // 记忆摘要里会带会话历史预览，部分兼容 API 容易因此提前截断菜谱 JSON。
    err = fridge_ai_context_build_preview(context_req, preview);
    if (err != ESP_OK) {
        set_status_done(false, trigger, page, 0, 0, 0, "构建厨房 AI 上下文失败");
        free(config);
        free(preview);
        free(result);
        free(context_req);
        free(ai_req);
        return err;
    }

    strlcpy(ai_req->task_type, page_task_type(page), sizeof(ai_req->task_type));
    strlcpy(ai_req->context_json, preview->preview_json, sizeof(ai_req->context_json));
    ai_req->preloaded_config = config;
    ai_req->context_injected = true;
    ai_req->needs_confirmation = false;
    ai_req->local_snapshot_version = preview->local_snapshot_version;
    strlcpy(ai_req->message, page_prompt(page), sizeof(ai_req->message));

    err = fridge_ai_client_assistant_chat_with_config(config, ai_req, result);
    set_status_reply_preview(result->chat.reply);
    if (err == ESP_OK) {
        err = store_generated_page_payload(result->chat.reply, now_sec > 0 ? now_sec : (mono_ms() / 1000), page);
    }

    if (err == ESP_OK) {
        meta.last_generated_at = now_sec > 0 ? now_sec : (mono_ms() / 1000);
        meta.last_error[0] = '\0';
        if (trigger == FRIDGE_KITCHEN_AI_TRIGGER_AUTO && page != FRIDGE_KITCHEN_AI_PAGE_ALL) {
            // 自动全量任务由 kitchen_ai_task 在三页全部成功后统一写 last_auto_day；
            // 这里先只记录最近成功时间，避免菜谱成功但营养/购物失败后当天不再重试。
            meta.last_auto_day = 0;
        }
        (void)save_meta(&meta);
        set_status_done(true, trigger, page, meta.last_generated_at, result->chat.http_status, result->chat.latency_ms, "");
        ESP_LOGI(TAG, "kitchen AI generated by %s page=%s, http=%d latency=%lu ms",
                 trigger_text(trigger),
                 fridge_kitchen_ai_page_text(page),
                 result->chat.http_status,
                 (unsigned long)result->chat.latency_ms);
    } else {
        const char *error = result->chat.error[0] ? result->chat.error :
                            (err == ESP_ERR_INVALID_RESPONSE ? "AI 返回格式不符合厨房页面 JSON" : esp_err_to_name(err));
        strlcpy(meta.last_error, error, sizeof(meta.last_error));
        (void)save_meta(&meta);
        set_status_done(false, trigger, page, 0, result->chat.http_status, result->chat.latency_ms, error);
        ESP_LOGW(TAG, "kitchen AI generate failed by %s page=%s: %s", trigger_text(trigger), fridge_kitchen_ai_page_text(page), error);
    }

    free(config);
    free(preview);
    free(result);
    free(context_req);
    free(ai_req);
    return err;
}

static bool should_auto_generate(void)
{
    int64_t now = mono_ms();
    if (now < s_next_auto_check_ms) {
        return false;
    }
    s_next_auto_check_ms = now + KITCHEN_AI_AUTO_CHECK_MS;

    if (!wall_time_ready() || !network_ready()) {
        s_first_ready_ms = 0;
        return false;
    }
    if (today_hour() < KITCHEN_AI_AUTO_TARGET_HOUR) {
        s_first_ready_ms = 0;
        return false;
    }
    if (s_first_ready_ms == 0) {
        s_first_ready_ms = now;
        s_next_auto_check_ms = now + (30 * 1000);
        return false;
    }
    if (now - s_first_ready_ms < KITCHEN_AI_AUTO_LATE_DELAY_MS) {
        s_next_auto_check_ms = now + (30 * 1000);
        return false;
    }

    kitchen_ai_meta_t meta = {0};
    load_meta(&meta);
    int64_t day = today_key();
    if (day == 0 || meta.last_auto_day == day) {
        return false;
    }
    if (meta.auto_attempt_day != day) {
        return true;
    }
    if (meta.auto_attempt_count >= KITCHEN_AI_AUTO_MAX_ATTEMPTS) {
        return false;
    }
    if (time(NULL) - meta.last_attempt_at < KITCHEN_AI_AUTO_RETRY_MS / 1000) {
        return false;
    }
    return true;
}

static void kitchen_ai_task(void *arg)
{
    (void)arg;
    kitchen_ai_job_t job = {0};
    while (true) {
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(30000)) == pdTRUE) {
            if (job.page == FRIDGE_KITCHEN_AI_PAGE_ALL) {
                esp_err_t err = generate_now(job.trigger, FRIDGE_KITCHEN_AI_PAGE_RECIPE);
                if (err == ESP_OK) {
                    err = generate_now(job.trigger, FRIDGE_KITCHEN_AI_PAGE_NUTRITION);
                }
                if (err == ESP_OK) {
                    err = generate_now(job.trigger, FRIDGE_KITCHEN_AI_PAGE_SHOPPING);
                }
                (void)err;
            } else {
                (void)generate_now(job.trigger, job.page);
            }
            continue;
        }
        if (should_auto_generate()) {
            const int64_t day = today_key();
            esp_err_t err = generate_now(FRIDGE_KITCHEN_AI_TRIGGER_AUTO, FRIDGE_KITCHEN_AI_PAGE_RECIPE);
            if (err == ESP_OK) {
                err = generate_now(FRIDGE_KITCHEN_AI_TRIGGER_AUTO, FRIDGE_KITCHEN_AI_PAGE_NUTRITION);
            }
            if (err == ESP_OK) {
                err = generate_now(FRIDGE_KITCHEN_AI_TRIGGER_AUTO, FRIDGE_KITCHEN_AI_PAGE_SHOPPING);
            }
            if (err == ESP_OK && day > 0) {
                kitchen_ai_meta_t meta = {0};
                load_meta(&meta);
                meta.last_auto_day = day;
                meta.last_error[0] = '\0';
                (void)save_meta(&meta);
                set_status_auto_day(day);
            }
        }
    }
}

esp_err_t fridge_kitchen_ai_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_LOGI(TAG,
             "kitchen AI init heap: internal_free=%u largest=%u spiram_free=%u stack=%u caps=INTERNAL",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)KITCHEN_AI_TASK_STACK);
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create lock failed");
    s_queue = xQueueCreate(KITCHEN_AI_QUEUE_LEN, sizeof(kitchen_ai_job_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "create queue failed");
    s_status.state = FRIDGE_KITCHEN_AI_STATE_IDLE;
    s_next_auto_check_ms = 0;

    kitchen_ai_meta_t meta = {0};
    load_meta(&meta);
    s_status.last_auto_day = meta.last_auto_day;
    s_status.last_generated_at = meta.last_generated_at;
    strlcpy(s_status.last_error, meta.last_error, sizeof(s_status.last_error));

    // 本任务会读取 NVS/LittleFS 并写缓存，Flash 事务期间 cache 可能短暂不可用；
    // 因此执行栈必须留在内部 SRAM，大块请求/响应结构仍在函数内优先放 PSRAM。
    BaseType_t ok = xTaskCreate(kitchen_ai_task,
                                "kitchen_ai",
                                KITCHEN_AI_TASK_STACK,
                                NULL,
                                KITCHEN_AI_TASK_PRIORITY,
                                &s_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create kitchen ai task failed");
    s_initialized = true;
    ESP_LOGI(TAG, "kitchen AI initialized");
    return ESP_OK;
}

esp_err_t fridge_kitchen_ai_generate_async(fridge_kitchen_ai_trigger_t trigger)
{
    return fridge_kitchen_ai_generate_page_async(trigger, FRIDGE_KITCHEN_AI_PAGE_ALL);
}

esp_err_t fridge_kitchen_ai_generate_page_async(fridge_kitchen_ai_trigger_t trigger, fridge_kitchen_ai_page_t page)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_queue, ESP_ERR_INVALID_STATE, TAG, "kitchen AI not initialized");
    ESP_RETURN_ON_FALSE(page >= FRIDGE_KITCHEN_AI_PAGE_ALL && page <= FRIDGE_KITCHEN_AI_PAGE_SHOPPING,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "kitchen AI page invalid");
    if (fridge_kitchen_ai_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_queue && uxQueueMessagesWaiting(s_queue) > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    kitchen_ai_job_t job = {.trigger = trigger, .page = page};
    return xQueueSend(s_queue, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t fridge_kitchen_ai_get_status(fridge_kitchen_ai_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "status output is NULL");
    if (!s_initialized || !s_lock) {
        memset(out, 0, sizeof(*out));
        out->state = FRIDGE_KITCHEN_AI_STATE_IDLE;
        strlcpy(out->last_error, "厨房 AI 尚未初始化", sizeof(out->last_error));
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "status lock timeout");
    *out = s_status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool fridge_kitchen_ai_is_running(void)
{
    bool running = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        running = s_status.running;
        xSemaphoreGive(s_lock);
    }
    return running;
}

const char *fridge_kitchen_ai_state_text(fridge_kitchen_ai_state_t state)
{
    switch (state) {
    case FRIDGE_KITCHEN_AI_STATE_RUNNING:
        return "running";
    case FRIDGE_KITCHEN_AI_STATE_OK:
        return "ok";
    case FRIDGE_KITCHEN_AI_STATE_ERROR:
        return "error";
    case FRIDGE_KITCHEN_AI_STATE_IDLE:
    default:
        return "idle";
    }
}
