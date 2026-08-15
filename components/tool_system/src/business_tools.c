// 冰箱小精灵业务写入 Tool 实现。
//
// 本文件只操作 /sdcard/agent/data 与 /sdcard/agent/MEMORY.md，不触碰 GPIO、时钟、电源或外设。
// 所有副作用必须经过 Tool Broker 的 WRITE/DANGEROUS 权限判断；在 confirm 模式下写入会等待用户确认。

#include "fridge_business_tools.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_agent_storage.h"
#include "fridge_kitchen_tools.h"
#include "fridge_task_scheduler.h"
#include "fridge_tfcard.h"
#include "fridge_tool_registry.h"

#define BUSINESS_JSON_MAX_BYTES (64U * 1024U)
#define BUSINESS_MEMORY_MD_MAX_BYTES (64U * 1024U)
#define INVENTORY_PATH "agent/data/inventory.json"
#define TASKS_PATH "agent/data/tasks.json"
#define MEMORY_JSON_PATH "agent/data/memory.json"
#define MEMORY_MD_PATH "agent/MEMORY.md"

static const char *TAG = "fridge_business_tools";
static uint32_t s_local_sequence;

#define BUSINESS_MAX_APPLIED_OPERATIONS 64
#define STOPWATCH_LAP_IDEMPOTENCY_CACHE_SIZE 32U

// 秒表计次是运行态副作用，不能因为 Agent/USB 重试而重复插入记录。
// 这里保留最近 32 个幂等键；秒表复位或设备重启后，新的计次会话重新开始。
typedef struct {
    bool valid;
    uint32_t generation;
    char key[64];
    uint64_t elapsed_ms;
    size_t lap_count;
    bool dropped_oldest;
} stopwatch_lap_idempotency_entry_t;

static stopwatch_lap_idempotency_entry_t s_stopwatch_lap_cache[STOPWATCH_LAP_IDEMPOTENCY_CACHE_SIZE];
static uint32_t s_stopwatch_lap_generation = 1;
static size_t s_stopwatch_lap_cache_next;

static void *business_alloc(size_t bytes)
{
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(1, bytes);
    return ptr;
}

static void result_error(fridge_tool_result_t *result, const char *code, const char *summary)
{
    result->status = FRIDGE_TOOL_RESULT_ERROR;
    strlcpy(result->code, code ? code : "ERROR", sizeof(result->code));
    strlcpy(result->summary, summary ? summary : "业务工具执行失败", sizeof(result->summary));
    strlcpy(result->data_json, "{}", sizeof(result->data_json));
}

static void result_ok_json(fridge_tool_result_t *result, const char *summary, cJSON *data)
{
    result->status = FRIDGE_TOOL_RESULT_OK;
    strlcpy(result->code, "SUCCESS", sizeof(result->code));
    strlcpy(result->summary, summary ? summary : "业务工具执行成功", sizeof(result->summary));
    char *printed = data ? cJSON_PrintUnformatted(data) : NULL;
    if (!printed) {
        result->status = FRIDGE_TOOL_RESULT_ERROR;
        strlcpy(result->code, "NO_MEM", sizeof(result->code));
        strlcpy(result->summary, "序列化业务 Tool 结果失败", sizeof(result->summary));
        strlcpy(result->data_json, "{}", sizeof(result->data_json));
        return;
    }
    if (strlen(printed) >= sizeof(result->data_json)) {
        snprintf(result->data_json,
                 sizeof(result->data_json),
                 "{\"truncated\":true,\"bytes\":%u}",
                 (unsigned)strlen(printed));
    } else {
        strlcpy(result->data_json, printed, sizeof(result->data_json));
    }
    cJSON_free(printed);
}

static const cJSON *json_get_ci(const cJSON *object, const char *name)
{
    return object && name ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
}

