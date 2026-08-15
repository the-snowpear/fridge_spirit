// 冰箱小精灵内置 Tool 实现。
//
// 当前阶段只实现只读工具，先验证 Agent → Tool Broker → 本地真实数据 的闭环。
// 这些工具不控制 GPIO、不启动摄像头/音频任务，因此不会引入新的硬件风险。

#include "fridge_builtin_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_agent_storage.h"
#include "fridge_diagnostics.h"
#include "fridge_sht4x.h"
#include "fridge_storage.h"
#include "fridge_tfcard.h"
#include "fridge_tool_registry.h"

static const char *TAG = "fridge_builtin_tools";

#define BUILTIN_INVENTORY_MAX_BYTES (64U * 1024U)
#define BUILTIN_MEMORY_PAGE_MAX 512U

static void result_ok(fridge_tool_result_t *result, const char *summary, const char *data_json)
{
    result->status = FRIDGE_TOOL_RESULT_OK;
    strlcpy(result->code, "SUCCESS", sizeof(result->code));
    strlcpy(result->summary, summary ? summary : "工具执行成功", sizeof(result->summary));
    strlcpy(result->data_json, data_json ? data_json : "{}", sizeof(result->data_json));
}

static void result_error(fridge_tool_result_t *result, const char *code, const char *summary)
{
    result->status = FRIDGE_TOOL_RESULT_ERROR;
    strlcpy(result->code, code ? code : "ERROR", sizeof(result->code));
    strlcpy(result->summary, summary ? summary : "工具执行失败", sizeof(result->summary));
    strlcpy(result->data_json, "{}", sizeof(result->data_json));
}

static void *builtin_alloc(size_t bytes)
{
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(1, bytes);
    return ptr;
}

static void result_ok_json(fridge_tool_result_t *result, const char *summary, cJSON *data)
{
    char *json = cJSON_PrintUnformatted(data);
    if (!json) {
        result_error(result, "NO_MEM", "序列化 Tool 结果失败");
        return;
    }
    if (strlen(json) >= sizeof(result->data_json)) {
        cJSON_free(json);
        result_error(result, "RESULT_TOO_LARGE", "Tool 结果超过 4KB，请使用分页参数");
        return;
    }
    result_ok(result, summary, json);
    cJSON_free(json);
}

static uint32_t argument_u32(const fridge_tool_call_t *call, const char *name, uint32_t fallback)
{
    cJSON *args = cJSON_Parse(call && call->arguments_json[0] ? call->arguments_json : "{}");
    if (!args) return fallback;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, name);
    const uint32_t result = cJSON_IsNumber(value) && value->valuedouble >= 0 &&
                            value->valuedouble <= UINT32_MAX ? (uint32_t)value->valuedouble : fallback;
    cJSON_Delete(args);
    return result;
}

static size_t utf8_safe_prefix(const char *text, size_t length)
{
    size_t i = 0;
    size_t last = 0;
    while (i < length) {
        const unsigned char c = (unsigned char)text[i];
        size_t sequence = 1;
        if (c >= 0xC2 && c <= 0xDF) sequence = 2;
        else if (c >= 0xE0 && c <= 0xEF) sequence = 3;
        else if (c >= 0xF0 && c <= 0xF4) sequence = 4;
        else if (c >= 0x80) {
            i++;
            last = i;
            continue;
        }
        if (i + sequence > length) break;
        bool valid = true;
        for (size_t k = 1; k < sequence; k++) {
            if (((unsigned char)text[i + k] & 0xC0U) != 0x80U) valid = false;
        }
        if (!valid) sequence = 1;
        i += sequence;
        last = i;
    }
    return last;
}

static void add_compact_string(cJSON *object, const cJSON *source, const char *name, size_t max_bytes)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(source, name);
    if (!cJSON_IsString(value)) return;
    char buffer[129];
    if (max_bytes >= sizeof(buffer)) max_bytes = sizeof(buffer) - 1;
    size_t length = strlen(value->valuestring);
    if (length > max_bytes) length = utf8_safe_prefix(value->valuestring, max_bytes);
    memcpy(buffer, value->valuestring, length);
    buffer[length] = '\0';
    cJSON_AddStringToObject(object, name, buffer);
}

