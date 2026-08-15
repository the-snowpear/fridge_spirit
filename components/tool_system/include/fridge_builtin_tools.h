// 冰箱小精灵内置 Tool 注册入口。
//
// 首批只注册只读工具，先把 Agent 调用本地真实数据的链路跑通；写入型库存、任务、记忆工具
// 后续在确认流和幂等键落地后再启用。

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fridge_builtin_tools_register_all(void);

#ifdef __cplusplus
}
#endif