static const char *json_string_value(const cJSON *object, const char *name)
{
    const cJSON *item = json_get_ci(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_number_value(const cJSON *object, const char *name, double *out)
{
    const cJSON *item = json_get_ci(object, name);
    if (!out || !cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static bool json_u32_value(const cJSON *object, const char *name, uint32_t *out)
{
    const cJSON *item = json_get_ci(object, name);
    if (!out || !cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static void get_idempotency_key(const fridge_tool_call_t *call, const cJSON *args, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (call && call->idempotency_key[0]) {
        strlcpy(out, call->idempotency_key, out_size);
        return;
    }
    const char *key = json_string_value(args, "idempotencyKey");
    if (!key) {
        key = json_string_value(args, "idempotency_key");
    }
    if (key) {
        strlcpy(out, key, out_size);
    }
}

static cJSON *parse_arguments(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    const char *text = (call && call->arguments_json[0]) ? call->arguments_json : "{}";
    cJSON *args = cJSON_Parse(text);
    if (!args || !cJSON_IsObject(args)) {
        if (args) {
            cJSON_Delete(args);
        }
        result_error(result, "INVALID_JSON", "工具参数必须是 JSON object 字符串");
        return NULL;
    }
    return args;
}

// 读取秒表实时快照。厨房工具组件内部会按单调时钟折算 running 状态下的毫秒数，
// Tool 层只返回结构化数据，不直接访问 LVGL 或修改页面对象。
static esp_err_t read_stopwatch_snapshot(fridge_kitchen_tools_snapshot_t *snapshot,
                                          fridge_tool_result_t *result)
{
    esp_err_t err = fridge_kitchen_tools_get_snapshot(snapshot);
    if (err != ESP_OK) {
        result_error(result, esp_err_to_name(err), "读取秒表状态失败");
    }
    return err;
}

// 将秒表快照转换为 Agent 可消费的毫秒级 JSON；elapsed_seconds 不作为 Agent 主字段。
static cJSON *stopwatch_snapshot_to_json(const fridge_kitchen_stopwatch_snapshot_t *stopwatch)
{
    if (!stopwatch) {
        return NULL;
    }
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return NULL;
    }
    cJSON_AddStringToObject(data, "state", fridge_kitchen_stopwatch_state_text(stopwatch->state));
    cJSON_AddBoolToObject(data, "running", stopwatch->state == FRIDGE_KITCHEN_STOPWATCH_RUNNING);
    cJSON_AddNumberToObject(data, "elapsedMs", (double)stopwatch->elapsed_ms);
    cJSON_AddNumberToObject(data, "lapCount", (double)stopwatch->lap_count);
    cJSON_AddNumberToObject(data, "maxLaps", (double)FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS);

    cJSON *laps = cJSON_AddArrayToObject(data, "lapsMs");
    if (!laps) {
        cJSON_Delete(data);
        return NULL;
    }
    for (size_t i = 0; i < stopwatch->lap_count && i < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS; i++) {
        cJSON_AddItemToArray(laps, cJSON_CreateNumber((double)stopwatch->laps_ms[i]));
    }
    return data;
}

static stopwatch_lap_idempotency_entry_t *find_stopwatch_lap_cache(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (size_t i = 0; i < STOPWATCH_LAP_IDEMPOTENCY_CACHE_SIZE; i++) {
        stopwatch_lap_idempotency_entry_t *entry = &s_stopwatch_lap_cache[i];
        if (entry->valid && entry->generation == s_stopwatch_lap_generation &&
            strcmp(entry->key, key) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void remember_stopwatch_lap(const char *key, uint64_t elapsed_ms,
                                   size_t lap_count, bool dropped_oldest)
{
    if (!key || !key[0]) {
        return;
    }
    stopwatch_lap_idempotency_entry_t *entry =
        &s_stopwatch_lap_cache[s_stopwatch_lap_cache_next % STOPWATCH_LAP_IDEMPOTENCY_CACHE_SIZE];
    s_stopwatch_lap_cache_next = (s_stopwatch_lap_cache_next + 1U) % STOPWATCH_LAP_IDEMPOTENCY_CACHE_SIZE;
    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    entry->generation = s_stopwatch_lap_generation;
    strlcpy(entry->key, key, sizeof(entry->key));
    entry->elapsed_ms = elapsed_ms;
    entry->lap_count = lap_count;
    entry->dropped_oldest = dropped_oldest;
}

static void clear_stopwatch_lap_cache(void)
{
    memset(s_stopwatch_lap_cache, 0, sizeof(s_stopwatch_lap_cache));
    s_stopwatch_lap_cache_next = 0;
    s_stopwatch_lap_generation++;
    if (s_stopwatch_lap_generation == 0) {
        s_stopwatch_lap_generation = 1;
    }
}

static esp_err_t tool_stopwatch_status(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(args);

    fridge_kitchen_tools_snapshot_t snapshot;
    esp_err_t err = read_stopwatch_snapshot(&snapshot, result);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *data = stopwatch_snapshot_to_json(&snapshot.stopwatch);
    if (!data) {
        result_error(result, "NO_MEM", "创建秒表状态结果失败");
        return ESP_ERR_NO_MEM;
    }
    result_ok_json(result, "已读取秒表状态", data);
    cJSON_Delete(data);
    return ESP_OK;
}

static esp_err_t tool_stopwatch_start(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(args);

    fridge_kitchen_tools_snapshot_t before;
    esp_err_t err = read_stopwatch_snapshot(&before, result);
    if (err != ESP_OK) {
        return err;
    }
    const bool already_running = before.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING;
    const bool resumed = before.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_PAUSED;
    if (!already_running) {
        err = fridge_kitchen_tools_stopwatch_start();
        if (err != ESP_OK) {
            result_error(result, esp_err_to_name(err), "开始或继续秒表失败");
            return err;
        }
    }

    fridge_kitchen_tools_snapshot_t after;
    err = read_stopwatch_snapshot(&after, result);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *data = stopwatch_snapshot_to_json(&after.stopwatch);
    if (!data) {
        result_error(result, "NO_MEM", "创建秒表开始结果失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(data, "alreadyRunning", already_running);
    cJSON_AddBoolToObject(data, "resumed", resumed);
    result_ok_json(result, already_running ? "秒表已经在计时" : resumed ? "秒表已继续" : "秒表已开始", data);
    cJSON_Delete(data);
    return ESP_OK;
}

static esp_err_t tool_stopwatch_pause(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(args);

    fridge_kitchen_tools_snapshot_t before;
    esp_err_t err = read_stopwatch_snapshot(&before, result);
    if (err != ESP_OK) {
        return err;
    }
    if (before.stopwatch.state != FRIDGE_KITCHEN_STOPWATCH_RUNNING &&
        before.stopwatch.state != FRIDGE_KITCHEN_STOPWATCH_PAUSED) {
        result_error(result, "STOPWATCH_NOT_RUNNING", "秒表当前没有可暂停的计时");
        return ESP_ERR_INVALID_STATE;
    }
    const bool already_paused = before.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_PAUSED;
    if (!already_paused) {
        err = fridge_kitchen_tools_stopwatch_pause();
        if (err != ESP_OK) {
            result_error(result, esp_err_to_name(err), "暂停秒表失败");
            return err;
        }
    }

    fridge_kitchen_tools_snapshot_t after;
    err = read_stopwatch_snapshot(&after, result);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *data = stopwatch_snapshot_to_json(&after.stopwatch);
    if (!data) {
        result_error(result, "NO_MEM", "创建秒表暂停结果失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(data, "alreadyPaused", already_paused);
    result_ok_json(result, already_paused ? "秒表已经暂停" : "秒表已暂停", data);
    cJSON_Delete(data);
    return ESP_OK;
}

static esp_err_t tool_stopwatch_lap(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) {
        return ESP_ERR_INVALID_ARG;
    }

    char idempotency_key[64] = {0};
    get_idempotency_key(call, args, idempotency_key, sizeof(idempotency_key));
    if (!idempotency_key[0]) {
        cJSON_Delete(args);
        result_error(result, "IDEMPOTENCY_KEY_REQUIRED", "stopwatch.lap 需要稳定的幂等键");
        return ESP_ERR_INVALID_ARG;
    }

    fridge_kitchen_tools_snapshot_t before;
    esp_err_t err = read_stopwatch_snapshot(&before, result);
    if (err != ESP_OK) {
        cJSON_Delete(args);
        return err;
    }
    if (before.stopwatch.elapsed_ms == 0) {
        cJSON_Delete(args);
        result_error(result, "STOPWATCH_NOT_STARTED", "秒表尚未产生可记录的计时");
        return ESP_ERR_INVALID_STATE;
    }

    stopwatch_lap_idempotency_entry_t *cached = find_stopwatch_lap_cache(idempotency_key);
    // 外部复位后旧缓存不能再次命中；新的会话必须使用新的幂等键。
    if (cached && before.stopwatch.lap_count < cached->lap_count) {
        cached->valid = false;
        cached = NULL;
    }
    if (cached) {
        cJSON *data = cJSON_CreateObject();
        if (!data) {
            cJSON_Delete(args);
            result_error(result, "NO_MEM", "创建重复计次结果失败");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(data, "duplicate", true);
        cJSON_AddStringToObject(data, "idempotencyKey", cached->key);
        cJSON_AddNumberToObject(data, "newestLapMs", (double)cached->elapsed_ms);
        cJSON_AddNumberToObject(data, "lapCount", (double)cached->lap_count);
        cJSON_AddBoolToObject(data, "droppedOldest", cached->dropped_oldest);
        result_ok_json(result, "该计次请求已执行，返回原结果", data);
        cJSON_Delete(data);
        cJSON_Delete(args);
        return ESP_OK;
    }

    const bool dropped_oldest = before.stopwatch.lap_count >= FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS;
    err = fridge_kitchen_tools_stopwatch_lap();
    if (err != ESP_OK) {
        cJSON_Delete(args);
        result_error(result, esp_err_to_name(err), "记录秒表计次失败");
        return err;
    }

    fridge_kitchen_tools_snapshot_t after;
    err = read_stopwatch_snapshot(&after, result);
    if (err != ESP_OK || after.stopwatch.lap_count == 0) {
        cJSON_Delete(args);
        if (err == ESP_OK) {
            result_error(result, "LAP_RESULT_INVALID", "计次已执行但读取结果失败");
            return ESP_ERR_INVALID_STATE;
        }
        return err;
    }
    const uint64_t newest_lap_ms = after.stopwatch.laps_ms[0];
    remember_stopwatch_lap(idempotency_key, newest_lap_ms, after.stopwatch.lap_count, dropped_oldest);

    cJSON *data = stopwatch_snapshot_to_json(&after.stopwatch);
    if (!data) {
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "创建秒表计次结果失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(data, "duplicate", false);
    cJSON_AddStringToObject(data, "idempotencyKey", idempotency_key);
    cJSON_AddNumberToObject(data, "newestLapMs", (double)newest_lap_ms);
    cJSON_AddBoolToObject(data, "droppedOldest", dropped_oldest);
    result_ok_json(result, "已记录秒表计次", data);
    cJSON_Delete(data);
    cJSON_Delete(args);
    return ESP_OK;
}

static esp_err_t tool_stopwatch_reset(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(args);

    fridge_kitchen_tools_snapshot_t before;
    esp_err_t err = read_stopwatch_snapshot(&before, result);
    if (err != ESP_OK) {
        return err;
    }
    err = fridge_kitchen_tools_stopwatch_reset();
    if (err != ESP_OK) {
        result_error(result, esp_err_to_name(err), "复位秒表失败");
        return err;
    }
    clear_stopwatch_lap_cache();

    fridge_kitchen_tools_snapshot_t after;
    err = read_stopwatch_snapshot(&after, result);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *data = stopwatch_snapshot_to_json(&after.stopwatch);
    if (!data) {
        result_error(result, "NO_MEM", "创建秒表复位结果失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(data, "clearedElapsedMs", (double)before.stopwatch.elapsed_ms);
    cJSON_AddNumberToObject(data, "clearedLapCount", (double)before.stopwatch.lap_count);
    result_ok_json(result, "秒表已复位并清空计次", data);
    cJSON_Delete(data);
    return ESP_OK;
}

static cJSON *read_json_root(const char *path, const char *fallback_json, fridge_tool_result_t *result)
{
    (void)fallback_json;
    char *text = business_alloc(BUSINESS_JSON_MAX_BYTES + 1);
    if (!text) {
        result_error(result, "NO_MEM", "分配业务 JSON 读取缓冲失败");
        return NULL;
    }

    size_t n = 0;
    esp_err_t err = fridge_agent_storage_json_read(path,
                                                   text,
                                                   BUSINESS_JSON_MAX_BYTES + 1,
                                                   &n,
                                                   NULL);
    if (err != ESP_OK) {
        free(text);
        result_error(result, esp_err_to_name(err), "读取 TF 卡业务 JSON 文件失败");
        return NULL;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        result_error(result, "INVALID_STORE_JSON", "TF 卡业务 JSON 文件格式无效");
        return NULL;
    }
    const char *array_name = strcmp(path, INVENTORY_PATH) == 0 ? "items" :
                             strcmp(path, TASKS_PATH) == 0 ? "tasks" :
                             strcmp(path, MEMORY_JSON_PATH) == 0 ? "items" : NULL;
    if (array_name && !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, array_name))) {
        cJSON_Delete(root);
        result_error(result, "INVALID_STORE_SCHEMA", "TF 卡业务 JSON 缺少必需数组字段");
        return NULL;
    }
    return root;
}

static esp_err_t write_json_root(const char *path, cJSON *root, fridge_tool_result_t *result)
{
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        result_error(result, "NO_MEM", "序列化业务 JSON 失败");
        return ESP_ERR_NO_MEM;
    }
    const size_t len = strlen(printed);
    if (len > BUSINESS_JSON_MAX_BYTES) {
        cJSON_free(printed);
        result_error(result, "STORE_TOO_LARGE", "业务 JSON 文件超过当前 64KB 保护上限");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = fridge_agent_storage_json_write(path, printed, len);
    cJSON_free(printed);
    if (err != ESP_OK) {
        result_error(result, esp_err_to_name(err), "写入 TF 卡业务 JSON 文件失败");
    }
    return err;
}

static cJSON *ensure_array(cJSON *root, const char *name)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsArray(array) ? array : NULL;
}

static cJSON *ensure_operations(cJSON *root)
{
    cJSON *operations = cJSON_GetObjectItemCaseSensitive(root, "applied_operations");
    if (cJSON_IsArray(operations)) return operations;
    if (operations) cJSON_DeleteItemFromObjectCaseSensitive(root, "applied_operations");
    operations = cJSON_CreateArray();
    if (operations) cJSON_AddItemToObject(root, "applied_operations", operations);
    return operations;
}

static bool operation_already_applied(cJSON *root, const char *tool_name,
                                      const char *key, fridge_tool_result_t *result)
{
    if (!key || !key[0]) return false;
    cJSON *operations = ensure_operations(root);
    if (!operations) return false;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, operations) {
        const char *entry_tool = json_string_value(entry, "tool");
        const char *entry_key = json_string_value(entry, "key");
        if (entry_tool && entry_key && strcmp(entry_tool, tool_name) == 0 && strcmp(entry_key, key) == 0) {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddBoolToObject(data, "duplicate", true);
            cJSON_AddStringToObject(data, "idempotencyKey", key);
            result_ok_json(result, "幂等操作已执行，返回既有结果", data);
            cJSON_Delete(data);
            return true;
        }
    }
    return false;
}

static esp_err_t record_operation(cJSON *root, const char *tool_name, const char *key, const char *target_id)
{
    if (!key || !key[0]) return ESP_OK;
    cJSON *operations = ensure_operations(root);
    if (!operations) return ESP_ERR_NO_MEM;
    while (cJSON_GetArraySize(operations) >= BUSINESS_MAX_APPLIED_OPERATIONS) {
        cJSON_DeleteItemFromArray(operations, 0);
    }
    cJSON *entry = cJSON_CreateObject();
    if (!entry) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(entry, "tool", tool_name);
    cJSON_AddStringToObject(entry, "key", key);
    if (target_id) cJSON_AddStringToObject(entry, "target_id", target_id);
    cJSON_AddNumberToObject(entry, "appliedMonotonicMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddItemToArray(operations, entry);
    return ESP_OK;
}

static uint32_t idempotency_hash(const char *tool_name, const char *key)
{
    uint32_t hash = 2166136261U;
    const char *parts[] = {tool_name ? tool_name : "", "|", key ? key : ""};
    for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
        for (const unsigned char *s = (const unsigned char *)parts[p]; *s; s++) {
            hash = (hash ^ *s) * 16777619U;
        }
    }
    return hash;
}

static int64_t current_epoch_ms(bool *ready)
{
    const time_t now = time(NULL);
    const bool valid = (int64_t)now >= 1700000000LL;
    if (ready) *ready = valid;
    return valid ? (int64_t)now * 1000LL : 0;
}

static cJSON *find_object_by_string(cJSON *array, const char *field, const char *value, int *out_index)
{
    if (!cJSON_IsArray(array) || !field || !value || !value[0]) {
        return NULL;
    }
    int index = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const cJSON *field_value = cJSON_GetObjectItemCaseSensitive(item, field);
        if (cJSON_IsString(field_value) && strcmp(field_value->valuestring, value) == 0) {
            if (out_index) {
                *out_index = index;
            }
            return item;
        }
        index++;
    }
    return NULL;
}

static void make_id(const char *prefix, char *out, size_t out_size)
{
    s_local_sequence++;
    snprintf(out,
             out_size,
             "%s_%llu_%lu",
             prefix ? prefix : "id",
             (unsigned long long)esp_timer_get_time(),
             (unsigned long)s_local_sequence);
}

static void add_string_if_present(cJSON *object, const cJSON *args, const char *src_key, const char *dst_key)
{
    const char *value = json_string_value(args, src_key);
    if (value) {
        cJSON_DeleteItemFromObjectCaseSensitive(object, dst_key ? dst_key : src_key);
        cJSON_AddStringToObject(object, dst_key ? dst_key : src_key, value);
    }
}

static void add_number_if_present(cJSON *object, const cJSON *args, const char *src_key, const char *dst_key)
{
    double value = 0;
    if (json_number_value(args, src_key, &value)) {
        cJSON_DeleteItemFromObjectCaseSensitive(object, dst_key ? dst_key : src_key);
        cJSON_AddNumberToObject(object, dst_key ? dst_key : src_key, value);
    }
}

static bool item_matches_query(const cJSON *item, const char *query)
{
    if (!query || !query[0]) {
        return true;
    }
    const char *fields[] = {"name", "category", "zone", "slot", "note", "text", "summary", "type"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, fields[i]);
        if (cJSON_IsString(value) && strstr(value->valuestring, query)) {
            return true;
        }
    }
    return false;
}

static int days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static bool parse_ymd(const char *text, int *out_days)
{
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (!text || sscanf(text, "%d-%u-%u", &y, &m, &d) != 3 || y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    if (out_days) {
        *out_days = days_from_civil(y, m, d);
    }
    return true;
}

static bool current_days(int *out_days)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        return false;
    }
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    if (out_days) {
        *out_days = days_from_civil(tm_now.tm_year + 1900, (unsigned)tm_now.tm_mon + 1, (unsigned)tm_now.tm_mday);
    }
    return true;
}

static esp_err_t tool_inventory_add(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *name = json_string_value(args, "name");
    if (!name || !name[0]) {
        cJSON_Delete(args);
        result_error(result, "NAME_REQUIRED", "inventory.add 需要 name 字段");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = read_json_root(INVENTORY_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    if (!items) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "创建库存数组失败");
        return ESP_ERR_NO_MEM;
    }

    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    cJSON *existing = find_object_by_string(items, "idempotency_key", idem, NULL);
    if (existing) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "duplicate", true);
        cJSON_AddStringToObject(data, "id", json_string_value(existing, "id") ? json_string_value(existing, "id") : "");
        cJSON_AddItemToObject(data, "item", cJSON_Duplicate(existing, true));
        result_ok_json(result, "幂等键已存在，返回原库存条目", data);
        cJSON_Delete(data);
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }

    char generated_id[64] = {0};
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        make_id("food", generated_id, sizeof(generated_id));
        id = generated_id;
    }
    if (find_object_by_string(items, "id", id, NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "ID_EXISTS", "库存 id 已存在，请使用 inventory.update");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *item = cJSON_CreateObject();
    if (!item) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "创建库存条目失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(item, "id", id);
    cJSON_AddStringToObject(item, "name", name);
    add_number_if_present(item, args, "quantity", "quantity");
    add_string_if_present(item, args, "unit", "unit");
    add_string_if_present(item, args, "category", "category");
    add_string_if_present(item, args, "zone", "zone");
    add_string_if_present(item, args, "slot", "slot");
    add_string_if_present(item, args, "expireDate", "expireDate");
    add_string_if_present(item, args, "expire_date", "expireDate");
    add_string_if_present(item, args, "note", "note");
    add_string_if_present(item, args, "source", "source");
    add_number_if_present(item, args, "confidence", "confidence");
    if (idem[0]) {
        cJSON_AddStringToObject(item, "idempotency_key", idem);
    }
    cJSON_AddNumberToObject(item, "createdAtMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(item, "updatedAtMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddItemToArray(items, item);

    esp_err_t err = write_json_root(INVENTORY_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "path", "/sdcard/" INVENTORY_PATH);
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(items));
        cJSON_AddBoolToObject(data, "duplicate", false);
        result_ok_json(result, "已写入库存条目", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(root);
    cJSON_Delete(args);
    return err;
}

static esp_err_t tool_inventory_update(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        cJSON_Delete(args);
        result_error(result, "ID_REQUIRED", "inventory.update 需要 id 字段");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = read_json_root(INVENTORY_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    if (operation_already_applied(root, "inventory.update", idem, result)) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }
    cJSON *item = find_object_by_string(items, "id", id, NULL);
    if (!item) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NOT_FOUND", "未找到指定库存条目");
        return ESP_ERR_NOT_FOUND;
    }

    add_string_if_present(item, args, "name", "name");
    add_number_if_present(item, args, "quantity", "quantity");
    add_string_if_present(item, args, "unit", "unit");
    add_string_if_present(item, args, "category", "category");
    add_string_if_present(item, args, "zone", "zone");
    add_string_if_present(item, args, "slot", "slot");
    add_string_if_present(item, args, "expireDate", "expireDate");
    add_string_if_present(item, args, "expire_date", "expireDate");
    add_string_if_present(item, args, "note", "note");
    cJSON_DeleteItemFromObjectCaseSensitive(item, "updatedAtMs");
    cJSON_AddNumberToObject(item, "updatedAtMs", (double)(esp_timer_get_time() / 1000));

    esp_err_t op_err = record_operation(root, "inventory.update", idem, id);
    if (op_err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "记录库存更新幂等状态失败");
        return op_err;
    }

    esp_err_t err = write_json_root(INVENTORY_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "path", "/sdcard/" INVENTORY_PATH);
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddItemToObject(data, "item", cJSON_Duplicate(item, true));
        result_ok_json(result, "已更新库存条目", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(root);
    cJSON_Delete(args);
    return err;
}

static esp_err_t tool_inventory_delete(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        cJSON_Delete(args);
        result_error(result, "ID_REQUIRED", "inventory.delete 需要 id 字段");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = read_json_root(INVENTORY_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    if (operation_already_applied(root, "inventory.delete", idem, result)) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }
    int index = -1;
    cJSON *item = find_object_by_string(items, "id", id, &index);
    if (!item || index < 0) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NOT_FOUND", "未找到指定库存条目");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *deleted = cJSON_Duplicate(item, true);
    cJSON_DeleteItemFromArray(items, index);

    esp_err_t op_err = record_operation(root, "inventory.delete", idem, id);
    if (op_err != ESP_OK) {
        if (deleted) cJSON_Delete(deleted);
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "记录库存删除幂等状态失败");
        return op_err;
    }

    esp_err_t err = write_json_root(INVENTORY_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "path", "/sdcard/" INVENTORY_PATH);
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(items));
        if (deleted) {
            cJSON_AddItemToObject(data, "deleted", deleted);
            deleted = NULL;
        }
        result_ok_json(result, "已删除库存条目", data);
        cJSON_Delete(data);
    }
    if (deleted) cJSON_Delete(deleted);
    cJSON_Delete(root);
    cJSON_Delete(args);
    return err;
}

static esp_err_t tool_inventory_list_expiring(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    uint32_t days = 7;
    (void)json_u32_value(args, "days", &days);
    if (days > 365) days = 365;

    cJSON *root = read_json_root(INVENTORY_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    int today = 0;
    const bool time_ready = current_days(&today);
    cJSON *matches = cJSON_CreateArray();
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        const char *expire = json_string_value(item, "expireDate");
        int expire_days = 0;
        if (!expire || !parse_ymd(expire, &expire_days)) {
            continue;
        }
        if (!time_ready || expire_days - today <= (int)days) {
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) {
                if (time_ready) {
                    cJSON_AddNumberToObject(copy, "daysLeft", expire_days - today);
                }
                cJSON_AddItemToArray(matches, copy);
            }
        }
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", "/sdcard/" INVENTORY_PATH);
    cJSON_AddNumberToObject(data, "days", days);
    cJSON_AddBoolToObject(data, "timeReady", time_ready);
    cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(matches));
    cJSON_AddItemToObject(data, "items", matches);
    result_ok_json(result, "已读取临期库存", data);
    cJSON_Delete(data);
    cJSON_Delete(root);
    cJSON_Delete(args);
    return ESP_OK;
}

static esp_err_t tool_task_list(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    (void)call;
    cJSON *root = read_json_root(TASKS_PATH, "{\"schema_version\":1,\"tasks\":[]}", result);
    if (!root) return ESP_FAIL;
    result_ok_json(result, "已读取任务列表", root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t append_task_common(const fridge_tool_call_t *call, fridge_tool_result_t *result, const char *kind)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *title = json_string_value(args, "title");
    if (!title) title = json_string_value(args, "label");
    if (!title || !title[0]) {
        cJSON_Delete(args);
        result_error(result, "TITLE_REQUIRED", "创建任务需要 title/label 字段");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t duration_seconds = 0;
    if (strcmp(kind, "timer") == 0 &&
        ((!json_u32_value(args, "durationSeconds", &duration_seconds) &&
          !json_u32_value(args, "seconds", &duration_seconds)) ||
         duration_seconds == 0 || duration_seconds > 86400)) {
        cJSON_Delete(args);
        result_error(result, "DURATION_REQUIRED", "计时器需要 1~86400 秒的 durationSeconds");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = read_json_root(TASKS_PATH, "{\"schema_version\":1,\"tasks\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *tasks = ensure_array(root, "tasks");

    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    cJSON *existing = find_object_by_string(tasks, "idempotency_key", idem, NULL);
    if (existing) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "duplicate", true);
        cJSON_AddStringToObject(data, "id", json_string_value(existing, "id") ? json_string_value(existing, "id") : "");
        cJSON_AddItemToObject(data, "task", cJSON_Duplicate(existing, true));
        result_ok_json(result, "幂等键已存在，返回原任务", data);
        cJSON_Delete(data);
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }

    char generated_id[64] = {0};
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        make_id("task", generated_id, sizeof(generated_id));
        id = generated_id;
    }
    cJSON *task = cJSON_CreateObject();
    if (!task) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "创建任务对象失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(task, "id", id);
    cJSON_AddStringToObject(task, "kind", kind);
    cJSON_AddStringToObject(task, "title", title);
    cJSON_AddStringToObject(task, "status", "pending");
    add_string_if_present(task, args, "dueAt", "dueAt");
    add_string_if_present(task, args, "remindAt", "dueAt");
    add_string_if_present(task, args, "note", "note");
    add_number_if_present(task, args, "durationSeconds", "durationSeconds");
    add_number_if_present(task, args, "seconds", "durationSeconds");
    if (strcmp(kind, "timer") == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(task, "durationSeconds");
        cJSON_AddNumberToObject(task, "durationSeconds", duration_seconds);
        const int64_t mono_ms = esp_timer_get_time() / 1000LL;
        cJSON_AddNumberToObject(task, "createdMonotonicMs", (double)mono_ms);
        bool clock_ready = false;
        const int64_t epoch_ms = current_epoch_ms(&clock_ready);
        if (clock_ready) {
            cJSON_AddNumberToObject(task, "createdAtEpochMs", (double)epoch_ms);
            cJSON_AddNumberToObject(task, "dueAtEpochMs", (double)(epoch_ms + (int64_t)duration_seconds * 1000LL));
        }
    }
    if (idem[0]) cJSON_AddStringToObject(task, "idempotency_key", idem);
    cJSON_AddNumberToObject(task, "createdAtMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddItemToArray(tasks, task);

    esp_err_t err = write_json_root(TASKS_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "path", "/sdcard/" TASKS_PATH);
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(tasks));
        cJSON_AddBoolToObject(data, "duplicate", false);
        result_ok_json(result, strcmp(kind, "timer") == 0 ? "已创建计时任务" : "已创建提醒任务", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(root);
    cJSON_Delete(args);
    if (err == ESP_OK) fridge_task_scheduler_notify_reload();
    return err;
}

static esp_err_t tool_task_create_timer(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    return append_task_common(call, result, "timer");
}

static esp_err_t tool_task_create_reminder(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    return append_task_common(call, result, "reminder");
}

static esp_err_t tool_task_cancel(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        cJSON_Delete(args);
        result_error(result, "ID_REQUIRED", "task.cancel 需要 id 字段");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = read_json_root(TASKS_PATH, "{\"schema_version\":1,\"tasks\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *tasks = ensure_array(root, "tasks");
    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    if (operation_already_applied(root, "task.cancel", idem, result)) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }
    cJSON *task = find_object_by_string(tasks, "id", id, NULL);
    if (!task) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NOT_FOUND", "未找到指定任务");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(task, "status");
    cJSON_AddStringToObject(task, "status", "canceled");
    cJSON_DeleteItemFromObjectCaseSensitive(task, "updatedAtMs");
    cJSON_AddNumberToObject(task, "updatedAtMs", (double)(esp_timer_get_time() / 1000));
    esp_err_t op_err = record_operation(root, "task.cancel", idem, id);
    if (op_err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "记录任务取消幂等状态失败");
        return op_err;
    }
    esp_err_t err = write_json_root(TASKS_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddStringToObject(data, "status", "canceled");
        result_ok_json(result, "已取消任务", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(root);
    cJSON_Delete(args);
    if (err == ESP_OK) fridge_task_scheduler_notify_reload();
    return err;
}

static esp_err_t tool_task_trigger(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        cJSON_Delete(args);
        result_error(result, "ID_REQUIRED", "task.trigger 需要 id 字段");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = read_json_root(TASKS_PATH, NULL, result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    if (operation_already_applied(root, "task.trigger", idem, result)) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }
    cJSON *task = find_object_by_string(ensure_array(root, "tasks"), "id", id, NULL);
    if (!task) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NOT_FOUND", "未找到指定计时任务");
        return ESP_ERR_NOT_FOUND;
    }
    const char *status = json_string_value(task, "status");
    if (status && strcmp(status, "triggered") == 0) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "duplicate", true);
        cJSON_AddStringToObject(data, "id", id);
        result_ok_json(result, "计时任务已经触发", data);
        cJSON_Delete(data);
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }
    if (status && strcmp(status, "canceled") == 0) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "TASK_CANCELED", "已取消的计时任务不能触发");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(task, "status");
    cJSON_AddStringToObject(task, "status", "triggered");
    cJSON_AddNumberToObject(task, "triggeredMonotonicMs", (double)(esp_timer_get_time() / 1000));
    bool clock_ready = false;
    const int64_t epoch_ms = current_epoch_ms(&clock_ready);
    if (clock_ready) cJSON_AddNumberToObject(task, "triggeredAtEpochMs", (double)epoch_ms);
    esp_err_t op_err = record_operation(root, "task.trigger", idem, id);
    if (op_err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "记录计时触发幂等状态失败");
        return op_err;
    }

    esp_err_t err = write_json_root(TASKS_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddStringToObject(data, "status", "triggered");
        result_ok_json(result, "计时任务已到期触发", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(root);
    cJSON_Delete(args);
    if (err == ESP_OK) fridge_task_scheduler_notify_reload();
    return err;
}

static esp_err_t tool_memory_add(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *text = json_string_value(args, "text");
    if (!text) text = json_string_value(args, "content");
    if (!text || !text[0]) {
        cJSON_Delete(args);
        result_error(result, "TEXT_REQUIRED", "memory.add 需要 text/content 字段");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = read_json_root(MEMORY_JSON_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    cJSON *existing = find_object_by_string(items, "idempotency_key", idem, NULL);
    if (existing) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "duplicate", true);
        cJSON_AddStringToObject(data, "id", json_string_value(existing, "id") ? json_string_value(existing, "id") : "");
        result_ok_json(result, "幂等键已存在，返回原记忆", data);
        cJSON_Delete(data);
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }
    char generated_id[64] = {0};
    make_id("mem", generated_id, sizeof(generated_id));
    cJSON *memory = cJSON_CreateObject();
    cJSON_AddStringToObject(memory, "id", generated_id);
    cJSON_AddStringToObject(memory, "text", text);
    add_string_if_present(memory, args, "type", "type");
    add_string_if_present(memory, args, "category", "category");
    add_string_if_present(memory, args, "source", "source");
    if (idem[0]) cJSON_AddStringToObject(memory, "idempotency_key", idem);
    cJSON_AddNumberToObject(memory, "createdAtMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddItemToArray(items, memory);
    esp_err_t err = write_json_root(MEMORY_JSON_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "path", "/sdcard/" MEMORY_JSON_PATH);
        cJSON_AddStringToObject(data, "id", generated_id);
        cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(items));
        result_ok_json(result, "已写入结构化记忆", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(root);
    cJSON_Delete(args);
    return err;
}

static esp_err_t tool_memory_search(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *query = json_string_value(args, "query");
    if (!query) query = json_string_value(args, "q");
    uint32_t limit = 8;
    (void)json_u32_value(args, "limit", &limit);
    if (limit == 0 || limit > 16) limit = 8;

    cJSON *root = read_json_root(MEMORY_JSON_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    cJSON *matches = cJSON_CreateArray();
    cJSON *item = NULL;
    uint32_t matched = 0;
    cJSON_ArrayForEach(item, items) {
        if (matched >= limit) break;
        if (item_matches_query(item, query)) {
            cJSON_AddItemToArray(matches, cJSON_Duplicate(item, true));
            matched++;
        }
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", "/sdcard/" MEMORY_JSON_PATH);
    cJSON_AddStringToObject(data, "query", query ? query : "");
    cJSON_AddNumberToObject(data, "count", matched);
    cJSON_AddItemToObject(data, "items", matches);
    result_ok_json(result, "已搜索结构化记忆", data);
    cJSON_Delete(data);
    cJSON_Delete(root);
    cJSON_Delete(args);
    return ESP_OK;
}

static esp_err_t tool_memory_delete(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *id = json_string_value(args, "id");
    if (!id || !id[0]) {
        cJSON_Delete(args);
        result_error(result, "ID_REQUIRED", "memory.delete 需要 id 字段");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = read_json_root(MEMORY_JSON_PATH, "{\"schema_version\":1,\"items\":[]}", result);
    if (!root) {
        cJSON_Delete(args);
        return ESP_FAIL;
    }
    cJSON *items = ensure_array(root, "items");
    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    if (operation_already_applied(root, "memory.delete", idem, result)) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        return ESP_OK;
    }

    int index = -1;
    cJSON *item = find_object_by_string(items, "id", id, &index);
    if (!item || index < 0) {
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NOT_FOUND", "未找到指定结构化记忆");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *deleted = cJSON_Duplicate(item, true);
    cJSON_DeleteItemFromArray(items, index);

    esp_err_t op_err = record_operation(root, "memory.delete", idem, id);
    if (op_err != ESP_OK) {
        cJSON_Delete(deleted);
        cJSON_Delete(root);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "记录记忆删除幂等状态失败");
        return op_err;
    }

    esp_err_t err = write_json_root(MEMORY_JSON_PATH, root, result);
    if (err == ESP_OK) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "path", "/sdcard/" MEMORY_JSON_PATH);
        cJSON_AddStringToObject(data, "id", id);
        cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(items));
        if (deleted) {
            cJSON_AddItemToObject(data, "deleted", deleted);
            deleted = NULL;
        }
        result_ok_json(result, "已删除结构化记忆", data);
        cJSON_Delete(data);
    }
    cJSON_Delete(deleted);
    cJSON_Delete(root);
    cJSON_Delete(args);
    return err;
}

static esp_err_t tool_memory_md_append(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    cJSON *args = parse_arguments(call, result);
    if (!args) return ESP_ERR_INVALID_ARG;
    const char *text = json_string_value(args, "text");
    if (!text) text = json_string_value(args, "content");
    if (!text || !text[0]) {
        cJSON_Delete(args);
        result_error(result, "TEXT_REQUIRED", "memory_md.append 需要 text/content 字段");
        return ESP_ERR_INVALID_ARG;
    }
    const char *title = json_string_value(args, "title");
    if (!title || !title[0]) title = "Agent 记忆补充";

    char *old_text = business_alloc(BUSINESS_MEMORY_MD_MAX_BYTES + 1);
    if (!old_text) {
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "分配 MEMORY.md 缓冲失败");
        return ESP_ERR_NO_MEM;
    }
    size_t old_len = 0;
    esp_err_t err = fridge_tfcard_text_read(MEMORY_MD_PATH, old_text, BUSINESS_MEMORY_MD_MAX_BYTES + 1, &old_len);
    if (err == ESP_ERR_NOT_FOUND) {
        old_text[0] = '\0';
        old_len = 0;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        free(old_text);
        cJSON_Delete(args);
        result_error(result, esp_err_to_name(err), "读取 MEMORY.md 失败");
        return err;
    }

    char idem[64] = {0};
    get_idempotency_key(call, args, idem, sizeof(idem));
    const uint32_t idem_hash = idempotency_hash("memory_md.append", idem);
    char marker[48];
    snprintf(marker, sizeof(marker), "<!-- idem:%08" PRIx32 " -->", idem_hash);
    if (idem[0] && strstr(old_text, marker)) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "duplicate", true);
        cJSON_AddStringToObject(data, "idempotencyKey", idem);
        result_ok_json(result, "MEMORY.md 已包含该幂等追加", data);
        cJSON_Delete(data);
        free(old_text);
        cJSON_Delete(args);
        return ESP_OK;
    }

    char entry[1024];
    const int entry_len = snprintf(entry,
                                   sizeof(entry),
                                   "\n\n%s\n### %s\n\n%s\n",
                                   marker,
                                   title,
                                   text);
    if (entry_len <= 0 || entry_len >= (int)sizeof(entry) || old_len + (size_t)entry_len > BUSINESS_MEMORY_MD_MAX_BYTES) {
        free(old_text);
        cJSON_Delete(args);
        result_error(result, "MEMORY_MD_TOO_LARGE", "MEMORY.md 追加内容超过当前保护上限");
        return ESP_ERR_INVALID_SIZE;
    }

    char *new_text = business_alloc(old_len + (size_t)entry_len + 1);
    if (!new_text) {
        free(old_text);
        cJSON_Delete(args);
        result_error(result, "NO_MEM", "分配 MEMORY.md 写入缓冲失败");
        return ESP_ERR_NO_MEM;
    }
    memcpy(new_text, old_text, old_len);
    memcpy(new_text + old_len, entry, (size_t)entry_len);
    err = fridge_agent_storage_text_write(MEMORY_MD_PATH, new_text, old_len + (size_t)entry_len);
    free(old_text);
    free(new_text);
    if (err != ESP_OK) {
        cJSON_Delete(args);
        result_error(result, esp_err_to_name(err), "写入 MEMORY.md 失败");
        return err;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "path", "/sdcard/" MEMORY_MD_PATH);
    cJSON_AddNumberToObject(data, "appendedBytes", entry_len);
    result_ok_json(result, "已追加 MEMORY.md", data);
    cJSON_Delete(data);
    cJSON_Delete(args);
    return ESP_OK;
}