static cJSON *compact_inventory_item(const cJSON *source)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;
    add_compact_string(item, source, "id", 64);
    add_compact_string(item, source, "name", 96);
    add_compact_string(item, source, "unit", 24);
    add_compact_string(item, source, "category", 48);
    add_compact_string(item, source, "zone", 48);
    add_compact_string(item, source, "slot", 48);
    add_compact_string(item, source, "expireDate", 16);
    // UI 库存快照使用 snake_case/文本数量；对 Agent 统一输出稳定的 camelCase 字段，
    // 让 LLM 不必了解两套历史 JSON schema。
    const cJSON *ui_quantity = cJSON_GetObjectItemCaseSensitive(source, "quantity");
    if (cJSON_IsString(ui_quantity) && ui_quantity->valuestring) {
        cJSON_AddStringToObject(item, "quantity", ui_quantity->valuestring);
    }
    const cJSON *ui_expire = cJSON_GetObjectItemCaseSensitive(source, "expire_date");
    if (cJSON_IsString(ui_expire) && ui_expire->valuestring) {
        cJSON_AddStringToObject(item, "expireDate", ui_expire->valuestring);
    }
    const cJSON *ui_location = cJSON_GetObjectItemCaseSensitive(source, "location");
    if (cJSON_IsString(ui_location) && ui_location->valuestring) {
        cJSON_AddStringToObject(item, "location", ui_location->valuestring);
    }
    const cJSON *ui_days_left = cJSON_GetObjectItemCaseSensitive(source, "days_left");
    if (cJSON_IsNumber(ui_days_left)) cJSON_AddNumberToObject(item, "daysLeft", ui_days_left->valuedouble);
    const cJSON *quantity = cJSON_GetObjectItemCaseSensitive(source, "quantity");
    if (cJSON_IsNumber(quantity)) cJSON_AddNumberToObject(item, "quantity", quantity->valuedouble);
    return item;
}

