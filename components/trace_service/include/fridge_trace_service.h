// 冰箱小精灵 Agent Trace Service 公共接口。
//
// Trace 用于比赛演示和调试，记录每次 Agent Run 的路由、工具调用和耗时。
// 本组件只写 TF 卡 /sdcard/agent/logs 下的有界 JSONL，不控制硬件、不记录 API Key 或 Wi-Fi 密码。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TRACE_RUNS_PATH "agent/logs/runs.jsonl"
#define FRIDGE_TRACE_RUNS_PREV_PATH "agent/logs/runs.prev.jsonl"
#define FRIDGE_TRACE_MAX_FILE_BYTES (64U * 1024U)
#define FRIDGE_TRACE_READ_MAX_BYTES 4096
#define FRIDGE_TRACE_MAX_ERROR_LEN 128

typedef struct {
    bool ready;                         // Trace Service 是否可用。
    uint32_t appended_runs;             // 本次启动成功追加的 Run 条数。
    uint32_t rotated_count;             // 本次启动日志轮转次数。
    uint64_t runs_bytes;                // 当前 runs.jsonl 估算大小。
    char last_error[FRIDGE_TRACE_MAX_ERROR_LEN + 1];
} fridge_trace_status_t;

esp_err_t fridge_trace_service_init(void);
esp_err_t fridge_trace_append_run(const char *trace_json);
esp_err_t fridge_trace_read_runs_tail(char *out, size_t out_size, size_t *out_len);
void fridge_trace_get_status(fridge_trace_status_t *out);

#ifdef __cplusplus
}
#endif
