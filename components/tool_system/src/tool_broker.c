// 冰箱小精灵 Agent Tool Broker 实现。
//
// 当前阶段实现 confirm / standard / auto 三档 Agent 权限模式。
// READ 工具可自动执行；标准模式自动执行普通 WRITE；自动模式由用户明确授予完全访问权。
// 后续接入 Trace 与幂等键后，所有写入工具继续复用这里的权限判定。

#include "fridge_tool_broker.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_agent_storage.h"
#include "fridge_builtin_tools.h"
#include "fridge_business_tools.h"
#include "fridge_task_scheduler.h"
#include "fridge_tool_registry.h"
#include "fridge_trace_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "fridge_tool_broker";
static const char *PERMISSION_NVS_NAMESPACE = "fridge_agent";
static const char *PERMISSION_NVS_KEY = "permission_mode";
static bool s_broker_ready;
static fridge_agent_permission_mode_t s_permission_mode = FRIDGE_AGENT_PERMISSION_CONFIRM;
static SemaphoreHandle_t s_execute_mutex;

static bool permission_mode_valid(fridge_agent_permission_mode_t mode)
{
    return mode == FRIDGE_AGENT_PERMISSION_CONFIRM ||
           mode == FRIDGE_AGENT_PERMISSION_STANDARD ||
           mode == FRIDGE_AGENT_PERMISSION_AUTO;
}

