// 冰箱小精灵业务写入 Tool 注册入口。
//
// 本头文件只暴露注册函数。业务工具仍必须经 Tool Broker 执行，确保 WRITE/DANGEROUS
// 权限、用户确认和结果格式统一，不允许 Agent 绕过 Broker 直接改 TF 卡数据。

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fridge_business_tools_register_all(void);

#ifdef __cplusplus
}
#endif
