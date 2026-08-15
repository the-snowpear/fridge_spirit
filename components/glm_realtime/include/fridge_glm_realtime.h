// 冰箱小精灵智谱 GLM-Realtime 公共接口。
// 设备端 WebSocket 双工会话 + Tool Broker；API Key 仅存 NVS，禁止回显明文。
// 硬件注意：会话期间占用 ES8311/共享 I2C，与摄像头、WakeNet 短录音互斥。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_GLM_RT_MAX_URL_LEN 192
#define FRIDGE_GLM_RT_MAX_MODEL_LEN 64
#define FRIDGE_GLM_RT_MAX_VOICE_LEN 32
#define FRIDGE_GLM_RT_MAX_API_KEY_LEN 256
#define FRIDGE_GLM_RT_MAX_INSTRUCTIONS_LEN 2048
#define FRIDGE_GLM_RT_MAX_ERROR_LEN 160
#define FRIDGE_GLM_RT_MAX_TEXT_LEN 768
#define FRIDGE_GLM_RT_DEFAULT_URL "wss://open.bigmodel.cn/api/paas/v4/realtime"
#define FRIDGE_GLM_RT_DEFAULT_MODEL "glm-realtime-flash"
#define FRIDGE_GLM_RT_DEFAULT_VOICE "tongtong"
#define FRIDGE_GLM_RT_DEFAULT_TIMEOUT_MS 30000
#define FRIDGE_GLM_RT_DEFAULT_INSTRUCTIONS \
    "你是冰箱小精灵，厨房语音助手。用简短中文回答。" \
    "需要查库存、创建计时/提醒、读写记忆时，必须调用已提供的 function tools。" \
    "不要编造库存；危险删除类操作不可用。"

typedef enum {
    FRIDGE_GLM_RT_STATE_IDLE = 0,
    FRIDGE_GLM_RT_STATE_CONNECTING,
    FRIDGE_GLM_RT_STATE_SESSION_READY,
    FRIDGE_GLM_RT_STATE_LISTENING,
    FRIDGE_GLM_RT_STATE_RESPONDING,
    FRIDGE_GLM_RT_STATE_TOOL_CALL,
    FRIDGE_GLM_RT_STATE_SPEAKING,
    FRIDGE_GLM_RT_STATE_ERROR,
} fridge_glm_rt_state_t;

typedef enum {
    FRIDGE_GLM_RT_EVENT_STATE = 0,
    FRIDGE_GLM_RT_EVENT_TRANSCRIPT,
    FRIDGE_GLM_RT_EVENT_TEXT_DELTA,
    FRIDGE_GLM_RT_EVENT_AUDIO_STATE,
    FRIDGE_GLM_RT_EVENT_TOOL_CALL,
    FRIDGE_GLM_RT_EVENT_TOOL_RESULT,
    FRIDGE_GLM_RT_EVENT_ERROR,
    FRIDGE_GLM_RT_EVENT_DONE,
} fridge_glm_rt_event_type_t;

// 配置视图：只含脱敏信息，可安全经 USB/Web 返回。
typedef struct {
    char url[FRIDGE_GLM_RT_MAX_URL_LEN + 1];
    char model[FRIDGE_GLM_RT_MAX_MODEL_LEN + 1];
    char voice[FRIDGE_GLM_RT_MAX_VOICE_LEN + 1];
    char instructions[FRIDGE_GLM_RT_MAX_INSTRUCTIONS_LEN + 1];
    char vad_mode[16];
    uint32_t timeout_ms;
    bool enabled;
    bool has_api_key;
    bool ready;
    char api_key_preview[32];
    char last_error[FRIDGE_GLM_RT_MAX_ERROR_LEN + 1];
} fridge_glm_rt_config_view_t;

// 配置更新：update_api_key 为 true 时才覆盖 NVS 中的 Key。
typedef struct {
    char url[FRIDGE_GLM_RT_MAX_URL_LEN + 1];
    char model[FRIDGE_GLM_RT_MAX_MODEL_LEN + 1];
    char voice[FRIDGE_GLM_RT_MAX_VOICE_LEN + 1];
    char instructions[FRIDGE_GLM_RT_MAX_INSTRUCTIONS_LEN + 1];
    char api_key[FRIDGE_GLM_RT_MAX_API_KEY_LEN + 1];
    char vad_mode[16];
    uint32_t timeout_ms;
    bool enabled;
    bool update_api_key;
    bool update_enabled;
    int64_t config_updated_at_ms;
} fridge_glm_rt_config_update_t;

typedef struct {
    bool ready;
    bool session_active;
    bool network_ok;
    bool internet_ready;
    bool audio_up_active;
    bool audio_down_active;
    bool tools_ready;
    fridge_glm_rt_state_t state;
    char state_name[24];
    char model[FRIDGE_GLM_RT_MAX_MODEL_LEN + 1];
    char voice[FRIDGE_GLM_RT_MAX_VOICE_LEN + 1];
    char last_error[FRIDGE_GLM_RT_MAX_ERROR_LEN + 1];
    uint32_t tools_exported;
    uint32_t audio_up_bytes;
    uint32_t audio_down_bytes;
    uint32_t tool_calls_ok;
    uint32_t tool_calls_fail;
    uint32_t last_event_age_ms;
} fridge_glm_rt_status_t;

// 事件回调载荷：字符串字段均已截断，不含 API Key。
typedef struct {
    fridge_glm_rt_event_type_t type;
    char name[32];
    char state[24];
    char role[16];
    char text[FRIDGE_GLM_RT_MAX_TEXT_LEN + 1];
    char tool_name[48];
    char call_id[48];
    char arguments_json[1024];
    char summary[192];
    char code[48];
    char status[32];
    bool is_final;
    uint32_t latency_ms;
} fridge_glm_rt_event_t;

typedef void (*fridge_glm_rt_event_cb_t)(const fridge_glm_rt_event_t *event, void *user_ctx);

esp_err_t fridge_glm_realtime_init(void);
esp_err_t fridge_glm_realtime_get_config(fridge_glm_rt_config_view_t *out);
esp_err_t fridge_glm_realtime_set_config(const fridge_glm_rt_config_update_t *config);
esp_err_t fridge_glm_realtime_clear_key(void);
esp_err_t fridge_glm_realtime_get_status(fridge_glm_rt_status_t *out);

// 异步：投递到 glm_rt 任务；成功仅表示已接受，结果靠事件。
esp_err_t fridge_glm_realtime_start(void);
esp_err_t fridge_glm_realtime_stop(void);
esp_err_t fridge_glm_realtime_cancel(void);
esp_err_t fridge_glm_realtime_send_text(const char *text);

esp_err_t fridge_glm_realtime_set_event_callback(fridge_glm_rt_event_cb_t cb, void *user_ctx);

const char *fridge_glm_rt_state_name(fridge_glm_rt_state_t state);
bool fridge_glm_realtime_is_active(void);

#ifdef __cplusplus
}
#endif
