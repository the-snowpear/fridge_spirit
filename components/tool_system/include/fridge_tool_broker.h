// 冰箱小精灵 Agent Tool Broker。
//
// Broker 是模型调用本地能力的唯一入口：先检查工具是否存在、权限是否允许，再执行工具，
// 最后把结果标准化为 ToolResult。后续 WRITE/DANGEROUS 确认、幂等键和 Trace 都在这里扩展。

#pragma once

#include "esp_err.h"
#include "fridge_tool_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fridge_tool_broker_init(void);
esp_err_t fridge_tool_broker_get_permission_mode(fridge_agent_permission_mode_t *out);
esp_err_t fridge_tool_broker_set_permission_mode(fridge_agent_permission_mode_t mode);
// 将用户明确选择的权限模式保存到 NVS。该调用可能擦写 Flash，必须由后台任务执行。
esp_err_t fridge_tool_broker_set_permission_mode_persisted(fridge_agent_permission_mode_t mode);
bool fridge_tool_broker_can_execute(fridge_tool_permission_t permission,
                                    bool requires_confirmation,
                                    bool user_confirmed);
esp_err_t fridge_tool_broker_call(const fridge_tool_call_t *call, fridge_tool_result_t *result);

#ifdef __cplusplus
}
#endif
