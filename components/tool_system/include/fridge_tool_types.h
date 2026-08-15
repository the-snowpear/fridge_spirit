// 冰箱小精灵 Agent Tool 公共类型。
//
// Tool 是 Agent 能调用的最小原子能力。模型只能提出 Tool Call，请求必须经过 Tool Broker
// 做存在性、参数、权限和执行结果规范化检查，不能绕过本层直接修改库存、记忆或设备状态。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TOOL_MAX_NAME_LEN 48
#define FRIDGE_TOOL_MAX_DESC_LEN 160
#define FRIDGE_TOOL_MAX_SCHEMA_LEN 512
#define FRIDGE_TOOL_MAX_ARGS_LEN 1024
#define FRIDGE_TOOL_MAX_RESULT_LEN 4096
#define FRIDGE_TOOL_MAX_SUMMARY_LEN 192
#define FRIDGE_TOOL_MAX_CODE_LEN 48
#define FRIDGE_TOOL_MAX_TOOLS 32

typedef enum {
    FRIDGE_TOOL_PERMISSION_READ = 0,      // 只读工具，默认允许自动执行。
    FRIDGE_TOOL_PERMISSION_WRITE = 1,     // 写入本地数据，是否自动执行由 Agent 权限模式决定。
    FRIDGE_TOOL_PERMISSION_DANGEROUS = 2, // 删除、清空、恢复出厂、修改密钥等危险操作，始终需要确认。
} fridge_tool_permission_t;

typedef enum {
    FRIDGE_AGENT_PERMISSION_CONFIRM = 0,  // 确认模式：仅 READ 自动执行；WRITE/DANGEROUS 等待确认。
    FRIDGE_AGENT_PERMISSION_STANDARD = 1, // 标准模式：READ 与普通 WRITE 自动执行；DANGEROUS 等待确认。
    FRIDGE_AGENT_PERMISSION_AUTO = 2,     // 自动模式：用户明确授予完全访问权，READ/WRITE/DANGEROUS 均自动执行。
} fridge_agent_permission_mode_t;

typedef enum {
    FRIDGE_TOOL_RESULT_OK = 0,
    FRIDGE_TOOL_RESULT_ERROR = 1,
    FRIDGE_TOOL_RESULT_DENIED = 2,
    FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION = 3,
} fridge_tool_result_status_t;

typedef struct fridge_tool_call fridge_tool_call_t;
typedef struct fridge_tool_result fridge_tool_result_t;
typedef esp_err_t (*fridge_tool_execute_fn_t)(const fridge_tool_call_t *call, fridge_tool_result_t *result);

typedef struct {
    char name[FRIDGE_TOOL_MAX_NAME_LEN + 1];          // 工具名，例如 inventory.list。
    char description[FRIDGE_TOOL_MAX_DESC_LEN + 1];   // 给模型和调试端看的简短说明。
    fridge_tool_permission_t permission;              // 权限等级。
    bool idempotent;                                  // 相同参数重复调用是否安全。
    bool requires_confirmation;                       // 是否必须用户确认。
    uint32_t timeout_ms;                              // 预期超时，0 表示使用默认值。
    char input_schema[FRIDGE_TOOL_MAX_SCHEMA_LEN + 1]; // 简化 JSON Schema 文本。
    fridge_tool_execute_fn_t execute;                 // 本地执行函数。
} fridge_tool_definition_t;

struct fridge_tool_call {
    char call_id[32];
    char run_id[32];
    char tool_name[FRIDGE_TOOL_MAX_NAME_LEN + 1];
    char arguments_json[FRIDGE_TOOL_MAX_ARGS_LEN + 1];
    char idempotency_key[64];
    bool user_confirmed; // 调用方是否已经完成用户确认；DANGEROUS 和部分 WRITE 依赖此字段。
};

struct fridge_tool_result {
    fridge_tool_result_status_t status;
    char code[FRIDGE_TOOL_MAX_CODE_LEN + 1];
    char summary[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
    char data_json[FRIDGE_TOOL_MAX_RESULT_LEN + 1];
    uint32_t latency_ms;
};

const char *fridge_tool_permission_name(fridge_tool_permission_t permission);
const char *fridge_agent_permission_mode_name(fridge_agent_permission_mode_t mode);
bool fridge_agent_permission_mode_from_name(const char *name, fridge_agent_permission_mode_t *out);
const char *fridge_tool_result_status_name(fridge_tool_result_status_t status);

#ifdef __cplusplus
}
#endif
