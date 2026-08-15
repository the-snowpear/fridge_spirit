// 冰箱小精灵 P4 ver2 原生 UI 编排。
// 保留已验证的 MIPI 双缓冲、GT911、摄像头显存互斥和单 LVGL 任务；
// HOME-01/CTX-01 的结构与硬件权限在固件中，TF 仅提供受校验的字体、A8 图标和示例数据。

#include "fridge_ui_p4.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "driver/ppa.h"
#include "fridge_ai_client.h"
#include "fridge_audio.h"
#include "fridge_camera.h"
#include "fridge_display_mipi.h"
#include "fridge_font_manager.h"
#include "fridge_kitchen_tools.h"
#include "fridge_ime.h"
#include "fridge_network.h"
#include "fridge_tfcard.h"
#include "fridge_tool_broker.h"
#include "fridge_sht4x.h"
#include "fridge_sensors.h"
#include "fridge_touch_gt911.h"
#include "fridge_ui_assets.h"
#include "fridge_ui_model.h"
#include "fridge_ui_scroll_ppa.h"
#include "fridge_ui_shell.h"
#include "fridge_ui_theme.h"
#include "fridge_voice_asr.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

// FreeType 渲染 Noto CJK 的 CFF 字形时调用层级较深，16KB 实测会越过栈边界。
// 32KB 在实际运行中仍会触发栈保护错误（Stack protection fault），特别是加载 FreeType CJK 字体后。
// 增加到 48KB 以覆盖 FreeType + LVGL + PPA + 业务模型的峰值栈使用。
// 历史长稳日志最低高水位约 27KB，当前完整首页启动约 42KB；保留 32KB 内部栈可
// 覆盖约 22KB 历史峰值并留 10KB 余量，同时释放 16KB 连续内部 RAM 给 SDMMC/ISP/JPEG。
#define UI_TASK_STACK 32768
#define UI_TASK_PRIORITY 5
#define UI_TICK_MS 5
#define UI_REFR_PERIOD_MS 10
#define UI_DEVICE_REFRESH_US 1000000LL
#define UI_KITCHEN_REFRESH_US 1000000LL
#define UI_ACTIVE_FRAME_GAP_US 250000LL
#define UI_PERF_SAMPLE_INTERVALS 10U
#define UI_SCREEN_PIXELS (1024U * 600U)
#define UI_RGB565_FRAME_BYTES (UI_SCREEN_PIXELS * sizeof(uint16_t))
#define UI_PORTRAIT_WIDTH 600U
#define UI_PORTRAIT_HEIGHT 1024U
#define UI_PORTRAIT_BUFFER_ALIGN 64U
#define UI_PAGE_REQUEST_QUEUE_LENGTH 4U
#define UI_CAMERA_CAPTURE_TASK_STACK 12288U
#define UI_CAMERA_CAPTURE_TASK_PRIORITY 4U
#define UI_CAMERA_AI_TASK_STACK 24576U
#define UI_CAMERA_AI_TASK_PRIORITY 4U
#define UI_VOICE_TASK_STACK 8192U
#define UI_VOICE_TASK_PRIORITY 4U
#define UI_VOICE_EVENT_QUEUE_LENGTH 4U
#define UI_SETTINGS_JOB_QUEUE_LENGTH 6U
#define UI_SETTINGS_JOB_TASK_STACK 6144U
#define UI_SETTINGS_JOB_TASK_PRIORITY 3U
#define UI_SETTINGS_EVENT_QUEUE_LENGTH 6U
#define UI_SETTINGS_WIFI_VERIFY_TIMEOUT_MS 30000U
#define UI_SETTINGS_WIFI_VERIFY_POLL_MS 250U
#define UI_SETTINGS_BRIGHTNESS_NVS_NAMESPACE "fridge_display"
#define UI_SETTINGS_BRIGHTNESS_NVS_KEY "brightness_pct"
// WakeNet/AFE 最后启动：字体、图标、目标页首帧和双缓冲完成后再静置 10 秒，
// 让 FreeType/SDMMC 的惰性字形读取先完成，避免模型抢走最后的内部 DMA 连续块。
#define UI_WAKE_AFTER_STABLE_US 10000000LL
// 首帧 ready 后，TF 字体/图标还会触发一次全量 Shell 同步。该窗口内创建 INV-05
// 会与 DIRECT 双缓冲/PPA 切换重叠，并在 Core 1 触发栈保护；拍照请求先暂存，
// 等首轮资源同步稳定后再执行。实机验证 16 秒可覆盖该窗口。
#define UI_CAMERA_PAGE_BOOT_SETTLE_US 16000000LL

typedef enum {
    FRIDGE_UI_REQUEST_PAGE = 0,
    FRIDGE_UI_REQUEST_ORIENTATION,
} fridge_ui_request_kind_t;

typedef struct {
    fridge_ui_request_kind_t kind;
    union {
        fridge_ui_p4_page_t page;
        fridge_ui_p4_orientation_t orientation;
    };
} fridge_ui_request_t;

static const char *TAG = "fridge_ui_p4";

// 页面白名单同时供异步方向请求和 LVGL 实际切换路径使用，避免两个入口分叉。
static bool page_supports_portrait(fridge_ui_p4_page_t page);

// 页面切换期间只观察 SDMMC/JPEG 共用的内部 DMA 堆，不触碰 LVGL 对象或触发额外分配。
static void log_internal_dma_heap(const char *phase, fridge_ui_p4_page_t page)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "internal_dma[%s:%s]: free=%u largest=%u minimum=%u",
             phase, fridge_ui_p4_page_key(page),
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}
static TaskHandle_t s_task;
static lv_display_t *s_display;
static esp_timer_handle_t s_tick_timer;
static atomic_bool s_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_resource_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_framebuffers_synced = ATOMIC_VAR_INIT(false);
static atomic_bool s_page_frame_ready = ATOMIC_VAR_INIT(false);
static atomic_uint s_current_page = ATOMIC_VAR_INIT(FRIDGE_UI_P4_PAGE_HOME);
static atomic_bool s_binding_dirty = ATOMIC_VAR_INIT(false);
static portMUX_TYPE s_binding_lock = portMUX_INITIALIZER_UNLOCKED;
static fridge_ui_p4_binding_model_t s_binding_model;
static atomic_uint s_current_orientation =
    ATOMIC_VAR_INIT(FRIDGE_UI_P4_ORIENTATION_LANDSCAPE);
static QueueHandle_t s_page_request_queue;
// 竖屏仍使用面板原生 1024x600 扫描；LVGL 在两块 600x1024 PSRAM 逻辑缓冲中绘制，
// flush 时由 PPA 顺时针旋转到当前安全的 MIPI DPI 帧缓冲。
static uint8_t *s_portrait_fb0;
static uint8_t *s_portrait_fb1;
static ppa_client_handle_t s_orientation_ppa;
// Agent Runtime 通过轻量回调桥接入；此处不包含 Runtime 头文件，避免 UI、Tool Broker
// 与 Runtime 形成组件循环依赖。注册可能发生在非 UI 任务，复制动作表时用短临界区保护。
static portMUX_TYPE s_agent_actions_lock = portMUX_INITIALIZER_UNLOCKED;
static fridge_ui_p4_agent_actions_t s_agent_actions;
static bool s_agent_actions_registered;
// 摄像头全屏预览期间置 true：UI 循环暂停 LVGL renderer，触摸改由摄像头任务消费。
static volatile bool s_camera_preview_mode;
static bool s_camera_preview_start_pending;
static uint32_t s_camera_preview_start_after_generation;
static bool s_camera_resume_preview_after_capture;
// ready 事件早于 TF 字体/双缓冲同步；启动阶段收到的页面请求保留最后一次，
// 待 HOME 完成资源重绘后再创建目标页，避免复杂库存子页参与字体重绑而阻塞 UI 看门狗。
static bool s_page_request_deferred;
static fridge_ui_p4_page_t s_deferred_page_request = FRIDGE_UI_P4_PAGE_HOME;
static int64_t s_ui_ready_since_us;
static atomic_bool s_camera_capture_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_camera_capture_done = ATOMIC_VAR_INIT(false);
static esp_err_t s_camera_capture_result;
// 图片上传与 HTTPS/TLS 识别在 PSRAM 栈后台任务执行。结果对象也放 PSRAM，
// UI 任务只在 done 置位后读取并释放，避免 LVGL 与 worker 并发访问同一结果。
typedef struct {
    esp_err_t err;
    // 配置必须由内部 SRAM 的 UI 任务预加载；PSRAM 栈 worker 禁止再访问 NVS/Flash。
    fridge_ai_config_update_t config;
    fridge_ai_image_result_t ai;
} ui_camera_ai_result_t;
static atomic_bool s_camera_ai_running = ATOMIC_VAR_INIT(false);
static atomic_bool s_camera_ai_done = ATOMIC_VAR_INIT(false);
static ui_camera_ai_result_t *s_camera_ai_result;
// CTX-04 的图片识别状态由唯一 UI 任务写入并随设备快照展示。
// 失败原因只保存简短摘要，不保存模型原始回复，避免在屏幕和日志中泄露图片内容。
typedef enum {
    UI_CAMERA_AI_PHASE_IDLE = 0,
    UI_CAMERA_AI_PHASE_CAPTURING,
    UI_CAMERA_AI_PHASE_PHOTO_READY,
    UI_CAMERA_AI_PHASE_RECOGNIZING,
    UI_CAMERA_AI_PHASE_SUCCEEDED,
    UI_CAMERA_AI_PHASE_FAILED,
} ui_camera_ai_phase_t;
static ui_camera_ai_phase_t s_camera_ai_phase;
static char s_camera_ai_detail[112];
// 抓拍后的 RGB565 回看图由 UI 持有；Shell 只借用指针，离页/重拍时必须先解除
// LVGL 图片源再释放 PSRAM，避免绘制缓存访问悬空地址。
static fridge_camera_preview_frame_t s_camera_review_frame;
// 手动语音采用“短录音 -> 云端 ASR -> Agent Runtime”的单次链路，UI 从不直接控制
// Realtime。所有可能等待 WakeNet、I2S 或 HTTPS 的步骤都在 PSRAM 后台任务完成。
typedef enum {
    UI_VOICE_PHASE_IDLE = 0,
    UI_VOICE_PHASE_STARTING,
    UI_VOICE_PHASE_RECORDING,
    UI_VOICE_PHASE_RECORDING_READY,
    UI_VOICE_PHASE_TRANSCRIBING,
    UI_VOICE_PHASE_CANCELLING,
} ui_voice_phase_t;

typedef enum {
    UI_VOICE_EVENT_RECORDING = 0,
    UI_VOICE_EVENT_CANCELLED,
    UI_VOICE_EVENT_AGENT_SUBMITTED,
    UI_VOICE_EVENT_FAILED,
} ui_voice_event_kind_t;

typedef struct {
    ui_voice_event_kind_t kind;
    esp_err_t err;
    char transcript[FRIDGE_UI_P4_AGENT_TEXT_MAX];
    char detail[FRIDGE_UI_P4_AGENT_DETAIL_MAX];
} ui_voice_event_t;

static QueueHandle_t s_voice_event_queue;
static atomic_bool s_voice_worker_running = ATOMIC_VAR_INIT(false);
static ui_voice_phase_t s_voice_phase = UI_VOICE_PHASE_IDLE;
static bool s_voice_waiting_agent_tts;

// 设置页耗时动作统一经后台任务执行。请求结构只在队列内短暂保存 Wi-Fi 密码，任务完成
// 后立即清零；事件和状态快照绝不携带密码，避免经 LVGL、日志或串口意外泄露。
typedef enum {
    UI_SETTINGS_JOB_SAVE_BRIGHTNESS = 0,
    UI_SETTINGS_JOB_SET_WAKE_WORD,
    UI_SETTINGS_JOB_SET_AGENT_PERMISSION_MODE,
    UI_SETTINGS_JOB_WIFI_SCAN,
    UI_SETTINGS_JOB_WIFI_CONNECT,
    UI_SETTINGS_JOB_WIFI_CONNECT_SAVED,
} ui_settings_job_kind_t;

typedef struct {
    ui_settings_job_kind_t kind;
    uint8_t brightness_percent;
    bool wake_word_enabled;
    uint8_t agent_permission_mode;
    bool save_after_verified;
    fridge_wifi_config_t wifi;
} ui_settings_job_t;

typedef struct {
    fridge_ui_shell_settings_action_t action;
    esp_err_t err;
    uint8_t brightness_percent;
    bool wake_word_user_enabled;
    bool wake_word_listening;
    uint8_t agent_permission_mode;
    char wake_word_detail[FRIDGE_UI_SHELL_SETTINGS_WAKE_DETAIL_LEN];
    fridge_ui_shell_settings_wifi_phase_t wifi_phase;
    char wifi_status[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN];
    char wifi_ssid[FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN + 1];
    char wifi_ip[FRIDGE_WIFI_MAX_IP_LEN];
    size_t wifi_ap_count;
    fridge_ui_shell_settings_wifi_ap_t wifi_aps[FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS];
} ui_settings_event_t;

static QueueHandle_t s_settings_job_queue;
static QueueHandle_t s_settings_event_queue;
static TaskHandle_t s_settings_task;
static fridge_ui_shell_settings_state_t s_settings_state;
static fridge_ui_assets_t s_assets;
static home_business_model_t s_home;
static more_business_model_t s_more;
static plan_business_model_t s_plan;
static shopping_business_model_t s_shopping;
static fridge_ui_device_model_t s_device;
static fridge_ui_safety_model_t s_safety;
static fridge_kitchen_tools_snapshot_t s_kitchen_tools;
// 只统计连续交互帧，忽略静止页面每秒一次的状态刷新；日志用于真机量化滚动帧率。
static int64_t s_perf_last_frame_us;
static uint64_t s_perf_interval_total_us;
static uint64_t s_perf_present_total_us;
static uint64_t s_perf_render_total_us;
static uint64_t s_perf_dirty_total_pixels;
static uint64_t s_perf_flush_total;
static uint32_t s_perf_interval_count;
static uint32_t s_perf_present_count;
static uint32_t s_perf_present_max_us;
static uint32_t s_perf_render_max_us;
static uint32_t s_perf_dirty_max_pixels;
static int64_t s_render_cycle_start_us;
static lv_area_t s_flush_union;
static uint32_t s_flush_count;
static bool s_flush_union_valid;
// 只统计 LVGL 成功提交的帧，用作异步资源整页重绘的显存同步屏障。
// lv_obj_invalidate() 只会唤醒刷新定时器，不能保证紧随其后的 handler 已经真正出帧。
static uint32_t s_lvgl_present_generation;
static bool s_page_frame_pending;
static uint32_t s_page_change_present_generation;
static bool s_orientation_framebuffer_sync_pending;
static uint32_t s_orientation_change_present_generation;

static bool agent_actions_snapshot(fridge_ui_p4_agent_actions_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_agent_actions_lock);
    bool registered = s_agent_actions_registered;
    if (registered) *out = s_agent_actions;
    portEXIT_CRITICAL(&s_agent_actions_lock);
    return registered;
}

// Shell 在唯一 LVGL 任务内调用这些回调；Runtime 实现必须只进行非阻塞入队，不能在
// 本调用栈中等待网络或工具执行。未注册时保持页面可浏览，并给出明确的可恢复提示。
static esp_err_t agent_chat_submit(void *user_ctx, const char *text)
{
    (void)user_ctx;
    fridge_ui_p4_agent_actions_t actions = {0};
    if (!text || !text[0] || !agent_actions_snapshot(&actions) || !actions.submit_text) {
        return ESP_ERR_INVALID_STATE;
    }
    return actions.submit_text(text, actions.user_ctx);
}

