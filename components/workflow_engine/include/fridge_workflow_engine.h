// 冰箱小精灵最小 Workflow Engine 公共接口。
//
// Workflow 是设备端确定性流程，不执行任意脚本；每一步副作用仍通过 Tool Broker。
// 当前最小版用于比赛演示和串口调试：启动内置流程、记录检查点、返回最终 ToolResult。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_WORKFLOW_MAX_ID_LEN 64
#define FRIDGE_WORKFLOW_MAX_STATE_LEN 32
#define FRIDGE_WORKFLOW_MAX_ERROR_LEN 160
#define FRIDGE_WORKFLOW_MAX_RESULT_LEN 2048
#define FRIDGE_WORKFLOW_ACTIVE_RUN_PATH "agent/runtime/active_run.json"

typedef enum {
    FRIDGE_WORKFLOW_STATE_IDLE = 0,
    FRIDGE_WORKFLOW_STATE_RUNNING,
    FRIDGE_WORKFLOW_STATE_WAITING_CONFIRMATION,
    FRIDGE_WORKFLOW_STATE_COMPLETED,
    FRIDGE_WORKFLOW_STATE_FAILED,
} fridge_workflow_state_t;

typedef struct {
    bool ready;                                      // Workflow Engine 是否初始化完成。
    uint32_t run_sequence;                          // 本次启动创建的 Workflow Run 数量。
    char last_workflow_id[FRIDGE_WORKFLOW_MAX_ID_LEN];
    char last_run_id[32];
    char last_state[FRIDGE_WORKFLOW_MAX_STATE_LEN];
    char last_error[FRIDGE_WORKFLOW_MAX_ERROR_LEN + 1];
} fridge_workflow_status_t;

typedef struct {
    bool ok;
    char workflow_id[FRIDGE_WORKFLOW_MAX_ID_LEN];
    char run_id[32];
    fridge_workflow_state_t state;
    char reply[256];
    char result_json[FRIDGE_WORKFLOW_MAX_RESULT_LEN + 1];
    char error[FRIDGE_WORKFLOW_MAX_ERROR_LEN + 1];
} fridge_workflow_run_result_t;

esp_err_t fridge_workflow_engine_init(void);
void fridge_workflow_engine_get_status(fridge_workflow_status_t *out);
esp_err_t fridge_workflow_engine_start(const char *workflow_id,
                                       const char *arguments_json,
                                       bool user_confirmed,
                                       fridge_workflow_run_result_t *out);
const char *fridge_workflow_state_name(fridge_workflow_state_t state);

#ifdef __cplusplus
}
#endif