static esp_err_t tool_inventory_list(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    char *inventory = builtin_alloc(BUILTIN_INVENTORY_MAX_BYTES + 1);
    if (!inventory) {
        result_error(result, "NO_MEM", "分配库存读取缓冲失败");
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    // 库存页、拍照确认和 Agent 必须共用 LittleFS UI 快照；此前读取 TF 的 Agent 演示
    // 文件会与真实库存脱节，导致模型把有食材的冰箱误判为空。
    esp_err_t err = fridge_storage_get_ui_inventory_snapshot(inventory,
                                                             BUILTIN_INVENTORY_MAX_BYTES + 1);
    n = err == ESP_OK ? strlen(inventory) : 0;
    if (err != ESP_OK) {
        free(inventory);
        result_error(result, esp_err_to_name(err), "读取本机库存快照失败");
        return err;
    }
    cJSON *root = cJSON_ParseWithLength(inventory, n);
    free(inventory);
    if (!root) {
        result_error(result, "INVALID_STORE_JSON", "库存 JSON 解析失败");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *source_items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(source_items)) {
        cJSON_Delete(root);
        result_error(result, "INVALID_STORE_SCHEMA", "库存 JSON 缺少 items 数组");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t offset = argument_u32(call, "offset", 0);
    uint32_t limit = argument_u32(call, "limit", 8);
    if (limit == 0 || limit > 16) limit = 8;
    const int total = cJSON_GetArraySize(source_items);
    if (offset > (uint32_t)total) offset = (uint32_t)total;

    cJSON *data = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!data || !items) {
        cJSON_Delete(data);
        cJSON_Delete(items);
        cJSON_Delete(root);
        result_error(result, "NO_MEM", "创建库存分页结果失败");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(data, "source", "local_ui_inventory");
    cJSON_AddNumberToObject(data, "total", total);
    cJSON_AddNumberToObject(data, "offset", offset);
    cJSON_AddItemToObject(data, "items", items);
    uint32_t emitted = 0;
    for (uint32_t i = offset; i < (uint32_t)total && emitted < limit; i++) {
        cJSON *compact = compact_inventory_item(cJSON_GetArrayItem(source_items, (int)i));
        if (!compact) break;
        cJSON_AddItemToArray(items, compact);
        char *trial = cJSON_PrintUnformatted(data);
        const bool fits = trial && strlen(trial) + 128 < sizeof(result->data_json);
        cJSON_free(trial);
        if (!fits) {
            cJSON_DeleteItemFromArray(items, cJSON_GetArraySize(items) - 1);
            break;
        }
        emitted++;
    }
    const uint32_t next_offset = offset + emitted;
    cJSON_AddNumberToObject(data, "count", emitted);
    cJSON_AddNumberToObject(data, "nextOffset", next_offset);
    cJSON_AddBoolToObject(data, "hasMore", next_offset < (uint32_t)total);
    result_ok_json(result, "已读取本机库存快照", data);
    cJSON_Delete(data);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t tool_memory_md_read(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    uint32_t offset = argument_u32(call, "offset", 0);
    uint32_t max_bytes = argument_u32(call, "maxBytes", BUILTIN_MEMORY_PAGE_MAX);
    if (max_bytes == 0 || max_bytes > BUILTIN_MEMORY_PAGE_MAX) max_bytes = BUILTIN_MEMORY_PAGE_MAX;
    char *memory_md = builtin_alloc(max_bytes + 1);
    if (!memory_md) {
        result_error(result, "NO_MEM", "分配 MEMORY.md 分页缓冲失败");
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    size_t total = 0;
    esp_err_t err = fridge_tfcard_text_read_range("agent/MEMORY.md",
                                                  offset,
                                                  memory_md,
                                                  max_bytes + 1,
                                                  &n,
                                                  &total);
    if (err != ESP_OK) {
        free(memory_md);
        result_error(result, esp_err_to_name(err), "读取 TF 卡 MEMORY.md 失败");
        return err;
    }
    // 调试方若传入了 UTF-8 多字节字符中间的任意 offset，跳过开头续字节，保证返回 JSON 始终有效。
    size_t leading_continuations = 0;
    while (leading_continuations < n &&
           (((unsigned char)memory_md[leading_continuations] & 0xC0U) == 0x80U)) {
        leading_continuations++;
    }
    if (leading_continuations > 0) {
        memmove(memory_md, memory_md + leading_continuations, n - leading_continuations);
        n -= leading_continuations;
        offset += (uint32_t)leading_continuations;
    }
    n = utf8_safe_prefix(memory_md, n);
    memory_md[n] = '\0';
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "source", "/sdcard/agent/MEMORY.md");
    cJSON_AddNumberToObject(data, "totalBytes", total);
    cJSON_AddNumberToObject(data, "offset", offset);
    cJSON_AddNumberToObject(data, "bytes", n);
    cJSON_AddNumberToObject(data, "nextOffset", offset + n);
    cJSON_AddBoolToObject(data, "hasMore", offset + n < total);
    cJSON_AddStringToObject(data, "content", memory_md);
    free(memory_md);
    result_ok_json(result, "已分页读取 TF 卡 Agent MEMORY.md", data);
    cJSON_Delete(data);
    return ESP_OK;
}

static esp_err_t tool_device_state_get(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    (void)call;
    fridge_device_status_t device = {0};
    fridge_agent_storage_status_t agent = {0};
    esp_err_t err = fridge_diagnostics_get_status(&device);
    if (err != ESP_OK) {
        result_error(result, esp_err_to_name(err), "读取设备诊断状态失败");
        return err;
    }
    fridge_agent_storage_get_status(&agent);

    fridge_sht4x_snapshot_t env = {0};
    fridge_sht4x_get_snapshot(&env);

    snprintf(result->data_json, sizeof(result->data_json),
             "{\"firmware\":\"%s\",\"chip\":\"%s\",\"uptime\":\"%s\","
             "\"freeHeapKb\":%u,\"minHeapKb\":%u,\"freePsramKb\":%u,"
             "\"wifiHealth\":\"%s\",\"usbHealth\":\"%s\",\"agentStorageReady\":%s,"
             "\"envValid\":%s,\"tempC\":%.1f,\"humidityRh\":%.0f}",
             device.firmware,
             device.chip,
             device.uptime,
             (unsigned)device.free_heap_kb,
             (unsigned)device.min_heap_kb,
             (unsigned)device.free_psram_kb,
             device.wifi_health ? device.wifi_health : "unknown",
             device.usb_health ? device.usb_health : "unknown",
             agent.ready ? "true" : "false",
             env.valid ? "true" : "false",
             env.valid ? env.temperature_c : 0.0f,
             env.valid ? env.humidity_rh : 0.0f);
    result->status = FRIDGE_TOOL_RESULT_OK;
    strlcpy(result->code, "SUCCESS", sizeof(result->code));
    strlcpy(result->summary, "已读取设备语义状态", sizeof(result->summary));
    return ESP_OK;
}

static esp_err_t register_one(const char *name,
                              const char *description,
                              const char *schema,
                              fridge_tool_execute_fn_t execute)
{
    fridge_tool_definition_t def = {0};
    strlcpy(def.name, name, sizeof(def.name));
    strlcpy(def.description, description, sizeof(def.description));
    def.permission = FRIDGE_TOOL_PERMISSION_READ;
    def.idempotent = true;
    def.requires_confirmation = false;
    def.timeout_ms = 3000;
    strlcpy(def.input_schema, schema ? schema : "{\"type\":\"object\",\"properties\":{}}", sizeof(def.input_schema));
    def.execute = execute;
    return fridge_tool_registry_register(&def);
}

esp_err_t fridge_builtin_tools_register_all(void)
{
    ESP_RETURN_ON_ERROR(register_one("inventory.list",
                                     "读取 TF 卡 Agent 库存 JSON。",
                                      "{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"integer\",\"minimum\":0},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16}}}",
                                     tool_inventory_list),
                        TAG, "注册 inventory.list 失败");
    ESP_RETURN_ON_ERROR(register_one("memory_md.read",
                                     "读取 TF 卡 Agent MEMORY.md 文本。",
                                      "{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"integer\",\"minimum\":0},\"maxBytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":512}}}",
                                     tool_memory_md_read),
                        TAG, "注册 memory_md.read 失败");
    ESP_RETURN_ON_ERROR(register_one("device.state.get",
                                     "读取设备诊断和 Agent 存储状态。",
                                     "{\"type\":\"object\",\"properties\":{}}",
                                     tool_device_state_get),
                        TAG, "注册 device.state.get 失败");
    return ESP_OK;
}