static esp_err_t agent_chat_cancel(void *user_ctx)
{
    (void)user_ctx;
    fridge_ui_p4_agent_actions_t actions = {0};
    if (!agent_actions_snapshot(&actions) || !actions.cancel_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return actions.cancel_active(actions.user_ctx);
}

// 清空动作只操作 Runtime 的本地会话快照；未注册或 Agent 正在执行时由 Runtime 返回状态码。
static esp_err_t agent_chat_clear_session(void *user_ctx)
{
    (void)user_ctx;
    fridge_ui_p4_agent_actions_t actions = {0};
    if (!agent_actions_snapshot(&actions) || !actions.clear_session) {
        return ESP_ERR_INVALID_STATE;
    }
    return actions.clear_session(actions.user_ctx);
}

static esp_err_t agent_chat_resolve_confirmation(void *user_ctx, const char *run_id,
                                                  const char *call_id, bool confirmed)
{
    (void)user_ctx;
    fridge_ui_p4_agent_actions_t actions = {0};
    if (!run_id || !call_id || !agent_actions_snapshot(&actions) ||
        !actions.resolve_confirmation) {
        return ESP_ERR_INVALID_STATE;
    }
    return actions.resolve_confirmation(run_id, call_id, confirmed, actions.user_ctx);
}

static void finish_voice_manual_session(void);

// UI 任务定期拉取有限数量事件，防止大量历史恢复事件占满一帧。Runtime 永远不能从
// 自己的任务触碰 LVGL；所有消息、确认卡和播报状态都在这里转交 Shell。
static void process_agent_events(void)
{
    fridge_ui_p4_agent_actions_t actions = {0};
    if (!agent_actions_snapshot(&actions) || !actions.poll_event) return;

    for (size_t i = 0; i < 12; i++) {
        fridge_ui_p4_agent_event_t event = {0};
        if (!actions.poll_event(&event, actions.user_ctx)) break;
        fridge_ui_shell_apply_agent_event(&event);
        // 手动语音从录音开始一直阻止 WakeNet 恢复，直到 Agent 的板载 TTS 已结束。
        // completed 状态早于异步 TTS，因此只在 TTS 终态或 Agent 失败/取消时解除互斥。
        if (s_voice_waiting_agent_tts) {
            bool tts_finished = event.kind == FRIDGE_UI_P4_AGENT_EVENT_TTS &&
                                (strcmp(event.state, "done") == 0 ||
                                 strcmp(event.state, "error") == 0 ||
                                 strcmp(event.state, "stopped") == 0);
            bool agent_failed = event.kind == FRIDGE_UI_P4_AGENT_EVENT_STATUS && !event.working &&
                                (strcmp(event.state, "FAILED") == 0 ||
                                 strcmp(event.state, "CANCELLED") == 0 ||
                                 strcmp(event.state, "failed") == 0 ||
                                 strcmp(event.state, "cancelled") == 0);
            if (tts_finished || agent_failed) {
                finish_voice_manual_session();
            }
        }
    }
}

// INV-05 的高清取景只更新左侧画面区域；其余 Shell 像素在摄像头接管双缓冲前已由 LVGL 绘制。
static const fridge_camera_preview_layout_t s_camera_page_preview_layout = {
    .view_x = 0,
    .view_y = 87,
    .view_width = 704,
    .view_height = 396,
    .capture_x = 284,
    .capture_y = 540,
    .capture_width = 136,
    .capture_height = 48,
    .cancel_x = 580,
    .cancel_y = 540,
    .cancel_width = 100,
    .cancel_height = 48,
    .draw_guides = true,
};

static const char *const s_page_keys[FRIDGE_UI_P4_PAGE_COUNT] = {
    [FRIDGE_UI_P4_PAGE_HOME] = "home",
    [FRIDGE_UI_P4_PAGE_MORE] = "more",
    [FRIDGE_UI_P4_PAGE_TIMER] = "timer",
    [FRIDGE_UI_P4_PAGE_TIMER_EDITOR] = "timer_edit",
    [FRIDGE_UI_P4_PAGE_ALARM] = "alarm",
    [FRIDGE_UI_P4_PAGE_ALARM_EDITOR] = "alarm_editor",
    [FRIDGE_UI_P4_PAGE_STOPWATCH] = "stopwatch",
    [FRIDGE_UI_P4_PAGE_CAMERA] = "camera",
    [FRIDGE_UI_P4_PAGE_PLAN] = "plan",
    [FRIDGE_UI_P4_PAGE_SHOPPING] = "shopping",
    [FRIDGE_UI_P4_PAGE_CHAT] = "chat",
    [FRIDGE_UI_P4_PAGE_CHAT_HISTORY] = "chat_history",
    [FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL] = "chat_tool_detail",
    [FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS] = "chat_pending_actions",
    [FRIDGE_UI_P4_PAGE_HOME_DETAIL] = "home_detail",
    [FRIDGE_UI_P4_PAGE_BINDING] = "binding",
    [FRIDGE_UI_P4_PAGE_SETTINGS] = "settings",
    [FRIDGE_UI_P4_PAGE_MAINTENANCE] = "maintenance",
    [FRIDGE_UI_P4_PAGE_INVENTORY] = "inventory",
    [FRIDGE_UI_P4_PAGE_INVENTORY_DETAIL] = "inventory_detail",
    [FRIDGE_UI_P4_PAGE_INVENTORY_EDIT] = "inventory_edit",
    [FRIDGE_UI_P4_PAGE_INVENTORY_RECOGNIZE] = "inventory_recognize",
    [FRIDGE_UI_P4_PAGE_INVENTORY_SEARCH] = "inventory_search",
    [FRIDGE_UI_P4_PAGE_INVENTORY_LAYOUT] = "inventory_layout",
    [FRIDGE_UI_P4_PAGE_SAFETY] = "safety",
};

static const char *const s_orientation_keys[FRIDGE_UI_P4_ORIENTATION_COUNT] = {
    [FRIDGE_UI_P4_ORIENTATION_LANDSCAPE] = "landscape",
    [FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY] = "portrait_privacy",
};

esp_err_t fridge_ui_p4_page_from_key(const char *key, fridge_ui_p4_page_t *out)
{
    if (!key || !out) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < FRIDGE_UI_P4_PAGE_COUNT; i++) {
        if (strcmp(key, s_page_keys[i]) == 0) {
            *out = (fridge_ui_p4_page_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

const char *fridge_ui_p4_page_key(fridge_ui_p4_page_t page)
{
    if (page < FRIDGE_UI_P4_PAGE_HOME || page >= FRIDGE_UI_P4_PAGE_COUNT) {
        return NULL;
    }
    return s_page_keys[page];
}

esp_err_t fridge_ui_p4_update_binding(const fridge_ui_p4_binding_model_t *model)
{
    if (!model) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_binding_lock);
    s_binding_model = *model;
    s_binding_model.qr_payload[sizeof(s_binding_model.qr_payload) - 1U] = '\0';
    s_binding_model.device_sn[sizeof(s_binding_model.device_sn) - 1U] = '\0';
    s_binding_model.status[sizeof(s_binding_model.status) - 1U] = '\0';
    s_binding_model.detail[sizeof(s_binding_model.detail) - 1U] = '\0';
    portEXIT_CRITICAL(&s_binding_lock);
    atomic_store_explicit(&s_binding_dirty, true, memory_order_release);
    return ESP_OK;
}

esp_err_t fridge_ui_p4_orientation_from_key(const char *key,
                                             fridge_ui_p4_orientation_t *out)
{
    if (!key || !out) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < FRIDGE_UI_P4_ORIENTATION_COUNT; i++) {
        if (strcmp(key, s_orientation_keys[i]) == 0) {
            *out = (fridge_ui_p4_orientation_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

const char *fridge_ui_p4_orientation_key(fridge_ui_p4_orientation_t orientation)
{
    if (orientation < FRIDGE_UI_P4_ORIENTATION_LANDSCAPE ||
        orientation >= FRIDGE_UI_P4_ORIENTATION_COUNT) {
        return NULL;
    }
    return s_orientation_keys[orientation];
}

esp_err_t fridge_ui_p4_request_page_async(fridge_ui_p4_page_t page)
{
    if (page < FRIDGE_UI_P4_PAGE_HOME || page >= FRIDGE_UI_P4_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = s_page_request_queue;
    if (!queue) return ESP_ERR_INVALID_STATE;

    // 隐私态禁止从 USB/Agent 直接打开摄像头；其它目标页由 UI 任务先恢复横屏再切页。
    const fridge_ui_p4_orientation_t orientation =
        (fridge_ui_p4_orientation_t)atomic_load_explicit(&s_current_orientation,
                                                          memory_order_acquire);
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        page == FRIDGE_UI_P4_PAGE_CAMERA) {
        return ESP_ERR_INVALID_STATE;
    }

    fridge_ui_request_t request = {
        .kind = FRIDGE_UI_REQUEST_PAGE,
        .page = page,
    };
    // 不阻塞 USB/Tool Broker；队列满时由调用方收到超时错误并决定是否重试。
    return xQueueSend(queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t fridge_ui_p4_request_orientation_async(fridge_ui_p4_orientation_t orientation)
{
    if (orientation < FRIDGE_UI_P4_ORIENTATION_LANDSCAPE ||
        orientation >= FRIDGE_UI_P4_ORIENTATION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = s_page_request_queue;
    if (!queue) return ESP_ERR_INVALID_STATE;

    // HOME、PLAN、CHAT-01～04 与 MORE-01/02/04/05/06/07/08 有竖屏隐私布局；其它页面保持横屏，
    // 避免输出被未移植布局裁切。
    const fridge_ui_p4_page_t page =
        (fridge_ui_p4_page_t)atomic_load_explicit(&s_current_page, memory_order_acquire);
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        !page_supports_portrait(page)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    fridge_ui_request_t request = {
        .kind = FRIDGE_UI_REQUEST_ORIENTATION,
        .orientation = orientation,
    };
    return xQueueSend(queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void perf_reset_sample(void)
{
    s_perf_interval_total_us = 0;
    s_perf_present_total_us = 0;
    s_perf_render_total_us = 0;
    s_perf_dirty_total_pixels = 0;
    s_perf_flush_total = 0;
    s_perf_interval_count = 0;
    s_perf_present_count = 0;
    s_perf_present_max_us = 0;
    s_perf_render_max_us = 0;
    s_perf_dirty_max_pixels = 0;
}

static void perf_record_frame(int64_t frame_done_us, uint32_t present_us,
                              uint32_t render_us, uint32_t dirty_pixels,
                              uint32_t flush_count)
{
    if (s_perf_last_frame_us > 0) {
        uint32_t interval_us = (uint32_t)(frame_done_us - s_perf_last_frame_us);
        if (interval_us <= UI_ACTIVE_FRAME_GAP_US) {
            s_perf_interval_total_us += interval_us;
            s_perf_interval_count++;
        } else {
            perf_reset_sample();
        }
    }
    s_perf_last_frame_us = frame_done_us;
    s_perf_present_total_us += present_us;
    s_perf_render_total_us += render_us;
    s_perf_dirty_total_pixels += dirty_pixels;
    s_perf_flush_total += flush_count;
    s_perf_present_count++;
    if (present_us > s_perf_present_max_us) s_perf_present_max_us = present_us;
    if (render_us > s_perf_render_max_us) s_perf_render_max_us = render_us;
    if (dirty_pixels > s_perf_dirty_max_pixels) s_perf_dirty_max_pixels = dirty_pixels;

    if (s_perf_interval_count >= UI_PERF_SAMPLE_INTERVALS) {
        uint32_t fps_x10 = (uint32_t)((uint64_t)s_perf_interval_count * 10000000ULL /
                                      s_perf_interval_total_us);
        uint32_t present_avg_us = (uint32_t)(s_perf_present_total_us / s_perf_present_count);
        uint32_t render_avg_us = (uint32_t)(s_perf_render_total_us / s_perf_present_count);
        uint32_t dirty_avg_pixels = (uint32_t)(s_perf_dirty_total_pixels / s_perf_present_count);
        uint32_t dirty_pct_x10 = (uint32_t)((uint64_t)dirty_avg_pixels * 1000ULL /
                                            UI_SCREEN_PIXELS);
        uint32_t dirty_max_pct_x10 = (uint32_t)((uint64_t)s_perf_dirty_max_pixels * 1000ULL /
                                                UI_SCREEN_PIXELS);
        uint32_t flush_avg_x10 = (uint32_t)(s_perf_flush_total * 10ULL /
                                            s_perf_present_count);
        ESP_LOGI(TAG,
                 "LVGL active=%lu.%lu fps render=%lu.%lums(max %lu.%lu) present=%lu.%lums(max %lu.%lu) redraw=%lu.%lu%%(max %lu.%lu) flush=%lu.%lu",
                 (unsigned long)(fps_x10 / 10U), (unsigned long)(fps_x10 % 10U),
                 (unsigned long)(render_avg_us / 1000U), (unsigned long)((render_avg_us % 1000U) / 100U),
                 (unsigned long)(s_perf_render_max_us / 1000U),
                 (unsigned long)((s_perf_render_max_us % 1000U) / 100U),
                 (unsigned long)(present_avg_us / 1000U), (unsigned long)((present_avg_us % 1000U) / 100U),
                 (unsigned long)(s_perf_present_max_us / 1000U),
                 (unsigned long)((s_perf_present_max_us % 1000U) / 100U),
                 (unsigned long)(dirty_pct_x10 / 10U), (unsigned long)(dirty_pct_x10 % 10U),
                 (unsigned long)(dirty_max_pct_x10 / 10U),
                 (unsigned long)(dirty_max_pct_x10 % 10U),
                 (unsigned long)(flush_avg_x10 / 10U), (unsigned long)(flush_avg_x10 % 10U));
        perf_reset_sample();
    }
}

// 竖屏逻辑缓冲在 UI 启动早期一次性分配，避免运行中 TF/摄像头占用 PSRAM 后产生
// 大块连续内存碎片。两块缓冲保持 DIRECT 模式的脏区同步能力。
static esp_err_t portrait_render_init(void)
{
    if (s_portrait_fb0 && s_portrait_fb1 && s_orientation_ppa) return ESP_OK;

    s_portrait_fb0 = heap_caps_aligned_alloc(
        UI_PORTRAIT_BUFFER_ALIGN, UI_RGB565_FRAME_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_portrait_fb1 = heap_caps_aligned_alloc(
        UI_PORTRAIT_BUFFER_ALIGN, UI_RGB565_FRAME_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_portrait_fb0 || !s_portrait_fb1) {
        if (s_portrait_fb0) heap_caps_free(s_portrait_fb0);
        if (s_portrait_fb1) heap_caps_free(s_portrait_fb1);
        s_portrait_fb0 = NULL;
        s_portrait_fb1 = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_portrait_fb0, 0xFF, UI_RGB565_FRAME_BYTES);
    memset(s_portrait_fb1, 0xFF, UI_RGB565_FRAME_BYTES);

    ppa_client_config_t client_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&client_config, &s_orientation_ppa);
    if (ret != ESP_OK) {
        heap_caps_free(s_portrait_fb0);
        heap_caps_free(s_portrait_fb1);
        s_portrait_fb0 = NULL;
        s_portrait_fb1 = NULL;
        return ret;
    }
    return ESP_OK;
}

// 把 600x1024 逻辑画面逆时针旋转到 1024x600 物理帧缓冲。该方向对应设备当前
// 竖直安装姿态；面板时序和 DPI 双缓冲始终不变，不触碰已验证的 EK79007 参数。
static esp_err_t rotate_portrait_frame(const void *source, void **out_frame)
{
    if (!source || !out_frame || !s_orientation_ppa) return ESP_ERR_INVALID_STATE;
    void *write_fb = NULL;
    void *visible_fb = NULL;
    esp_err_t ret = fridge_display_mipi_get_ui_frame_buffers(&write_fb, &visible_fb);
    if (ret != ESP_OK || !write_fb || !visible_fb || write_fb == visible_fb) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    ppa_srm_oper_config_t config = {
        .in = {
            .buffer = source,
            .pic_w = UI_PORTRAIT_WIDTH,
            .pic_h = UI_PORTRAIT_HEIGHT,
            .block_w = UI_PORTRAIT_WIDTH,
            .block_h = UI_PORTRAIT_HEIGHT,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = write_fb,
            .buffer_size = UI_RGB565_FRAME_BYTES,
            .pic_w = 1024,
            .pic_h = 600,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        // PPA 角度按逆时针定义；90° 相对旧竖屏方向翻转 180°。
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ret = ppa_do_scale_rotate_mirror(s_orientation_ppa, &config);
    if (ret == ESP_OK) *out_frame = write_fb;
    return ret;
}

// 竖屏 PPA 滚动器只更新 600x1024 逻辑缓冲中的主视窗；提交前继续复用同一
// 硬件旋转路径，不让滚动组件接触 EK79007 的物理帧缓冲所有权。
static esp_err_t present_portrait_logical_frame(const void *logical_frame, void *user_ctx)
{
    (void)user_ctx;
    void *physical_frame = NULL;
    esp_err_t ret = rotate_portrait_frame(logical_frame, &physical_frame);
    if (ret != ESP_OK) return ret;
    return fridge_display_mipi_present_frame(physical_frame);
}

static void display_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    // 汇总本轮所有脏区的包围盒与 flush 次数，用于区分“绘制慢”和“等待 VSYNC 慢”。
    if (!s_flush_union_valid) {
        s_flush_union = *area;
        s_flush_union_valid = true;
    } else {
        if (area->x1 < s_flush_union.x1) s_flush_union.x1 = area->x1;
        if (area->y1 < s_flush_union.y1) s_flush_union.y1 = area->y1;
        if (area->x2 > s_flush_union.x2) s_flush_union.x2 = area->x2;
        if (area->y2 > s_flush_union.y2) s_flush_union.y2 = area->y2;
    }
    s_flush_count++;
    // 摄像头预览独占 DPI 帧缓冲时跳过 LVGL 提交，仍需 flush_ready 以免卡死。
    if (fridge_display_mipi_camera_owns_framebuffers()) {
        lv_display_flush_ready(display);
        return;
    }
    // DIRECT 双缓冲可能为一轮刷新上报多个脏区；所有脏区都画进同一离屏缓冲，
    // 只在最后一个脏区完成时翻页，避免同一轮内容被重复提交和错误换缓冲。
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }
    int64_t present_start_us = esp_timer_get_time();
    void *present_frame = px_map;
    const fridge_ui_p4_orientation_t orientation =
        (fridge_ui_p4_orientation_t)atomic_load_explicit(&s_current_orientation,
                                                          memory_order_acquire);
    esp_err_t ret = ESP_OK;
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        ret = rotate_portrait_frame(px_map, &present_frame);
    }
    if (ret == ESP_OK) {
        ret = fridge_display_mipi_present_frame(present_frame);
    }
    int64_t present_done_us = esp_timer_get_time();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MIPI frame present failed: %s", esp_err_to_name(ret));
    }
    lv_display_flush_ready(display);
    if (ret == ESP_OK) {
        s_lvgl_present_generation++;
        uint32_t dirty_pixels = s_flush_union_valid ?
                                (uint32_t)lv_area_get_width(&s_flush_union) *
                                (uint32_t)lv_area_get_height(&s_flush_union) : 0;
        uint32_t render_us = s_render_cycle_start_us > 0 ?
                             (uint32_t)(present_done_us - s_render_cycle_start_us) : 0;
        perf_record_frame(present_done_us, (uint32_t)(present_done_us - present_start_us),
                          render_us, dirty_pixels, s_flush_count);
    }
    s_flush_union_valid = false;
    s_flush_count = 0;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint8_t s_press_log_left = 8;
    // 预览模式下触摸由摄像头任务消费以退出，不注入 LVGL，避免误触页面按钮。
    if (s_camera_preview_mode ||
        atomic_load_explicit(&s_camera_capture_running, memory_order_acquire)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    fridge_touch_point_t point = {0};
    if (fridge_touch_gt911_read(&point) == ESP_OK && point.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        const fridge_ui_p4_orientation_t orientation =
            (fridge_ui_p4_orientation_t)atomic_load_explicit(&s_current_orientation,
                                                              memory_order_acquire);
        if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
            // 逻辑画面逆时针输出：物理触点反变换为 x=599-y、y=x。
            data->point.x = 599 - point.y;
            data->point.y = point.x;
        } else {
            data->point.x = point.x;
            data->point.y = point.y;
        }
        // 前几次按下打印坐标，确认 GT911→LVGL 链路；避免长稳时刷爆串口。
        if (s_press_log_left > 0) {
            s_press_log_left--;
            ESP_LOGI(TAG, "touch press x=%u y=%u points=%u",
                     (unsigned)point.x, (unsigned)point.y, (unsigned)point.points);
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(UI_TICK_MS);
}

// 在 LVGL 页面建好前先刷一帧纯色，证明 DPI 通路活着，避免“背光开但全黑”。
static void paint_boot_splash(void *fb0, void *fb1)
{
    if (!fb0) return;
    uint16_t *fb = (uint16_t *)fb0;
    const size_t count = (size_t)FRIDGE_MIPI_WIDTH * FRIDGE_MIPI_HEIGHT;
    // #FFFDF8 量化后的 RGB565 暖米白；显示边界仍保持现有 RGB565 字节序。
    const uint16_t color = 0xFFDF;
    for (size_t i = 0; i < count; i++) fb[i] = color;
    // DIRECT 双缓冲只同步脏区，因此启动时先给另一缓冲相同底色，避免未覆盖区域出现随机像素。
    if (fb1) memcpy(fb1, fb0, count * sizeof(uint16_t));
    esp_err_t ret = fridge_display_mipi_present_frame(fb0);
    ESP_LOGI(TAG, "boot splash present: %s (fb1=%p)", esp_err_to_name(ret), fb1);
}

// UI 仅维护手动 ASR 会话的展示快照。ASR/Agent/TTS 的工作状态由后台任务和
// Agent 事件驱动，不能在这里调用可能阻塞的 WakeNet、I2S 或 HTTPS 接口。
static void collect_voice_status(fridge_ui_device_model_t *out)
{
    if (!out) return;
    out->voice_active = s_voice_phase != UI_VOICE_PHASE_IDLE;
    out->voice_recording = s_voice_phase == UI_VOICE_PHASE_RECORDING;
    out->voice_can_submit = s_voice_phase == UI_VOICE_PHASE_RECORDING ||
                            s_voice_phase == UI_VOICE_PHASE_RECORDING_READY;
    out->voice_can_cancel = out->voice_can_submit;
    out->voice_error = false;
    out->voice_error_text[0] = '\0';
    out->voice_state[0] = '\0';

    // 录音最长 6 秒，底层到达上限后会保留 PCM 并切到 READY。这里把弹窗改为“可提交”，
    // 避免继续伪装成无限聆听，也不会自动把未确认的音频送去云端。
    if (s_voice_phase == UI_VOICE_PHASE_RECORDING) {
        fridge_audio_status_t audio = {0};
        if (fridge_audio_get_status(&audio) == ESP_OK) {
            if (audio.state == FRIDGE_AUDIO_STATE_READY) {
                s_voice_phase = UI_VOICE_PHASE_RECORDING_READY;
                out->voice_recording = false;
                out->voice_can_submit = true;
                out->voice_can_cancel = true;
            } else if (audio.state == FRIDGE_AUDIO_STATE_ERROR) {
                s_voice_phase = UI_VOICE_PHASE_IDLE;
                out->voice_active = false;
                out->voice_can_submit = false;
                out->voice_can_cancel = false;
                out->voice_error = true;
                strlcpy(out->voice_error_text,
                        audio.error[0] ? audio.error : "录音发生异常",
                        sizeof(out->voice_error_text));
                finish_voice_manual_session();
                return;
            }
        }
    }

    switch (s_voice_phase) {
    case UI_VOICE_PHASE_STARTING:
        strlcpy(out->voice_state, "正在启动", sizeof(out->voice_state));
        break;
    case UI_VOICE_PHASE_RECORDING:
        strlcpy(out->voice_state, "聆听中", sizeof(out->voice_state));
        break;
    case UI_VOICE_PHASE_RECORDING_READY:
        strlcpy(out->voice_state, "录音已完成", sizeof(out->voice_state));
        break;
    case UI_VOICE_PHASE_TRANSCRIBING:
        strlcpy(out->voice_state, "正在识别", sizeof(out->voice_state));
        break;
    case UI_VOICE_PHASE_CANCELLING:
        strlcpy(out->voice_state, "正在取消", sizeof(out->voice_state));
        break;
    case UI_VOICE_PHASE_IDLE:
    default:
        break;
    }
}

static void collect_device_status(fridge_ui_device_model_t *out)
{
    fridge_ui_device_model_t next = {0};
    // 温湿度：从 SHT40 快照读取；未就绪或 CRC 失败时保留占位符。
    fridge_sht4x_snapshot_t env = {0};
    if (fridge_sht4x_get_snapshot(&env) == ESP_OK && env.valid) {
        snprintf(next.temperature, sizeof(next.temperature), "%.1f°C", env.temperature_c);
        snprintf(next.humidity, sizeof(next.humidity), "%.0f%%", env.humidity_rh);
    } else {
        strlcpy(next.temperature, "--.-°C", sizeof(next.temperature));
        strlcpy(next.humidity, "--%", sizeof(next.humidity));
    }

    time_t now = time(NULL);
    struct tm local = {0};
    if (now >= 1704067200 && localtime_r(&now, &local)) {
        static const char *const weekdays[] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六",
        };
        next.time_valid = true;
        snprintf(next.time_text, sizeof(next.time_text), "%02d:%02d", local.tm_hour, local.tm_min);
        snprintf(next.date_text, sizeof(next.date_text), "%d月%d日", local.tm_mon + 1,
                 local.tm_mday);
        strlcpy(next.weekday_text, weekdays[local.tm_wday], sizeof(next.weekday_text));
    } else {
        strlcpy(next.time_text, "--:--", sizeof(next.time_text));
        strlcpy(next.date_text, "未校时", sizeof(next.date_text));
    }

    fridge_network_status_t network = {0};
    if (fridge_network_get_status(&network) == ESP_OK && network.initialized) {
        next.wifi = network.connected ? FRIDGE_UI_HEALTH_OK :
                    network.connecting ? FRIDGE_UI_HEALTH_WARN : FRIDGE_UI_HEALTH_OFFLINE;
        next.cloud = network.internet_ready ? FRIDGE_UI_HEALTH_OK :
                     network.connected ? FRIDGE_UI_HEALTH_WARN : FRIDGE_UI_HEALTH_OFFLINE;
        next.wifi_rssi = network.rssi;
    }

    fridge_tfcard_status_t tfcard = {0};
    fridge_tfcard_get_status(&tfcard);
    next.tfcard = tfcard.mounted ? FRIDGE_UI_HEALTH_OK :
                  tfcard.format_required ? FRIDGE_UI_HEALTH_DANGER : FRIDGE_UI_HEALTH_OFFLINE;

    fridge_camera_status_t camera = {0};
    fridge_camera_get_status(&camera);
    next.camera = camera.initialized ? FRIDGE_UI_HEALTH_OK :
                  camera.last_error[0] ? FRIDGE_UI_HEALTH_DANGER : FRIDGE_UI_HEALTH_OFFLINE;
    next.camera_has_frame = camera.has_frame;
    next.camera_ai_active = s_camera_ai_phase == UI_CAMERA_AI_PHASE_CAPTURING ||
                            s_camera_ai_phase == UI_CAMERA_AI_PHASE_RECOGNIZING;
    next.camera_ai_error = s_camera_ai_phase == UI_CAMERA_AI_PHASE_FAILED;
    switch (s_camera_ai_phase) {
    case UI_CAMERA_AI_PHASE_CAPTURING:
        strlcpy(next.camera_ai_state, "正在拍摄", sizeof(next.camera_ai_state));
        break;
    case UI_CAMERA_AI_PHASE_PHOTO_READY:
        strlcpy(next.camera_ai_state, "照片已就绪", sizeof(next.camera_ai_state));
        break;
    case UI_CAMERA_AI_PHASE_RECOGNIZING:
        strlcpy(next.camera_ai_state, "正在识别", sizeof(next.camera_ai_state));
        break;
    case UI_CAMERA_AI_PHASE_SUCCEEDED:
        strlcpy(next.camera_ai_state, "识别完成", sizeof(next.camera_ai_state));
        break;
    case UI_CAMERA_AI_PHASE_FAILED:
        strlcpy(next.camera_ai_state, "识别失败", sizeof(next.camera_ai_state));
        break;
    case UI_CAMERA_AI_PHASE_IDLE:
    default:
        break;
    }
    strlcpy(next.camera_ai_detail, s_camera_ai_detail, sizeof(next.camera_ai_detail));

    fridge_audio_status_t audio = {0};
    if (fridge_audio_get_status(&audio) == ESP_OK) {
        next.microphone = audio.state == FRIDGE_AUDIO_STATE_ERROR ?
                          FRIDGE_UI_HEALTH_DANGER : FRIDGE_UI_HEALTH_OK;
    } else {
        next.microphone = FRIDGE_UI_HEALTH_OFFLINE;
    }
    collect_voice_status(&next);
    // 连续对话的所有音频、网络与 WakeNet 操作都由 voice_asr 协调任务处理；UI 只复制状态。
    fridge_voice_dialog_status_t dialog = {0};
    if (fridge_voice_asr_dialog_get_status(&dialog) == ESP_OK) {
        next.continuous_voice_active = dialog.active;
        next.continuous_voice_error = dialog.state == FRIDGE_VOICE_DIALOG_STATE_ERROR;
        strlcpy(next.continuous_voice_state, dialog.state_text,
                sizeof(next.continuous_voice_state));
        strlcpy(next.continuous_voice_detail, dialog.detail,
                sizeof(next.continuous_voice_detail));
        if (next.continuous_voice_error) {
            strlcpy(next.continuous_voice_error_text, dialog.detail,
                    sizeof(next.continuous_voice_error_text));
        }
    }
    *out = next;
}

// 读取 P4 MPU6050 的最近快照，供 MORE-04 只读展示。
// 雷达在接线确认前保持未就绪；LVGL 绘制期间不会直接访问 I2C/UART。
static void collect_safety_status(fridge_ui_safety_model_t *out)
{
    if (!out) return;
    fridge_ui_safety_model_t next = {0};
    // P4 雷达 UART 接线尚未按板卡手册确认，不能沿用旧 S3 GPIO20/21。
    // 暂不链接 radar 组件，页面保留“未就绪”状态，避免错误配置 GPIO 或误报有人。
    fridge_sensor_snapshot_t sensors = {0};
    if (fridge_sensors_get_snapshot(&sensors) == ESP_OK) {
        next.imu_ready = sensors.imu_ready;
        next.angle_delta = sensors.angle_delta;
        next.vibration_peak = sensors.vibration_peak;
        next.ready = next.ready || sensors.imu_ready;
        // MQ-5 只读展示字段：直接从 sensors 快照复制，绘制期间不再读 ADC。
        next.mq5_state = sensors.mq5_state;
        next.mq5_ready = sensors.mq5_ready;
        next.mq5_error = sensors.mq5_error != ESP_OK;
        next.mq5_raw_12bit = sensors.mq5_raw_12bit;
        next.mq5_value_10bit = sensors.mq5_value_10bit;
        next.mq5_adc_percent = sensors.mq5_adc_percent;
        next.mq5_updated_at_ms = sensors.mq5_updated_at_ms;
        next.mq5_warmup_elapsed_sec = sensors.mq5_warmup_elapsed_sec;
        next.mq5_daily_warmup_complete = sensors.mq5_daily_warmup_complete;
        next.mq5_burn_in_confirmed = sensors.mq5_burn_in_confirmed;
        next.mq5_baseline_ready = sensors.mq5_baseline_ready;
        next.mq5_baseline_raw_12bit = sensors.mq5_baseline_raw_12bit;
        next.mq5_delta_from_baseline = sensors.mq5_delta_from_baseline;
    }
    *out = next;
}

// 每个手动语音会话只在这里解除“对话进行中”标记。保持该标记直到 Agent 的 TTS
// 终态，避免 WakeNet 在 ASR/LLM 间隙抢回 ES8311 的 I2S0 与共享 I2C。
static void finish_voice_manual_session(void)
{
    if (!s_voice_waiting_agent_tts && s_voice_phase == UI_VOICE_PHASE_IDLE) {
        // 录音启动失败和取消事件也可能在 Agent 尚未接管时进入；manual finish 本身幂等。
        fridge_voice_asr_manual_session_finish();
        return;
    }
    fridge_voice_asr_manual_session_finish();
    s_voice_waiting_agent_tts = false;
}

static void refresh_voice_device(void)
{
    collect_voice_status(&s_device);
    fridge_ui_shell_update_device(&s_device);
}

// 把 WakeNet 策略枚举转成面向用户的短说明。用户开关与当前监听状态分离展示，
// 因此内部 DMA 不足、音频占用等情况不会被误报成“已监听”。
static void format_wake_word_detail(const fridge_wake_word_status_t *status,
                                    char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!status) {
        strlcpy(out, "唤醒服务未就绪", out_size);
        return;
    }
    if (status->state == FRIDGE_WAKE_WORD_STATE_LISTENING) {
        strlcpy(out, "当前正在监听", out_size);
        return;
    }

    switch (status->policy_reason) {
    case FRIDGE_WAKE_POLICY_DISABLED_BY_USER:
        strlcpy(out, "用户已关闭", out_size);
        break;
    case FRIDGE_WAKE_POLICY_DISPLAY_NOT_READY:
        strlcpy(out, "等待屏幕初始化", out_size);
        break;
    case FRIDGE_WAKE_POLICY_SCREEN_OFF:
        strlcpy(out, "屏幕休眠时暂停", out_size);
        break;
    case FRIDGE_WAKE_POLICY_AUDIO_BUSY:
        strlcpy(out, "录音占用中，暂时暂停", out_size);
        break;
    case FRIDGE_WAKE_POLICY_SPEAKER_BUSY:
        strlcpy(out, "语音播放中，暂时暂停", out_size);
        break;
    case FRIDGE_WAKE_POLICY_LOW_INTERNAL_DMA:
        snprintf(out, out_size, "内部 DMA 不足（%u/%uKB）",
                 (unsigned)(status->largest_internal_dma_block / 1024U),
                 (unsigned)(status->min_dma_block_threshold / 1024U));
        break;
    case FRIDGE_WAKE_POLICY_STARTING:
        strlcpy(out, "正在启动监听", out_size);
        break;
    case FRIDGE_WAKE_POLICY_STOPPING:
        strlcpy(out, "正在停止监听", out_size);
        break;
    case FRIDGE_WAKE_POLICY_LISTENING:
        strlcpy(out, "当前正在监听", out_size);
        break;
    default:
        strlcpy(out, status->error[0] ? status->error : "当前暂停", out_size);
        break;
    }
}

// 设置页状态只由 LVGL 任务写入；后台任务通过无密码事件提交结果，避免并发读写 UI 快照。
static void refresh_settings_runtime_state(void)
{
    s_settings_state.brightness_percent = fridge_display_mipi_get_brightness();
    fridge_agent_permission_mode_t permission = FRIDGE_AGENT_PERMISSION_CONFIRM;
    if (fridge_tool_broker_get_permission_mode(&permission) == ESP_OK) {
        s_settings_state.agent_permission_mode = (uint8_t)permission;
    } else {
        s_settings_state.agent_permission_mode = FRIDGE_AGENT_PERMISSION_CONFIRM;
    }
    fridge_wake_word_status_t wake = {0};
    if (fridge_wake_word_get_status(&wake) == ESP_OK) {
        s_settings_state.wake_word_user_enabled = wake.user_enabled;
        s_settings_state.wake_word_listening =
            wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING;
        format_wake_word_detail(&wake, s_settings_state.wake_word_detail,
                                sizeof(s_settings_state.wake_word_detail));
    } else {
        s_settings_state.wake_word_user_enabled = fridge_wake_word_get_user_enabled();
        s_settings_state.wake_word_listening = false;
        strlcpy(s_settings_state.wake_word_detail, "唤醒服务未就绪",
                sizeof(s_settings_state.wake_word_detail));
    }
}

static void post_settings_event(const ui_settings_event_t *event)
{
    if (!event || !s_settings_event_queue) return;
    if (xQueueSend(s_settings_event_queue, event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "settings event queue full, action=%d", (int)event->action);
    }
}

static void post_settings_wifi_phase(fridge_ui_shell_settings_wifi_phase_t phase,
                                     esp_err_t err, const fridge_wifi_config_t *config,
                                     const fridge_network_status_t *network,
                                     const char *detail)
{
    ui_settings_event_t event = {
        .action = FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT,
        .err = err,
        .wifi_phase = phase,
    };
    if (config) strlcpy(event.wifi_ssid, config->ssid, sizeof(event.wifi_ssid));
    if (network) strlcpy(event.wifi_ip, network->ip, sizeof(event.wifi_ip));
    strlcpy(event.wifi_status, detail ? detail : "", sizeof(event.wifi_status));
    post_settings_event(&event);
}

// 密码错误、DHCP 失败或外网校验超时后都只恢复旧的已保存配置。候选凭据从未写入 NVS，
// 因而失败不会覆盖可用网络；恢复动作本身仍在 network 组件自己的后台任务中执行。
static void restore_saved_wifi_after_test(const fridge_wifi_config_t *config,
                                          const fridge_network_status_t *network,
                                          const char *reason)
{
    char message[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
    snprintf(message, sizeof(message), "%s；正在恢复原网络", reason ? reason : "连接测试失败");
    post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_RESTORING, ESP_FAIL,
                             config, network, message);
    esp_err_t restore_err = fridge_network_connect_saved_async();
    if (restore_err != ESP_OK && restore_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "restore saved Wi-Fi failed: %s", esp_err_to_name(restore_err));
    }
}

static void sort_settings_wifi_aps(fridge_wifi_ap_t *aps, size_t count)
{
    if (!aps) return;
    for (size_t i = 1; i < count; i++) {
        fridge_wifi_ap_t current = aps[i];
        size_t j = i;
        while (j > 0 && current.rssi > aps[j - 1].rssi) {
            aps[j] = aps[j - 1];
            j--;
        }
        aps[j] = current;
    }
}

// 已关联 AP 后等待自动 SNTP 任务完成。该等待只在设置后台任务执行，绝不阻塞 LVGL。
static bool wait_for_settings_wifi_internet_ready(fridge_network_status_t *network)
{
    if (!network) return false;
    const int64_t deadline_us = esp_timer_get_time() +
                                (int64_t)UI_SETTINGS_WIFI_VERIFY_TIMEOUT_MS * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(UI_SETTINGS_WIFI_VERIFY_POLL_MS));
        if (fridge_network_get_status(network) != ESP_OK) continue;
        if (network->internet_ready) return true;
        if (!network->connected && !network->connecting) break;
    }
    return false;
}

// 设置后台任务承担 Wi-Fi 扫描、DHCP/SNTP 验证、NVS 写入与 WakeNet 偏好写入。
// 此任务不接触 LVGL，所有可见结果一律经 s_settings_event_queue 回到 ui_task。
static void settings_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ui_settings_job_t job = {0};
        if (xQueueReceive(s_settings_job_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        if (job.kind == UI_SETTINGS_JOB_SAVE_BRIGHTNESS) {
            ui_settings_event_t event = {
                .action = FRIDGE_UI_SHELL_SETTINGS_SET_BRIGHTNESS,
                .brightness_percent = job.brightness_percent,
                .err = ESP_FAIL,
            };
            nvs_handle_t nvs = 0;
            esp_err_t err = nvs_open(UI_SETTINGS_BRIGHTNESS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
            if (err == ESP_OK) {
                err = nvs_set_u8(nvs, UI_SETTINGS_BRIGHTNESS_NVS_KEY,
                                 job.brightness_percent);
                if (err == ESP_OK) err = nvs_commit(nvs);
                nvs_close(nvs);
            }
            event.err = err;
            post_settings_event(&event);
            continue;
        }

        if (job.kind == UI_SETTINGS_JOB_SET_WAKE_WORD) {
            esp_err_t err = fridge_wake_word_set_user_enabled(job.wake_word_enabled);
            // 策略更新只投递启动/停止请求，不会在设置线程内同步加载模型或等待 I2S。
            fridge_wake_word_policy_update(fridge_display_mipi_is_ready(),
                                           fridge_display_mipi_is_backlight_enabled());
            fridge_wake_word_status_t wake = {0};
            (void)fridge_wake_word_get_status(&wake);
            ui_settings_event_t event = {
                .action = FRIDGE_UI_SHELL_SETTINGS_SET_WAKE_WORD_ENABLED,
                .err = err,
                .wake_word_user_enabled = wake.user_enabled,
                .wake_word_listening = wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING,
            };
            format_wake_word_detail(&wake, event.wake_word_detail,
                                    sizeof(event.wake_word_detail));
            post_settings_event(&event);
            continue;
        }

        if (job.kind == UI_SETTINGS_JOB_SET_AGENT_PERMISSION_MODE) {
            ui_settings_event_t event = {
                .action = FRIDGE_UI_SHELL_SETTINGS_SET_AGENT_PERMISSION_MODE,
                .agent_permission_mode = job.agent_permission_mode,
                .err = fridge_tool_broker_set_permission_mode_persisted(
                    (fridge_agent_permission_mode_t)job.agent_permission_mode),
            };
            post_settings_event(&event);
            continue;
        }

        if (job.kind == UI_SETTINGS_JOB_WIFI_SCAN) {
            fridge_wifi_ap_t aps[FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS] = {0};
            size_t count = 0;
            esp_err_t err = fridge_network_scan(aps, sizeof(aps) / sizeof(aps[0]), &count);
            ui_settings_event_t event = {
                .action = FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN,
                .err = err,
                .wifi_phase = err == ESP_OK ? FRIDGE_UI_SHELL_SETTINGS_WIFI_IDLE :
                                               FRIDGE_UI_SHELL_SETTINGS_WIFI_FAILED,
            };
            if (err == ESP_OK) {
                sort_settings_wifi_aps(aps, count);
                char saved_ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1] = {0};
                const bool has_saved_ssid =
                    fridge_network_get_saved_ssid(saved_ssid, sizeof(saved_ssid)) == ESP_OK;
                event.wifi_ap_count = count;
                for (size_t i = 0; i < count; i++) {
                    strlcpy(event.wifi_aps[i].ssid, aps[i].ssid,
                            sizeof(event.wifi_aps[i].ssid));
                    event.wifi_aps[i].rssi = aps[i].rssi;
                    event.wifi_aps[i].channel = aps[i].channel;
                    event.wifi_aps[i].secured = aps[i].secured;
                    event.wifi_aps[i].saved = has_saved_ssid &&
                                               strcmp(aps[i].ssid, saved_ssid) == 0;
                    strlcpy(event.wifi_aps[i].authmode, aps[i].authmode,
                            sizeof(event.wifi_aps[i].authmode));
                }
                memset(saved_ssid, 0, sizeof(saved_ssid));
                snprintf(event.wifi_status, sizeof(event.wifi_status),
                         "扫描完成，发现 %u 个网络", (unsigned)count);
            } else {
                fridge_network_status_t network = {0};
                (void)fridge_network_get_status(&network);
                strlcpy(event.wifi_status,
                        network.last_error[0] ? network.last_error : "Wi-Fi 扫描失败",
                        sizeof(event.wifi_status));
            }
            post_settings_event(&event);
            continue;
        }

        if (job.kind == UI_SETTINGS_JOB_WIFI_CONNECT_SAVED) {
            char saved_ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1] = {0};
            fridge_network_status_t network = {0};
            esp_err_t err = fridge_network_get_saved_ssid(saved_ssid, sizeof(saved_ssid));
            if (err != ESP_OK || strcmp(saved_ssid, job.wifi.ssid) != 0) {
                post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_FAILED,
                                         err == ESP_OK ? ESP_ERR_INVALID_STATE : err,
                                         &job.wifi, NULL,
                                         "已保存网络已变更，请重新扫描后输入密码");
                memset(saved_ssid, 0, sizeof(saved_ssid));
                memset(&job.wifi, 0, sizeof(job.wifi));
                continue;
            }

            post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTING, ESP_OK,
                                     &job.wifi, NULL, "正在使用已保存凭据并等待 DHCP…");
            err = fridge_network_connect_saved();
            if (err != ESP_OK) {
                (void)fridge_network_get_status(&network);
                post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_FAILED, err,
                                         &job.wifi, &network,
                                         network.last_error[0] ? network.last_error :
                                                                 "已保存 Wi-Fi 关联或 DHCP 失败");
                memset(saved_ssid, 0, sizeof(saved_ssid));
                memset(&job.wifi, 0, sizeof(job.wifi));
                continue;
            }

            (void)fridge_network_get_status(&network);
            char verify_detail[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
            snprintf(verify_detail, sizeof(verify_detail),
                     "已使用保存凭据，IP %s；正在验证 SNTP 外网…",
                     network.ip[0] ? network.ip : "已获取");
            post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_VERIFYING, ESP_OK,
                                     &job.wifi, &network, verify_detail);
            if (wait_for_settings_wifi_internet_ready(&network)) {
                char success[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
                snprintf(success, sizeof(success), "已保存网络验证成功，IP %s",
                         network.ip[0] ? network.ip : "已获取");
                post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTED, ESP_OK,
                                         &job.wifi, &network, success);
            } else {
                post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_FAILED, ESP_ERR_TIMEOUT,
                                         &job.wifi, &network,
                                         network.last_error[0] ? network.last_error :
                                                                 "DHCP 已成功，但 SNTP 外网验证超时");
            }
            memset(saved_ssid, 0, sizeof(saved_ssid));
            memset(&job.wifi, 0, sizeof(job.wifi));
            continue;
        }

        if (job.kind == UI_SETTINGS_JOB_WIFI_CONNECT) {
            fridge_network_status_t network = {0};
            post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTING, ESP_OK,
                                     &job.wifi, NULL, "正在关联热点并等待 DHCP…");
            esp_err_t err = fridge_network_connect(&job.wifi, false);
            if (err != ESP_OK) {
                (void)fridge_network_get_status(&network);
                restore_saved_wifi_after_test(&job.wifi, &network,
                                              network.last_error[0] ? network.last_error :
                                                                        "Wi-Fi 关联或 DHCP 失败");
                memset(&job.wifi, 0, sizeof(job.wifi));
                continue;
            }

            (void)fridge_network_get_status(&network);
            char verify_detail[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
            snprintf(verify_detail, sizeof(verify_detail), "DHCP 成功，IP %s；正在验证 SNTP 外网…",
                     network.ip[0] ? network.ip : "已获取");
            post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_VERIFYING, ESP_OK,
                                     &job.wifi, &network, verify_detail);

            if (!wait_for_settings_wifi_internet_ready(&network)) {
                restore_saved_wifi_after_test(&job.wifi, &network,
                                              network.last_error[0] ? network.last_error :
                                                                        "DHCP 已成功，但 SNTP 外网验证超时");
                memset(&job.wifi, 0, sizeof(job.wifi));
                continue;
            }

            err = job.save_after_verified ? fridge_network_save_config(&job.wifi) : ESP_OK;
            if (err == ESP_OK) {
                char success[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
                snprintf(success, sizeof(success), "DHCP 与 SNTP 验证成功，IP %s；%s",
                         network.ip[0] ? network.ip : "已获取",
                         job.save_after_verified ? "已保存" : "未保存");
                post_settings_wifi_phase(FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTED, ESP_OK,
                                         &job.wifi, &network, success);
            } else {
                restore_saved_wifi_after_test(&job.wifi, &network,
                                              "联网验证成功，但保存凭据失败");
            }
            memset(&job.wifi, 0, sizeof(job.wifi));
        }
    }
}

// Shell 回调位于唯一 LVGL 任务：亮度 LEDC 写寄存器可即时执行，Flash/NVS 写入仍交给
// settings_worker。其余操作只复制固定长度请求到队列，绝不在触摸回调中扫描或联网。
static esp_err_t settings_action(void *user_ctx, fridge_ui_shell_settings_action_t action,
                                 const fridge_ui_shell_settings_request_t *request)
{
    (void)user_ctx;
    if (!request || !s_settings_job_queue) return ESP_ERR_INVALID_STATE;

    if (action == FRIDGE_UI_SHELL_SETTINGS_SET_BRIGHTNESS) {
        if (request->brightness_percent < 10 || request->brightness_percent > 100) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = fridge_display_mipi_set_brightness(request->brightness_percent);
        if (err != ESP_OK) return err;
        s_settings_state.brightness_percent = request->brightness_percent;
        fridge_ui_shell_update_settings(&s_settings_state);
        if (!request->persist) return ESP_OK;

        ui_settings_job_t job = {
            .kind = UI_SETTINGS_JOB_SAVE_BRIGHTNESS,
            .brightness_percent = request->brightness_percent,
        };
        if (xQueueSend(s_settings_job_queue, &job, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
        return ESP_OK;
    }

    ui_settings_job_t job = {0};
    if (action == FRIDGE_UI_SHELL_SETTINGS_SET_WAKE_WORD_ENABLED) {
        job.kind = UI_SETTINGS_JOB_SET_WAKE_WORD;
        job.wake_word_enabled = request->wake_word_enabled;
    } else if (action == FRIDGE_UI_SHELL_SETTINGS_SET_AGENT_PERMISSION_MODE) {
        if (request->agent_permission_mode > FRIDGE_AGENT_PERMISSION_AUTO) {
            return ESP_ERR_INVALID_ARG;
        }
        job.kind = UI_SETTINGS_JOB_SET_AGENT_PERMISSION_MODE;
        job.agent_permission_mode = request->agent_permission_mode;
    } else if (action == FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN) {
        job.kind = UI_SETTINGS_JOB_WIFI_SCAN;
        s_settings_state.wifi_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_SCANNING;
        strlcpy(s_settings_state.wifi_status, "正在扫描附近 Wi-Fi…",
                sizeof(s_settings_state.wifi_status));
        fridge_ui_shell_update_settings(&s_settings_state);
    } else if (action == FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT) {
        if (!request->ssid[0]) return ESP_ERR_INVALID_ARG;
        job.kind = UI_SETTINGS_JOB_WIFI_CONNECT;
        job.save_after_verified = request->save_after_verified;
        strlcpy(job.wifi.ssid, request->ssid, sizeof(job.wifi.ssid));
        strlcpy(job.wifi.password, request->password, sizeof(job.wifi.password));
    } else if (action == FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT_SAVED) {
        if (!request->ssid[0]) return ESP_ERR_INVALID_ARG;
        // 已保存连接只传 SSID；网络组件自行从 NVS 读取凭据，UI 队列不保存密码。
        job.kind = UI_SETTINGS_JOB_WIFI_CONNECT_SAVED;
        strlcpy(job.wifi.ssid, request->ssid, sizeof(job.wifi.ssid));
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(s_settings_job_queue, &job, 0) != pdTRUE) {
        memset(&job, 0, sizeof(job));
        return ESP_ERR_TIMEOUT;
    }
    memset(&job, 0, sizeof(job));
    return ESP_OK;
}

static void process_settings_events(void)
{
    if (!s_settings_event_queue) return;
    ui_settings_event_t event = {0};
    while (xQueueReceive(s_settings_event_queue, &event, 0) == pdTRUE) {
        if (event.action == FRIDGE_UI_SHELL_SETTINGS_SET_BRIGHTNESS) {
            s_settings_state.brightness_percent = event.brightness_percent;
            if (event.err != ESP_OK) {
                fridge_ui_shell_show_toast("亮度偏好保存失败，将在本次开机内保持");
            }
        } else if (event.action == FRIDGE_UI_SHELL_SETTINGS_SET_WAKE_WORD_ENABLED) {
            s_settings_state.wake_word_user_enabled = event.wake_word_user_enabled;
            s_settings_state.wake_word_listening = event.wake_word_listening;
            strlcpy(s_settings_state.wake_word_detail, event.wake_word_detail,
                    sizeof(s_settings_state.wake_word_detail));
            if (event.err != ESP_OK) fridge_ui_shell_show_toast("语音唤醒设置失败");
        } else if (event.action == FRIDGE_UI_SHELL_SETTINGS_SET_AGENT_PERMISSION_MODE) {
            if (event.err == ESP_OK) {
                s_settings_state.agent_permission_mode = event.agent_permission_mode;
                fridge_ui_shell_show_toast("小精灵权限模式已保存");
            } else {
                fridge_ui_shell_show_toast("小精灵权限模式保存失败");
            }
        } else if (event.action == FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN ||
                   event.action == FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT) {
            s_settings_state.wifi_phase = event.wifi_phase;
            strlcpy(s_settings_state.wifi_status, event.wifi_status,
                    sizeof(s_settings_state.wifi_status));
            strlcpy(s_settings_state.wifi_ssid, event.wifi_ssid,
                    sizeof(s_settings_state.wifi_ssid));
            strlcpy(s_settings_state.wifi_ip, event.wifi_ip, sizeof(s_settings_state.wifi_ip));
            if (event.action == FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN && event.err == ESP_OK) {
                s_settings_state.wifi_ap_count = event.wifi_ap_count;
                memcpy(s_settings_state.wifi_aps, event.wifi_aps,
                       sizeof(s_settings_state.wifi_aps));
            }
        }
        fridge_ui_shell_update_settings(&s_settings_state);
    }
}

// 显示驱动不持久化用户偏好：开机时由 UI 编排层从 NVS 恢复一次，防止驱动层在
// 亮度拖动期间频繁擦写 Flash。损坏或旧固件留下的范围外值一律回退到安全的 100%。
static void load_saved_brightness_preference(void)
{
    uint8_t brightness = 100;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(UI_SETTINGS_BRIGHTNESS_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t stored = 0;
        err = nvs_get_u8(nvs, UI_SETTINGS_BRIGHTNESS_NVS_KEY, &stored);
        nvs_close(nvs);
        if (err == ESP_OK && stored >= 10 && stored <= 100) {
            brightness = stored;
        }
    }
    if (fridge_display_mipi_set_brightness(brightness) != ESP_OK) {
        ESP_LOGW(TAG, "apply saved display brightness failed");
    }
    s_settings_state.brightness_percent = brightness;
}

static void show_voice_error(const char *detail)
{
    s_voice_phase = UI_VOICE_PHASE_IDLE;
    finish_voice_manual_session();
    collect_voice_status(&s_device);
    s_device.voice_error = true;
    strlcpy(s_device.voice_error_text,
            detail && detail[0] ? detail : "语音操作失败",
            sizeof(s_device.voice_error_text));
    fridge_ui_shell_update_device(&s_device);
}

static bool post_voice_event(ui_voice_event_kind_t kind, esp_err_t err,
                             const char *transcript, const char *detail)
{
    if (!s_voice_event_queue) {
        ESP_LOGE(TAG, "voice event queue unavailable");
        return false;
    }
    ui_voice_event_t event = {
        .kind = kind,
        .err = err,
    };
    strlcpy(event.transcript, transcript ? transcript : "", sizeof(event.transcript));
    strlcpy(event.detail, detail ? detail : "", sizeof(event.detail));
    if (xQueueSend(s_voice_event_queue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "voice event queue full, kind=%d", (int)kind);
        return false;
    }
    return true;
}

static bool start_voice_worker(TaskFunction_t task, const char *name)
{
    if (!task || atomic_exchange_explicit(&s_voice_worker_running, true, memory_order_acq_rel)) {
        return false;
    }
    // 语音工作任务只进行 WakeNet/I2S/HTTPS 编排；其栈可安全放在 PSRAM，LVGL 主任务仍在内部 SRAM。
    if (xTaskCreateWithCaps(task, name, UI_VOICE_TASK_STACK, NULL, UI_VOICE_TASK_PRIORITY,
                            NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_voice_worker_running, false, memory_order_release);
        ESP_LOGE(TAG, "create voice worker failed: free_internal=%u free_psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return false;
    }
    return true;
}

static void copy_utf8_for_agent(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!text) return;
    size_t used = 0;
    while (text[used] != '\0' && used < out_size - 1U) {
        unsigned char first = (unsigned char)text[used];
        size_t bytes = first < 0x80U ? 1U :
                       (first >= 0xC2U && first <= 0xDFU) ? 2U :
                       (first >= 0xE0U && first <= 0xEFU) ? 3U :
                       (first >= 0xF0U && first <= 0xF4U) ? 4U : 1U;
        if (used + bytes > out_size - 1U) break;
        for (size_t i = 1; i < bytes; i++) {
            if (((unsigned char)text[used + i] & 0xC0U) != 0x80U) {
                bytes = 1U;
                break;
            }
        }
        if (used + bytes > out_size - 1U) break;
        used += bytes;
    }
    memcpy(out, text, used);
    out[used] = '\0';
}

static void voice_start_task(void *arg)
{
    (void)arg;
    // manual_record_begin 会先阻止 WakeNet 自动恢复并等待其退出，再允许短录音接管 ES8311。
    esp_err_t err = fridge_voice_asr_manual_record_begin();
    if (err == ESP_OK) {
        err = fridge_audio_start_recording();
    }
    atomic_store_explicit(&s_voice_worker_running, false, memory_order_release);
    bool posted = post_voice_event(err == ESP_OK ? UI_VOICE_EVENT_RECORDING : UI_VOICE_EVENT_FAILED,
                                   err, NULL,
                                   err == ESP_OK ? "" : "音频正在被摄像头或其他会话使用");
    if (err != ESP_OK || !posted) {
        if (err == ESP_OK) {
            (void)fridge_audio_stop_recording();
        }
        fridge_voice_asr_manual_session_finish();
    }
    vTaskDeleteWithCaps(NULL);
}

static void voice_cancel_task(void *arg)
{
    (void)arg;
    fridge_audio_status_t audio = {0};
    esp_err_t err = fridge_audio_get_status(&audio);
    if (err == ESP_OK && audio.state == FRIDGE_AUDIO_STATE_RECORDING) {
        err = fridge_audio_stop_recording();
    }
    atomic_store_explicit(&s_voice_worker_running, false, memory_order_release);
    (void)post_voice_event(err == ESP_OK ? UI_VOICE_EVENT_CANCELLED : UI_VOICE_EVENT_FAILED,
                           err, NULL, err == ESP_OK ? "" : "停止录音失败");
    // 取消和停止失败都不能继续保留手动会话锁；UI 再次清理是幂等的。
    fridge_voice_asr_manual_session_finish();
    vTaskDeleteWithCaps(NULL);
}

static void voice_submit_task(void *arg)
{
    (void)arg;
    fridge_audio_status_t audio = {0};
    esp_err_t err = fridge_audio_get_status(&audio);
    if (err == ESP_OK && audio.state == FRIDGE_AUDIO_STATE_RECORDING) {
        err = fridge_audio_stop_recording();
    } else if (err == ESP_OK && audio.state != FRIDGE_AUDIO_STATE_READY) {
        err = ESP_ERR_INVALID_STATE;
    }

    fridge_asr_result_t asr = {0};
    if (err == ESP_OK) {
        err = fridge_voice_asr_transcribe_latest(&asr);
    }
    char input[FRIDGE_UI_P4_AGENT_TEXT_MAX] = {0};
    if (err == ESP_OK) {
        copy_utf8_for_agent(input, sizeof(input), asr.text);
        if (!input[0]) {
            err = ESP_ERR_INVALID_RESPONSE;
            strlcpy(asr.error, "ASR 转写为空，请重新录音", sizeof(asr.error));
        }
    }
    if (err == ESP_OK) {
        // Agent Runtime 的 submit_text 只入队；LLM、工具确认与板载 ES8311 TTS 仍在其自身任务处理。
        err = agent_chat_submit(NULL, input);
    }
    atomic_store_explicit(&s_voice_worker_running, false, memory_order_release);
    bool posted = post_voice_event(err == ESP_OK ? UI_VOICE_EVENT_AGENT_SUBMITTED : UI_VOICE_EVENT_FAILED,
                                   err, input,
                                   err == ESP_OK ? "" :
                                   (asr.error[0] ? asr.error : "语音识别或助手提交失败"));
    if (err != ESP_OK || !posted) {
        fridge_voice_asr_manual_session_finish();
    }
    vTaskDeleteWithCaps(NULL);
}

static void process_voice_events(void)
{
    if (!s_voice_event_queue) return;
    for (size_t i = 0; i < UI_VOICE_EVENT_QUEUE_LENGTH; i++) {
        ui_voice_event_t event = {0};
        if (xQueueReceive(s_voice_event_queue, &event, 0) != pdTRUE) break;
        switch (event.kind) {
        case UI_VOICE_EVENT_RECORDING:
            s_voice_phase = UI_VOICE_PHASE_RECORDING;
            refresh_voice_device();
            break;
        case UI_VOICE_EVENT_CANCELLED:
            s_voice_phase = UI_VOICE_PHASE_IDLE;
            finish_voice_manual_session();
            refresh_voice_device();
            fridge_ui_shell_show_toast("已取消本次录音");
            break;
        case UI_VOICE_EVENT_AGENT_SUBMITTED:
            s_voice_phase = UI_VOICE_PHASE_IDLE;
            // Agent Runtime 的最终 TTS 事件到达前，保持 WakeNet 停止，避免抢占板载音频链路。
            s_voice_waiting_agent_tts = true;
            refresh_voice_device();
            fridge_ui_shell_show_toast("已识别，正在交给助手处理");
            break;
        case UI_VOICE_EVENT_FAILED:
        default:
            show_voice_error(event.detail[0] ? event.detail : esp_err_to_name(event.err));
            break;
        }
    }
}

static void schedule_camera_preview_start(void)
{
    // 必须先提交一帧完整 INV-05，再由摄像头复制该帧作为两块显存的静态底图；
    // 否则相机抢到双缓冲时可能仍显示前一页的残影。
    s_camera_preview_start_pending = true;
    s_camera_preview_start_after_generation = s_lvgl_present_generation;
}

static void start_camera_preview_if_ready(void)
{
    if (!s_camera_preview_start_pending || s_camera_preview_mode ||
        atomic_load_explicit(&s_camera_capture_running, memory_order_acquire) ||
        atomic_load_explicit(&s_camera_ai_running, memory_order_acquire) ||
        s_lvgl_present_generation == s_camera_preview_start_after_generation) {
        return;
    }
    s_camera_preview_start_pending = false;
    esp_err_t ret = fridge_camera_start_hd_preview_with_layout(&s_camera_page_preview_layout);
    if (ret != ESP_OK) {
        fridge_ui_shell_show_toast("摄像头预览启动失败");
        ESP_LOGW(TAG, "start camera page preview failed: %s", esp_err_to_name(ret));
        return;
    }
    s_camera_preview_mode = true;
    ESP_LOGI(TAG, "camera page HD preview requested after shell frame %lu",
             (unsigned long)s_lvgl_present_generation);
}

static void camera_capture_task(void *arg)
{
    (void)arg;
    // 后台任务执行官方最高 1920x1080 RAW10→RGB565→硬件 JPEG，
    // 并等待自动曝光达到有效亮度，禁止阻塞唯一的 LVGL 任务。
    s_camera_capture_result = fridge_camera_capture_ai_fullres();
    atomic_store_explicit(&s_camera_capture_done, true, memory_order_release);
    // 抓拍任务使用 PSRAM 栈，退出时必须由 WithCaps 删除接口回收。
    vTaskDeleteWithCaps(NULL);
}

static bool start_camera_capture_task(void)
{
    if (atomic_load_explicit(&s_camera_capture_running, memory_order_acquire)) {
        return false;
    }
    atomic_store_explicit(&s_camera_capture_done, false, memory_order_release);
    atomic_store_explicit(&s_camera_capture_running, true, memory_order_release);
    // 抓拍任务只负责 V4L2、JPEG 与状态回传，不在 ISR 上下文运行；将其栈放到 PSRAM，
    // 避免 UI/Agent 运行后内部 SRAM 连续块不足 12KB 时无法创建任务。
    if (xTaskCreatePinnedToCoreWithCaps(camera_capture_task, "camera_page_capture",
                                        UI_CAMERA_CAPTURE_TASK_STACK, NULL,
                                        UI_CAMERA_CAPTURE_TASK_PRIORITY, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_camera_capture_running, false, memory_order_release);
        ESP_LOGE(TAG, "capture task create failed: stack=%u free_internal=%u largest_internal=%u free_psram=%u",
                 (unsigned)UI_CAMERA_CAPTURE_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }
    return true;
}

static void camera_pressed(void *user_ctx)
{
    (void)user_ctx;
    // 页面切换和相机启动分两步：本回调只进入队列，实际高清预览在完整页面首帧后异步启动。
    esp_err_t ret = fridge_ui_p4_request_page_async(FRIDGE_UI_P4_PAGE_CAMERA);
    if (ret != ESP_OK) {
        fridge_ui_shell_show_toast("拍照页面请求过多，请稍后重试");
        ESP_LOGW(TAG, "queue camera page failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "queued camera page with HD preview");
}

static void release_camera_review(void)
{
    fridge_ui_shell_set_camera_review(NULL, 0, 0, 0);
    fridge_camera_free_preview_frame(&s_camera_review_frame);
}

// 竖屏 Shell 的页面入口统一回到异步队列。若目标不是 HOME-02，UI 任务会先恢复
// 横屏 DIRECT 双缓冲再切页，避免 Shell 回调直接重绑显示缓冲。
static void shell_navigate_requested(void *user_ctx, fridge_ui_p4_page_t page)
{
    (void)user_ctx;
    esp_err_t ret = fridge_ui_p4_request_page_async(page);
    if (ret != ESP_OK) {
        fridge_ui_shell_show_toast(
            page == FRIDGE_UI_P4_PAGE_CAMERA ?
                "隐私模式已关闭摄像头" : "页面切换请求失败，请稍后重试");
    }
}

// 购物页动作在 UI 编排层同步完成：先以副本写入 LittleFS，成功后再让 Shell 刷新。
// 这里运行在唯一 LVGL 任务，Shell 因而不接触 Flash 文件和业务模型。
static esp_err_t shopping_action(void *user_ctx, fridge_ui_shell_shopping_action_t action,
                                 const fridge_ui_shell_shopping_request_t *request)
{
    (void)user_ctx;
    switch (action) {
    case FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PENDING:
    case FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PURCHASED: {
        if (!request) return ESP_ERR_INVALID_ARG;
        esp_err_t ret = fridge_ui_shopping_model_move_selected(
            &s_shopping, request->item_ids, request->item_id_count,
            action == FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PURCHASED);
        if (ret == ESP_OK) fridge_ui_shell_update_shopping(&s_shopping);
        return ret;
    }
    case FRIDGE_UI_SHELL_SHOPPING_ADD_MANUAL:
    case FRIDGE_UI_SHELL_SHOPPING_CLEAR_DONE: {
        fridge_ui_shopping_mutation_t mutation =
            action == FRIDGE_UI_SHELL_SHOPPING_ADD_MANUAL ?
                FRIDGE_UI_SHOPPING_MUTATE_ADD_MANUAL : FRIDGE_UI_SHOPPING_MUTATE_CLEAR_DONE;
        esp_err_t ret = fridge_ui_shopping_model_mutate(&s_shopping, mutation,
                                                         request ? request->item_id : NULL,
                                                         request ? request->name : NULL);
        if (ret == ESP_OK) fridge_ui_shell_update_shopping(&s_shopping);
        return ret;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void camera_reshoot(void *user_ctx)
{
    (void)user_ctx;
    if (atomic_load_explicit(&s_camera_capture_running, memory_order_acquire) ||
        atomic_load_explicit(&s_camera_ai_running, memory_order_acquire)) {
        fridge_ui_shell_show_toast("正在处理当前照片，请稍候");
        return;
    }
    release_camera_review();
    (void)fridge_camera_clear_frame();
    s_camera_ai_phase = UI_CAMERA_AI_PHASE_IDLE;
    strlcpy(s_camera_ai_detail, "正在重新打开摄像头", sizeof(s_camera_ai_detail));
    s_camera_resume_preview_after_capture = true;
    fridge_ui_shell_force_redraw();
    schedule_camera_preview_start();
    fridge_ui_shell_show_toast("正在重新打开摄像头…");
}

static void camera_ai_task(void *arg)
{
    ui_camera_ai_result_t *result = (ui_camera_ai_result_t *)arg;
    fridge_camera_frame_view_t frame = {0};
    result->err = fridge_camera_get_frame(&frame);
    if (result->err == ESP_OK) {
        fridge_ai_image_request_t request = {
            .jpeg = frame.data,
            .jpeg_len = frame.len,
            .width = frame.width,
            .height = frame.height,
        };
        strlcpy(request.task_type, "recognize_ingredients", sizeof(request.task_type));
        // worker 使用 PSRAM 栈，只允许调用不会读取 NVS 的预加载配置接口。
        result->err = fridge_ai_client_analyze_image_with_config(&result->config,
                                                                  &request,
                                                                  &result->ai);
    }
    atomic_store_explicit(&s_camera_ai_done, true, memory_order_release);
    vTaskDeleteWithCaps(NULL);
}

static void camera_upload(void *user_ctx)
{
    (void)user_ctx;
    if (atomic_load_explicit(&s_camera_ai_running, memory_order_acquire)) {
        fridge_ui_shell_show_toast("照片正在上传识别，请稍候");
        return;
    }
    fridge_camera_frame_view_t frame = {0};
    if (fridge_camera_get_frame(&frame) != ESP_OK) {
        s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
        strlcpy(s_camera_ai_detail, "没有可上传的照片，请先重拍",
                sizeof(s_camera_ai_detail));
        fridge_ui_shell_show_toast("没有可上传的照片，请先重拍");
        return;
    }
    (void)frame;

    ui_camera_ai_result_t *result =
        heap_caps_calloc(1, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) {
        s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
        strlcpy(s_camera_ai_detail, "识别任务内存不足", sizeof(s_camera_ai_detail));
        fridge_ui_shell_show_toast("识别任务内存不足");
        return;
    }
    // NVS/Flash 会在写读期间关闭 cache，必须在当前内部 SRAM 的 LVGL 任务完成配置读取。
    // API Key 只保存在设备内存中传给 worker，禁止写入日志或 UI。
    char preflight_error[FRIDGE_AI_MAX_ERROR_LEN + 1] = {0};
    esp_err_t preflight = fridge_ai_client_load_active_config(&result->config);
    if (preflight == ESP_OK) {
        preflight = fridge_ai_client_ensure_network_ready(preflight_error,
                                                          sizeof(preflight_error));
    }
    if (preflight != ESP_OK) {
        char toast[192];
        const char *reason =
            preflight_error[0] ? preflight_error : esp_err_to_name(preflight);
        snprintf(toast, sizeof(toast), "无法上传：%s", reason);
        s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
        strlcpy(s_camera_ai_detail, reason, sizeof(s_camera_ai_detail));
        memset(&result->config, 0, sizeof(result->config));
        free(result);
        fridge_ui_shell_show_toast(toast);
        return;
    }
    s_camera_ai_result = result;
    atomic_store_explicit(&s_camera_ai_done, false, memory_order_release);
    atomic_store_explicit(&s_camera_ai_running, true, memory_order_release);
    s_camera_ai_phase = UI_CAMERA_AI_PHASE_RECOGNIZING;
    strlcpy(s_camera_ai_detail, "正在上传照片并等待模型分析",
            sizeof(s_camera_ai_detail));
    if (xTaskCreatePinnedToCoreWithCaps(camera_ai_task, "camera_page_ai",
                                        UI_CAMERA_AI_TASK_STACK, result,
                                        UI_CAMERA_AI_TASK_PRIORITY, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store_explicit(&s_camera_ai_running, false, memory_order_release);
        s_camera_ai_result = NULL;
        memset(&result->config, 0, sizeof(result->config));
        free(result);
        s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
        strlcpy(s_camera_ai_detail, "识别任务启动失败", sizeof(s_camera_ai_detail));
        fridge_ui_shell_show_toast("识别任务启动失败");
        return;
    }
    fridge_ui_shell_show_toast("正在上传照片并识别…");
}

static void camera_exit(void *user_ctx)
{
    (void)user_ctx;
    s_camera_preview_start_pending = false;
    s_camera_resume_preview_after_capture = false;
    (void)fridge_camera_request_hd_preview_stop();
    release_camera_review();
    if (!atomic_load_explicit(&s_camera_capture_running, memory_order_acquire) &&
        !atomic_load_explicit(&s_camera_ai_running, memory_order_acquire)) {
        (void)fridge_camera_clear_frame();
    }
    (void)fridge_ui_shell_set_page(FRIDGE_UI_P4_PAGE_HOME);
    fridge_ui_shell_force_redraw();
}

static void camera_release(void *user_ctx)
{
    (void)user_ctx;
    s_camera_preview_start_pending = false;
    s_camera_resume_preview_after_capture = false;
    (void)fridge_camera_request_hd_preview_stop();
    // Shell 已在回调前解除图片源，此处可以直接释放 UI 持有的 PSRAM。
    fridge_camera_free_preview_frame(&s_camera_review_frame);
    if (!atomic_load_explicit(&s_camera_capture_running, memory_order_acquire) &&
        !atomic_load_explicit(&s_camera_ai_running, memory_order_acquire)) {
        (void)fridge_camera_clear_frame();
    }
}

static void voice_submitted(void *user_ctx);
static void voice_cancelled(void *user_ctx);

// 对话页内语音保持原有“录音后手动提交”语义，不能与右侧连续语音共用入口。
static void manual_voice_pressed(void *user_ctx)
{
    (void)user_ctx;
    // 摄像头持有帧缓冲/共享 I2C 时不能启动板载 ES8311 录音，避免 GPIO28/29 共享总线冲突。
    if (s_camera_preview_mode || fridge_camera_hd_preview_running()) {
        fridge_ui_shell_show_toast("请先退出摄像头预览，再启动语音");
        return;
    }
    fridge_voice_dialog_status_t dialog = {0};
    if (fridge_voice_asr_dialog_get_status(&dialog) == ESP_OK && dialog.active) {
        fridge_ui_shell_show_toast("请先结束右侧连续对话");
        return;
    }
    if (s_voice_waiting_agent_tts) {
        fridge_ui_shell_show_toast("上一条语音仍在回复，请稍候");
        return;
    }
    if (s_voice_phase == UI_VOICE_PHASE_RECORDING ||
        s_voice_phase == UI_VOICE_PHASE_RECORDING_READY) {
        // 状态舱主按钮在录音时与浮层“提交”保持同义，方便用户不必移动视线。
        voice_submitted(user_ctx);
        return;
    }
    if (s_voice_phase != UI_VOICE_PHASE_IDLE ||
        atomic_load_explicit(&s_voice_worker_running, memory_order_acquire)) {
        fridge_ui_shell_show_toast("语音正在处理，请稍候");
        return;
    }
    s_voice_phase = UI_VOICE_PHASE_STARTING;
    refresh_voice_device();
    if (!start_voice_worker(voice_start_task, "voice_record")) {
        show_voice_error("无法启动语音录音任务");
    }
}

// 右侧固定按钮只启动/结束连续对话；回调在 LVGL 任务执行，因此仅投递后台命令。
static void continuous_voice_pressed(void *user_ctx)
{
    (void)user_ctx;
    if (s_camera_preview_mode || fridge_camera_hd_preview_running()) {
        fridge_ui_shell_show_toast("请先退出摄像头预览，再启动语音");
        return;
    }
    if (s_voice_phase != UI_VOICE_PHASE_IDLE ||
        atomic_load_explicit(&s_voice_worker_running, memory_order_acquire)) {
        fridge_ui_shell_show_toast("对话页语音正在处理，请稍候");
        return;
    }
    fridge_voice_dialog_status_t dialog = {0};
    esp_err_t status_err = fridge_voice_asr_dialog_get_status(&dialog);
    esp_err_t err = status_err == ESP_OK && dialog.active ?
                        fridge_voice_asr_dialog_stop() : fridge_voice_asr_dialog_start();
    if (err != ESP_OK) {
        fridge_ui_shell_show_toast(status_err == ESP_OK && dialog.active ?
                                      "无法结束连续对话" : "连续语音暂不可用");
    }
}

static void voice_submitted(void *user_ctx)
{
    (void)user_ctx;
    if (s_voice_phase != UI_VOICE_PHASE_RECORDING &&
        s_voice_phase != UI_VOICE_PHASE_RECORDING_READY) {
        fridge_ui_shell_show_toast("当前没有可提交的录音");
        return;
    }
    if (atomic_load_explicit(&s_voice_worker_running, memory_order_acquire)) {
        fridge_ui_shell_show_toast("录音状态正在更新，请稍候");
        return;
    }
    s_voice_phase = UI_VOICE_PHASE_TRANSCRIBING;
    refresh_voice_device();
    if (!start_voice_worker(voice_submit_task, "voice_submit")) {
        // 录音仍然保留在音频缓冲中，回到可提交状态而不是擅自丢弃用户语音。
        s_voice_phase = UI_VOICE_PHASE_RECORDING_READY;
        refresh_voice_device();
        fridge_ui_shell_show_toast("无法启动识别任务，请重试提交");
    }
}

static void voice_cancelled(void *user_ctx)
{
    (void)user_ctx;
    if (s_voice_phase != UI_VOICE_PHASE_RECORDING &&
        s_voice_phase != UI_VOICE_PHASE_RECORDING_READY) {
        fridge_ui_shell_show_toast("当前语音不能取消");
        return;
    }
    if (atomic_load_explicit(&s_voice_worker_running, memory_order_acquire)) {
        fridge_ui_shell_show_toast("录音状态正在更新，请稍候");
        return;
    }
    s_voice_phase = UI_VOICE_PHASE_CANCELLING;
    refresh_voice_device();
    if (!start_voice_worker(voice_cancel_task, "voice_cancel")) {
        s_voice_phase = UI_VOICE_PHASE_RECORDING_READY;
        refresh_voice_device();
        fridge_ui_shell_show_toast("无法停止录音，请重试");
    }
}

// 所有厨房工具操作都从 Shell 回调进入当前 LVGL 任务；不在 UI 侧创建额外任务或软件定时器，
// 实际的倒计时、闹钟持久化和秒表状态仍由 kitchen_tools 组件统一维护。
static esp_err_t kitchen_action(void *user_ctx, fridge_ui_shell_kitchen_action_t action,
                                const fridge_ui_shell_kitchen_request_t *request)
{
    (void)user_ctx;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    uint8_t alarm_id = 0;

    switch (action) {
        case FRIDGE_UI_SHELL_KITCHEN_TIMER_START:
            ret = request ? fridge_kitchen_tools_timer_start_ex(
                                request->seconds,
                                request->label,
                                request->source,
                                request->timer_sound,
                                NULL) :
                            ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_TIMER_PAUSE:
            ret = (request && request->timer_id) ?
                      fridge_kitchen_tools_timer_pause_id(request->timer_id) :
                      fridge_kitchen_tools_timer_pause();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_TIMER_RESUME:
            ret = (request && request->timer_id) ?
                      fridge_kitchen_tools_timer_resume_id(request->timer_id) :
                      fridge_kitchen_tools_timer_resume();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_TIMER_CANCEL:
            ret = (request && request->timer_id) ?
                      fridge_kitchen_tools_timer_cancel_id(request->timer_id) :
                      fridge_kitchen_tools_timer_cancel();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_TIMER_DISMISS:
            ret = (request && request->timer_id) ?
                      fridge_kitchen_tools_timer_dismiss_id(request->timer_id) :
                      fridge_kitchen_tools_timer_dismiss();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_TIMER_ADD_SECONDS:
            ret = (request && request->timer_id) ?
                      fridge_kitchen_tools_timer_add_seconds(request->timer_id,
                                                             (int32_t)request->seconds) :
                      ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_START:
            ret = fridge_kitchen_tools_stopwatch_start();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_PAUSE:
            ret = fridge_kitchen_tools_stopwatch_pause();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_LAP:
            ret = fridge_kitchen_tools_stopwatch_lap();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_RESET:
            ret = fridge_kitchen_tools_stopwatch_reset();
            break;
        case FRIDGE_UI_SHELL_KITCHEN_ALARM_CREATE:
            ret = request ? fridge_kitchen_tools_alarm_create(request->alarm_config, &alarm_id) :
                            ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_ALARM_UPDATE:
            ret = request ? fridge_kitchen_tools_alarm_update(request->alarm_id,
                                                               request->alarm_config) :
                            ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_ALARM_SET_ENABLED:
            ret = request ? fridge_kitchen_tools_alarm_set_enabled(request->alarm_id,
                                                                    request->alarm_enabled) :
                            ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_ALARM_PREVIEW:
            ret = request ? fridge_kitchen_tools_alarm_preview(request->alarm_config) :
                            ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_ALARM_CANCEL:
            ret = request ? fridge_kitchen_tools_alarm_cancel(request->alarm_id) :
                            ESP_ERR_INVALID_ARG;
            break;
        case FRIDGE_UI_SHELL_KITCHEN_ALARM_DISMISS:
            ret = request ? fridge_kitchen_tools_alarm_dismiss(request->alarm_id) :
                            ESP_ERR_INVALID_ARG;
            break;
        default:
            break;
    }

    // 回调完成后立即回读快照，避免用户操作后必须等下一秒才看到运行/暂停或闹钟列表变化。
    if (fridge_kitchen_tools_get_snapshot(&s_kitchen_tools) == ESP_OK) {
        fridge_ui_shell_update_kitchen_tools(&s_kitchen_tools);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "kitchen action %d failed: %s", (int)action, esp_err_to_name(ret));
    } else if (action == FRIDGE_UI_SHELL_KITCHEN_ALARM_CREATE) {
        ESP_LOGI(TAG, "alarm created id=%u", (unsigned)alarm_id);
    }
    return ret;
}

static void leave_camera_preview_mode(bool show_exit_toast)
{
    s_camera_preview_mode = false;
    (void)fridge_display_mipi_release_from_camera();
    fridge_camera_status_t camera = {0};
    fridge_camera_get_status(&camera);
    if (camera.last_error[0]) {
        char message[192];
        snprintf(message, sizeof(message), "摄像头预览失败：%s", camera.last_error);
        fridge_ui_shell_show_toast(message);
    } else if (show_exit_toast) {
        fridge_ui_shell_show_toast("已退出摄像头预览");
    }
    fridge_ui_shell_force_redraw();
    fridge_ui_scroll_ppa_invalidate_cache();
    // 摄像头可能在最后一帧覆盖了两块显存；释放所有权后立刻完整重绘 Shell。
    if (s_display && fridge_display_mipi_begin_ui_render() == ESP_OK) {
        lv_refr_now(s_display);
        fridge_display_mipi_end_ui_render();
    }
    ESP_LOGI(TAG, "left camera HD preview mode; full shell redrawn");
}

static void handle_camera_preview_stopped(void)
{
    fridge_camera_preview_action_t action = fridge_camera_take_hd_preview_action();
    leave_camera_preview_mode(action == FRIDGE_CAMERA_PREVIEW_ACTION_NONE);
    if (action == FRIDGE_CAMERA_PREVIEW_ACTION_CAPTURE) {
        s_camera_resume_preview_after_capture = true;
        if (start_camera_capture_task()) {
            s_camera_ai_phase = UI_CAMERA_AI_PHASE_CAPTURING;
            strlcpy(s_camera_ai_detail, "正在生成高清照片",
                    sizeof(s_camera_ai_detail));
            fridge_ui_shell_show_toast("正在拍摄高清照片…");
        } else {
            s_camera_resume_preview_after_capture = false;
            s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
            strlcpy(s_camera_ai_detail, "拍摄任务启动失败",
                    sizeof(s_camera_ai_detail));
            fridge_ui_shell_show_toast("拍摄任务启动失败");
        }
    } else if (action == FRIDGE_CAMERA_PREVIEW_ACTION_CANCEL) {
        s_camera_resume_preview_after_capture = false;
        (void)fridge_ui_shell_set_page(FRIDGE_UI_P4_PAGE_HOME);
        fridge_ui_shell_force_redraw();
    }
}

static void handle_camera_capture_completed(void)
{
    if (!atomic_load_explicit(&s_camera_capture_done, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s_camera_capture_done, false, memory_order_release);
    atomic_store_explicit(&s_camera_capture_running, false, memory_order_release);

    if (s_camera_capture_result == ESP_OK && s_camera_resume_preview_after_capture) {
        fridge_camera_status_t camera = {0};
        fridge_camera_get_status(&camera);
        fridge_camera_preview_frame_t review = {0};
        esp_err_t review_ret = fridge_camera_take_captured_preview(&review);
        if (review_ret == ESP_OK) {
            release_camera_review();
            s_camera_review_frame = review;
            fridge_ui_shell_set_camera_review(review.rgb565, review.len,
                                              review.width, review.height);
            s_camera_ai_phase = UI_CAMERA_AI_PHASE_PHOTO_READY;
            strlcpy(s_camera_ai_detail, "点击上传开始识别",
                    sizeof(s_camera_ai_detail));
            char message[96];
            snprintf(message, sizeof(message), "高清照片已保存：%dx%d", camera.width, camera.height);
            fridge_ui_shell_show_toast(message);
            s_camera_resume_preview_after_capture = false;
        } else {
            fridge_ui_shell_show_toast("照片已保存，但回看图生成失败");
            fridge_ui_shell_force_redraw();
            schedule_camera_preview_start();
        }
    } else {
        if (s_camera_capture_result != ESP_OK) {
            fridge_camera_status_t camera = {0};
            fridge_camera_get_status(&camera);
            char message[192];
            snprintf(message, sizeof(message), "拍摄失败：%s",
                     camera.last_error[0] ? camera.last_error : esp_err_to_name(s_camera_capture_result));
            s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
            strlcpy(s_camera_ai_detail,
                    camera.last_error[0] ? camera.last_error :
                                           esp_err_to_name(s_camera_capture_result),
                    sizeof(s_camera_ai_detail));
            fridge_ui_shell_show_toast(message);
            if (s_camera_resume_preview_after_capture) {
                fridge_ui_shell_force_redraw();
                schedule_camera_preview_start();
            }
        } else {
            // 外部页面请求可能在抓拍任务完成前离开拍照页；此时不再展示回看，
            // 直接释放新帧，避免后台完成后长期占用 JPEG/PSRAM。
            (void)fridge_camera_clear_frame();
        }
    }
}

static const cJSON *camera_ai_get_alias(const cJSON *object,
                                        const char *const *keys,
                                        size_t key_count)
{
    if (!cJSON_IsObject(object)) return NULL;
    for (size_t i = 0; i < key_count; ++i) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, keys[i]);
        if (item) return item;
    }
    return NULL;
}

static bool camera_ai_parse_prefill(const char *reply,
                                    char name[48], char quantity[24], char note[80],
                                    char *parse_error, size_t parse_error_size)
{
    if (parse_error && parse_error_size) parse_error[0] = '\0';
    if (!reply || !reply[0]) {
        if (parse_error && parse_error_size) {
            strlcpy(parse_error, "模型返回了空内容", parse_error_size);
        }
        return false;
    }

    // OpenAI-compatible 服务不一定遵守 response_format：允许回复带 Markdown
    // 代码围栏或简短前缀，但仍必须从首个 JSON 对象开始解析。
    const char *json_start = strchr(reply, '{');
    if (!json_start) {
        if (parse_error && parse_error_size) {
            strlcpy(parse_error, "回复中没有 JSON 对象", parse_error_size);
        }
        return false;
    }
    cJSON *parsed = cJSON_Parse(json_start);
    if (!cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        if (parse_error && parse_error_size) {
            strlcpy(parse_error, "JSON 语法无效", parse_error_size);
        }
        return false;
    }

    // 少数兼容服务会再包一层 reply/content/result/data；只解一层，避免接受
    // 与食材识别无关的任意深层对象。
    static const char *const wrapper_keys[] = {"reply", "content", "result", "data"};
    const cJSON *root = parsed;
    cJSON *nested = NULL;
    const cJSON *wrapper =
        camera_ai_get_alias(parsed, wrapper_keys,
                            sizeof(wrapper_keys) / sizeof(wrapper_keys[0]));
    if (cJSON_IsObject(wrapper)) {
        root = wrapper;
    } else if (cJSON_IsString(wrapper) && wrapper->valuestring) {
        const char *nested_start = strchr(wrapper->valuestring, '{');
        if (nested_start) {
            nested = cJSON_Parse(nested_start);
            if (cJSON_IsObject(nested)) root = nested;
        }
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const bool schema_valid =
        !schema || (cJSON_IsNumber(schema) && schema->valueint == 1) ||
        (cJSON_IsString(schema) && schema->valuestring &&
         strcmp(schema->valuestring, "1") == 0);
    const bool type_valid =
        !type || (cJSON_IsString(type) && type->valuestring &&
                  strcmp(type->valuestring, "recognize_ingredients") == 0);
    if (!schema_valid || !type_valid) {
        if (parse_error && parse_error_size) {
            strlcpy(parse_error,
                    !schema_valid ? "schema_version 不是 1" :
                                    "识别任务类型不匹配",
                    parse_error_size);
        }
        cJSON_Delete(nested);
        cJSON_Delete(parsed);
        return false;
    }

    static const char *const candidate_list_keys[] = {
        "candidates", "items", "ingredients",
    };
    const cJSON *candidate_list =
        camera_ai_get_alias(root, candidate_list_keys,
                            sizeof(candidate_list_keys) /
                                sizeof(candidate_list_keys[0]));
    const cJSON *candidate = cJSON_IsArray(candidate_list) ?
                                 cJSON_GetArrayItem(candidate_list, 0) : NULL;
    static const char *const name_keys[] = {"name", "item", "ingredient", "food"};
    if (!candidate && camera_ai_get_alias(root, name_keys,
                                          sizeof(name_keys) / sizeof(name_keys[0]))) {
        candidate = root;
    }
    if (!cJSON_IsObject(candidate)) {
        if (parse_error && parse_error_size) {
            strlcpy(parse_error,
                    cJSON_IsArray(candidate_list) ?
                        "模型没有识别到食材" :
                        "缺少 candidates/items 食材数组",
                    parse_error_size);
        }
        cJSON_Delete(nested);
        cJSON_Delete(parsed);
        return false;
    }

    const cJSON *name_json =
        camera_ai_get_alias(candidate, name_keys,
                            sizeof(name_keys) / sizeof(name_keys[0]));
    static const char *const quantity_keys[] = {"quantity", "amount", "count"};
    static const char *const confidence_keys[] = {"confidence", "score"};
    static const char *const doubt_keys[] = {"doubt", "uncertainty", "note"};
    const cJSON *quantity_json =
        camera_ai_get_alias(candidate, quantity_keys,
                            sizeof(quantity_keys) / sizeof(quantity_keys[0]));
    const cJSON *confidence_json =
        camera_ai_get_alias(candidate, confidence_keys,
                            sizeof(confidence_keys) / sizeof(confidence_keys[0]));
    const cJSON *doubt_json =
        camera_ai_get_alias(candidate, doubt_keys,
                            sizeof(doubt_keys) / sizeof(doubt_keys[0]));
    if (!cJSON_IsString(name_json) || !name_json->valuestring ||
        !name_json->valuestring[0]) {
        if (parse_error && parse_error_size) {
            strlcpy(parse_error, "首个候选食材缺少名称", parse_error_size);
        }
        cJSON_Delete(nested);
        cJSON_Delete(parsed);
        return false;
    }

    strlcpy(name, name_json->valuestring, 48);
    if (cJSON_IsString(quantity_json) && quantity_json->valuestring &&
        quantity_json->valuestring[0]) {
        strlcpy(quantity, quantity_json->valuestring, 24);
    } else if (cJSON_IsNumber(quantity_json)) {
        snprintf(quantity, 24, "%g份", quantity_json->valuedouble);
    } else {
        strlcpy(quantity, "1份", 24);
    }

    double confidence = -1.0;
    if (cJSON_IsNumber(confidence_json)) {
        confidence = confidence_json->valuedouble;
    } else if (cJSON_IsString(confidence_json) && confidence_json->valuestring) {
        confidence = strtod(confidence_json->valuestring, NULL);
    }
    const char *doubt =
        cJSON_IsString(doubt_json) && doubt_json->valuestring ?
            doubt_json->valuestring : "";
    if (confidence >= 0.0) {
        int percent = (int)((confidence <= 1.0 ? confidence * 100.0 : confidence) + 0.5);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        snprintf(note, 80, "AI识别，置信度%d%%%s%s", percent,
                 doubt[0] ? "；" : "", doubt);
    } else {
        snprintf(note, 80, "AI识别%s%s", doubt[0] ? "；" : "", doubt);
    }

    // needs_confirmation 不再作为模型格式门槛：设备始终强制进入 INV-04，
    // 只有用户确认保存后才会写库存，模型无法绕过本地确认规则。
    cJSON_Delete(nested);
    cJSON_Delete(parsed);
    return true;
}

static void handle_camera_ai_completed(void)
{
    if (!atomic_load_explicit(&s_camera_ai_done, memory_order_acquire)) return;
    atomic_store_explicit(&s_camera_ai_done, false, memory_order_release);
    atomic_store_explicit(&s_camera_ai_running, false, memory_order_release);

    ui_camera_ai_result_t *result = s_camera_ai_result;
    s_camera_ai_result = NULL;
    if (!result) {
        s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
        strlcpy(s_camera_ai_detail, "识别结果丢失，请重试",
                sizeof(s_camera_ai_detail));
        fridge_ui_shell_show_toast("识别结果丢失，请重试");
        return;
    }

    char name[48] = {0};
    char quantity[24] = {0};
    char note[80] = {0};
    char parse_error[112] = {0};
    if (result->err == ESP_OK &&
        camera_ai_parse_prefill(result->ai.chat.reply, name, quantity, note,
                                parse_error, sizeof(parse_error))) {
        if (fridge_ui_shell_open_inventory_prefill(name, quantity, note) == ESP_OK) {
            s_camera_ai_phase = UI_CAMERA_AI_PHASE_SUCCEEDED;
            strlcpy(s_camera_ai_detail, "请在新增库存页核对后保存",
                    sizeof(s_camera_ai_detail));
            const fridge_ui_p4_page_t previous_page =
                (fridge_ui_p4_page_t)atomic_exchange_explicit(
                    &s_current_page, FRIDGE_UI_P4_PAGE_INVENTORY_EDIT,
                    memory_order_acq_rel);
            if (previous_page != FRIDGE_UI_P4_PAGE_INVENTORY_EDIT) {
                s_page_frame_pending = true;
                s_page_change_present_generation = s_lvgl_present_generation;
                atomic_store_explicit(&s_page_frame_ready, false, memory_order_release);
            }
            (void)fridge_camera_clear_frame();
            fridge_ui_shell_show_toast("识别完成，请核对后保存");
        } else {
            s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
            strlcpy(s_camera_ai_detail, "新增库存页打开失败",
                    sizeof(s_camera_ai_detail));
            fridge_ui_shell_show_toast("识别完成，但新增库存页打开失败");
        }
    } else {
        const char *error = result->ai.chat.error[0]
                                ? result->ai.chat.error
                                : (result->err == ESP_OK ?
                                       (parse_error[0] ? parse_error :
                                                         "模型返回的结构化内容无效")
                                                         : esp_err_to_name(result->err));
        s_camera_ai_phase = UI_CAMERA_AI_PHASE_FAILED;
        strlcpy(s_camera_ai_detail, error, sizeof(s_camera_ai_detail));
        ESP_LOGW(TAG, "camera AI failed: err=%s http=%d reply_len=%u reason=%s",
                 esp_err_to_name(result->err), result->ai.chat.http_status,
                 (unsigned)strlen(result->ai.chat.reply), error);
        char toast[192];
        snprintf(toast, sizeof(toast), "识别失败：%s", error);
        fridge_ui_shell_show_toast(toast);
    }
    // 配置含 API Key 明文，任务结束后先清零再释放 PSRAM。
    memset(&result->config, 0, sizeof(result->config));
    free(result);
}

static bool font_bindings_changed(const lv_font_t *bound[FRIDGE_FONT_ROLE_COUNT])
{
    bool changed = false;
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        const lv_font_t *current = fridge_font_manager_get((fridge_font_role_t)i);
        if (current != bound[i]) {
            bound[i] = current;
            changed = true;
        }
    }
    return changed;
}

static bool camera_page_boot_settled(void)
{
    return s_ui_ready_since_us != 0 &&
           esp_timer_get_time() - s_ui_ready_since_us >= UI_CAMERA_PAGE_BOOT_SETTLE_US;
}

static bool page_request_boot_settled(fridge_ui_p4_page_t page)
{
    const bool resources_synced =
        atomic_load_explicit(&s_resource_ready, memory_order_acquire) &&
        atomic_load_explicit(&s_framebuffers_synced, memory_order_acquire);
    if (!resources_synced) return false;
    return page != FRIDGE_UI_P4_PAGE_CAMERA || camera_page_boot_settled();
}

// 竖屏隐私模式移植 HOME-01/HOME-02、PLAN-01、CHAT-01～04、MORE-01/02/04/05/06/07/08；
// 其它页面继续使用横屏对象树，未移植页面由上层提示而不自动改成其它页面。
static bool page_supports_portrait(fridge_ui_p4_page_t page)
{
    return page == FRIDGE_UI_P4_PAGE_HOME || page == FRIDGE_UI_P4_PAGE_HOME_DETAIL ||
           page == FRIDGE_UI_P4_PAGE_PLAN ||
           page == FRIDGE_UI_P4_PAGE_MORE || page == FRIDGE_UI_P4_PAGE_TIMER ||
           page == FRIDGE_UI_P4_PAGE_ALARM || page == FRIDGE_UI_P4_PAGE_STOPWATCH ||
           page == FRIDGE_UI_P4_PAGE_CHAT ||
           page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
           page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
           page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS ||
           page == FRIDGE_UI_P4_PAGE_SAFETY ||
           page == FRIDGE_UI_P4_PAGE_SETTINGS ||
           page == FRIDGE_UI_P4_PAGE_MAINTENANCE;
}

// 按当前逻辑页面重新绑定 PPA 滚动缓存。
// 页面切换会销毁并重建竖屏滚动容器，旧对象不能继续作为缓存目标。
static esp_err_t bind_ppa_for_orientation(fridge_ui_p4_orientation_t orientation)
{
    const bool portrait = orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    lv_obj_t *scroll = portrait ? fridge_ui_shell_get_portrait_scroll() :
                                  fridge_ui_shell_get_main_scroll();
    esp_err_t ret = fridge_ui_scroll_ppa_bind(
        scroll,
        portrait ? FRIDGE_UI_SCROLL_PPA_PORTRAIT : FRIDGE_UI_SCROLL_PPA_LANDSCAPE,
        portrait ? present_portrait_logical_frame : NULL,
        NULL);
    if (ret == ESP_OK) {
        // bind 会清除上一页的 overlay；页面重建后重新挂当前工具的实时数字区域。
        fridge_ui_scroll_ppa_set_dynamic_overlay(
            fridge_ui_shell_get_ppa_dynamic_overlay());
    }
    return ret;
}

static esp_err_t apply_orientation_request(fridge_ui_p4_orientation_t orientation)
{
    const fridge_ui_p4_orientation_t current =
        (fridge_ui_p4_orientation_t)atomic_load_explicit(&s_current_orientation,
                                                          memory_order_acquire);
    if (orientation == current) return ESP_OK;
    if (!s_display) {
        return ESP_ERR_INVALID_STATE;
    }
    const fridge_ui_p4_page_t page =
        (fridge_ui_p4_page_t)atomic_load_explicit(&s_current_page, memory_order_acquire);
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        !page_supports_portrait(page)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fridge_display_mipi_camera_owns_framebuffers()) return ESP_ERR_INVALID_STATE;
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        esp_err_t init_ret = portrait_render_init();
        if (init_ret != ESP_OK) return init_ret;
    }

    // HOME 横屏 PPA 缓存直接读写 DPI 双缓冲，不能与独立竖屏逻辑缓冲并行。
    fridge_ui_scroll_ppa_request_finish();
    fridge_ui_scroll_ppa_process();
    fridge_ui_scroll_ppa_set_dynamic_overlay(NULL);
    fridge_ui_scroll_ppa_set_enabled(false);

    // 切换分辨率和 DIRECT 双缓冲前，先同步并清空 LVGL 上一套缓冲的脏区。
    // LVGL 会把 DIRECT 模式上一帧的同步区域留在内部链表中；若直接把
    // 1024px 横屏缓冲换成 600px 竖屏缓冲，下一次刷新会用旧坐标计算负宽度。
    lv_refr_now(s_display);

    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        lv_display_set_resolution(s_display, UI_PORTRAIT_WIDTH, UI_PORTRAIT_HEIGHT);
        lv_display_set_buffers(s_display, s_portrait_fb0, s_portrait_fb1,
                               UI_RGB565_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    } else {
        void *write_fb = NULL;
        void *visible_fb = NULL;
        esp_err_t ret = fridge_display_mipi_get_ui_frame_buffers(&write_fb, &visible_fb);
        if (ret != ESP_OK || !write_fb || !visible_fb || write_fb == visible_fb) {
            return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
        }
        lv_display_set_resolution(s_display, 1024, 600);
        lv_display_set_buffers(s_display, write_fb, visible_fb,
                               UI_RGB565_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    }

    esp_err_t ret = fridge_ui_shell_set_orientation(orientation);
    if (ret != ESP_OK) return ret;
    ret = bind_ppa_for_orientation(orientation);
    if (ret != ESP_OK) return ret;
    fridge_ui_scroll_ppa_set_enabled(true);
    atomic_store_explicit(&s_current_orientation, orientation, memory_order_release);
    s_page_frame_pending = true;
    s_page_change_present_generation = s_lvgl_present_generation;
    s_orientation_framebuffer_sync_pending = true;
    s_orientation_change_present_generation = s_lvgl_present_generation;
    atomic_store_explicit(&s_page_frame_ready, false, memory_order_release);
    atomic_store_explicit(&s_framebuffers_synced, false, memory_order_release);
    fridge_ui_shell_force_redraw();
    ESP_LOGI(TAG, "UI orientation applied: %s",
             fridge_ui_p4_orientation_key(orientation));
    return ESP_OK;
}

static void apply_page_request(const fridge_ui_request_t *request)
{
    if (!request || request->kind != FRIDGE_UI_REQUEST_PAGE) return;
    const fridge_ui_p4_page_t previous_page =
        (fridge_ui_p4_page_t)atomic_load_explicit(&s_current_page, memory_order_acquire);
    const fridge_ui_p4_orientation_t orientation =
        (fridge_ui_p4_orientation_t)atomic_load_explicit(&s_current_orientation,
                                                          memory_order_acquire);
    const bool unsupported_portrait_page =
        orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        !page_supports_portrait(request->page);
    if (unsupported_portrait_page) {
        if (request->page == FRIDGE_UI_P4_PAGE_CAMERA) {
            fridge_ui_shell_show_toast("隐私模式已关闭摄像头");
            return;
        }
        esp_err_t orientation_ret =
            apply_orientation_request(FRIDGE_UI_P4_ORIENTATION_LANDSCAPE);
        if (orientation_ret != ESP_OK) {
            ESP_LOGW(TAG, "leave portrait before page switch failed: %s",
                     esp_err_to_name(orientation_ret));
            return;
        }
    }
    if (request->page != FRIDGE_UI_P4_PAGE_CAMERA) {
        // 页面离开时只投递退出标记，资源仍由摄像头任务自己 close/munmap，
        // 不能在 LVGL 线程并发释放视频缓冲。
        s_camera_preview_start_pending = false;
        s_camera_resume_preview_after_capture = false;
        (void)fridge_camera_request_hd_preview_stop();
        release_camera_review();
        if (!atomic_load_explicit(&s_camera_capture_running, memory_order_acquire) &&
            !atomic_load_explicit(&s_camera_ai_running, memory_order_acquire)) {
            (void)fridge_camera_clear_frame();
        }
    }
    log_internal_dma_heap("page_switch_before", request->page);
    esp_err_t ret = fridge_ui_shell_set_page(request->page);
    log_internal_dma_heap("page_switch_after", request->page);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "async page request %s failed: %s",
                 fridge_ui_p4_page_key(request->page), esp_err_to_name(ret));
    } else {
        if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
            page_supports_portrait(request->page)) {
            // 竖屏页面切换会重建根节点，必须绑定新滚动容器；CHAT-01 打开 IME
            // 时由 Shell 暂停 PPA，关闭后恢复消息视口缓存。
            esp_err_t ppa_ret = bind_ppa_for_orientation(orientation);
            if (ppa_ret == ESP_OK) {
                fridge_ui_scroll_ppa_set_enabled(true);
            } else {
                ESP_LOGW(TAG, "rebind portrait PPA after page switch failed: %s",
                         esp_err_to_name(ppa_ret));
                // 新页面仍然保留，但回到横屏以避免竖屏滚动缓存失效后继续刷新旧对象。
                (void)apply_orientation_request(FRIDGE_UI_P4_ORIENTATION_LANDSCAPE);
            }
        }
        // 横屏二级 CHAT 键保持旧兼容别名；竖屏则保留真实页面键供 WebUI/status 使用。
        const fridge_ui_p4_page_t applied_page =
            orientation == FRIDGE_UI_P4_ORIENTATION_LANDSCAPE &&
            (request->page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
             request->page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
             request->page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS) ?
                FRIDGE_UI_P4_PAGE_CHAT : request->page;
        atomic_store_explicit(&s_current_page, applied_page, memory_order_release);
        if (previous_page != applied_page) {
            // 页面对象已经在 LVGL 任务中创建，但必须等一次成功 flush 后才允许
            // 外部截屏读取可见缓冲，避免读到页面切换前的旧帧。
            s_page_frame_pending = true;
            s_page_change_present_generation = s_lvgl_present_generation;
            atomic_store_explicit(&s_page_frame_ready, false, memory_order_release);
        } else {
            atomic_store_explicit(&s_page_frame_ready, true, memory_order_release);
        }
        if (request->page == FRIDGE_UI_P4_PAGE_CAMERA &&
            !atomic_load_explicit(&s_camera_capture_running, memory_order_acquire) &&
            !atomic_load_explicit(&s_camera_ai_running, memory_order_acquire)) {
            s_camera_resume_preview_after_capture = true;
            schedule_camera_preview_start();
        }
        if (unsupported_portrait_page) {
            // 未移植页面不自动改成另一页；先回到横屏，再明确告诉用户当前页面仍未适配竖屏。
            fridge_ui_shell_show_toast("该页面尚未移植竖屏，已切回横屏");
        }
    }
}

static void process_page_requests(void)
{
    if (!s_page_request_queue) return;

    fridge_ui_request_t request = {0};
    while (xQueueReceive(s_page_request_queue, &request, 0) == pdTRUE) {
        if (request.kind == FRIDGE_UI_REQUEST_ORIENTATION) {
            esp_err_t ret = apply_orientation_request(request.orientation);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "async orientation request %s failed: %s",
                         fridge_ui_p4_orientation_key(request.orientation),
                         esp_err_to_name(ret));
            }
            continue;
        }
        if (!page_request_boot_settled(request.page)) {
            // USB Tool 与触摸入口都复用该队列；资源同步期间只保留最后一次目标页请求。
            s_page_request_deferred = true;
            s_deferred_page_request = request.page;
            ESP_LOGI(TAG, "defer page %s until UI resource sync settles",
                     fridge_ui_p4_page_key(request.page));
            continue;
        }
        // 资源已稳定时新请求直接生效，并覆盖启动阶段尚未执行的旧目标页。
        s_page_request_deferred = false;
        apply_page_request(&request);
    }

    if (s_page_request_deferred && page_request_boot_settled(s_deferred_page_request)) {
        const fridge_ui_p4_page_t page = s_deferred_page_request;
        s_page_request_deferred = false;
        const fridge_ui_request_t deferred_request = {
            .kind = FRIDGE_UI_REQUEST_PAGE,
            .page = page,
        };
        ESP_LOGI(TAG, "apply deferred page %s after UI boot settle",
                 fridge_ui_p4_page_key(page));
        apply_page_request(&deferred_request);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    atomic_store_explicit(&s_ready, false, memory_order_release);
    atomic_store_explicit(&s_resource_ready, false, memory_order_release);
    atomic_store_explicit(&s_framebuffers_synced, false, memory_order_release);
    atomic_store_explicit(&s_page_frame_ready, false, memory_order_release);
    atomic_store_explicit(&s_current_page, FRIDGE_UI_P4_PAGE_HOME, memory_order_release);
    memset(&s_binding_model, 0, sizeof(s_binding_model));
    atomic_store_explicit(&s_binding_dirty, true, memory_order_release);
    atomic_store_explicit(&s_current_orientation, FRIDGE_UI_P4_ORIENTATION_LANDSCAPE,
                          memory_order_release);
    s_page_frame_pending = false;
    s_page_change_present_generation = 0;
    s_orientation_framebuffer_sync_pending = false;
    s_orientation_change_present_generation = 0;
    ESP_LOGI(TAG, "ui_task start");
    lv_init();
    void *fb0 = NULL;
    void *fb1 = NULL;
    if (fridge_display_mipi_get_frame_buffers(&fb0, &fb1) != ESP_OK) {
        ESP_LOGE(TAG, "MIPI frame buffers unavailable");
        atomic_store_explicit(&s_ready, false, memory_order_release);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    // 关键：先刷一帧纯色 splash，确认 DPI present 通路，再做任何 TF 文件访问。
    paint_boot_splash(fb0, fb1);
    vTaskDelay(pdMS_TO_TICKS(20));
    // 面板已完成黑场首帧并恢复背光后才应用用户亮度，避免上电时在可见画面上跳变。
    load_saved_brightness_preference();

    s_display = lv_display_create(1024, 600);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, display_flush_cb);
    // splash 后 fb0 正被 DPI 扫描，先把 fb1 交给 LVGL 绘制。DIRECT 只重画脏区，
    // LVGL 会在双缓冲之间自动同步这些区域，避免任意小变化都重画 1024x600 整屏。
    lv_display_set_buffers(s_display, fb1, fb0,
                           UI_RGB565_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_DIRECT);
    // 10ms 与 100Hz FreeRTOS tick 对齐，移除 30fps 软件上限；实际帧率由约 56.8Hz 面板和绘制耗时决定。
    lv_timer_set_period(lv_display_get_refr_timer(s_display), UI_REFR_PERIOD_MS);
    ESP_LOGI(TAG, "LVGL direct double buffer, refresh request=%dms (panel-limited)",
             UI_REFR_PERIOD_MS);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    ESP_LOGI(TAG, "font manager fast init...");
    (void)fridge_font_manager_init();
    // 闹钟名称与播报文字复用本地全拼输入法；词库未挂载时仍保留内置候选，TF 就绪后会自动补全。
    (void)fridge_ime_init();
    fridge_ui_theme_init();
    fridge_ui_assets_init(&s_assets);
    fridge_ui_home_model_set_fallback(&s_home);
    fridge_ui_more_model_set_fallback(&s_more);
    fridge_ui_plan_model_set_fallback(&s_plan);
    fridge_ui_shopping_model_set_fallback(&s_shopping);
    esp_err_t shopping_load_err = fridge_ui_shopping_model_load(&s_shopping);
    if (shopping_load_err != ESP_OK) {
        ESP_LOGW(TAG, "shopping list load failed, use in-memory seed: %s",
                 esp_err_to_name(shopping_load_err));
    }
    fridge_ui_shell_actions_t actions = {
        .camera_pressed = camera_pressed,
        .camera_reshoot = camera_reshoot,
        .camera_upload = camera_upload,
        .camera_exit = camera_exit,
        .camera_release = camera_release,
        .continuous_voice_pressed = continuous_voice_pressed,
        .manual_voice_pressed = manual_voice_pressed,
        .voice_submitted = voice_submitted,
        .voice_cancelled = voice_cancelled,
        .navigate_requested = shell_navigate_requested,
        .chat_submit = agent_chat_submit,
        .chat_cancel = agent_chat_cancel,
        .chat_clear_session = agent_chat_clear_session,
        .chat_resolve_confirmation = agent_chat_resolve_confirmation,
        .kitchen_action = kitchen_action,
        .shopping_action = shopping_action,
        .settings_action = settings_action,
    };
    ESP_LOGI(TAG, "create ver2 HOME-01 shell...");
    if (fridge_ui_shell_create(lv_screen_active(), &s_home, &actions) != ESP_OK) {
        ESP_LOGE(TAG, "ver2 shell create failed");
        atomic_store_explicit(&s_ready, false, memory_order_release);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    fridge_ui_shell_update_shopping(&s_shopping);
    esp_err_t ppa_ret = fridge_ui_scroll_ppa_init(s_display,
                                                   fridge_ui_shell_get_main_scroll());
    if (ppa_ret != ESP_OK) {
        // PPA 是性能增强而不是启动依赖；初始化失败时保留原 LVGL 完整功能。
        ESP_LOGW(TAG, "PPA scroll accelerator unavailable, LVGL fallback: %s",
                 esp_err_to_name(ppa_ret));
    }
    fridge_ui_shell_set_resource_state(false, "等待 TF 资源");
    collect_device_status(&s_device);
    fridge_ui_shell_update_device(&s_device);
    collect_safety_status(&s_safety);
    fridge_ui_shell_update_safety(&s_safety);
    refresh_settings_runtime_state();
    fridge_ui_shell_update_settings(&s_settings_state);
    if (fridge_kitchen_tools_get_snapshot(&s_kitchen_tools) == ESP_OK) {
        fridge_ui_shell_update_kitchen_tools(&s_kitchen_tools);
    }

    const esp_timer_create_args_t timer_args = {
        .callback = tick_cb,
        .name = "lvgl_p4_tick",
    };
    if (esp_timer_create(&timer_args, &s_tick_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_tick_timer, UI_TICK_MS * 1000);
    }

    // 强制立刻出 Flash 降级首帧，再在同一 UI 任务中延后绑定 TF 字体、图标和示例数据。
    if (fridge_display_mipi_begin_ui_render() == ESP_OK) {
        lv_timer_handler();
        lv_refr_now(s_display);
        fridge_ui_scroll_ppa_process();
        fridge_display_mipi_end_ui_render();
    }
    atomic_store_explicit(&s_page_frame_ready, true, memory_order_release);
    atomic_store_explicit(&s_ready, true, memory_order_release);
    s_ui_ready_since_us = esp_timer_get_time();
    ESP_LOGI(TAG, "P4 ver2 HOME-01 ready 1024x600 (flash fallback), free_psram=%luKB stack_hwm=%lu",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));

    const lv_font_t *bound_fonts[FRIDGE_FONT_ROLE_COUNT];
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        bound_fonts[i] = fridge_font_manager_get((fridge_font_role_t)i);
    }
    fridge_ime_snapshot_t ime_snapshot = {0};
    fridge_ime_get_snapshot(&ime_snapshot);
    bool ime_dictionary_loaded = ime_snapshot.dictionary_loaded;
    bool freetype_ready = false;
    int ime_attempts = 0;
    int font_attempts = 0;
    int asset_attempts = 0;
    int64_t last_ime_try_us = 0;
    int64_t last_font_try_us = 0;
    int64_t last_asset_try_us = 0;
    int64_t last_device_refresh_us = 0;
    int64_t last_kitchen_refresh_us = 0;
    bool resource_framebuffers_synced = false;
    bool resource_framebuffer_sync_pending = false;
    uint32_t resource_redraw_generation = 0;
    int64_t wake_ui_stable_since_us = 0;

    while (true) {
        // 所有页面对象更新都在当前 LVGL 任务执行，外部任务只负责投递请求。
        process_page_requests();
        process_voice_events();
        process_settings_events();
        process_agent_events();
        handle_camera_capture_completed();
        handle_camera_ai_completed();
        int64_t now_us = esp_timer_get_time();
        fridge_ui_shell_tick(now_us);
        if (atomic_exchange_explicit(&s_binding_dirty, false, memory_order_acq_rel)) {
            fridge_ui_p4_binding_model_t binding = {0};
            portENTER_CRITICAL(&s_binding_lock);
            binding = s_binding_model;
            portEXIT_CRITICAL(&s_binding_lock);
            fridge_ui_shell_update_binding(&binding);
        }
        if (s_camera_preview_mode && !fridge_camera_hd_preview_running()) {
            handle_camera_preview_stopped();
        }

        bool scroll_active = fridge_ui_scroll_ppa_is_active();
        if (!scroll_active && !s_assets.loaded) {
            int64_t retry_us = asset_attempts < 30 ? 500000LL : 5000000LL;
            if (now_us - last_asset_try_us >= retry_us) {
                last_asset_try_us = now_us;
                asset_attempts++;
                fridge_tfcard_status_t tfcard = {0};
                fridge_tfcard_get_status(&tfcard);
                if (tfcard.mounted) {
                    esp_err_t ret = fridge_ui_assets_try_load(&s_assets, &s_home, &s_more, &s_plan);
                    if (ret == ESP_OK) {
                        fridge_ui_shell_bind_assets(&s_assets);
                        fridge_ui_shell_update_home(&s_home);
                        fridge_ui_shell_update_more(&s_more);
                        fridge_ui_shell_update_plan(&s_plan);
                        fridge_ui_shell_update_shopping(&s_shopping);
                        fridge_ui_scroll_ppa_invalidate_cache();
                    } else {
                        // TF 挂载和首帧并行，资源包的早期一次读取失败可由后续重试恢复。
                        // 在确认长期失败前保持“加载中”，避免把正常启动过程误报成资源降级。
                        if (asset_attempts == 30) {
                            fridge_ui_shell_set_resource_state(false, s_assets.last_error);
                            ESP_LOGW(TAG, "ver2 asset pack unavailable; slow retry: %s",
                                     s_assets.last_error);
                        }
                    }
                } else if (asset_attempts == 30) {
                    fridge_ui_shell_set_resource_state(false,
                                                       tfcard.last_error[0] ? tfcard.last_error :
                                                       "TF 卡未挂载");
                }
            }
        }

        if (!scroll_active && !ime_dictionary_loaded) {
            // UI 早于 TF 卡启动：前 30 次每 500ms 尝试，之后每 2s 慢速重试。
            // 加载和候选刷新都在 LVGL 任务执行，避免与触屏按键并发改写 6.5 万词索引。
            int64_t retry_us = ime_attempts < 30 ? 500000LL : 2000000LL;
            if (now_us - last_ime_try_us >= retry_us) {
                last_ime_try_us = now_us;
                ime_attempts++;
                esp_err_t ime_ret = fridge_ime_try_load_tf_dictionary();
                if (ime_ret == ESP_OK) {
                    ime_dictionary_loaded = true;
                    fridge_ui_shell_refresh_ime();
                    fridge_ui_shell_force_redraw();
                    fridge_ui_scroll_ppa_invalidate_cache();
                    ESP_LOGI(TAG, "TF full IME dictionary rebound to active editors");
                } else if (ime_attempts == 30) {
                    ESP_LOGW(TAG, "TF IME dictionary still unavailable; slow retry: %s",
                             esp_err_to_name(ime_ret));
                }
            }
        }

        if (!scroll_active && !freetype_ready) {
            int64_t retry_us = font_attempts < 30 ? 500000LL : 2000000LL;
            if (now_us - last_font_try_us >= retry_us) {
                last_font_try_us = now_us;
                font_attempts++;
                esp_err_t pack_ret = fridge_font_manager_try_load_tf_pack();
                esp_err_t ft_ret = fridge_font_manager_load_freetype();
                if ((pack_ret == ESP_OK || ft_ret == ESP_OK) &&
                    font_bindings_changed(bound_fonts)) {
                    fridge_ui_theme_refresh_fonts();
                    fridge_ui_shell_force_redraw();
                    fridge_ui_scroll_ppa_invalidate_cache();
                    ESP_LOGI(TAG, "TF fonts rebound through global ver2 styles");
                }
                if (ft_ret == ESP_OK) {
                    freetype_ready = true;
                } else if (font_attempts == 30) {
                    ESP_LOGW(TAG, "TF/FreeType still unavailable; slow retry: pack=%s ft=%s",
                             esp_err_to_name(pack_ret), esp_err_to_name(ft_ret));
                }
            }
        }

        // 资源状态门必须同时满足 TF 页面包和 FreeType；仅 manifest/icon 加载完成时
        // 页面仍可能保留 Flash fallback，不能让截屏脚本提前读取。
        if (s_assets.loaded && freetype_ready) {
            atomic_store_explicit(&s_resource_ready, true, memory_order_release);
            // 图标和 FreeType 字体都完成绑定后，才允许页面从“加载中”转为资源就绪。
            fridge_ui_shell_set_resource_state(true, s_assets.package_version);
        }

        if (!scroll_active && s_assets.loaded && freetype_ready &&
            !resource_framebuffers_synced && !resource_framebuffer_sync_pending) {
            // PPA 只覆盖 704px 主滚动视口；顶部状态栏、中间导航和右侧状态舱仍由
            // DIRECT 双缓冲保存。TF 图标/字体异步绑定后先请求一次完整 Shell 重绘，
            // 记录当前提交代数；只有后续确实完成一帧 LVGL 提交，才能同步另一块缓冲。
            // 否则刷新定时器尚未到期时会把旧占位帧复制过去，并被 PPA 再次翻回屏幕。
            fridge_ui_shell_force_redraw();
            resource_redraw_generation = s_lvgl_present_generation;
            resource_framebuffer_sync_pending = true;
        }

        if (!scroll_active && now_us - last_device_refresh_us >= UI_DEVICE_REFRESH_US) {
            last_device_refresh_us = now_us;
            // WakeNet/AFE 只能在完整 UI 稳定门之后启动。页面请求、首帧或双缓冲任一
            // 未完成就重置静置计时；不再使用 30 秒兜底强拉模型，UI/TF 可用性优先。
            const bool wake_ui_stable =
                resource_framebuffers_synced &&
                !resource_framebuffer_sync_pending &&
                !s_page_request_deferred &&
                atomic_load_explicit(&s_page_frame_ready, memory_order_acquire);
            if (!wake_ui_stable) {
                wake_ui_stable_since_us = 0;
            } else if (wake_ui_stable_since_us == 0) {
                wake_ui_stable_since_us = now_us;
            }
            const bool wake_start_allowed =
                wake_ui_stable_since_us > 0 &&
                now_us - wake_ui_stable_since_us >= UI_WAKE_AFTER_STABLE_US;
            fridge_wake_word_policy_update(
                wake_start_allowed && fridge_display_mipi_is_ready(),
                wake_start_allowed && fridge_display_mipi_is_backlight_enabled());
            collect_device_status(&s_device);
            fridge_ui_shell_update_device(&s_device);
            collect_safety_status(&s_safety);
            fridge_ui_shell_update_safety(&s_safety);
            refresh_settings_runtime_state();
            fridge_ui_shell_update_settings(&s_settings_state);
        }

        // kitchen_tools 使用单调时钟折算倒计时/秒表，UI 每秒读取一次只刷新变化文本。
        if (!scroll_active && now_us - last_kitchen_refresh_us >= UI_KITCHEN_REFRESH_US) {
            last_kitchen_refresh_us = now_us;
            if (fridge_kitchen_tools_get_snapshot(&s_kitchen_tools) == ESP_OK) {
                fridge_ui_shell_update_kitchen_tools(&s_kitchen_tools);
            }
        }

        // 从 LVGL 渲染开始就与摄像头互斥；仅跳过 flush 太晚，renderer 已经写过帧缓冲。
        uint32_t wait_ms = 5;
        if (!s_camera_preview_mode && fridge_display_mipi_begin_ui_render() == ESP_OK) {
            s_render_cycle_start_us = esp_timer_get_time();
            wait_ms = lv_timer_handler();
            s_render_cycle_start_us = 0;
            if (s_orientation_framebuffer_sync_pending &&
                s_lvgl_present_generation != s_orientation_change_present_generation) {
                void *write_fb = NULL;
                void *visible_fb = NULL;
                if (fridge_display_mipi_get_ui_frame_buffers(&write_fb, &visible_fb) == ESP_OK &&
                    write_fb && visible_fb && write_fb != visible_fb) {
                    memcpy(write_fb, visible_fb, UI_RGB565_FRAME_BYTES);
                    s_orientation_framebuffer_sync_pending = false;
                    atomic_store_explicit(&s_framebuffers_synced, true, memory_order_release);
                    ESP_LOGI(TAG, "orientation framebuffers synchronized");
                }
            }
            if (resource_framebuffer_sync_pending &&
                s_lvgl_present_generation != resource_redraw_generation) {
                void *write_fb = NULL;
                void *visible_fb = NULL;
                if (fridge_display_mipi_get_ui_frame_buffers(&write_fb, &visible_fb) == ESP_OK &&
                    write_fb && visible_fb) {
                    // 屏幕为 RGB565；显式使用 16-bit 帧大小，不能依赖 lv_color_t 的实现尺寸。
                    memcpy(write_fb, visible_fb, UI_RGB565_FRAME_BYTES);
                    resource_framebuffer_sync_pending = false;
                    resource_framebuffers_synced = true;
                    atomic_store_explicit(&s_framebuffers_synced, true, memory_order_release);
                    fridge_ui_scroll_ppa_invalidate_cache();
                    ESP_LOGI(TAG, "TF resource framebuffers synchronized");
                }
            }
            // 资源整页重绘尚未实际提交时，禁止 PPA 缓存重建。重建过程会临时滚动对象并
            // 清理 invalid 队列，若抢在资源帧前执行，会把刚排队的顶栏/导航/状态舱重绘丢掉。
            // 同步完成后下一轮立即恢复 PPA；手势期间仍维持原来的加速热路径。
            if (!resource_framebuffer_sync_pending) {
                fridge_ui_scroll_ppa_process();
            }
            if (s_page_frame_pending &&
                s_lvgl_present_generation != s_page_change_present_generation) {
                s_page_frame_pending = false;
                atomic_store_explicit(&s_page_frame_ready, true, memory_order_release);
            }
            fridge_display_mipi_end_ui_render();
        }
        // 仅在完整 INV-05 首帧已经提交且 UI 不再持有渲染锁后，才允许摄像头接管双缓冲。
        start_camera_preview_if_ready();
        uint32_t delay_ms = wait_ms > 5 ? 5 : (wait_ms == 0 ? 1 : wait_ms);
        TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        // CONFIG_FREERTOS_HZ=100 时 1~5ms 会截断为 0；至少阻塞 1 tick，避免饿死 IDLE0。
        vTaskDelay(delay_ticks > 0 ? delay_ticks : 1);
    }
}

esp_err_t fridge_ui_p4_init(void)
{
    // main 会在重型 TF/Agent 初始化前后各调用一次。任务句柄从 xTaskCreate 返回时
    // 已经有效，不能只等待较晚才置位的 s_ready，否则会并发创建两个 LVGL 任务。
    if (atomic_load_explicit(&s_ready, memory_order_acquire) || s_task) return ESP_OK;
    if (!fridge_display_mipi_is_ready()) return ESP_ERR_INVALID_STATE;

    // 小容量页面请求队列在 UI 任务启动前创建，允许 USB ready 与 LVGL 首帧之间的请求排队。
    if (!s_page_request_queue) {
        s_page_request_queue = xQueueCreate(UI_PAGE_REQUEST_QUEUE_LENGTH,
                                            sizeof(fridge_ui_request_t));
        if (!s_page_request_queue) return ESP_ERR_NO_MEM;
    }
    // 后台语音任务只向此小队列投递结果；UI 任务统一更新 LVGL，避免并发触碰对象树。
    if (!s_voice_event_queue) {
        s_voice_event_queue = xQueueCreate(UI_VOICE_EVENT_QUEUE_LENGTH, sizeof(ui_voice_event_t));
        if (!s_voice_event_queue) return ESP_ERR_NO_MEM;
    }
    // 设置工作队列与结果队列在 LVGL 启动前建立。Wi-Fi/NVS/WakeNet 任务只写结果事件，
    // 不持有任何 LVGL 对象，避免扫描或 SNTP 超时拖慢触摸和屏幕刷新。
    if (!s_settings_job_queue) {
        s_settings_job_queue = xQueueCreate(UI_SETTINGS_JOB_QUEUE_LENGTH,
                                            sizeof(ui_settings_job_t));
        if (!s_settings_job_queue) return ESP_ERR_NO_MEM;
    }
    if (!s_settings_event_queue) {
        s_settings_event_queue = xQueueCreate(UI_SETTINGS_EVENT_QUEUE_LENGTH,
                                              sizeof(ui_settings_event_t));
        if (!s_settings_event_queue) return ESP_ERR_NO_MEM;
    }
    if (!s_settings_task) {
        // NVS 擦写期间 Flash cache 可能短暂关闭，任务栈必须保持在内部 RAM，不能放 PSRAM。
        if (xTaskCreate(settings_worker_task, "settings_worker", UI_SETTINGS_JOB_TASK_STACK,
                        NULL, UI_SETTINGS_JOB_TASK_PRIORITY, &s_settings_task) != pdPASS) {
            s_settings_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    // UI 会与 LittleFS/NVS 状态读取并发；Flash Cache 关闭时 P4 不允许任一 CPU
    // 正运行在 PSRAM 栈，因此常驻 LVGL 栈必须保留在内部 RAM。
    if (xTaskCreate(ui_task, "ui_p4", UI_TASK_STACK, NULL, UI_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_page_request_queue);
        s_page_request_queue = NULL;
        vQueueDelete(s_voice_event_queue);
        s_voice_event_queue = NULL;
        if (s_settings_task) {
            vTaskDelete(s_settings_task);
            s_settings_task = NULL;
        }
        vQueueDelete(s_settings_job_queue);
        s_settings_job_queue = NULL;
        vQueueDelete(s_settings_event_queue);
        s_settings_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool fridge_ui_p4_is_ready(void)
{
    return atomic_load_explicit(&s_ready, memory_order_acquire);
}

esp_err_t fridge_ui_p4_get_status(fridge_ui_p4_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    const unsigned page = atomic_load_explicit(&s_current_page, memory_order_acquire);
    const unsigned orientation =
        atomic_load_explicit(&s_current_orientation, memory_order_acquire);
    out->ready = atomic_load_explicit(&s_ready, memory_order_acquire);
    out->resource_ready = atomic_load_explicit(&s_resource_ready, memory_order_acquire);
    out->framebuffers_synced = atomic_load_explicit(&s_framebuffers_synced, memory_order_acquire);
    out->page_frame_ready = atomic_load_explicit(&s_page_frame_ready, memory_order_acquire);
    out->page = page < FRIDGE_UI_P4_PAGE_COUNT ? (fridge_ui_p4_page_t)page : FRIDGE_UI_P4_PAGE_HOME;
    out->orientation = orientation < FRIDGE_UI_P4_ORIENTATION_COUNT ?
                           (fridge_ui_p4_orientation_t)orientation :
                           FRIDGE_UI_P4_ORIENTATION_LANDSCAPE;
    out->privacy_mode =
        out->orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    return ESP_OK;
}

esp_err_t fridge_ui_p4_register_agent_actions(const fridge_ui_p4_agent_actions_t *actions)
{
    if (!actions || !actions->submit_text || !actions->cancel_active || !actions->clear_session ||
        !actions->resolve_confirmation || !actions->poll_event) {
        return ESP_ERR_INVALID_ARG;
    }

    // 仅保存函数指针和调用上下文，不调用 Runtime，也不访问 LVGL；因此可由 Runtime
    // 初始化任务或 main 的编排路径安全调用。后续注册采用“最新动作表”语义，便于重启。
    portENTER_CRITICAL(&s_agent_actions_lock);
    s_agent_actions = *actions;
    s_agent_actions_registered = true;
    portEXIT_CRITICAL(&s_agent_actions_lock);
    ESP_LOGI(TAG, "Agent UI actions registered");
    return ESP_OK;
}
