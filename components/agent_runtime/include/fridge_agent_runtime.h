// 冰箱小精灵单会话 Agent Runtime 公共接口。
//
// UI 只通过异步命令和事件与本组件交互；HTTP、TF、Tool 和音频任务绝不直接访问 LVGL。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_AGENT_RUNTIME_MAX_INPUT_LEN 512
#define FRIDGE_AGENT_RUNTIME_MAX_REPLY_LEN 2048
#define FRIDGE_AGENT_RUNTIME_MAX_TRACE_LEN 2048
#define FRIDGE_AGENT_RUNTIME_MAX_ERROR_LEN 160
#define FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN 32
#define FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN 32
#define FRIDGE_AGENT_RUNTIME_UI_TEXT_LEN 512
#define FRIDGE_AGENT_RUNTIME_UI_DETAIL_LEN 192
#define FRIDGE_AGENT_RUNTIME_MAX_TOOL_ARGS_LEN 1024
#define FRIDGE_AGENT_RUNTIME_TTS_MAX_TEXT_LEN 240

typedef enum {
    FRIDGE_AGENT_RUN_IDLE = 0,
    FRIDGE_AGENT_RUN_ROUTING,
    FRIDGE_AGENT_RUN_BUILDING_CONTEXT,
    FRIDGE_AGENT_RUN_WAITING_MODEL,
    FRIDGE_AGENT_RUN_WAITING_TOOL,
    FRIDGE_AGENT_RUN_WAITING_CONFIRMATION,
    FRIDGE_AGENT_RUN_RESPONDING,
    FRIDGE_AGENT_RUN_COMPLETED,
    FRIDGE_AGENT_RUN_FAILED,
    FRIDGE_AGENT_RUN_CANCELLED,
} fridge_agent_run_state_t;

typedef enum {
    FRIDGE_AGENT_INTENT_UNKNOWN = 0,
    FRIDGE_AGENT_INTENT_INVENTORY_QUERY,
    FRIDGE_AGENT_INTENT_MEMORY_READ,
    FRIDGE_AGENT_INTENT_DEVICE_STATE,
    FRIDGE_AGENT_INTENT_HELP,
} fridge_agent_intent_t;

// 面向 UI 的轻量事件。该类型不引用 ui_p4，避免 Agent/Tool/UI 出现静态依赖环。
typedef enum {
    FRIDGE_AGENT_RUNTIME_EVENT_SESSION_RESET = 0,
    FRIDGE_AGENT_RUNTIME_EVENT_USER_MESSAGE,
    FRIDGE_AGENT_RUNTIME_EVENT_ASSISTANT_MESSAGE,
    FRIDGE_AGENT_RUNTIME_EVENT_TOOL,
    FRIDGE_AGENT_RUNTIME_EVENT_STATUS,
    FRIDGE_AGENT_RUNTIME_EVENT_TTS,
} fridge_agent_runtime_event_kind_t;

typedef struct {
    fridge_agent_runtime_event_kind_t kind;
    char run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN];
    char call_id[FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN];
    char time[8];
    char state[32];
    char text[FRIDGE_AGENT_RUNTIME_UI_TEXT_LEN + 1];
    char tool_name[48];
    char tool_label[64];
    char detail[FRIDGE_AGENT_RUNTIME_UI_DETAIL_LEN + 1];
    char latency[16];
    uint8_t risk;
    uint8_t tool_status;
    bool requires_confirmation;
    bool working;
    // 仅在本轮 COMPLETED 的助手消息和状态事件中为 true，供连续语音会话决定是否收尾。
    bool dialog_complete;
} fridge_agent_runtime_ui_event_t;

typedef struct {
    bool ready;                                           // Runtime 是否初始化完成。
    uint32_t run_sequence;                                // 已创建 Run 数量。
    char last_run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN];
    char last_state[32];
    char last_intent[48];
    char last_error[FRIDGE_AGENT_RUNTIME_MAX_ERROR_LEN + 1];
    uint32_t context_bytes;                              // 最近一次完整 Agent 上下文字节数。
    uint16_t tool_schema_count;                          // 最近一次注入的完整工具 Schema 数量。
    uint8_t model_steps;                                 // 当前/最近 Run 已执行的模型步骤数。
    uint32_t cloud_stack_high_water;                     // PSRAM Cloud Worker 剩余栈高水位。
    uint32_t free_psram_bytes;                           // 查询时剩余 PSRAM。
    uint32_t internal_dma_largest_bytes;                 // 查询时内部 DMA 最大连续块。
    bool cloud_pending;                                  // 是否仍有 HTTPS Worker 在收尾。
} fridge_agent_runtime_status_t;

