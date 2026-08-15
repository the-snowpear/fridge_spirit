// 冰箱小精灵 Agent Tool Registry。
//
// Registry 只保存工具定义和执行函数指针，不直接执行工具。所有执行必须走 Tool Broker，
// 以保证权限、Trace 和结果格式一致。

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "fridge_tool_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fridge_tool_registry_init(void);
esp_err_t fridge_tool_registry_register(const fridge_tool_definition_t *definition);
esp_err_t fridge_tool_registry_find(const char *name, fridge_tool_definition_t *out);
esp_err_t fridge_tool_registry_list(fridge_tool_definition_t *out, size_t capacity, size_t *out_count);
size_t fridge_tool_registry_count(void);

#ifdef __cplusplus
}
#endif
