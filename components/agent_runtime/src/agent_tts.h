// 冰箱小精灵 P4 Agent 云端 TTS 适配。
// 复用 fridge_tts NVS 配置和 MOSS 协议，但只通过板载 ES8311 的 16k 输出路径播放。

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "fridge_agent_runtime.h"

typedef void (*fridge_agent_tts_state_cb_t)(const char *state, const char *detail, void *user_ctx);

esp_err_t fridge_agent_tts_synthesize_and_play(const char *text,
                                                fridge_agent_tts_state_cb_t state_cb,
                                                void *user_ctx);
void fridge_agent_tts_stop(void);
bool fridge_agent_tts_is_active(void);
void fridge_agent_tts_get_status(fridge_agent_runtime_tts_status_t *out);
