// 冰箱小精灵厨房 AI 生成公共接口。
// 本组件只做网络 AI 请求、结构化结果校验和 LittleFS 缓存写入；不新增 GPIO、时钟或外设控制。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_KITCHEN_AI_MAX_ERROR_LEN 160
#define FRIDGE_KITCHEN_AI_MAX_REPLY_PREVIEW_LEN 1024

typedef enum {
    FRIDGE_KITCHEN_AI_TRIGGER_AUTO = 0,
    FRIDGE_KITCHEN_AI_TRIGGER_MANUAL,
} fridge_kitchen_ai_trigger_t;

typedef enum {
    FRIDGE_KITCHEN_AI_PAGE_ALL = 0,
    FRIDGE_KITCHEN_AI_PAGE_RECIPE,
    FRIDGE_KITCHEN_AI_PAGE_NUTRITION,
    FRIDGE_KITCHEN_AI_PAGE_SHOPPING,
} fridge_kitchen_ai_page_t;

typedef enum {
    FRIDGE_KITCHEN_AI_STATE_IDLE = 0,
    FRIDGE_KITCHEN_AI_STATE_RUNNING,
    FRIDGE_KITCHEN_AI_STATE_OK,
    FRIDGE_KITCHEN_AI_STATE_ERROR,
} fridge_kitchen_ai_state_t;

// 厨房 AI 生成状态快照：供 LVGL 页面展示按钮状态、最近结果和错误原因。
typedef struct {
    bool running;
    bool last_ok;
    fridge_kitchen_ai_state_t state;
    fridge_kitchen_ai_trigger_t last_trigger;
    fridge_kitchen_ai_page_t current_page;
    fridge_kitchen_ai_page_t last_page;
    int64_t last_generated_at;
    int64_t last_auto_day;
    int http_status;
    uint32_t latency_ms;
    size_t last_reply_len;
    char last_error[FRIDGE_KITCHEN_AI_MAX_ERROR_LEN + 1];
    char last_reply_preview[FRIDGE_KITCHEN_AI_MAX_REPLY_PREVIEW_LEN + 1];
} fridge_kitchen_ai_status_t;

esp_err_t fridge_kitchen_ai_init(void);
esp_err_t fridge_kitchen_ai_generate_async(fridge_kitchen_ai_trigger_t trigger);
esp_err_t fridge_kitchen_ai_generate_page_async(fridge_kitchen_ai_trigger_t trigger, fridge_kitchen_ai_page_t page);
esp_err_t fridge_kitchen_ai_get_status(fridge_kitchen_ai_status_t *out);
bool fridge_kitchen_ai_is_running(void);

const char *fridge_kitchen_ai_state_text(fridge_kitchen_ai_state_t state);
const char *fridge_kitchen_ai_page_text(fridge_kitchen_ai_page_t page);

#ifdef __cplusplus
}
#endif
