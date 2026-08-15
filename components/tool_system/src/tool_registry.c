// 冰箱小精灵 Agent Tool Registry 实现。
//
// 这里采用固定大小数组，避免在 ESP32-P4 长时间运行中频繁分配小块内存造成碎片。

#include "fridge_tool_registry.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "fridge_tool_registry";

static fridge_tool_definition_t s_tools[FRIDGE_TOOL_MAX_TOOLS];
static size_t s_tool_count;

const char *fridge_tool_permission_name(fridge_tool_permission_t permission)
{
    switch (permission) {
    case FRIDGE_TOOL_PERMISSION_READ:
        return "READ";
    case FRIDGE_TOOL_PERMISSION_WRITE:
        return "WRITE";
    case FRIDGE_TOOL_PERMISSION_DANGEROUS:
        return "DANGEROUS";
    default:
        return "UNKNOWN";
    }
}

const char *fridge_agent_permission_mode_name(fridge_agent_permission_mode_t mode)
{
    switch (mode) {
    case FRIDGE_AGENT_PERMISSION_CONFIRM:
        return "confirm";
    case FRIDGE_AGENT_PERMISSION_STANDARD:
        return "standard";
    case FRIDGE_AGENT_PERMISSION_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

bool fridge_agent_permission_mode_from_name(const char *name, fridge_agent_permission_mode_t *out)
{
    if (!name || !out) {
        return false;
    }
    if (strcmp(name, "confirm") == 0 || strcmp(name, "CONFIRM") == 0) {
        *out = FRIDGE_AGENT_PERMISSION_CONFIRM;
        return true;
    }
    if (strcmp(name, "standard") == 0 || strcmp(name, "STANDARD") == 0) {
        *out = FRIDGE_AGENT_PERMISSION_STANDARD;
        return true;
    }
    if (strcmp(name, "auto") == 0 || strcmp(name, "AUTO") == 0) {
        *out = FRIDGE_AGENT_PERMISSION_AUTO;
        return true;
    }
    return false;
}

const char *fridge_tool_result_status_name(fridge_tool_result_status_t status)
{
    switch (status) {
    case FRIDGE_TOOL_RESULT_OK:
        return "ok";
    case FRIDGE_TOOL_RESULT_ERROR:
        return "error";
    case FRIDGE_TOOL_RESULT_DENIED:
        return "denied";
    case FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION:
        return "pending_confirmation";
    default:
        return "unknown";
    }
}

esp_err_t fridge_tool_registry_init(void)
{
    memset(s_tools, 0, sizeof(s_tools));
    s_tool_count = 0;
    return ESP_OK;
}

esp_err_t fridge_tool_registry_register(const fridge_tool_definition_t *definition)
{
    if (!definition || !definition->name[0] || !definition->execute) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, definition->name) == 0) {
            s_tools[i] = *definition;
            ESP_LOGI(TAG, "updated tool: %s", definition->name);
            return ESP_OK;
        }
    }

    if (s_tool_count >= FRIDGE_TOOL_MAX_TOOLS) {
        ESP_LOGW(TAG, "tool registry full, cannot register %s", definition->name);
        return ESP_ERR_NO_MEM;
    }

    s_tools[s_tool_count++] = *definition;
    ESP_LOGI(TAG, "registered tool: %s", definition->name);
    return ESP_OK;
}

esp_err_t fridge_tool_registry_find(const char *name, fridge_tool_definition_t *out)
{
    if (!name || !name[0] || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            *out = s_tools[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_tool_registry_list(fridge_tool_definition_t *out, size_t capacity, size_t *out_count)
{
    if (!out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t n = s_tool_count < capacity ? s_tool_count : capacity;
    if (out && capacity > 0) {
        memcpy(out, s_tools, n * sizeof(s_tools[0]));
    }
    *out_count = n;
    return ESP_OK;
}

size_t fridge_tool_registry_count(void)
{
    return s_tool_count;
}
