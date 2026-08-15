// 冰箱小精灵厨房计时工具公共接口。
// 本组件只做软件计时、闹钟持久化和到点提醒，不新增 GPIO 或外设引脚；
// P4 到点提示复用板载 ES8311，禁止依赖旧 speaker 组件。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN 32
#define FRIDGE_KITCHEN_TOOL_MAX_SOURCE_LEN 32
#define FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN 160
#define FRIDGE_KITCHEN_TOOL_MAX_ALARMS 5
#define FRIDGE_KITCHEN_TIMER_MAX 5
#define FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS 20

typedef enum {
    FRIDGE_KITCHEN_TIMER_IDLE = 0,
    FRIDGE_KITCHEN_TIMER_RUNNING,
    FRIDGE_KITCHEN_TIMER_PAUSED,
    FRIDGE_KITCHEN_TIMER_RINGING,
    FRIDGE_KITCHEN_TIMER_DONE,
} fridge_kitchen_timer_state_t;

typedef enum {
    FRIDGE_KITCHEN_STOPWATCH_IDLE = 0,
    FRIDGE_KITCHEN_STOPWATCH_RUNNING,
    FRIDGE_KITCHEN_STOPWATCH_PAUSED,
} fridge_kitchen_stopwatch_state_t;

// 到时声音：与闹钟共用语义，铃声走本地提示，语音播报走 TTS。
typedef enum {
    FRIDGE_KITCHEN_TIMER_SOUND_CHIME = 0,
    FRIDGE_KITCHEN_TIMER_SOUND_VOICE,
} fridge_kitchen_timer_sound_t;

// 单条倒计时快照：remaining_seconds 会按当前单调时钟实时折算。
// id 从 1 起；0 表示空槽。source 用于 UI「来源」文案，不参与业务鉴权。
typedef struct {
    uint8_t id;
    fridge_kitchen_timer_state_t state;
    fridge_kitchen_timer_sound_t sound;
    uint32_t duration_seconds;
    uint32_t remaining_seconds;
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1];
    char source[FRIDGE_KITCHEN_TOOL_MAX_SOURCE_LEN + 1];
    int64_t started_at_ms;
    int64_t target_at_ms;
} fridge_kitchen_timer_snapshot_t;

