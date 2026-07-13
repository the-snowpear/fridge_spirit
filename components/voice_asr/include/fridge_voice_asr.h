#pragma once

// ESP32-P4 唤醒后云端 ASR 编排接口。
// 本组件只负责“小冰小冰 -> 录音 -> 转写”，不调用 AI，也不控制扬声器。

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fridge_asr.h"

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

// 创建自动录音任务和持久监听策略任务；不会立即加载模型。
esp_err_t fridge_voice_asr_init(void);

// P4 外设初始化完成后开启策略评估；静态测试屏按常亮处理。
void fridge_voice_asr_set_hardware_ready(void);

esp_err_t fridge_voice_asr_set_event_callback(fridge_voice_asr_event_cb_t cb, void *user_ctx);

// 手动 ASR 和自动唤醒 ASR 共用同一个串行 HTTPS 执行器。
esp_err_t fridge_voice_asr_transcribe_latest(fridge_asr_result_t *out);

// 手动 ASR 录音窗口暂停持久 WakeNet，防止录音结束后模型抢占 I2S 并清空 PCM。
esp_err_t fridge_voice_asr_manual_record_begin(void);
void fridge_voice_asr_manual_session_finish(void);

const char *fridge_voice_asr_state_text(fridge_voice_asr_state_t state);

#ifdef __cplusplus
}
#endif