static esp_err_t register_tool(const char *name,
                               const char *description,
                               fridge_tool_permission_t permission,
                               bool idempotent,
                               bool requires_confirmation,
                               const char *schema,
                               fridge_tool_execute_fn_t execute)
{
    fridge_tool_definition_t def = {0};
    strlcpy(def.name, name, sizeof(def.name));
    strlcpy(def.description, description, sizeof(def.description));
    def.permission = permission;
    def.idempotent = idempotent;
    def.requires_confirmation = requires_confirmation;
    def.timeout_ms = 5000;
    strlcpy(def.input_schema, schema ? schema : "{\"type\":\"object\",\"properties\":{}}", sizeof(def.input_schema));
    def.execute = execute;
    return fridge_tool_registry_register(&def);
}

esp_err_t fridge_business_tools_register_all(void)
{
    ESP_RETURN_ON_ERROR(register_tool("inventory.add", "新增库存条目到 TF 卡 Agent 库存。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":96},\"quantity\":{\"type\":\"number\",\"minimum\":0},\"unit\":{\"type\":\"string\",\"maxLength\":24},\"expireDate\":{\"type\":\"string\",\"maxLength\":16}}}", tool_inventory_add), TAG, "注册 inventory.add 失败");
    ESP_RETURN_ON_ERROR(register_tool("inventory.update", "更新 TF 卡库存条目。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"id\"],\"properties\":{\"id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64},\"name\":{\"type\":\"string\",\"maxLength\":96},\"quantity\":{\"type\":\"number\",\"minimum\":0}}}", tool_inventory_update), TAG, "注册 inventory.update 失败");
    ESP_RETURN_ON_ERROR(register_tool("inventory.delete", "删除 TF 卡库存条目，必须显式确认。", FRIDGE_TOOL_PERMISSION_DANGEROUS, true, true,
                                     "{\"type\":\"object\",\"required\":[\"id\"],\"properties\":{\"id\":{\"type\":\"string\"}}}", tool_inventory_delete), TAG, "注册 inventory.delete 失败");
    ESP_RETURN_ON_ERROR(register_tool("inventory.list_expiring", "读取指定天数内临期库存。", FRIDGE_TOOL_PERMISSION_READ, true, false,
                                     "{\"type\":\"object\",\"properties\":{\"days\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":365}}}", tool_inventory_list_expiring), TAG, "注册 inventory.list_expiring 失败");
    ESP_RETURN_ON_ERROR(register_tool("task.list", "读取 TF 卡任务列表。", FRIDGE_TOOL_PERMISSION_READ, true, false,
                                     "{\"type\":\"object\",\"properties\":{}}", tool_task_list), TAG, "注册 task.list 失败");
    ESP_RETURN_ON_ERROR(register_tool("task.create_timer", "创建并持久化本地计时任务。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"title\"],\"properties\":{\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":128},\"durationSeconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400},\"seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400}}}", tool_task_create_timer), TAG, "注册 task.create_timer 失败");
    ESP_RETURN_ON_ERROR(register_tool("task.create_reminder", "创建本地提醒任务记录。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"title\"],\"properties\":{\"title\":{\"type\":\"string\"},\"dueAt\":{\"type\":\"string\"}}}", tool_task_create_reminder), TAG, "注册 task.create_reminder 失败");
    ESP_RETURN_ON_ERROR(register_tool("task.cancel", "取消本地任务记录。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"id\"],\"properties\":{\"id\":{\"type\":\"string\"}}}", tool_task_cancel), TAG, "注册 task.cancel 失败");
    ESP_RETURN_ON_ERROR(register_tool("task.trigger", "由本地调度器幂等触发到期计时任务。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"id\"],\"properties\":{\"id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}}}", tool_task_trigger), TAG, "注册 task.trigger 失败");
    ESP_RETURN_ON_ERROR(register_tool("memory.search", "搜索 TF 卡结构化家庭记忆。", FRIDGE_TOOL_PERMISSION_READ, true, false,
                                     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"}}}", tool_memory_search), TAG, "注册 memory.search 失败");
    ESP_RETURN_ON_ERROR(register_tool("memory.add", "写入结构化家庭记忆。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"text\"],\"properties\":{\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":512},\"type\":{\"type\":\"string\",\"maxLength\":48}}}", tool_memory_add), TAG, "注册 memory.add 失败");
    ESP_RETURN_ON_ERROR(register_tool("memory.delete", "删除指定结构化家庭记忆，必须显式确认。", FRIDGE_TOOL_PERMISSION_DANGEROUS, true, true,
                                     "{\"type\":\"object\",\"required\":[\"id\"],\"properties\":{\"id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}}}", tool_memory_delete), TAG, "注册 memory.delete 失败");
    ESP_RETURN_ON_ERROR(register_tool("memory_md.append", "追加 TF 卡 MEMORY.md 长期记忆。", FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"required\":[\"text\"],\"properties\":{\"title\":{\"type\":\"string\",\"maxLength\":96},\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":768}}}", tool_memory_md_append), TAG, "注册 memory_md.append 失败");
    // 秒表工具只改变运行态计时，不直接访问 LVGL；页面刷新由 ui_p4 任务读取同一份厨房快照完成。
    ESP_RETURN_ON_ERROR(register_tool("stopwatch.status", "读取秒表当前状态、毫秒数和计次记录。",
                                     FRIDGE_TOOL_PERMISSION_READ, true, false,
                                     "{\"type\":\"object\",\"properties\":{}}",
                                     tool_stopwatch_status), TAG, "注册 stopwatch.status 失败");
    ESP_RETURN_ON_ERROR(register_tool("stopwatch.start", "开始或继续秒表，不会清空已有计时。",
                                     FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"properties\":{}}",
                                     tool_stopwatch_start), TAG, "注册 stopwatch.start 失败");
    ESP_RETURN_ON_ERROR(register_tool("stopwatch.pause", "暂停秒表并保留当前毫秒数和计次记录。",
                                     FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"properties\":{}}",
                                     tool_stopwatch_pause), TAG, "注册 stopwatch.pause 失败");
    ESP_RETURN_ON_ERROR(register_tool("stopwatch.lap", "记录当前秒表计次；重复调用通过幂等键返回原结果。",
                                     FRIDGE_TOOL_PERMISSION_WRITE, true, false,
                                     "{\"type\":\"object\",\"properties\":{\"idempotencyKey\":{\"type\":\"string\",\"maxLength\":64},\"idempotency_key\":{\"type\":\"string\",\"maxLength\":64}}}",
                                     tool_stopwatch_lap), TAG, "注册 stopwatch.lap 失败");
    ESP_RETURN_ON_ERROR(register_tool("stopwatch.reset", "清空秒表计时和全部计次记录，必须用户确认。",
                                     FRIDGE_TOOL_PERMISSION_DANGEROUS, true, true,
                                     "{\"type\":\"object\",\"properties\":{}}",
                                     tool_stopwatch_reset), TAG, "注册 stopwatch.reset 失败");
    return ESP_OK;
}
