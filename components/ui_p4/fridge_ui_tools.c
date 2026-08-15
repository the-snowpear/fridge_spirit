// 冰箱小精灵 P4 UI Tool 注册。
//
// ui.switch_page / ui.set_orientation 归属 UI 组件：Tool 只把合法请求投递到
// UI 的 FreeRTOS 队列，绝不在 Tool Broker、Agent 或 USB 任务中直接触碰 LVGL。

#include "fridge_ui_p4.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "fridge_tool_registry.h"

static const char *TAG = "fridge_ui_tools";
static bool s_switch_page_registered;
static bool s_orientation_registered;

// 统一填充 Tool 错误结果，保证 USB 和 Agent 调用获得一致的可读错误信息。
static void ui_tool_result_error(fridge_tool_result_t *result, const char *code, const char *summary)
{
    result->status = FRIDGE_TOOL_RESULT_ERROR;
    strlcpy(result->code, code ? code : "ERROR", sizeof(result->code));
    strlcpy(result->summary, summary ? summary : "P4 UI 工具执行失败", sizeof(result->summary));
    strlcpy(result->data_json, "{}", sizeof(result->data_json));
}

// ui.switch_page 的执行函数只入队；真正的页面创建与 LVGL 修改始终在 ui_p4 任务执行。
static esp_err_t tool_ui_switch_page(const fridge_tool_call_t *call, fridge_tool_result_t *result)
{
    const char *arguments = call && call->arguments_json[0] ? call->arguments_json : "{}";
    cJSON *args = cJSON_Parse(arguments);
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        ui_tool_result_error(result, "INVALID_JSON", "ui.switch_page 参数必须是 JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *page_value = cJSON_GetObjectItemCaseSensitive(args, "page");
    if (!cJSON_IsString(page_value) || !page_value->valuestring[0]) {
        cJSON_Delete(args);
        ui_tool_result_error(result, "PAGE_REQUIRED", "ui.switch_page 需要 page 字段");
        return ESP_ERR_INVALID_ARG;
    }

    fridge_ui_p4_page_t page;
    if (fridge_ui_p4_page_from_key(page_value->valuestring, &page) != ESP_OK) {
        cJSON_Delete(args);
        ui_tool_result_error(result, "INVALID_PAGE", "ui.switch_page 的 page 不受支持");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = fridge_ui_p4_request_page_async(page);
    cJSON_Delete(args);
    if (err != ESP_OK) {
        ui_tool_result_error(result,
                             esp_err_to_name(err),
                             err == ESP_ERR_TIMEOUT ? "P4 页面请求队列已满，请稍后重试" :
                             "P4 UI 尚未启动，暂时无法切换页面");
        return err;
    }

    result->status = FRIDGE_TOOL_RESULT_OK;
    strlcpy(result->code, "SUCCESS", sizeof(result->code));
    strlcpy(result->summary, "已提交 P4 页面切换请求", sizeof(result->summary));
    snprintf(result->data_json, sizeof(result->data_json),
             "{\"page\":\"%s\",\"queued\":true}", fridge_ui_p4_page_key(page));
    return ESP_OK;
}

// ui.set_orientation 只提交稳定键名；面板仍保持原生 1024x600 MIPI 扫描。
static esp_err_t tool_ui_set_orientation(const fridge_tool_call_t *call,
                                         fridge_tool_result_t *result)
{
    const char *arguments = call && call->arguments_json[0] ? call->arguments_json : "{}";
    cJSON *args = cJSON_Parse(arguments);
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        ui_tool_result_error(result, "INVALID_JSON",
                             "ui.set_orientation 参数必须是 JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, "orientation");
    if (!cJSON_IsString(value) || !value->valuestring[0]) {
        cJSON_Delete(args);
        ui_tool_result_error(result, "ORIENTATION_REQUIRED",
                             "ui.set_orientation 需要 orientation 字段");
        return ESP_ERR_INVALID_ARG;
    }

    fridge_ui_p4_orientation_t orientation;
    if (fridge_ui_p4_orientation_from_key(value->valuestring, &orientation) != ESP_OK) {
        cJSON_Delete(args);
        ui_tool_result_error(result, "INVALID_ORIENTATION",
                             "orientation 仅支持 landscape 或 portrait_privacy");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = fridge_ui_p4_request_orientation_async(orientation);
    cJSON_Delete(args);
    if (err != ESP_OK) {
        ui_tool_result_error(
            result, esp_err_to_name(err),
            err == ESP_ERR_NOT_SUPPORTED ?
                "当前仅 HOME-02 支持竖屏，请先切换到 home_detail" :
            err == ESP_ERR_TIMEOUT ?
                "P4 UI 请求队列已满，请稍后重试" :
                "P4 UI 尚未启动，暂时无法切换横竖屏");
        return err;
    }

    result->status = FRIDGE_TOOL_RESULT_OK;
    strlcpy(result->code, "SUCCESS", sizeof(result->code));
    strlcpy(result->summary, "已提交 P4 横竖屏切换请求", sizeof(result->summary));
    snprintf(result->data_json, sizeof(result->data_json),
             "{\"orientation\":\"%s\",\"privacy\":%s,\"queued\":true}",
             fridge_ui_p4_orientation_key(orientation),
             orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ? "true" : "false");
    return ESP_OK;
}

esp_err_t fridge_ui_p4_register_tools(void)
{
    if (s_switch_page_registered && s_orientation_registered) {
        return ESP_OK;
    }

    // 注册必须发生在 fridge_tool_broker_init() 之后；main 负责保证该初始化顺序。
    if (!s_switch_page_registered) {
        fridge_tool_definition_t definition = {0};
        strlcpy(definition.name, "ui.switch_page", sizeof(definition.name));
        strlcpy(definition.description, "异步切换 P4 UI 页面，不写入业务数据。",
                sizeof(definition.description));
        definition.permission = FRIDGE_TOOL_PERMISSION_READ;
        definition.idempotent = true;
        definition.requires_confirmation = false;
        definition.timeout_ms = 3000;
        strlcpy(definition.input_schema,
                "{\"type\":\"object\",\"required\":[\"page\"],\"properties\":{\"page\":{\"type\":\"string\",\"enum\":[\"home\",\"home_detail\",\"inventory\",\"inventory_detail\",\"inventory_edit\",\"camera\",\"inventory_recognize\",\"inventory_search\",\"inventory_layout\",\"more\",\"timer\",\"alarm\",\"stopwatch\",\"plan\",\"shopping\",\"chat\",\"binding\",\"settings\",\"maintenance\"]}}}",
                sizeof(definition.input_schema));
        definition.execute = tool_ui_switch_page;
        const esp_err_t err = fridge_tool_registry_register(&definition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register ui.switch_page failed: %s", esp_err_to_name(err));
            return err;
        }
        s_switch_page_registered = true;
    }

    if (!s_orientation_registered) {
        fridge_tool_definition_t definition = {0};
        strlcpy(definition.name, "ui.set_orientation", sizeof(definition.name));
        strlcpy(definition.description,
                "异步切换 P4 横屏标准或竖屏隐私模式，不修改面板时序。",
                sizeof(definition.description));
        definition.permission = FRIDGE_TOOL_PERMISSION_READ;
        definition.idempotent = true;
        definition.requires_confirmation = false;
        definition.timeout_ms = 3000;
        strlcpy(definition.input_schema,
                "{\"type\":\"object\",\"required\":[\"orientation\"],\"properties\":{\"orientation\":{\"type\":\"string\",\"enum\":[\"landscape\",\"portrait_privacy\"]}}}",
                sizeof(definition.input_schema));
        definition.execute = tool_ui_set_orientation;
        const esp_err_t err = fridge_tool_registry_register(&definition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register ui.set_orientation failed: %s", esp_err_to_name(err));
            return err;
        }
        s_orientation_registered = true;
    }
    return ESP_OK;
}
