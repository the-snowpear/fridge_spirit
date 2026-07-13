// 冰箱小精灵公共语音会话接口。
// 该组件只复用最近一次录音结果，不直接控制 I2S 引脚；录音的开始/停止由调用方负责。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "fridge_ai_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_VOICE_SESSION_MAX_ERROR_LEN 160
#define FRIDGE_VOICE_SESSION_TRANSCRIPT_LEN 768
#define FRIDGE_VOICE_SESSION_REPLY_LEN FRIDGE_AI_MAX_REPLY_LEN

// 语音会话可选行为开关：由不同调用方决定是否在组件内部处理 TTS 播报和唤醒词暂停/恢复。
// 屏幕 AI 页需要这两项（play_tts/manage_wake_word=true）；USB 测试和状态机自动语音不需要（全 false）。
typedef struct {
    bool play_tts;          // 完成后若 TTS 开关打开，则把可展示回复合成并播放
    bool manage_wake_word;  // 运行期间暂停唤醒词监听，结束后按原状态恢复
} fridge_voice_session_options_t;

typedef struct {
    char transcript[FRIDGE_VOICE_SESSION_TRANSCRIPT_LEN + 1];
    char reply[FRIDGE_VOICE_SESSION_REPLY_LEN + 1];
    char asr_model[64 + 1];
    char ai_model[64 + 1];
    char error[FRIDGE_VOICE_SESSION_MAX_ERROR_LEN + 1];
    int asr_http_status;
    int ai_http_status;
    uint32_t asr_latency_ms;
    uint32_t ai_latency_ms;
    size_t audio_bytes;
    size_t history_pruned_count;
    bool tts_played;                // 本轮是否实际触发了 TTS 播放，供调用方设置状态文案
} fridge_voice_session_result_t;

// 执行“最近录音 -> ASR -> AI -> 保存历史 -> 上行同步”，options 控制 TTS/唤醒词等可选行为。
// 注意：该函数会进行 HTTPS/TLS 请求，必须在普通任务上下文调用，不能放在 UI/USB 热路径或中断中。
esp_err_t fridge_voice_session_run_latest_recording_ex(const fridge_voice_session_options_t *options,
                                                       fridge_voice_session_result_t *out);

// 兼容入口：等价于 options 全为 false（不播报、不管理唤醒词），USB 测试与状态机沿用此入口。
esp_err_t fridge_voice_session_run_latest_recording(fridge_voice_session_result_t *out);

#ifdef __cplusplus
}
#endif