typedef struct {
    bool ok;
    char run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN];
    fridge_agent_run_state_t state;
    fridge_agent_intent_t intent;
    char selected_tool[48];
    char reply[FRIDGE_AGENT_RUNTIME_MAX_REPLY_LEN + 1];
    char trace_json[FRIDGE_AGENT_RUNTIME_MAX_TRACE_LEN + 1];
    char error[FRIDGE_AGENT_RUNTIME_MAX_ERROR_LEN + 1];
    int http_status;                                    // 最近一个模型步骤的 HTTP 状态，未请求云端时为 0。
    uint32_t model_latency_ms;                          // 最近一个模型步骤的网络耗时。
    bool dialog_complete;                               // 模型确认本轮已结束且无需继续候听。
} fridge_agent_run_result_t;

// 面向 USB/Web 的当前 Run 快照。该接口只复制状态，不消费 LVGL 专用事件队列，
// 避免 Web 轮询抢走本地 CHAT-01 应处理的消息。
typedef struct {
    bool active;
    char run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN];
    char state[32];
    char intent[48];
    char reply[FRIDGE_AGENT_RUNTIME_MAX_REPLY_LEN + 1];
    char error[FRIDGE_AGENT_RUNTIME_MAX_ERROR_LEN + 1];
    int http_status;
    uint32_t model_latency_ms;
    bool dialog_complete;
    bool requires_confirmation;
    char call_id[FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN];
    char tool_name[48 + 1];
    char tool_arguments_json[FRIDGE_AGENT_RUNTIME_MAX_TOOL_ARGS_LEN + 1];
    char tool_permission[16];
} fridge_agent_runtime_run_snapshot_t;

// P4 板载 ES8311 TTS 快照，供本地 Agent 与 USB/Web 测试共用同一播放链路。
typedef struct {
    char state[16];
    uint32_t sample_rate;
    size_t audio_bytes;
    size_t played_bytes;
    uint32_t duration_ms;
    uint32_t latency_ms;
    int http_status;
    char model[65];
    char voice[100];
    char error[161];
} fridge_agent_runtime_tts_status_t;

esp_err_t fridge_agent_runtime_init(void);
void fridge_agent_runtime_get_status(fridge_agent_runtime_status_t *out);

// 异步前台会话入口。输入只会进入唯一的 foreground Session；函数成功仅表示请求已排队，
// UI 必须通过已注册的事件回调读取状态和消息，禁止等待云端请求完成。
esp_err_t fridge_agent_runtime_submit_text(const char *input);
// USB/Web 异步入口需要立即取得 Run ID，后续通过只读快照轮询，不阻塞串口任务。
esp_err_t fridge_agent_runtime_submit_text_with_id(const char *input,
                                                   char *out_run_id,
                                                   size_t out_run_id_size);
// 请求取消当前前台 Run。网络层不强杀 TLS 请求，但迟到结果会被丢弃，且不会再触发工具或播报。
esp_err_t fridge_agent_runtime_cancel_active(void);
// 对处于 WAITING_CONFIRMATION 的 Tool Call 作出决定；确认后才可能执行 WRITE/DANGEROUS 工具。
esp_err_t fridge_agent_runtime_resolve_confirmation(const char *run_id,
                                                    const char *call_id,
                                                    bool confirmed);
// 非阻塞读取 UI 事件。只有 UI 任务可消费并转换为 LVGL 更新。
bool fridge_agent_runtime_poll_ui_event(fridge_agent_runtime_ui_event_t *out);

// 获取当前或最近一次 Run 的只读快照；不会消费 UI 事件。
void fridge_agent_runtime_get_run_snapshot(fridge_agent_runtime_run_snapshot_t *out);

// 清空唯一 foreground Session。运行中或 HTTPS Worker 收尾时拒绝，避免历史与当前 Run 分叉。
esp_err_t fridge_agent_runtime_clear_session(void);

// 兼容已有 USB 调试入口的同步最小闭环；新屏幕页面不得调用本接口。
esp_err_t fridge_agent_runtime_run_text(const char *input, fridge_agent_run_result_t *out);

// 同步执行一次 TTS；调用方应放在独立内部 SRAM 栈任务，禁止从 UI/USB 协议任务直接阻塞调用。
esp_err_t fridge_agent_runtime_tts_play_blocking(const char *text);
void fridge_agent_runtime_tts_stop(void);
bool fridge_agent_runtime_tts_is_active(void);
void fridge_agent_runtime_tts_get_status(fridge_agent_runtime_tts_status_t *out);

const char *fridge_agent_run_state_name(fridge_agent_run_state_t state);
const char *fridge_agent_intent_name(fridge_agent_intent_t intent);

#ifdef __cplusplus
}
#endif
