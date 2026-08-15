#pragma once

// ESP32-P4 本地 ASR 与连续语音会话编排接口。
// 手动对话页仍只使用短录音转写；WakeNet 与全局右侧语音入口复用连续 ASR→Agent→TTS 链路。

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fridge_asr.h"
#include "fridge_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRIDGE_VOICE_ASR_STATE_IDLE = 0,
    FRIDGE_VOICE_ASR_STATE_RECORDING,
    FRIDGE_VOICE_ASR_STATE_TRANSCRIBING,
    FRIDGE_VOICE_ASR_STATE_DONE,
    FRIDGE_VOICE_ASR_STATE_ERROR,
} fridge_voice_asr_state_t;

typedef struct {
    fridge_voice_asr_state_t state;
    uint32_t trigger_id;
    fridge_asr_result_t result;
    char error[FRIDGE_ASR_MAX_ERROR_LEN + 1];
} fridge_voice_asr_event_t;

typedef void (*fridge_voice_asr_event_cb_t)(const fridge_voice_asr_event_t *event, void *user_ctx);

typedef enum {
    FRIDGE_VOICE_DIALOG_STATE_IDLE = 0,
    FRIDGE_VOICE_DIALOG_STATE_PROMPTING,
    FRIDGE_VOICE_DIALOG_STATE_LISTENING,
    FRIDGE_VOICE_DIALOG_STATE_TRANSCRIBING,
    FRIDGE_VOICE_DIALOG_STATE_THINKING,
    FRIDGE_VOICE_DIALOG_STATE_WAITING_CONFIRMATION,
    FRIDGE_VOICE_DIALOG_STATE_SPEAKING,
    FRIDGE_VOICE_DIALOG_STATE_ERROR,
} fridge_voice_dialog_state_t;

typedef struct {
    bool active;
    fridge_voice_dialog_state_t state;
    char state_text[24];
    char detail[96];
} fridge_voice_dialog_status_t;

// 创建连续会话协调任务和持久监听策略任务；不会立即加载模型。
esp_err_t fridge_voice_asr_init(void);

// P4 外设初始化完成后开启策略评估；静态测试屏按常亮处理。
void fridge_voice_asr_set_hardware_ready(void);

esp_err_t fridge_voice_asr_set_event_callback(fridge_voice_asr_event_cb_t cb, void *user_ctx);

// 手动 ASR 和自动唤醒 ASR 共用同一个串行 HTTPS 执行器。
esp_err_t fridge_voice_asr_transcribe_latest(fridge_asr_result_t *out);

// 手动 ASR 录音窗口暂停持久 WakeNet，防止录音结束后模型抢占 I2S 并清空 PCM。
esp_err_t fridge_voice_asr_manual_record_begin(void);
void fridge_voice_asr_manual_session_finish(void);

// 连续会话入口只投递后台命令，不在 WakeNet/LVGL 回调中等待 I2S、HTTPS 或 TTS。
esp_err_t fridge_voice_asr_dialog_start(void);
esp_err_t fridge_voice_asr_dialog_stop(void);
esp_err_t fridge_voice_asr_dialog_get_status(fridge_voice_dialog_status_t *out);

// 由 main 的 Agent Runtime→UI 桥同步投递已复制的事件。函数不消费 UI 队列，也不访问 LVGL。
esp_err_t fridge_voice_asr_dialog_notify_agent_event(const fridge_agent_runtime_ui_event_t *event);

const char *fridge_voice_dialog_state_text(fridge_voice_dialog_state_t state);

const char *fridge_voice_asr_state_text(fridge_voice_asr_state_t state);

#ifdef __cplusplus
}
#endif
