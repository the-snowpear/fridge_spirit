#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_WAKE_WORD_TEXT "小冰小冰"
#define FRIDGE_WAKE_WORD_MODEL "wn9_xiaobinxiaobin_tts"

typedef enum {
    FRIDGE_WAKE_WORD_STATE_IDLE = 0,
    FRIDGE_WAKE_WORD_STATE_LISTENING,
    FRIDGE_WAKE_WORD_STATE_ERROR,
} fridge_wake_word_state_t;

// 自动策略当前判定原因：用于在 USB/Web 状态里解释“为什么现在没在监听 / 内存够不够”。
typedef enum {
    FRIDGE_WAKE_POLICY_DISABLED_BY_USER = 0,  // 用户开关关闭
    FRIDGE_WAKE_POLICY_DISPLAY_NOT_READY,     // 屏幕尚未初始化完成
    FRIDGE_WAKE_POLICY_SCREEN_OFF,            // 黑屏休眠/待机，不在亮屏态
    FRIDGE_WAKE_POLICY_AUDIO_BUSY,            // 麦克风正在录音或唤醒流被占用
    FRIDGE_WAKE_POLICY_SPEAKER_BUSY,          // 扬声器正在合成/播放 TTS
    FRIDGE_WAKE_POLICY_LOW_INTERNAL_DMA,      // 内部 DMA 连续块低于门槛
    FRIDGE_WAKE_POLICY_STARTING,              // 满足条件，正在启动监听
    FRIDGE_WAKE_POLICY_LISTENING,             // 已在监听
    FRIDGE_WAKE_POLICY_STOPPING,              // 正在停止监听
} fridge_wake_policy_reason_t;

typedef void (*fridge_wake_word_event_cb_t)(void *user_ctx);

typedef struct {
    bool enabled;
    fridge_wake_word_state_t state;
    uint32_t trigger_count;
    uint32_t last_trigger_ms;
    int32_t vad_state;
    int32_t rms;
    int32_t peak_abs;
    uint32_t timeout_count;
    bool user_enabled;                          // 用户开关（NVS 持久化）
    bool auto_allowed;                          // 当前策略是否允许自动监听
    fridge_wake_policy_reason_t policy_reason;  // 策略判定原因
    size_t largest_internal_dma_block;          // 当前内部 DMA 最大连续块（字节）
    size_t min_dma_block_threshold;             // 启动所需的最小连续块门槛（字节）
    char wake_word[32];
    char model[48];
    char error[128];
} fridge_wake_word_status_t;

// 初始化本地唤醒词服务。
// 硬件约束：服务复用 INMP441 I2S RX，启动监听前必须确认麦克风为 3.3V 供电并与主控共地。
esp_err_t fridge_wake_word_init(void);

// 注册唤醒事件回调。回调在唤醒任务上下文中执行，不能做长时间阻塞操作。
esp_err_t fridge_wake_word_set_event_callback(fridge_wake_word_event_cb_t cb, void *user_ctx);

// 启动/停止本地唤醒监听；监听期间会占用麦克风 I2S RX，和手动录音互斥。
esp_err_t fridge_wake_word_start(void);
esp_err_t fridge_wake_word_stop(void);

// 用户开关（持久化到 NVS namespace fridge_wake / key user_en，默认 false）。
// 关闭只表示“用户不希望自动监听”，策略层会据此停止；不影响底层模型是否已初始化。
bool fridge_wake_word_get_user_enabled(void);
esp_err_t fridge_wake_word_set_user_enabled(bool enabled);

// 自动策略评估入口：由 UI 主循环周期调用（建议 1s 一次）。
// display_ready/screen_awake 由调用方提供；函数内部综合用户开关、音频/扬声器忙闲和内部 DMA 余量，
// 决定启动或停止 WakeNet。重要：该函数不阻塞调用者——停止只投递请求，不在此等待任务退出。
void fridge_wake_word_policy_update(bool display_ready, bool screen_awake);

// 注册“唤醒后自动对话”回调。唤醒命中时由 wake 任务调用，回调内部禁止做录音/HTTPS 等阻塞工作，
// 只应投递信号给独立任务处理。与 set_event_callback 相互独立，可同时使用。
typedef void (*fridge_wake_word_dialog_cb_t)(void *user_ctx);
esp_err_t fridge_wake_word_set_dialog_callback(fridge_wake_word_dialog_cb_t cb, void *user_ctx);

// 唤醒后自动对话流程结束时调用：清除“对话进行中”内部标记，并由策略层在下个 tick 决定是否恢复监听。
void fridge_wake_word_notify_dialog_finished(void);

// 清零触发次数和最近触发时间，保留当前启停状态。
esp_err_t fridge_wake_word_reset_stats(void);

esp_err_t fridge_wake_word_get_status(fridge_wake_word_status_t *out);
const char *fridge_wake_word_state_text(fridge_wake_word_state_t state);
// 把策略原因枚举转成稳定英文短串（disabled_by_user / screen_off / audio_busy / ...）。
const char *fridge_wake_policy_reason_text(fridge_wake_policy_reason_t reason);

#ifdef __cplusplus
}
#endif