// 秒表状态快照：elapsed_ms 按单调时钟实时折算，laps_ms[0] 保存最近一次计次。
// 计时状态不写 Flash；数组上限用于约束 LVGL/USB 快照的内存占用，复位时清空。
typedef struct {
    fridge_kitchen_stopwatch_state_t state;
    uint64_t elapsed_ms;
    uint32_t elapsed_seconds;
    int64_t started_at_ms;
    size_t lap_count;
    uint64_t laps_ms[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
} fridge_kitchen_stopwatch_snapshot_t;

// 闹钟类型：固定闹钟和周期提醒在管理页分组展示，二者均可配置重复规则。
typedef enum {
    FRIDGE_KITCHEN_ALARM_KIND_FIXED = 0,
    FRIDGE_KITCHEN_ALARM_KIND_CYCLE,
} fridge_kitchen_alarm_kind_t;

// 周期规则：自定义时使用 weekday_mask 指定周日到周六的有效位。
typedef enum {
    FRIDGE_KITCHEN_ALARM_REPEAT_DAILY = 0,
    FRIDGE_KITCHEN_ALARM_REPEAT_WEEKDAYS,
    FRIDGE_KITCHEN_ALARM_REPEAT_SUNDAY,
    FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM,
} fridge_kitchen_alarm_repeat_t;

// 到时声音：铃声走板载 ES8311 本地提示音，语音播报走既有 TTS 链路。
typedef enum {
    FRIDGE_KITCHEN_ALARM_SOUND_CHIME = 0,
    FRIDGE_KITCHEN_ALARM_SOUND_VOICE,
} fridge_kitchen_alarm_sound_t;

typedef enum {
    FRIDGE_KITCHEN_ALARM_SOURCE_MANUAL = 0,
    FRIDGE_KITCHEN_ALARM_SOURCE_AGENT,
} fridge_kitchen_alarm_source_t;

// weekday_mask 位定义与 struct tm.tm_wday 一致：bit0=周日，bit6=周六。
#define FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL 0x7FU
#define FRIDGE_KITCHEN_ALARM_WEEKDAY_WORKDAYS 0x3EU
#define FRIDGE_KITCHEN_ALARM_WEEKDAY_SUNDAY 0x01U

// 闹钟配置是创建与编辑页的唯一输入，避免 UI 直接修改运行期快照。
typedef struct {
    fridge_kitchen_alarm_kind_t kind;
    fridge_kitchen_alarm_repeat_t repeat;
    uint8_t weekday_mask;
    fridge_kitchen_alarm_sound_t sound;
    fridge_kitchen_alarm_source_t source;
    uint8_t hour;
    uint8_t minute;
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1];
    char speech_text[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} fridge_kitchen_alarm_config_t;

// 闹钟状态：hour/minute 使用本地时区时间；周期、声音和来源都持久化在本地缓存中。
typedef struct {
    uint8_t id;
    bool enabled;
    bool ringing;
    fridge_kitchen_alarm_kind_t kind;
    fridge_kitchen_alarm_repeat_t repeat;
    uint8_t weekday_mask;
    fridge_kitchen_alarm_sound_t sound;
    fridge_kitchen_alarm_source_t source;
    uint8_t hour;
    uint8_t minute;
    int64_t last_trigger_day;
    char label[FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN + 1];
    char speech_text[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} fridge_kitchen_alarm_t;

typedef struct {
    bool initialized;
    bool time_ready;
    // 兼容字段：主定时器（优先 RINGING，其次 remaining 最小的 RUNNING/PAUSED）。
    fridge_kitchen_timer_snapshot_t timer;
    // 全部倒计时槽位（含 DONE）；timer_count 为有效条目数（id!=0）。
    size_t timer_count;
    fridge_kitchen_timer_snapshot_t timers[FRIDGE_KITCHEN_TIMER_MAX];
    fridge_kitchen_stopwatch_snapshot_t stopwatch;
    size_t alarm_count;
    fridge_kitchen_alarm_t alarms[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    char last_alert[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
    char last_error[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} fridge_kitchen_tools_snapshot_t;

typedef struct {
    bool executed;
    bool needs_confirmation;
    char tool[16];
    char action[16];
    char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} fridge_kitchen_tools_ai_result_t;

esp_err_t fridge_kitchen_tools_init(void);
esp_err_t fridge_kitchen_tools_get_snapshot(fridge_kitchen_tools_snapshot_t *out);

// 兼容旧接口：操作主定时器；start 会占用一个新槽位。
esp_err_t fridge_kitchen_tools_timer_start(uint32_t seconds, const char *label);
esp_err_t fridge_kitchen_tools_timer_pause(void);
esp_err_t fridge_kitchen_tools_timer_resume(void);
esp_err_t fridge_kitchen_tools_timer_cancel(void);
esp_err_t fridge_kitchen_tools_timer_dismiss(void);

// 多倒计时接口：按 id 控制；source 为空时记为「手动」。
esp_err_t fridge_kitchen_tools_timer_start_ex(uint32_t seconds,
                                              const char *label,
                                              const char *source,
                                              fridge_kitchen_timer_sound_t sound,
                                              uint8_t *out_id);
esp_err_t fridge_kitchen_tools_timer_pause_id(uint8_t id);
esp_err_t fridge_kitchen_tools_timer_resume_id(uint8_t id);
esp_err_t fridge_kitchen_tools_timer_cancel_id(uint8_t id);
esp_err_t fridge_kitchen_tools_timer_add_seconds(uint8_t id, int32_t delta_seconds);
esp_err_t fridge_kitchen_tools_timer_dismiss_id(uint8_t id);
esp_err_t fridge_kitchen_tools_timer_clear_done(void);

esp_err_t fridge_kitchen_tools_stopwatch_start(void);
esp_err_t fridge_kitchen_tools_stopwatch_pause(void);
esp_err_t fridge_kitchen_tools_stopwatch_lap(void);
esp_err_t fridge_kitchen_tools_stopwatch_reset(void);

esp_err_t fridge_kitchen_tools_alarm_set(uint8_t hour, uint8_t minute, const char *label, uint8_t *out_id);
esp_err_t fridge_kitchen_tools_alarm_create(const fridge_kitchen_alarm_config_t *config, uint8_t *out_id);
esp_err_t fridge_kitchen_tools_alarm_update(uint8_t id, const fridge_kitchen_alarm_config_t *config);
esp_err_t fridge_kitchen_tools_alarm_set_enabled(uint8_t id, bool enabled);
esp_err_t fridge_kitchen_tools_alarm_preview(const fridge_kitchen_alarm_config_t *config);
esp_err_t fridge_kitchen_tools_alarm_cancel(uint8_t id);
esp_err_t fridge_kitchen_tools_alarm_dismiss(uint8_t id);

// 执行 AI 返回的白名单 JSON 指令；非法或槽位不足时只返回需要确认，不改变本地状态。
esp_err_t fridge_kitchen_tools_execute_ai_json(const char *json, fridge_kitchen_tools_ai_result_t *out);

const char *fridge_kitchen_timer_state_text(fridge_kitchen_timer_state_t state);
const char *fridge_kitchen_stopwatch_state_text(fridge_kitchen_stopwatch_state_t state);

#ifdef __cplusplus
}
#endif