// 从 NVS 恢复上次由设置页明确保存的模式。读取失败一律收敛到确认模式，
// 避免损坏的偏好记录让设备以全放行状态启动。
static void load_permission_mode(void)
{
    s_permission_mode = FRIDGE_AGENT_PERMISSION_CONFIRM;
    nvs_handle_t handle = 0;
    uint8_t stored = FRIDGE_AGENT_PERMISSION_CONFIRM;
    if (nvs_open(PERMISSION_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        esp_err_t err = nvs_get_u8(handle, PERMISSION_NVS_KEY, &stored);
        nvs_close(handle);
        if (err == ESP_OK && permission_mode_valid((fridge_agent_permission_mode_t)stored)) {
            s_permission_mode = (fridge_agent_permission_mode_t)stored;
        }
    }
}

// MQTT 组件提供该弱回调，把本地 WRITE/DANGEROUS Tool 的结果转成 P4 v2 同步事件。
// 使用弱符号避免 tool_system 反向依赖 mqtt_protocol 形成 ESP-IDF 组件循环。
void fridge_mqtt_notify_tool_success(const fridge_tool_call_t *call,
                                     const fridge_tool_result_t *result)
    __attribute__((weak));

static void set_result_error(fridge_tool_result_t *result, fridge_tool_result_status_t status,
                             const char *code, const char *summary)
{
    if (!result) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->status = status;
    strlcpy(result->code, code ? code : "ERROR", sizeof(result->code));
    strlcpy(result->summary, summary ? summary : "tool call failed", sizeof(result->summary));
    strlcpy(result->data_json, "{}", sizeof(result->data_json));
}

static bool schema_type_matches(const cJSON *value, const char *type)
{
    if (!type || !value) return false;
    if (strcmp(type, "string") == 0) return cJSON_IsString(value);
    if (strcmp(type, "number") == 0) return cJSON_IsNumber(value);
    if (strcmp(type, "integer") == 0) {
        return cJSON_IsNumber(value) && value->valuedouble == (double)((int64_t)value->valuedouble);
    }
    if (strcmp(type, "boolean") == 0) return cJSON_IsBool(value);
    if (strcmp(type, "object") == 0) return cJSON_IsObject(value);
    if (strcmp(type, "array") == 0) return cJSON_IsArray(value);
    return false;
}

static esp_err_t validate_arguments(const fridge_tool_definition_t *definition,
                                    const char *arguments_json,
                                    fridge_tool_result_t *result)
{
    cJSON *args = cJSON_Parse(arguments_json && arguments_json[0] ? arguments_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                         "SCHEMA_VALIDATION_FAILED", "工具参数必须是 JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *schema = cJSON_Parse(definition->input_schema[0] ? definition->input_schema : "{}");
    if (!schema || !cJSON_IsObject(schema)) {
        cJSON_Delete(args);
        cJSON_Delete(schema);
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                         "INVALID_TOOL_SCHEMA", "工具注册的参数 Schema 无效");
        return ESP_ERR_INVALID_STATE;
    }

    const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    if (cJSON_IsArray(required)) {
        const cJSON *name = NULL;
        cJSON_ArrayForEach(name, required) {
            if (cJSON_IsString(name) && !cJSON_GetObjectItemCaseSensitive(args, name->valuestring)) {
                char summary[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
                snprintf(summary, sizeof(summary), "缺少必填参数：%s", name->valuestring);
                cJSON_Delete(args);
                cJSON_Delete(schema);
                set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                                 "SCHEMA_VALIDATION_FAILED", summary);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    if (cJSON_IsObject(properties)) {
        const cJSON *property = NULL;
        cJSON_ArrayForEach(property, properties) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, property->string);
            if (!value) continue;
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(property, "type");
            if (cJSON_IsString(type) && !schema_type_matches(value, type->valuestring)) {
                char summary[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
                snprintf(summary, sizeof(summary), "参数 %s 类型应为 %s", property->string, type->valuestring);
                cJSON_Delete(args);
                cJSON_Delete(schema);
                set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                                 "SCHEMA_VALIDATION_FAILED", summary);
                return ESP_ERR_INVALID_ARG;
            }
            if (cJSON_IsNumber(value)) {
                const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(property, "minimum");
                const cJSON *maximum = cJSON_GetObjectItemCaseSensitive(property, "maximum");
                if ((cJSON_IsNumber(minimum) && value->valuedouble < minimum->valuedouble) ||
                    (cJSON_IsNumber(maximum) && value->valuedouble > maximum->valuedouble)) {
                    char summary[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
                    snprintf(summary, sizeof(summary), "参数 %s 超出允许范围", property->string);
                    cJSON_Delete(args);
                    cJSON_Delete(schema);
                    set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                                     "SCHEMA_VALIDATION_FAILED", summary);
                    return ESP_ERR_INVALID_ARG;
                }
            }
            if (cJSON_IsString(value)) {
                const size_t length = strlen(value->valuestring);
                const cJSON *min_length = cJSON_GetObjectItemCaseSensitive(property, "minLength");
                const cJSON *max_length = cJSON_GetObjectItemCaseSensitive(property, "maxLength");
                if ((cJSON_IsNumber(min_length) && length < (size_t)min_length->valuedouble) ||
                    (cJSON_IsNumber(max_length) && length > (size_t)max_length->valuedouble)) {
                    char summary[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
                    snprintf(summary, sizeof(summary), "参数 %s 长度不符合要求", property->string);
                    cJSON_Delete(args);
                    cJSON_Delete(schema);
                    set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                                     "SCHEMA_VALIDATION_FAILED", summary);
                    return ESP_ERR_INVALID_ARG;
                }
            }
        }
    }

    cJSON_Delete(args);
    cJSON_Delete(schema);
    return ESP_OK;
}

static void append_tool_trace(const fridge_tool_call_t *call, const fridge_tool_result_t *result)
{
    cJSON *trace = cJSON_CreateObject();
    if (!trace) return;
    cJSON_AddStringToObject(trace, "type", "tool_call");
    cJSON_AddStringToObject(trace, "run_id", call->run_id);
    cJSON_AddStringToObject(trace, "call_id", call->call_id);
    cJSON_AddStringToObject(trace, "tool", call->tool_name);
    cJSON_AddStringToObject(trace, "status", fridge_tool_result_status_name(result->status));
    cJSON_AddStringToObject(trace, "code", result->code);
    cJSON_AddNumberToObject(trace, "latency_ms", result->latency_ms);
    char *json = cJSON_PrintUnformatted(trace);
    if (json) {
        (void)fridge_trace_append_run(json);
        cJSON_free(json);
    }
    cJSON_Delete(trace);
}

esp_err_t fridge_tool_broker_init(void)
{
    if (s_broker_ready) return ESP_OK;
    s_execute_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_execute_mutex, ESP_ERR_NO_MEM, TAG, "创建 Tool Broker 互斥锁失败");
    esp_err_t err = fridge_tool_registry_init();
    if (err == ESP_OK) err = fridge_builtin_tools_register_all();
    if (err == ESP_OK) err = fridge_business_tools_register_all();
    if (err == ESP_OK) err = fridge_task_scheduler_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_execute_mutex);
        s_execute_mutex = NULL;
        ESP_LOGE(TAG, "Tool Broker 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    load_permission_mode();
    s_broker_ready = true;
    ESP_LOGI(TAG, "Tool Broker ready, mode=%s, tools=%u",
             fridge_agent_permission_mode_name(s_permission_mode),
             (unsigned)fridge_tool_registry_count());
    return ESP_OK;
}

esp_err_t fridge_tool_broker_get_permission_mode(fridge_agent_permission_mode_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_permission_mode;
    return ESP_OK;
}

esp_err_t fridge_tool_broker_set_permission_mode(fridge_agent_permission_mode_t mode)
{
    if (!permission_mode_valid(mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_permission_mode = mode;
    ESP_LOGI(TAG, "Tool permission mode set to %s", fridge_agent_permission_mode_name(s_permission_mode));
    return ESP_OK;
}

esp_err_t fridge_tool_broker_set_permission_mode_persisted(fridge_agent_permission_mode_t mode)
{
    if (!permission_mode_valid(mode)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PERMISSION_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, PERMISSION_NVS_KEY, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    return fridge_tool_broker_set_permission_mode(mode);
}

bool fridge_tool_broker_can_execute(fridge_tool_permission_t permission,
                                    bool requires_confirmation,
                                    bool user_confirmed)
{
    if (permission == FRIDGE_TOOL_PERMISSION_READ && !requires_confirmation) {
        return true;
    }
    if (user_confirmed) {
        return true;
    }
    if (s_permission_mode == FRIDGE_AGENT_PERMISSION_STANDARD) {
        // 标准模式按用户设置自动执行所有普通写入；危险工具仍需要明确确认。
        return permission == FRIDGE_TOOL_PERMISSION_WRITE;
    }
    if (s_permission_mode == FRIDGE_AGENT_PERMISSION_AUTO) {
        // 自动模式等价于用户在设备端明确授予完全访问权，包含删除、清空和密钥修改。
        // 仅设置页的二次风险确认会调用持久化接口写入该模式。
        return permission == FRIDGE_TOOL_PERMISSION_READ ||
               permission == FRIDGE_TOOL_PERMISSION_WRITE ||
               permission == FRIDGE_TOOL_PERMISSION_DANGEROUS;
    }
    (void)requires_confirmation;
    return false;
}

esp_err_t fridge_tool_broker_call(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    if (!call || !result || !call->tool_name[0]) {
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR, "INVALID_ARG", "tool_name is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_broker_ready) {
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR, "BROKER_NOT_READY", "Tool Broker 尚未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    fridge_tool_definition_t definition = {0};
    esp_err_t err = fridge_tool_registry_find(call->tool_name, &definition);
    if (err != ESP_OK) {
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR, "TOOL_NOT_FOUND", "工具不存在或尚未注册");
        return err;
    }

    fridge_tool_call_t effective_call = *call;
    if (!effective_call.arguments_json[0]) {
        strlcpy(effective_call.arguments_json, "{}", sizeof(effective_call.arguments_json));
    }
    if (definition.permission != FRIDGE_TOOL_PERMISSION_READ &&
        definition.idempotent && !effective_call.idempotency_key[0]) {
        // USB request_id/Agent call_id 在调用方重试时应保持稳定，可作为未显式传 key 时的安全兜底。
        strlcpy(effective_call.idempotency_key,
                effective_call.call_id[0] ? effective_call.call_id : effective_call.run_id,
                sizeof(effective_call.idempotency_key));
    }
    err = validate_arguments(&definition, effective_call.arguments_json, result);
    if (err != ESP_OK) return err;

    if (!fridge_tool_broker_can_execute(definition.permission,
                                         definition.requires_confirmation,
                                         call->user_confirmed)) {
        char summary[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
        snprintf(summary, sizeof(summary),
                 "工具权限为 %s，当前 Agent 权限模式为 %s，需要用户确认后才能执行",
                 fridge_tool_permission_name(definition.permission),
                 fridge_agent_permission_mode_name(s_permission_mode));
        set_result_error(result, FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION,
                         "CONFIRMATION_REQUIRED",
                         summary);
        return ESP_ERR_NOT_ALLOWED;
    }

    const TickType_t lock_timeout = pdMS_TO_TICKS(definition.timeout_ms ? definition.timeout_ms : 5000);
    if (xSemaphoreTake(s_execute_mutex, lock_timeout) != pdTRUE) {
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                         "TOOL_BUSY_TIMEOUT", "等待 Tool 串行执行锁超时");
        return ESP_ERR_TIMEOUT;
    }

    // Tool 处理器可能执行“读 JSON→修改→写回”。必须在整个处理器期间持有
    // Agent Storage 事务锁，避免 MQTT sync.apply 在两次 I/O 之间插入并造成更新丢失。
    err = fridge_agent_storage_transaction_begin(
        definition.timeout_ms ? definition.timeout_ms : 5000U);
    if (err != ESP_OK) {
        xSemaphoreGive(s_execute_mutex);
        set_result_error(result, FRIDGE_TOOL_RESULT_ERROR,
                         "AGENT_STORAGE_BUSY", "等待 Agent 数据事务锁超时");
        return err;
    }

    const int64_t start_us = esp_timer_get_time();
    memset(result, 0, sizeof(*result));
    err = definition.execute(&effective_call, result);
    fridge_agent_storage_transaction_end();
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    result->latency_ms = (uint32_t)(elapsed_us > 0 ? elapsed_us / 1000 : 0);

    if (err != ESP_OK) {
        if (result->code[0] == '\0') {
            strlcpy(result->code, esp_err_to_name(err), sizeof(result->code));
        }
        if (result->summary[0] == '\0') {
            strlcpy(result->summary, "工具执行失败", sizeof(result->summary));
        }
        if (result->data_json[0] == '\0') {
            strlcpy(result->data_json, "{}", sizeof(result->data_json));
        }
        result->status = FRIDGE_TOOL_RESULT_ERROR;
        xSemaphoreGive(s_execute_mutex);
        append_tool_trace(&effective_call, result);
        return err;
    }

    // execute() 也可能在序列化结果失败时通过 ToolResult 报错；不能因 C 返回值为 ESP_OK 而覆盖掉它。
    if (result->status != FRIDGE_TOOL_RESULT_OK) {
        if (result->code[0] == '\0') strlcpy(result->code, "TOOL_RESULT_ERROR", sizeof(result->code));
        if (result->summary[0] == '\0') strlcpy(result->summary, "工具返回失败状态", sizeof(result->summary));
        if (result->data_json[0] == '\0') strlcpy(result->data_json, "{}", sizeof(result->data_json));
        xSemaphoreGive(s_execute_mutex);
        append_tool_trace(&effective_call, result);
        return result->status == FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION ? ESP_ERR_NOT_ALLOWED : ESP_FAIL;
    }

    if (result->code[0] == '\0') {
        strlcpy(result->code, "SUCCESS", sizeof(result->code));
    }
    if (result->summary[0] == '\0') {
        strlcpy(result->summary, "工具执行成功", sizeof(result->summary));
    }
    if (result->data_json[0] == '\0') {
        strlcpy(result->data_json, "{}", sizeof(result->data_json));
    }
    result->status = FRIDGE_TOOL_RESULT_OK;
    xSemaphoreGive(s_execute_mutex);
    if (definition.permission != FRIDGE_TOOL_PERMISSION_READ &&
        fridge_mqtt_notify_tool_success) {
        fridge_mqtt_notify_tool_success(&effective_call, result);
    }
    append_tool_trace(&effective_call, result);
    return ESP_OK;
}
