// ver2 横屏 Shell 实现。
// 固定几何来自参考稿：704px 主区 + 72px 导航 + 248px 状态舱，HOME 仅主内容区纵向滚动。

#include "fridge_ui_shell.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_diagnostics.h"
#include "fridge_ime.h"
#include "fridge_tfcard.h"
#include "fridge_ui_inventory_model.h"
#include "fridge_ui_scroll_ppa.h"
#include "fridge_ui_theme.h"

#define UI_MAIN_W 704
#define UI_NAV_W 72
#define UI_CABIN_W 248
#define UI_SCREEN_H 600
// HOME-02 长页会同时显示 26 个复用图标；预留 96 槽避免固定 Shell 图标挤占动态页槽位。
#define UI_ICON_SLOT_MAX 128
#define UI_TOAST_DURATION_US 2200000LL
#define UI_VOICE_POPUP_DOT_INTERVAL_US 320000LL
#define UI_VOICE_POPUP_PULSE_PERIOD_US 1100000LL
#define UI_HEADER_H 87
#define UI_STAT_CARD_H 116
#define UI_HOME_COLUMNS_Y 124
#define UI_HOME_TIPS_Y 466
#define UI_HOME_SUMMARIES_Y 704
#define UI_PAGE_H (UI_SCREEN_H - UI_HEADER_H)
#define UI_CARD_BOTTOM_INSET 10
#define UI_STAT_LABEL_BOTTOM_INSET 2
#define UI_EXPIRING_META_BOTTOM_INSET 1
#define UI_COMPACT_BODY_LINE_SPACE (-5)
#define UI_TIMER_PRESET_MAX 6
#define UI_STOPWATCH_REFRESH_US 16000LL
#define UI_ALARM_CLICK_CONFIRM_US 0LL
#define UI_ALARM_FORM_ROW_H 78
#define UI_CAMERA_FOOTER_H 72
#define UI_CAMERA_FRAME_H (UI_SCREEN_H - UI_HEADER_H - UI_CAMERA_FOOTER_H)
#define UI_CAMERA_STATUS_H 40
#define UI_CAMERA_PLACEHOLDER_SIZE 269
#define UI_CAMERA_PLACEHOLDER_X ((UI_MAIN_W - UI_CAMERA_PLACEHOLDER_SIZE) / 2)
#define UI_CAMERA_PLACEHOLDER_Y 74
#define UI_CAMERA_FRAME_BG 0x1D2A24
#define UI_CAMERA_PLACEHOLDER_BG 0x2A3731
#define UI_CAMERA_PLACEHOLDER_LINE 0x6B7F74
#define UI_CAMERA_PLACEHOLDER_TEXT 0xCFE0D6
// 竖屏逻辑布局严格复用 ver2：600x1024 = 主区 672 + 导航 64 + 状态舱 288。
// 物理 MIPI 仍保持 1024x600，以下尺寸只作用于 LVGL 逻辑对象树。
#define UI_PORTRAIT_W 600
#define UI_PORTRAIT_H 1024
#define UI_PORTRAIT_MAIN_H 672
#define UI_PORTRAIT_NAV_Y UI_PORTRAIT_MAIN_H
#define UI_PORTRAIT_NAV_H 64
#define UI_PORTRAIT_CABIN_Y (UI_PORTRAIT_NAV_Y + UI_PORTRAIT_NAV_H)
#define UI_PORTRAIT_CABIN_H 288
#define UI_PORTRAIT_CONTENT_X 16
#define UI_PORTRAIT_CONTENT_W 568
#define UI_PORTRAIT_HOME_GAP 12
#define UI_PORTRAIT_HOME_STAT_GAP 8
#define UI_PORTRAIT_HOME_STAT_H 124
#define UI_PORTRAIT_HOME_STATS_H \
    (UI_PORTRAIT_HOME_STAT_H * 2 + UI_PORTRAIT_HOME_STAT_GAP)
#define UI_PORTRAIT_HOME_EXP_Y \
    (UI_PORTRAIT_HOME_STATS_H + UI_PORTRAIT_HOME_GAP)
#define UI_PORTRAIT_HOME_EXP_H 316
#define UI_PORTRAIT_HOME_REC_Y \
    (UI_PORTRAIT_HOME_EXP_Y + UI_PORTRAIT_HOME_EXP_H + UI_PORTRAIT_HOME_GAP)
#define UI_PORTRAIT_HOME_REC_H 278
#define UI_PORTRAIT_HOME_TIPS_Y \
    (UI_PORTRAIT_HOME_REC_Y + UI_PORTRAIT_HOME_REC_H + UI_PORTRAIT_HOME_GAP)
#define UI_PORTRAIT_HOME_TIPS_H 226
#define UI_PORTRAIT_HOME_MINI_Y \
    (UI_PORTRAIT_HOME_TIPS_Y + UI_PORTRAIT_HOME_TIPS_H + UI_PORTRAIT_HOME_GAP)
#define UI_PORTRAIT_HOME_MINI_H 80
#define UI_PORTRAIT_HOME_SYNC_Y \
    (UI_PORTRAIT_HOME_MINI_Y + UI_PORTRAIT_HOME_MINI_H + UI_PORTRAIT_HOME_GAP)
#define UI_PORTRAIT_HOME_CONTENT_H 1256
// PLAN-01 竖屏正文按 ver2 单列堆叠；尾部预留安全空白，避免最后一张卡贴住导航栏。
#define UI_PORTRAIT_PLAN_GAP 12
#define UI_PORTRAIT_PLAN_TODAY_H 268
#define UI_PORTRAIT_PLAN_SHOP_H 176
#define UI_PORTRAIT_PLAN_EXPIRE_H 260
#define UI_PORTRAIT_PLAN_WEEK_H 136
#define UI_PORTRAIT_PLAN_AGENT_H 176
#define UI_PORTRAIT_PLAN_CONTENT_H 1120
// CHAT-01 竖屏把消息流、输入区和完整 IME 分成三个互不遮挡的层。
// PPA 只搬运消息视口，输入区/IME 保持在主区底部的动态对象树中。
#define UI_PORTRAIT_CHAT_GAP 8
#define UI_PORTRAIT_CHAT_INPUT_H 104
#define UI_PORTRAIT_CHAT_IME_H 282
#define UI_PORTRAIT_CHAT_INPUT_Y \
    (UI_PORTRAIT_MAIN_H - UI_PORTRAIT_CHAT_INPUT_H)
#define UI_PORTRAIT_CHAT_SCROLL_H \
    (UI_PORTRAIT_CHAT_INPUT_Y - UI_HEADER_H - UI_PORTRAIT_CHAT_GAP)
#define UI_PORTRAIT_CHAT_IME_Y \
    (UI_PORTRAIT_CHAT_INPUT_Y - UI_PORTRAIT_CHAT_IME_H - UI_PORTRAIT_CHAT_GAP)
#define UI_PORTRAIT_CHAT_OPEN_SCROLL_H \
    (UI_PORTRAIT_CHAT_IME_Y - UI_HEADER_H - UI_PORTRAIT_CHAT_INPUT_H - \
     UI_PORTRAIT_CHAT_GAP * 2)
#define UI_PORTRAIT_CHAT_SUBPAGE_FOOTER_H 60
#define UI_PORTRAIT_CHAT_SUBPAGE_SCROLL_H \
    (UI_PORTRAIT_MAIN_H - UI_HEADER_H - UI_PORTRAIT_CHAT_SUBPAGE_FOOTER_H - \
     UI_PORTRAIT_CHAT_GAP)
// MORE-01/02/07/08 竖屏正文只挂在 portrait_scroll 下；固定层不参与 PPA 的纵向复制。
#define UI_PORTRAIT_MORE_CARD_H 132
#define UI_PORTRAIT_MORE_CONTENT_H 760
#define UI_PORTRAIT_TIMER_CONTENT_H 1420
#define UI_PORTRAIT_ALARM_CONTENT_H 1320
#define UI_PORTRAIT_STOPWATCH_CONTENT_H 1320
#define UI_PORTRAIT_TOOL_GAP 12
#define UI_PORTRAIT_ALARM_CARD_H 104
#define UI_PORTRAIT_ALARM_ROW_STEP 116
// MORE-04/05/06 竖屏分区页使用收窄后的左栏，给右侧真实状态文案留出空间；
// 600px 正文宽度扣除左右 16px 后，右侧内容为 406px。
#define UI_PORTRAIT_SPLIT_SIDE_W 150
#define UI_PORTRAIT_SPLIT_GAP 12
#define UI_PORTRAIT_SPLIT_CONTENT_X \
    (UI_PORTRAIT_SPLIT_SIDE_W + UI_PORTRAIT_SPLIT_GAP)
#define UI_PORTRAIT_SPLIT_CONTENT_W \
    (UI_PORTRAIT_CONTENT_W - UI_PORTRAIT_SPLIT_CONTENT_X)
#define UI_PORTRAIT_SPLIT_CONTENT_H 1120
#define UI_PORTRAIT_SPLIT_ROW_H 94

typedef fridge_ui_p4_page_t shell_page_t;

static const char *TAG = "fridge_ui_shell";

// 页面重建前后记录内部 DMA 堆，便于关联 LVGL 页面对象与 SDMMC DMA 可用空间。
static void log_internal_dma_heap(const char *phase, shell_page_t page)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "internal_dma[%s:%s]: free=%u largest=%u minimum=%u",
             phase, fridge_ui_p4_page_key(page),
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

// 保留 Shell 内部旧符号，页面编号直接复用 fridge_ui_p4.h 的同一套枚举类型。
#define SHELL_PAGE_HOME FRIDGE_UI_P4_PAGE_HOME
#define SHELL_PAGE_MORE FRIDGE_UI_P4_PAGE_MORE
#define SHELL_PAGE_TIMER FRIDGE_UI_P4_PAGE_TIMER
#define SHELL_PAGE_TIMER_EDITOR FRIDGE_UI_P4_PAGE_TIMER_EDITOR
#define SHELL_PAGE_ALARM FRIDGE_UI_P4_PAGE_ALARM
#define SHELL_PAGE_ALARM_EDITOR FRIDGE_UI_P4_PAGE_ALARM_EDITOR
#define SHELL_PAGE_STOPWATCH FRIDGE_UI_P4_PAGE_STOPWATCH
#define SHELL_PAGE_CAMERA FRIDGE_UI_P4_PAGE_CAMERA
#define SHELL_PAGE_PLAN FRIDGE_UI_P4_PAGE_PLAN
#define SHELL_PAGE_SHOPPING FRIDGE_UI_P4_PAGE_SHOPPING
#define SHELL_PAGE_HOME_DETAIL FRIDGE_UI_P4_PAGE_HOME_DETAIL
#define SHELL_PAGE_BINDING FRIDGE_UI_P4_PAGE_BINDING
#define SHELL_PAGE_SETTINGS FRIDGE_UI_P4_PAGE_SETTINGS
#define SHELL_PAGE_MAINTENANCE FRIDGE_UI_P4_PAGE_MAINTENANCE
#define SHELL_PAGE_SAFETY FRIDGE_UI_P4_PAGE_SAFETY
#define SHELL_PAGE_INVENTORY FRIDGE_UI_P4_PAGE_INVENTORY
#define SHELL_PAGE_INVENTORY_DETAIL FRIDGE_UI_P4_PAGE_INVENTORY_DETAIL
#define SHELL_PAGE_INVENTORY_EDIT FRIDGE_UI_P4_PAGE_INVENTORY_EDIT
#define SHELL_PAGE_INVENTORY_RECOGNIZE FRIDGE_UI_P4_PAGE_INVENTORY_RECOGNIZE
#define SHELL_PAGE_INVENTORY_SEARCH FRIDGE_UI_P4_PAGE_INVENTORY_SEARCH
#define SHELL_PAGE_INVENTORY_LAYOUT FRIDGE_UI_P4_PAGE_INVENTORY_LAYOUT
#define UI_TIMER_OTHER_MAX (FRIDGE_KITCHEN_TIMER_MAX - 1)
#define UI_TIMER_DONE_MAX FRIDGE_KITCHEN_TIMER_MAX

// MORE-05/06 共用参考稿的分区模板：左侧 180px 分区导航 + 右侧内容区。
// 主区宽 672px，扣掉分区栏和 16px 间距后右侧内容宽 476px。
#define UI_SPLIT_SIDE_W 180
#define UI_SPLIT_GAP 16
#define UI_SPLIT_CONTENT_X (UI_SPLIT_SIDE_W + UI_SPLIT_GAP)
#define UI_SPLIT_CONTENT_W (672 - UI_SPLIT_CONTENT_X)
#define UI_SPLIT_TAB_H 48
#define UI_SPLIT_TAB_GAP 8
#define UI_SPLIT_SECTION_MAX 5
#define UI_SETTINGS_MODAL_W 620
#define UI_SETTINGS_MODAL_H 540
#define UI_SETTINGS_WIFI_ROW_H 64

typedef enum {
    SETTINGS_WIFI_MODAL_NONE = 0,
    SETTINGS_WIFI_MODAL_LIST,
    SETTINGS_WIFI_MODAL_PASSWORD,
} settings_wifi_modal_mode_t;

typedef enum {
    INVENTORY_INPUT_NAME = 0,
    INVENTORY_INPUT_NOTE,
    INVENTORY_INPUT_SEARCH,
} inventory_input_target_t;

typedef struct {
    char ssid[FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN + 1];
    bool secured;
    bool saved;
} settings_wifi_row_ctx_t;

typedef enum {
    ALARM_PENDING_NONE = 0,
    ALARM_PENDING_NEW,
    ALARM_PENDING_EDIT,
    ALARM_PENDING_TOGGLE,
    ALARM_PENDING_EDITOR_ADJUST,
    ALARM_PENDING_EDITOR_KIND,
    ALARM_PENDING_EDITOR_REPEAT,
    ALARM_PENDING_EDITOR_WEEKDAY,
    ALARM_PENDING_EDITOR_SOUND,
    ALARM_PENDING_EDITOR_NAME,
    ALARM_PENDING_EDITOR_SPEECH,
} alarm_pending_action_t;

typedef struct {
    lv_obj_t *image;
    lv_obj_t *fallback;
    fridge_ui_icon_id_t id;
    uint16_t scale;
} icon_slot_t;

typedef struct {
    lv_obj_t *number;
    lv_obj_t *label;
} stat_view_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *meta;
    lv_obj_t *badge_box;
    lv_obj_t *badge;
    lv_obj_t *expiry_box;
    lv_obj_t *expiry;
} expiring_view_t;

// HOME-01 竖屏对象引用独立保存；横屏 HOME 对象隐藏但继续保留，避免切换方向时丢失业务状态。
typedef struct {
    stat_view_t stats[FRIDGE_UI_HOME_STATS_MAX];
    expiring_view_t expiring[FRIDGE_UI_HOME_EXPIRING_MAX];
    lv_obj_t *expiring_side;
    lv_obj_t *rec_title;
    lv_obj_t *rec_meta;
    lv_obj_t *rec_coverage;
    lv_obj_t *rec_ready;
    lv_obj_t *rec_reason;
    lv_obj_t *rec_primary;
    lv_obj_t *rec_secondary;
    lv_obj_t *tip_text[FRIDGE_UI_HOME_TIPS_MAX];
    lv_obj_t *tip_action[FRIDGE_UI_HOME_TIPS_MAX];
    lv_obj_t *dinner;
    lv_obj_t *shopping;
    lv_obj_t *sync_line;
    // 竖屏 HOME-01 的 CTX-01：下一事项、晚餐同步和三枚统计胶囊。
    lv_obj_t *ctx_next_title;
    lv_obj_t *ctx_detail;
    lv_obj_t *ctx_summary_values[3];
} portrait_home_view_t;

// PLAN-01 竖屏对象引用独立保存；未移植的 PLAN-02～06 只显示提示，不伪造业务数据。
typedef struct {
    lv_obj_t *today_dishes[FRIDGE_UI_PLAN_MEAL_SLOTS];
    lv_obj_t *expiring_names[FRIDGE_UI_PLAN_EXPIRING_MAX];
    lv_obj_t *expiring_pills[FRIDGE_UI_PLAN_EXPIRING_MAX];
    lv_obj_t *shopping_values[2];
    lv_obj_t *week_boxes[FRIDGE_UI_PLAN_WEEK_DAYS];
    lv_obj_t *week_dates[FRIDGE_UI_PLAN_WEEK_DAYS];
    lv_obj_t *week_labels[FRIDGE_UI_PLAN_WEEK_DAYS];
    lv_obj_t *week_conflict_dots[FRIDGE_UI_PLAN_WEEK_DAYS];
    lv_obj_t *agent_tip;
} portrait_plan_view_t;

// MORE-01 竖屏对象引用。系统与维护卡进入对应的竖屏分区页，角标仍只取真实工具快照。
typedef struct {
    lv_obj_t *titles[6];
    lv_obj_t *descriptions[6];
    lv_obj_t *badges[2];
    lv_obj_t *badge_labels[2];
    lv_obj_t *summary;
    lv_obj_t *summary_wifi;
    lv_obj_t *summary_tf;
    lv_obj_t *summary_time;
    lv_obj_t *context_status;
    lv_obj_t *context_detail;
} portrait_more_view_t;

// MORE-02 竖屏对象引用。主计时数字区域作为动态覆盖层，滚动 PPA 不会冻结剩余时间。
typedef struct {
    lv_obj_t *content;
    lv_obj_t *hero;
    lv_obj_t *face_region;
    lv_obj_t *face;
    lv_obj_t *label;
    lv_obj_t *chip;
    lv_obj_t *state;
    lv_obj_t *ops;
    lv_obj_t *pause;
    lv_obj_t *pause_label;
    lv_obj_t *plus;
    lv_obj_t *cancel;
    lv_obj_t *dismiss;
    lv_obj_t *other_title;
    lv_obj_t *other_rows[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_names[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_remains[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_meta[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_pause[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_pause_labels[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_plus[UI_TIMER_OTHER_MAX];
    lv_obj_t *other_cancel[UI_TIMER_OTHER_MAX];
    uint8_t other_ids[UI_TIMER_OTHER_MAX];
    lv_obj_t *done_title;
    lv_obj_t *done_rows[UI_TIMER_DONE_MAX];
    lv_obj_t *done_names[UI_TIMER_DONE_MAX];
    lv_obj_t *done_meta[UI_TIMER_DONE_MAX];
    uint8_t last_primary_id;
    fridge_kitchen_timer_state_t last_primary_state;
    size_t last_other_count;
    size_t last_done_count;
    bool initialized;
} portrait_timer_view_t;

// MORE-07 竖屏对象引用。闹钟主卡点击只提示编辑页未移植，开关仍接真实后端。
typedef struct {
    lv_obj_t *content;
    lv_obj_t *status;
    lv_obj_t *fixed_title;
    lv_obj_t *cycle_title;
    lv_obj_t *fixed_empty;
    lv_obj_t *cycle_empty;
    lv_obj_t *cards[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_main[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_times[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_names[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_meta[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_kind[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_toggles[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *card_knobs[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    uint8_t card_ids[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    size_t last_count;
    bool initialized;
} portrait_alarm_view_t;

// MORE-08 竖屏对象引用。秒表数字区域作为动态覆盖层，计次行仍由 PPA 静态复制。
typedef struct {
    lv_obj_t *content;
    lv_obj_t *stage;
    lv_obj_t *face_region;
    lv_obj_t *face;
    lv_obj_t *state;
    lv_obj_t *reset;
    lv_obj_t *toggle;
    lv_obj_t *toggle_label;
    lv_obj_t *lap;
    icon_slot_t *toggle_play_slot;
    icon_slot_t *toggle_pause_slot;
    lv_obj_t *laps_count;
    lv_obj_t *laps_empty;
    lv_obj_t *laps_empty_label;
    lv_obj_t *laps_list;
    lv_obj_t *lap_rows[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
    lv_obj_t *lap_labels[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
    lv_obj_t *lap_times[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
    fridge_kitchen_stopwatch_state_t last_state;
    size_t last_lap_count;
    bool initialized;
} portrait_stopwatch_view_t;

// MORE-04 竖屏分区页的动态值引用。最多五个分区、每区最多六行，空指针表示当前分区未创建。
typedef struct {
    lv_obj_t *values[5][6];
    size_t value_count;
} portrait_safety_view_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *main;
    lv_obj_t *nav;
    lv_obj_t *cabin;
    lv_obj_t *header;
    lv_obj_t *main_scroll;
    lv_obj_t *context_scroll;
    lv_obj_t *home_content;
    lv_obj_t *page_content;
    lv_obj_t *camera_page;
    lv_obj_t *camera_photo;
    lv_obj_t *camera_placeholder;
    lv_obj_t *camera_capture_button;
    lv_obj_t *camera_capture_label;
    lv_image_dsc_t camera_photo_dsc;
    bool camera_review_active;
    lv_obj_t *header_title;
    lv_obj_t *header_id_box;
    lv_obj_t *header_id;
    // 四字页名需要把页面 ID 角标右移，避免覆盖标题末字。
    bool header_wide_title;
    lv_obj_t *header_back;
    lv_obj_t *header_back_label;
    lv_obj_t *header_search;
    lv_obj_t *header_plus;
    lv_obj_t *header_grid;
    lv_obj_t *header_new;
    // CHAT-01 专属的会话清除入口，其他页面保持隐藏，避免误删除。
    lv_obj_t *header_chat_clear;
    lv_obj_t *nav_items[5];
    lv_obj_t *nav_labels[5];
    // 竖屏隐私页使用独立覆盖层，不改动成熟横屏对象树；横屏恢复时只需切换可见性。
    fridge_ui_p4_orientation_t orientation;
    lv_obj_t *portrait_root;
    lv_obj_t *portrait_scroll;
    lv_obj_t *portrait_header_sub;
    lv_obj_t *portrait_time;
    lv_obj_t *portrait_date;
    lv_obj_t *portrait_temperature;
    lv_obj_t *portrait_humidity;
    lv_obj_t *portrait_health_boxes[5];
    lv_obj_t *portrait_health_dots[5];
    icon_slot_t *portrait_health_icons[5];
    lv_obj_t *portrait_camera_label;
    size_t portrait_icon_slot_start;
    portrait_home_view_t portrait_home;
    portrait_plan_view_t portrait_plan;
    portrait_more_view_t portrait_more;
    portrait_timer_view_t portrait_timer;
    portrait_alarm_view_t portrait_alarm;
    portrait_stopwatch_view_t portrait_stopwatch;
    portrait_safety_view_t portrait_safety;
    lv_obj_t *header_sub;
    lv_obj_t *toast;
    lv_obj_t *toast_label;
    int64_t toast_deadline_us;
    fridge_ui_shell_actions_t actions;
    const fridge_ui_assets_t *assets;
    icon_slot_t icon_slots[UI_ICON_SLOT_MAX];
    size_t icon_slot_count;
    size_t page_icon_slot_start;
    size_t camera_icon_slot_start;
    stat_view_t stats[FRIDGE_UI_HOME_STATS_MAX];
    expiring_view_t expiring[FRIDGE_UI_HOME_EXPIRING_MAX];
    lv_obj_t *expiring_side;
    lv_obj_t *rec_title;
    lv_obj_t *rec_meta;
    lv_obj_t *rec_coverage;
    lv_obj_t *rec_ready;
    lv_obj_t *rec_reason;
    lv_obj_t *rec_primary;
    lv_obj_t *rec_secondary;
    lv_obj_t *tip_text[FRIDGE_UI_HOME_TIPS_MAX];
    lv_obj_t *tip_action[FRIDGE_UI_HOME_TIPS_MAX];
    lv_obj_t *dinner;
    lv_obj_t *shopping;
    lv_obj_t *sync_line;
    lv_obj_t *time;
    lv_obj_t *date;
    lv_obj_t *weekday;
    lv_obj_t *temperature;
    lv_obj_t *humidity;
    lv_obj_t *health_boxes[5];
    lv_obj_t *health_dots[5];
    lv_obj_t *summary_values[4];
    lv_obj_t *next_title;
    lv_obj_t *next_remaining;
    lv_obj_t *next_source;
    lv_obj_t *recent_sync;
    lv_obj_t *ctx_default;
    lv_obj_t *ctx_tool;
    lv_obj_t *ctx_camera;
    // 连续语音会话运行时暂时覆盖页面专属右侧上下文；不显示对话正文或识别文本。
    lv_obj_t *ctx_continuous_voice;
    lv_obj_t *ctx_continuous_voice_state;
    lv_obj_t *ctx_continuous_voice_detail;
    lv_obj_t *ctx_camera_mode;
    lv_obj_t *ctx_camera_sensor;
    lv_obj_t *ctx_camera_wifi;
    lv_obj_t *ctx_camera_cloud;
    lv_obj_t *ctx_camera_task;
    lv_obj_t *ctx_camera_hint;
    lv_obj_t *camera_network_dot;
    lv_obj_t *camera_network_text;
    lv_obj_t *ctx_tool_id;
    lv_obj_t *ctx_tool_face;
    lv_obj_t *ctx_tool_source;
    lv_obj_t *ctx_tool_pause;
    lv_obj_t *ctx_tool_cancel;
    lv_obj_t *ctx_tool_all;
    lv_obj_t *ctx_tool_all_label;
    lv_obj_t *voice_button_label;
    // 手动语音浮层挂在根页面，不替换右侧动态上下文。只有 LVGL 任务会更新这些对象。
    lv_obj_t *voice_popup;
    lv_obj_t *voice_popup_state;
    lv_obj_t *voice_popup_detail;
    lv_obj_t *voice_popup_pulse_ring;
    lv_obj_t *voice_popup_cancel;
    lv_obj_t *voice_popup_submit;
    lv_obj_t *voice_popup_submit_label;
    int64_t voice_popup_dot_deadline_us;
    int64_t voice_popup_pulse_started_us;
    uint8_t voice_popup_dot_count;
    lv_obj_t *timer_hero;
    lv_obj_t *timer_label;
    lv_obj_t *timer_chip;
    lv_obj_t *timer_face;
    lv_obj_t *timer_state;
    lv_obj_t *timer_ops;
    lv_obj_t *timer_start;
    lv_obj_t *timer_pause;
    lv_obj_t *timer_plus;
    lv_obj_t *timer_cancel;
    lv_obj_t *timer_start_label;
    lv_obj_t *timer_pause_label;
    lv_obj_t *timer_dismiss;
    lv_obj_t *timer_other_title;
    lv_obj_t *timer_other_rows[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_names[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_remains[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_meta[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_pause[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_pause_labels[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_plus[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_other_cancel[UI_TIMER_OTHER_MAX];
    uint8_t timer_other_ids[UI_TIMER_OTHER_MAX];
    lv_obj_t *timer_done_title;
    lv_obj_t *timer_done_rows[UI_TIMER_DONE_MAX];
    lv_obj_t *timer_done_names[UI_TIMER_DONE_MAX];
    lv_obj_t *timer_done_meta[UI_TIMER_DONE_MAX];
    // MORE-03 倒计时编辑草稿（时/分/名称索引/声音）。
    uint8_t timer_edit_hour;
    uint8_t timer_edit_minute;
    uint8_t timer_edit_name_idx;
    fridge_kitchen_timer_sound_t timer_edit_sound;
    lv_obj_t *timer_edit_hour_label;
    lv_obj_t *timer_edit_min_label;
    lv_obj_t *timer_edit_name_label;
    // OV-09 到时浮层（挂 root，跨页可见）。
    lv_obj_t *timer_ring_root;
    lv_obj_t *timer_ring_name;
    lv_obj_t *timer_ring_src;
    uint8_t timer_ring_id;
    lv_obj_t *more_badge_boxes[2];
    lv_obj_t *more_badge_labels[2];
    lv_obj_t *more_device_dot;
    lv_obj_t *more_device;
    lv_obj_t *more_wifi;
    lv_obj_t *more_tf;
    lv_obj_t *binding_qr;
    lv_obj_t *binding_qr_placeholder;
    lv_obj_t *binding_status;
    lv_obj_t *binding_detail;
    lv_obj_t *binding_expiry;
    fridge_ui_p4_binding_model_t binding;
    // MORE-05/06 分区模板：当前选中的分区下标按页面各自保存，切页后回到第一个分区。
    // 分区栏按钮和右侧内容容器都在页面重建时一并销毁，这里只保存下标和内容宿主。
    uint8_t settings_section;
    uint8_t maintenance_section;
    uint8_t safety_section;
    lv_obj_t *split_content;
    // 分区切换时只销毁右侧内容；该索引用于同步回收右侧临时图标槽，避免 TF 资源
    // 重新绑定时访问已经删除的 FWD 图标对象。
    size_t split_content_icon_slot_start;
    lv_obj_t *split_tabs[UI_SPLIT_SECTION_MAX];
    lv_obj_t *split_tab_labels[UI_SPLIT_SECTION_MAX];
    fridge_ui_shell_settings_state_t settings;
    fridge_ui_safety_model_t safety;
    bool settings_initialized;
    bool settings_brightness_dragging;
    lv_obj_t *settings_wifi_summary;
    lv_obj_t *settings_brightness_value;
    lv_obj_t *settings_brightness_slider;
    lv_obj_t *settings_wake_switch;
    lv_obj_t *settings_wake_status;
    lv_obj_t *settings_agent_mode_buttons[3];
    lv_obj_t *settings_agent_mode_summary;
    lv_obj_t *settings_agent_auto_dialog;
    // Wi-Fi 弹窗只在 LVGL 任务内创建和更新。密码输入框开启 LVGL password mode，
    // Shell 不缓存其明文，也不把它复制到状态快照。
    lv_obj_t *settings_wifi_scrim;
    lv_obj_t *settings_wifi_dialog;
    lv_obj_t *settings_wifi_list_panel;
    lv_obj_t *settings_wifi_password_panel;
    lv_obj_t *settings_wifi_status;
    lv_obj_t *settings_wifi_list;
    lv_obj_t *settings_wifi_selected_label;
    lv_obj_t *settings_wifi_password_input;
    lv_obj_t *settings_wifi_keyboard;
    settings_wifi_modal_mode_t settings_wifi_modal_mode;
    settings_wifi_row_ctx_t settings_wifi_rows[FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS];
    fridge_ui_shell_settings_wifi_ap_t
        settings_wifi_modal_aps[FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS];
    size_t settings_wifi_modal_ap_count;
    fridge_ui_shell_settings_wifi_phase_t settings_wifi_modal_phase;
    char settings_wifi_modal_status[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN];
    char settings_wifi_selected_ssid[FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN + 1];
    bool settings_wifi_selected_secured;
    lv_obj_t *alarm_status;
    lv_obj_t *alarm_empty;
    lv_obj_t *alarm_cards[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_main[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_times[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_names[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_meta[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_kind[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_toggles[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_card_knobs[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    uint8_t alarm_card_ids[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
    lv_obj_t *alarm_editor_footer;
    lv_obj_t *alarm_delete_scrim;
    lv_obj_t *alarm_delete_dialog;
    lv_obj_t *alarm_text_scrim;
    lv_obj_t *alarm_text_dialog;
    lv_obj_t *alarm_textarea;
    lv_obj_t *alarm_ime_preedit;
    lv_obj_t *alarm_ime_candidates[FRIDGE_IME_MAX_CANDIDATES];
    uint8_t alarm_editing_id;
    bool alarm_editing_speech;
    fridge_kitchen_alarm_config_t alarm_draft;
    bool alarm_pending;
    alarm_pending_action_t alarm_pending_action;
    uint8_t alarm_pending_id;
    intptr_t alarm_pending_value;
    int64_t alarm_pending_confirm_us;
    lv_obj_t *stopwatch_face;
    lv_obj_t *stopwatch_face_region;
    lv_obj_t *stopwatch_face_card;
    lv_obj_t *stopwatch_state;
    lv_obj_t *stopwatch_reset;
    lv_obj_t *stopwatch_toggle;
    lv_obj_t *stopwatch_toggle_label;
    lv_obj_t *stopwatch_lap;
    lv_obj_t *stopwatch_toggle_play_image;
    lv_obj_t *stopwatch_toggle_play_fallback;
    lv_obj_t *stopwatch_toggle_pause_image;
    lv_obj_t *stopwatch_toggle_pause_fallback;
    lv_obj_t *stopwatch_laps_count;
    lv_obj_t *stopwatch_laps_empty;
    lv_obj_t *stopwatch_laps_empty_label;
    lv_obj_t *stopwatch_laps_list;
    lv_obj_t *stopwatch_lap_rows[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
    lv_obj_t *stopwatch_lap_labels[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
    lv_obj_t *stopwatch_lap_times[FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS];
    /* PLAN-01 页面对象 */
    lv_obj_t *plan_today_dishes[FRIDGE_UI_PLAN_MEAL_SLOTS];
    lv_obj_t *plan_expiring_names[FRIDGE_UI_PLAN_EXPIRING_MAX];
    lv_obj_t *plan_expiring_pills[FRIDGE_UI_PLAN_EXPIRING_MAX];
    lv_obj_t *plan_shopping_count;
    lv_obj_t *plan_shopping_items[2];
    lv_obj_t *plan_week_range;
    lv_obj_t *plan_week_boxes[FRIDGE_UI_PLAN_WEEK_DAYS];
    lv_obj_t *plan_week_labels[FRIDGE_UI_PLAN_WEEK_DAYS];
    lv_obj_t *plan_agent_tip;
    /* CTX-06 计划/烹饪侧舱 */
    lv_obj_t *ctx_plan;
    lv_obj_t *ctx_plan_dinner;
    lv_obj_t *ctx_plan_shopping;
    lv_obj_t *ctx_plan_expiring;
    lv_obj_t *ctx_plan_cooking_section;
    lv_obj_t *ctx_plan_cooking_sub;
    lv_obj_t *ctx_plan_cooking_step;
    lv_obj_t *ctx_plan_cooking_bar;
    plan_business_model_t plan;
    bool plan_initialized;
    /* PLAN-06 购物页对象。选择态只在 Shell 内存中保留，不写入购物 JSON。 */
    shopping_business_model_t shopping_model;
    bool shopping_initialized;
    bool shopping_selected[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    lv_obj_t *shopping_subtitle;
    lv_obj_t *shopping_rows[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    lv_obj_t *shopping_checks[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    icon_slot_t *shopping_check_icons[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    lv_obj_t *shopping_names[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    lv_obj_t *shopping_meta[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    lv_obj_t *shopping_groups[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    lv_obj_t *shopping_footer;
    lv_obj_t *shopping_clear_scrim;
    lv_obj_t *shopping_clear_dialog;
    lv_obj_t *shopping_add_scrim;
    lv_obj_t *shopping_add_dialog;
    lv_obj_t *shopping_textarea;
    lv_obj_t *shopping_ime_preedit;
    lv_obj_t *shopping_ime_candidates[FRIDGE_IME_MAX_CANDIDATES];
    uint8_t shopping_sort_mode;
    // INV-01..08 的范围、筛选与编辑草稿只在 UI 任务中保存；持久化统一交给库存模型。
    fridge_ui_inventory_model_t inventory;
    bool inventory_initialized;
    uint8_t inventory_scope_level;
    uint8_t inventory_zone_index;
    uint8_t inventory_layer_index;
    bool inventory_list_view;
    uint8_t inventory_selected_item;
    uint8_t inventory_search_filter;
    uint8_t inventory_search_sort;
    uint8_t inventory_category_index;
    uint8_t inventory_layout_level;
    bool inventory_layout_dirty;
    bool inventory_recognize_keep[3];
    bool inventory_pick_location;
    char inventory_search_text[48];
    fridge_ui_inventory_item_t inventory_draft;
    fridge_ui_inventory_layout_draft_t inventory_layout_draft;
    bool inventory_edit_existing;
    lv_obj_t *inventory_input_scrim;
    lv_obj_t *inventory_input_dialog;
    lv_obj_t *inventory_textarea;
    lv_obj_t *inventory_ime_preedit;
    lv_obj_t *inventory_ime_candidates[FRIDGE_IME_MAX_CANDIDATES];
    inventory_input_target_t inventory_input_target;
    // CTX-02 库存动态上下文：随空间范围、搜索与编辑页切换而刷新。
    lv_obj_t *ctx_inventory;
    lv_obj_t *ctx_inventory_browse;
    lv_obj_t *ctx_inventory_layout;
    lv_obj_t *ctx_inventory_path;
    lv_obj_t *ctx_inventory_items;
    lv_obj_t *ctx_inventory_expiring;
    lv_obj_t *ctx_inventory_expired;
    lv_obj_t *ctx_inventory_unlocated;
    /* CHAT-01 对话页面对象 */
    lv_obj_t *chat_page;
    lv_obj_t *chat_scroll;
    lv_obj_t *chat_message_flow;
    lv_obj_t *chat_status_row;
    lv_obj_t *chat_permission_chip;
    lv_obj_t *chat_permission_label;
    lv_obj_t *chat_sync_dot;
    lv_obj_t *chat_sync_label;
    lv_obj_t *chat_agent_label;
    lv_obj_t *chat_tts_label;
    lv_obj_t *chat_input_area;
    lv_obj_t *chat_textarea;
    lv_obj_t *chat_voice_btn;
    lv_obj_t *chat_send_stop_btn;
    lv_obj_t *chat_send_stop_label;
    lv_obj_t *chat_ime_panel;
    lv_obj_t *chat_ime_underline;
    lv_obj_t *chat_ime_candidates[FRIDGE_IME_MAX_CANDIDATES];
    lv_obj_t *chat_clear_scrim;
    lv_obj_t *chat_clear_dialog;
    chat_business_model_t chat;
    bool chat_initialized;
    bool chat_ime_open;
    shell_page_t page;
    uint16_t timer_selected_minutes;
    bool timer_preset_user_selected;
    uint8_t alarm_hour;
    uint8_t alarm_minute;
    bool resource_ready;
    bool device_initialized;
    bool kitchen_initialized;
    bool more_initialized;
    char resource_detail[80];
    home_business_model_t home;
    fridge_ui_device_model_t device;
    fridge_kitchen_tools_snapshot_t kitchen;
    more_business_model_t more;
    int64_t stopwatch_last_ui_refresh_us;
    fridge_kitchen_stopwatch_state_t stopwatch_last_ui_state;
    uint8_t stopwatch_last_ui_lap_count;
    bool main_scroll_dragging;
    bool scroll_press_moved;
    lv_point_t scroll_press_origin;
    lv_indev_t *scroll_press_indev;
    lv_obj_t *scroll_press_container;
    bool created;
} shell_state_t;

static shell_state_t s_shell;

// 前向声明：定时器事件在页面创建之前注册，会回调到后文实现。
static void switch_page(shell_page_t page);
static void refresh_tool_page(void);
static void alarm_new_event(lv_event_t *event);
static void create_timer_editor_page(lv_obj_t *page);
static void create_timer_ring_overlay(lv_obj_t *root);
static void refresh_timer_ring_overlay(void);
static void create_chat_page(lv_obj_t *page);
static void create_portrait_chat_page(void);
static void refresh_portrait_chat_page(void);
static void portrait_chat_history_event(lv_event_t *event);
static void portrait_chat_new_event(lv_event_t *event);
static void portrait_chat_continue_event(lv_event_t *event);
static void portrait_chat_tool_event(lv_event_t *event);
static void portrait_chat_back_event(lv_event_t *event);
static void create_home_detail_page(lv_obj_t *page);
static void create_portrait_home_page(lv_obj_t *content);
static void create_portrait_plan_page(lv_obj_t *content);
static void create_portrait_more_page(lv_obj_t *content);
static void create_portrait_timer_page(lv_obj_t *content);
static void create_portrait_alarm_page(lv_obj_t *content);
static void create_portrait_stopwatch_page(lv_obj_t *content);
static void create_portrait_safety_page(lv_obj_t *content);
static void create_portrait_settings_page(lv_obj_t *content);
static void create_portrait_maintenance_page(lv_obj_t *content);
static void refresh_portrait_more_page(void);
static void refresh_portrait_timer_page(void);
static void refresh_portrait_alarm_page(void);
static void refresh_portrait_stopwatch_page(int64_t now_us);
static void refresh_portrait_safety_page(void);
static void clear_portrait_shell(void);
static bool portrait_page_supported(shell_page_t page);
static void create_binding_page(lv_obj_t *page);
static void refresh_binding_page(void);
static void create_settings_page(lv_obj_t *page);
static void create_maintenance_page(lv_obj_t *page);
static void create_safety_page(lv_obj_t *page);
static void rebuild_current_split_content(void);
static void close_settings_wifi_modal(void);
static void refresh_chat_page(bool scroll_to_latest);
static void close_chat_ime(void);
static void chat_show_clear_confirm(void);
static void close_chat_clear_confirm(void);
static void create_inventory_page(lv_obj_t *page);
static void create_inventory_detail_page(lv_obj_t *page);
static void create_inventory_edit_page(lv_obj_t *page);
static void create_inventory_recognize_page(lv_obj_t *page);
static void create_inventory_search_page(lv_obj_t *page);
static void create_inventory_layout_page(lv_obj_t *page);
static void add_divider(lv_obj_t *parent, int y);
static void inventory_back_event(lv_event_t *event);
static void inventory_search_event(lv_event_t *event);
static void inventory_open_new_event(lv_event_t *event);
static void inventory_layout_event(lv_event_t *event);
static void create_ctx_inventory(lv_obj_t *ctx);
static void refresh_ctx_inventory(void);

static lv_color_t color_hex(uint32_t color)
{
    return lv_color_hex(color);
}

static void clear_container_style(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

static void style_box(lv_obj_t *object, uint32_t background, uint32_t border,
                      int radius, int border_width)
{
    lv_obj_set_style_bg_color(object, color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, color_hex(border), 0);
    lv_obj_set_style_border_width(object, border_width, 0);
    // 用外轮廓把 1px 边框视觉加厚为 2px；border 本身不变，避免子控件整体位移。
    lv_obj_set_style_outline_color(object, color_hex(border), 0);
    lv_obj_set_style_outline_opa(object, border_width > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(object, border_width > 0 ? 1 : 0, 0);
    lv_obj_set_style_outline_pad(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                          uint32_t background, uint32_t border, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    style_box(box, background, border, radius, 1);
    return box;
}

// 横纵分隔线共用同一无边框实色对象，确保 1px 线宽不会被容器边框叠加加粗。
static lv_obj_t *make_solid_line(lv_obj_t *parent, int x, int y, int width, int height,
                                 uint32_t color)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, width, height);
    lv_obj_set_style_bg_color(line, color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, fridge_font_role_t role,
                            uint32_t color, int x, int y, int width, int height,
                            lv_label_long_mode_t long_mode)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, fridge_ui_theme_font_style(role), 0);
    lv_obj_set_style_text_color(label, color_hex(color), 0);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_label_set_long_mode(label, long_mode);
    lv_label_set_text(label, text ? text : "");
    return label;
}

// 单行文字使用字体真实行高；TF 字体切换后 LVGL 会重新计算内容高度，避免固定高度裁字。
static void set_single_line_content_height(lv_obj_t *label)
{
    if (!label) return;
    lv_obj_set_height(label, LV_SIZE_CONTENT);
}

// 胶囊和普通按钮必须同时按真实行高做水平、垂直居中，不能只设置 text_align。
static void center_single_line_label(lv_obj_t *label, int width)
{
    if (!label) return;
    lv_obj_set_width(label, width);
    set_single_line_content_height(label);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

// 卡片末行统一保留底部安全区，避免不同字体真实行高变化后再次贴边。
static void align_card_bottom_label(lv_obj_t *label, int x, int width, int bottom_inset)
{
    if (!label) return;
    lv_obj_set_width(label, width);
    set_single_line_content_height(label);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, x, -bottom_inset);
}

// Noto CJK 的 FreeType 行框高于浏览器 CSS 字号；多行正文收紧 5px 后接近参考稿。
static void apply_compact_body_lines(lv_obj_t *label)
{
    if (!label) return;
    lv_obj_set_style_text_line_space(label, UI_COMPACT_BODY_LINE_SPACE, 0);
}

static void set_label_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *old = lv_label_get_text(label);
    if (!old || strcmp(old, text) != 0) lv_label_set_text(label, text);
}

static void cancel_pending_alarm_action(void);
static bool scroll_click_is_suppressed(lv_event_t *event);
static void create_plan_page(lv_obj_t *page);
static void refresh_plan_page(void);
static void refresh_ctx_plan(void);
static void create_shopping_page(lv_obj_t *page);
static void refresh_shopping_page(void);
static void close_shopping_dialogs(void);

// 找到控件所属的 Shell 滚动容器；固定顶栏、导航和状态舱底部按钮会返回 NULL。
static lv_obj_t *find_scrollable_ancestor(lv_obj_t *object)
{
    for (lv_obj_t *current = object; current; current = lv_obj_get_parent(current)) {
        if (current == s_shell.main_scroll || current == s_shell.portrait_scroll ||
            current == s_shell.context_scroll ||
            current == s_shell.chat_scroll) return current;
    }
    return NULL;
}

// 手机式手势仲裁：按下只建立点击候选，任一方向移动 8px 后永久转为拖动。
static void scroll_press_track_event(lv_event_t *event)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(event);
    lv_obj_t *container = find_scrollable_ancestor(target);
    if (!container) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_shell.scroll_press_indev = indev;
        s_shell.scroll_press_container = container;
        s_shell.scroll_press_origin = point;
        s_shell.scroll_press_moved = false;
        s_shell.main_scroll_dragging = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (container != s_shell.scroll_press_container) return;
        int32_t dx = point.x - s_shell.scroll_press_origin.x;
        int32_t dy = point.y - s_shell.scroll_press_origin.y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dy >= 8 || dx >= 8) {
            s_shell.scroll_press_moved = true;
            s_shell.main_scroll_dragging = true;
            cancel_pending_alarm_action();
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (container != s_shell.scroll_press_container) return;
        s_shell.scroll_press_moved = true;
        cancel_pending_alarm_action();
    }
}

// 所有可点击控件统一注册同一套滚动保护，避免页面或状态舱出现漏网按钮。
static void add_click_event(lv_obj_t *object, lv_event_cb_t event_cb, void *event_data)
{
    if (!object || !event_cb) return;
    lv_obj_add_event_cb(object, scroll_press_track_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(object, scroll_press_track_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(object, scroll_press_track_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(object, event_cb, LV_EVENT_CLICKED, event_data);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int width, int height,
                             uint32_t background, uint32_t foreground, uint32_t border,
                             const char *label_text, fridge_font_role_t role,
                             lv_event_cb_t event_cb, void *event_data, lv_obj_t **out_label)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    style_box(button, background, border, 8, 1);
    lv_obj_set_style_bg_color(button, color_hex(FRIDGE_UI_COLOR_MINT), LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    if (event_cb) add_click_event(button, event_cb, event_data);
    lv_obj_t *label = make_label(button, label_text, role, foreground, 4, 0,
                                 width - 8, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(label, width - 8);
    if (out_label) *out_label = label;
    return button;
}

static void register_icon(lv_obj_t *parent, fridge_ui_icon_id_t id, uint32_t color,
                          int x, int y, int visual_size, const char *fallback)
{
    if (s_shell.icon_slot_count >= UI_ICON_SLOT_MAX) return;
    icon_slot_t *slot = &s_shell.icon_slots[s_shell.icon_slot_count++];
    slot->id = id;
    slot->scale = (uint16_t)((visual_size * LV_SCALE_NONE) / 28);
    slot->image = lv_image_create(parent);
    lv_obj_remove_style_all(slot->image);
    lv_obj_set_pos(slot->image, x, y);
    lv_obj_set_size(slot->image, 28, 28);
    lv_obj_set_style_image_recolor(slot->image, color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(slot->image, LV_OPA_COVER, 0);
    lv_image_set_scale(slot->image, slot->scale);
    lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
    slot->fallback = make_label(parent, fallback ? fallback : "·", FRIDGE_FONT_BODY_SMALL,
                                color, x, y, 28, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(slot->fallback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(slot->fallback, slot->image, LV_ALIGN_CENTER, 0, 0);
    // 动态页可能在 TF 资源已经绑定后才创建；此处立即复用已校验的 A8 图标，
    // 而不是只能显示 Flash 文本回退。
    if (s_shell.assets && s_shell.assets->loaded) {
        const lv_image_dsc_t *image = fridge_ui_assets_get_icon(s_shell.assets, id);
        if (image) {
            lv_image_set_src(slot->image, image);
            lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(slot->fallback, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 健康状态变化时同步更新 A8 图标和文字回退色；隐私模式下摄像头必须始终呈灰色关闭态。
static void set_icon_slot_color(icon_slot_t *slot, uint32_t color)
{
    if (!slot) return;
    if (slot->image) {
        lv_obj_set_style_image_recolor(slot->image, color_hex(color), 0);
    }
    if (slot->fallback) {
        lv_obj_set_style_text_color(slot->fallback, color_hex(color), 0);
    }
}

// 勾选框的 A8 图标和文字回退必须同步显隐；选择态本身不重建整页对象树。
static void set_icon_slot_visible(icon_slot_t *slot, bool visible)
{
    if (!slot) return;
    if (!visible) {
        if (slot->image) lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
        if (slot->fallback) lv_obj_add_flag(slot->fallback, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const lv_image_dsc_t *image = s_shell.assets && s_shell.assets->loaded ?
                                  fridge_ui_assets_get_icon(s_shell.assets, slot->id) : NULL;
    if (image && slot->image) {
        lv_image_set_src(slot->image, image);
        lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
        if (slot->fallback) lv_obj_add_flag(slot->fallback, LV_OBJ_FLAG_HIDDEN);
    } else if (slot->fallback) {
        lv_obj_remove_flag(slot->fallback, LV_OBJ_FLAG_HIDDEN);
    }
}

// 将页头操作图标按按钮内容区居中。按钮带 1px 边框时，固定坐标会产生
// 约 1px 的视觉偏差；相对对齐还能让 A8 图标和文字回退保持同一中心。
static void center_header_icon(lv_obj_t *button)
{
    if (!button || s_shell.icon_slot_count == 0) return;
    icon_slot_t *slot = &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
    lv_obj_align(slot->image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(slot->fallback, LV_ALIGN_CENTER, 0, 0);
}

// 秒表操作按钮按网页参考稿采用“图标在上、文字在下”的 64px 大触控布局。
// 图标优先使用 TF A8 资源；资源尚未加载时保留 ASCII/符号回退，避免按钮变成空白。
static lv_obj_t *make_stopwatch_button(lv_obj_t *parent, int x, int y, int width, int height,
                                       uint32_t background, uint32_t foreground, uint32_t border,
                                       const char *label_text, fridge_ui_icon_id_t icon_id,
                                       const char *icon_fallback, lv_event_cb_t event_cb,
                                       lv_obj_t **out_label, icon_slot_t **out_icon_slot)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    style_box(button, background, border, 8, 1);
    lv_obj_set_style_bg_color(button, color_hex(FRIDGE_UI_COLOR_MINT), LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    if (event_cb) add_click_event(button, event_cb, NULL);

    register_icon(button, icon_id, foreground, (width - 28) / 2, 4, 28,
                  icon_fallback ? icon_fallback : "·");
    if (out_icon_slot && s_shell.icon_slot_count > 0) {
        *out_icon_slot = &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
    }
    lv_obj_t *label = make_label(button, label_text, FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                 foreground, 4, 36, width - 8, LV_SIZE_CONTENT,
                                 LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
    if (out_label) *out_label = label;
    return button;
}

static uint32_t tone_color(fridge_ui_tone_t tone)
{
    if (tone == FRIDGE_UI_TONE_DANGER) return FRIDGE_UI_COLOR_TOMATO;
    if (tone == FRIDGE_UI_TONE_WARN) return FRIDGE_UI_COLOR_YOLK;
    return FRIDGE_UI_COLOR_SAGE;
}

static uint32_t tone_background(fridge_ui_tone_t tone)
{
    if (tone == FRIDGE_UI_TONE_DANGER) return FRIDGE_UI_COLOR_DANGER_BG;
    if (tone == FRIDGE_UI_TONE_WARN) return FRIDGE_UI_COLOR_WARN_BG;
    return FRIDGE_UI_COLOR_MINT;
}

// 闹钟动作仍统一在 LVGL tick 内分派，但点击确认不再增加固定等待。
static void cancel_pending_alarm_action(void)
{
    s_shell.alarm_pending = false;
    s_shell.alarm_pending_action = ALARM_PENDING_NONE;
    s_shell.alarm_pending_id = 0;
    s_shell.alarm_pending_value = 0;
    s_shell.alarm_pending_confirm_us = 0;
}

// 任一滚动容器开始移动后，永久取消本次点击候选；滚动结束不会补发业务动作。
static void main_scroll_activity_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *container = (lv_obj_t *)lv_event_get_target(event);
    if (code == LV_EVENT_SCROLL_BEGIN) {
        cancel_pending_alarm_action();
        s_shell.scroll_press_container = container;
        s_shell.scroll_press_moved = true;
        s_shell.main_scroll_dragging = true;
    } else if (code == LV_EVENT_SCROLL) {
        cancel_pending_alarm_action();
        s_shell.scroll_press_container = container;
        s_shell.scroll_press_moved = true;
        s_shell.main_scroll_dragging = true;
    } else if (code == LV_EVENT_SCROLL_END) {
        s_shell.main_scroll_dragging = false;
    }
}

static void unsupported_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const char *name = (const char *)lv_event_get_user_data(event);
    char text[72];
    snprintf(text, sizeof(text), "%s将在后续页面移植中开放", name ? name : "该功能");
    fridge_ui_shell_show_toast(text);
}

// 竖屏 MORE-01 的系统/维护入口和工具编辑入口必须停留在当前页，统一提示明确边界。
static void portrait_unsupported_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    fridge_ui_shell_show_toast("该页面尚未移植竖屏");
}

static void camera_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        fridge_ui_shell_show_toast("竖屏隐私模式已关闭摄像头");
        return;
    }
    if (s_shell.actions.camera_pressed) {
        s_shell.actions.camera_pressed(s_shell.actions.user_ctx);
    }
}

static void voice_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.actions.continuous_voice_pressed) {
        s_shell.actions.continuous_voice_pressed(s_shell.actions.user_ctx);
    } else {
        fridge_ui_shell_show_toast("连续语音暂不可用");
    }
}

static void voice_popup_submit_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.actions.voice_submitted) {
        s_shell.actions.voice_submitted(s_shell.actions.user_ctx);
    }
}

static void voice_popup_cancel_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.actions.voice_cancelled) {
        s_shell.actions.voice_cancelled(s_shell.actions.user_ctx);
    }
}

static void refresh_tool_page(void);
static void refresh_header_sub(void);
static void switch_page(shell_page_t page);
static void refresh_cabin_context_visibility(void);
static void refresh_ctx_tool(void);
static void refresh_ctx_camera(void);
static void refresh_camera_live_status(void);
static uint32_t health_color(fridge_ui_health_t health);

static void format_clock_duration(uint32_t seconds, char *out, size_t out_size)
{
    uint32_t hours = seconds / 3600U;
    uint32_t minutes = (seconds / 60U) % 60U;
    uint32_t remain = seconds % 60U;
    if (hours > 0U) {
        snprintf(out, out_size, "%02lu:%02lu:%02lu", (unsigned long)hours,
                 (unsigned long)minutes, (unsigned long)remain);
    } else {
        snprintf(out, out_size, "%02lu:%02lu", (unsigned long)minutes,
                 (unsigned long)remain);
    }
}

static uint16_t timer_preset_minutes(size_t index)
{
    static const uint16_t fallback[UI_TIMER_PRESET_MAX] = {1, 3, 5, 10, 15, 30};
    if (index >= UI_TIMER_PRESET_MAX) return fallback[0];
    if (s_shell.more_initialized && s_shell.more.timer_presets_minutes[index] > 0U) {
        return s_shell.more.timer_presets_minutes[index];
    }
    return fallback[index];
}

static fridge_ui_icon_id_t more_card_icon(size_t index)
{
    static const fridge_ui_icon_id_t fallback[6] = {
        FRIDGE_UI_ICON_TIMER, FRIDGE_UI_ICON_BELL, FRIDGE_UI_ICON_STOPWATCH,
        FRIDGE_UI_ICON_WARN, FRIDGE_UI_ICON_INFO, FRIDGE_UI_ICON_CLOCK,
    };
    if (index >= 3U || !s_shell.more_initialized) return fallback[index < 6U ? index : 0U];
    const char *icon = s_shell.more.cards[index].icon;
    if (strcmp(icon, "timer") == 0) return FRIDGE_UI_ICON_TIMER;
    if (strcmp(icon, "bell") == 0) return FRIDGE_UI_ICON_BELL;
    if (strcmp(icon, "stopwatch") == 0) return FRIDGE_UI_ICON_STOPWATCH;
    return fallback[index < 6U ? index : 0U];
}

static const char *timer_default_name(void)
{
    return s_shell.more_initialized && s_shell.more.timer_default_name[0] ?
               s_shell.more.timer_default_name : "厨房计时";
}

static const char *alarm_default_name(void)
{
    return s_shell.more_initialized && s_shell.more.alarm_default_name[0] ?
               s_shell.more.alarm_default_name : "厨房提醒";
}

static bool dispatch_kitchen_action(fridge_ui_shell_kitchen_action_t action,
                                    const fridge_ui_shell_kitchen_request_t *request,
                                    const char *success, const char *failure)
{
    if (!s_shell.actions.kitchen_action) {
        fridge_ui_shell_show_toast("厨房计时工具尚未就绪");
        return false;
    }
    esp_err_t ret = s_shell.actions.kitchen_action(s_shell.actions.user_ctx, action, request);
    if (ret == ESP_OK) {
        if (success) fridge_ui_shell_show_toast(success);
        return true;
    }
    if (ret == ESP_ERR_INVALID_STATE && action == FRIDGE_UI_SHELL_KITCHEN_ALARM_CREATE) {
        fridge_ui_shell_show_toast("时间未同步，请先连接 Wi-Fi");
    } else if (ret == ESP_ERR_NO_MEM && action == FRIDGE_UI_SHELL_KITCHEN_TIMER_START) {
        fridge_ui_shell_show_toast("定时器已满，最多 5 个");
    } else {
        fridge_ui_shell_show_toast(failure ? failure : "操作失败，请稍后重试");
    }
    return false;
}

static void page_back_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        // CHAT-02/03/04 在竖屏内保持真实页面键，返回只回到 CHAT-01，不能先恢复横屏。
        if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
            s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
            s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS) {
            if (s_shell.actions.navigate_requested) {
                s_shell.actions.navigate_requested(s_shell.actions.user_ctx,
                                                   FRIDGE_UI_P4_PAGE_CHAT);
            }
            return;
        }
        if (s_shell.page == SHELL_PAGE_TIMER ||
            s_shell.page == SHELL_PAGE_ALARM ||
            s_shell.page == SHELL_PAGE_STOPWATCH ||
            s_shell.page == SHELL_PAGE_SAFETY ||
            s_shell.page == SHELL_PAGE_SETTINGS ||
            s_shell.page == SHELL_PAGE_MAINTENANCE) {
            if (s_shell.actions.navigate_requested) {
                s_shell.actions.navigate_requested(s_shell.actions.user_ctx, SHELL_PAGE_MORE);
            }
            return;
        }
        if (s_shell.actions.navigate_requested) {
            s_shell.actions.navigate_requested(s_shell.actions.user_ctx, SHELL_PAGE_HOME);
        }
        return;
    }
    if (s_shell.page == SHELL_PAGE_INVENTORY ||
        s_shell.page == SHELL_PAGE_INVENTORY_DETAIL ||
        s_shell.page == SHELL_PAGE_INVENTORY_EDIT ||
        s_shell.page == SHELL_PAGE_INVENTORY_RECOGNIZE ||
        s_shell.page == SHELL_PAGE_INVENTORY_SEARCH ||
        s_shell.page == SHELL_PAGE_INVENTORY_LAYOUT) {
        inventory_back_event(event);
    } else if (s_shell.page == SHELL_PAGE_CAMERA) {
        switch_page(SHELL_PAGE_INVENTORY);
    } else if (s_shell.page == SHELL_PAGE_ALARM_EDITOR) {
        switch_page(SHELL_PAGE_ALARM);
    } else if (s_shell.page == SHELL_PAGE_TIMER_EDITOR) {
        switch_page(SHELL_PAGE_TIMER);
    } else if (s_shell.page == SHELL_PAGE_HOME_DETAIL) {
        switch_page(SHELL_PAGE_HOME);
    } else if (s_shell.page == SHELL_PAGE_SHOPPING) {
        switch_page(SHELL_PAGE_PLAN);
    } else {
        switch_page(s_shell.page == SHELL_PAGE_MORE ? SHELL_PAGE_HOME : SHELL_PAGE_MORE);
    }
}

// 顶栏动作按当前页面分派。库存页使用参考稿的搜索/新增/空间编辑三个图标入口，
// 首页仍保留原行为，不把按钮挪进主内容区。
static void header_search_event(lv_event_t *event)
{
    if (s_shell.page == SHELL_PAGE_INVENTORY) {
        inventory_search_event(event);
    } else {
        fridge_ui_shell_show_toast("搜索入口正在准备");
    }
}

static void header_plus_event(lv_event_t *event)
{
    if (s_shell.page == SHELL_PAGE_INVENTORY) {
        inventory_open_new_event(event);
    } else {
        fridge_ui_shell_show_toast("快速添加入口正在准备");
    }
}

// CHAT-01 的危险操作只负责打开二次确认层，真正清空必须由确认按钮触发。
static void header_chat_clear_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT) chat_show_clear_confirm();
}

static void header_grid_event(lv_event_t *event)
{
    if (s_shell.page == SHELL_PAGE_INVENTORY) {
        inventory_layout_event(event);
    }
}

// 点击只对本次按压所属的滚动容器生效；8px 位移或滚动开始后立即吞掉。
static bool scroll_click_is_suppressed(lv_event_t *event)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(event);
    lv_obj_t *container = find_scrollable_ancestor(target);
    if (!container || container != s_shell.scroll_press_container) return false;
    return s_shell.main_scroll_dragging || s_shell.scroll_press_moved;
}

// 竖屏移植 HOME、PLAN、CHAT、MORE 工具页与 MORE-04～06；其它入口交还编排层，
// 先恢复横屏再打开目标页。
static void portrait_nav_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    shell_page_t page = (shell_page_t)(uintptr_t)lv_event_get_user_data(event);
    if (page == SHELL_PAGE_CAMERA) {
        fridge_ui_shell_show_toast("竖屏隐私模式已关闭摄像头");
        return;
    }
    if (s_shell.actions.navigate_requested) {
        s_shell.actions.navigate_requested(s_shell.actions.user_ctx, page);
    }
}

static void nav_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    shell_page_t page = (shell_page_t)(uintptr_t)lv_event_get_user_data(event);
    // 从固定导航进入库存必须落在参考稿 INV-01 根视图；分区/层级钻取只由库存页内部保留。
    if (page == SHELL_PAGE_INVENTORY) {
        s_shell.inventory_scope_level = 0;
        s_shell.inventory_list_view = false;
        s_shell.inventory_pick_location = false;
    }
    switch_page(page);
}

static void more_card_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    shell_page_t page = (shell_page_t)(uintptr_t)lv_event_get_user_data(event);
    // 白名单只放行 MORE-01 上真实存在的入口，避免误传枚举值导致创建未移植页面。
    if (page != SHELL_PAGE_TIMER && page != SHELL_PAGE_ALARM &&
        page != SHELL_PAGE_STOPWATCH && page != SHELL_PAGE_BINDING &&
        page != SHELL_PAGE_SETTINGS && page != SHELL_PAGE_MAINTENANCE &&
        page != SHELL_PAGE_SAFETY) {
        return;
    }
    switch_page(page);
}

static void timer_start_with(uint32_t seconds, const char *label, const char *source,
                             fridge_kitchen_timer_sound_t sound)
{
    fridge_ui_shell_kitchen_request_t request = {
        .seconds = seconds,
        .label = label,
        .source = source ? source : "手动",
        .timer_sound = sound,
    };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_START, &request,
                                "定时器已开始", "定时器启动失败")) {
        refresh_tool_page();
    }
}

static void timer_preset_event(lv_event_t *event)
{
    // 参考 MORE-02：点预设立即开始，不再“先选后开”。
    if (scroll_click_is_suppressed(event)) return;
    uint16_t minutes = (uint16_t)(uintptr_t)lv_event_get_user_data(event);
    if (minutes == 0) return;
    s_shell.timer_selected_minutes = minutes;
    s_shell.timer_preset_user_selected = true;
    char label[24];
    snprintf(label, sizeof(label), "%u 分钟", (unsigned)minutes);
    timer_start_with((uint32_t)minutes * 60U, label, "手动", FRIDGE_KITCHEN_TIMER_SOUND_VOICE);
}

static void timer_start_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    timer_start_with((uint32_t)s_shell.timer_selected_minutes * 60U,
                     timer_default_name(), "手动", FRIDGE_KITCHEN_TIMER_SOUND_VOICE);
}

static void timer_pause_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = s_shell.kitchen.timer.id;
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id };
    fridge_ui_shell_kitchen_action_t action =
        s_shell.kitchen.timer.state == FRIDGE_KITCHEN_TIMER_PAUSED ?
            FRIDGE_UI_SHELL_KITCHEN_TIMER_RESUME : FRIDGE_UI_SHELL_KITCHEN_TIMER_PAUSE;
    if (dispatch_kitchen_action(action, id ? &request : NULL,
                                action == FRIDGE_UI_SHELL_KITCHEN_TIMER_RESUME ?
                                    "定时器已继续" : "定时器已暂停",
                                "定时器状态更新失败")) {
        refresh_tool_page();
    }
}

static void timer_plus_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = s_shell.kitchen.timer.id;
    if (id == 0) return;
    fridge_ui_shell_kitchen_request_t request = {
        .timer_id = id,
        .seconds = 60,
    };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_ADD_SECONDS, &request,
                                "已延长 1 分钟", "延长失败")) {
        refresh_tool_page();
    }
}

static void timer_cancel_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = s_shell.kitchen.timer.id;
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_CANCEL, id ? &request : NULL,
                                "定时器已取消", "定时器取消失败")) {
        refresh_tool_page();
    }
}

static void timer_dismiss_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = s_shell.timer_ring_id ? s_shell.timer_ring_id : s_shell.kitchen.timer.id;
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_DISMISS, id ? &request : NULL,
                                "已确认定时提醒", "提醒确认失败")) {
        if (s_shell.timer_ring_root) {
            lv_obj_add_flag(s_shell.timer_ring_root, LV_OBJ_FLAG_HIDDEN);
        }
        s_shell.timer_ring_id = 0;
        refresh_tool_page();
    }
}

static void timer_ring_plus_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = s_shell.timer_ring_id;
    if (id == 0) return;
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id, .seconds = 60 };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_ADD_SECONDS, &request,
                                "已延长 1 分钟", "延长失败")) {
        if (s_shell.timer_ring_root) {
            lv_obj_add_flag(s_shell.timer_ring_root, LV_OBJ_FLAG_HIDDEN);
        }
        s_shell.timer_ring_id = 0;
        refresh_tool_page();
    }
}

static void timer_other_pause_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (id == 0) return;
    fridge_kitchen_timer_state_t state = FRIDGE_KITCHEN_TIMER_IDLE;
    for (size_t i = 0; i < s_shell.kitchen.timer_count; i++) {
        if (s_shell.kitchen.timers[i].id == id) {
            state = s_shell.kitchen.timers[i].state;
            break;
        }
    }
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id };
    fridge_ui_shell_kitchen_action_t action =
        state == FRIDGE_KITCHEN_TIMER_PAUSED ?
            FRIDGE_UI_SHELL_KITCHEN_TIMER_RESUME : FRIDGE_UI_SHELL_KITCHEN_TIMER_PAUSE;
    if (dispatch_kitchen_action(action, &request, NULL, "操作失败")) {
        refresh_tool_page();
    }
}

static void timer_other_plus_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (id == 0) return;
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id, .seconds = 60 };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_ADD_SECONDS, &request,
                                "已延长 1 分钟", "延长失败")) {
        refresh_tool_page();
    }
}

static void timer_other_cancel_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (id == 0) return;
    fridge_ui_shell_kitchen_request_t request = { .timer_id = id };
    if (dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_TIMER_CANCEL, &request,
                                "定时器已取消", "取消失败")) {
        refresh_tool_page();
    }
}

static void timer_new_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    switch_page(SHELL_PAGE_TIMER_EDITOR);
}

static void alarm_new_event(lv_event_t *event);

static void header_new_event(lv_event_t *event)
{
    if (s_shell.page == SHELL_PAGE_TIMER) {
        timer_new_event(event);
        return;
    }
    alarm_new_event(event);
}

static void alarm_open_text_editor(bool speech);
static void alarm_show_delete_confirm(void);

static void alarm_queue_action(alarm_pending_action_t action, uint8_t alarm_id)
{
    s_shell.alarm_pending = true;
    s_shell.alarm_pending_action = action;
    s_shell.alarm_pending_id = alarm_id;
    s_shell.alarm_pending_value = 0;
    s_shell.alarm_pending_confirm_us = esp_timer_get_time() + UI_ALARM_CLICK_CONFIRM_US;
}

static void alarm_queue_editor_action(alarm_pending_action_t action, intptr_t value)
{
    s_shell.alarm_pending = true;
    s_shell.alarm_pending_action = action;
    s_shell.alarm_pending_id = 0;
    s_shell.alarm_pending_value = value;
    s_shell.alarm_pending_confirm_us = esp_timer_get_time() + UI_ALARM_CLICK_CONFIRM_US;
}

static void alarm_new_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_action(ALARM_PENDING_NEW, 0);
}

static void alarm_card_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_action(ALARM_PENDING_EDIT, (uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void alarm_toggle_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_action(ALARM_PENDING_TOGGLE, (uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void alarm_editor_adjust_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_ADJUST,
                              (intptr_t)lv_event_get_user_data(event));
}

static void alarm_editor_kind_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_KIND,
                              (intptr_t)lv_event_get_user_data(event));
}

static void alarm_editor_repeat_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_REPEAT,
                              (intptr_t)lv_event_get_user_data(event));
}

static void alarm_editor_weekday_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_WEEKDAY,
                              (intptr_t)lv_event_get_user_data(event));
}

static void alarm_editor_sound_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_SOUND,
                              (intptr_t)lv_event_get_user_data(event));
}

static void alarm_editor_name_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_NAME, 0);
}

static void alarm_editor_speech_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    alarm_queue_editor_action(ALARM_PENDING_EDITOR_SPEECH, 0);
}

static void alarm_editor_cancel_event(lv_event_t *event)
{
    (void)event;
    switch_page(SHELL_PAGE_ALARM);
}

static void alarm_editor_preview_event(lv_event_t *event)
{
    (void)event;
    fridge_ui_shell_kitchen_request_t request = {.alarm_config = &s_shell.alarm_draft};
    (void)dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_ALARM_PREVIEW, &request,
                                  s_shell.alarm_draft.sound == FRIDGE_KITCHEN_ALARM_SOUND_CHIME ?
                                      "正在播放铃声" : "正在生成语音播报",
                                  "提醒声音播放失败");
}

static bool alarm_editor_save(bool force_enable)
{
    fridge_ui_shell_kitchen_request_t request = {
        .alarm_id = s_shell.alarm_editing_id,
        .alarm_config = &s_shell.alarm_draft,
    };
    fridge_ui_shell_kitchen_action_t action = s_shell.alarm_editing_id ?
        FRIDGE_UI_SHELL_KITCHEN_ALARM_UPDATE : FRIDGE_UI_SHELL_KITCHEN_ALARM_CREATE;
    if (!dispatch_kitchen_action(action, &request,
                                 s_shell.alarm_editing_id ? "闹钟已保存" : "闹钟已创建",
                                 "闹钟保存失败或数量已满")) {
        return false;
    }
    if (force_enable && s_shell.alarm_editing_id) {
        request.alarm_enabled = true;
        if (!dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_ALARM_SET_ENABLED, &request,
                                     NULL, "闹钟开启失败")) {
            return false;
        }
    }
    switch_page(SHELL_PAGE_ALARM);
    return true;
}

static void alarm_editor_save_event(lv_event_t *event)
{
    (void)event;
    (void)alarm_editor_save(false);
}

static void alarm_editor_save_enable_event(lv_event_t *event)
{
    (void)event;
    (void)alarm_editor_save(true);
}

static void alarm_editor_delete_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.alarm_editing_id) alarm_show_delete_confirm();
}

static void stopwatch_start_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    (void)dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_START, NULL,
                                  "秒表已开始", "秒表启动失败");
}

static void stopwatch_pause_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    (void)dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_PAUSE, NULL,
                                  "秒表已暂停", "秒表未在运行");
}

// 参考页的中间主按钮是开始/暂停二态；按钮本身不重建，只根据业务快照分派动作。
static void stopwatch_toggle_event(lv_event_t *event)
{
    if (s_shell.kitchen.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        stopwatch_pause_event(event);
    } else {
        stopwatch_start_event(event);
    }
}

static void stopwatch_lap_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    (void)dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_LAP, NULL,
                                  "已记录计次", "计次记录失败");
}

static void stopwatch_reset_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    (void)dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_RESET, NULL,
                                  "秒表已重置", "秒表重置失败");
}

static void add_card_heading(lv_obj_t *card, const char *title, const char *side,
                             lv_obj_t **side_label)
{
    make_label(card, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               12, 8, 150, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    int side_width = lv_obj_get_width(card) - 178;
    // 竖屏根节点刚创建时部分 LVGL 父对象尚未完成一次 layout；此时宽度可能暂为 0，
    // 不能把标题右侧标签创建成负宽度，否则“全部 7 项 / 本周菜单 / 标准模式”会被裁掉。
    if (side_width < 120) side_width = UI_PORTRAIT_CONTENT_W - 178;
    lv_obj_t *label = make_label(card, side ? side : "", FRIDGE_FONT_BODY_SMALL,
                                 FRIDGE_UI_COLOR_MUTED, 166, 10, side_width, LV_SIZE_CONTENT,
                                 LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    if (side_label) *side_label = label;
}

static void create_header(lv_obj_t *main)
{
    lv_obj_t *header = make_box(main, 0, 0, UI_MAIN_W, UI_HEADER_H, FRIDGE_UI_COLOR_PAPER,
                                 FRIDGE_UI_COLOR_LINE, 0);
    s_shell.header = header;
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    // 页头垂直坐标按浏览器参考稿的 88px 页面头复刻；标题、角标和副标题保持同一文字基线。
    s_shell.header_title = make_label(header, "首页", FRIDGE_FONT_TITLE_STRONG, FRIDGE_UI_COLOR_INK,
                                       24, 10, 180, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    // P4 实屏的 18px 字体下，MORE-08 比浏览器小字号参考稿更宽；胶囊留出左右内边距，
    // 避免最后一个字符贴边或被圆角裁掉，同时不改变标题字体。
    s_shell.header_id_box = make_box(header, 86, 27, 78, 22, FRIDGE_UI_COLOR_MINT,
                                     FRIDGE_UI_COLOR_MINT, 4);
    s_shell.header_id = make_label(s_shell.header_id_box, "HOME-01", FRIDGE_FONT_BODY_SMALL,
                                   FRIDGE_UI_COLOR_SAGE_DARK, 2, 0, 74, LV_SIZE_CONTENT,
                                   LV_LABEL_LONG_CLIP);
    center_single_line_label(s_shell.header_id, 74);
    lv_obj_add_flag(s_shell.header_id_box, LV_OBJ_FLAG_HIDDEN);
    s_shell.header_sub = make_label(header, "未校时 · 冰箱一切正常", FRIDGE_FONT_BODY_SMALL,
                                     FRIDGE_UI_COLOR_MUTED, 24, 47, 520, LV_SIZE_CONTENT,
                                     LV_LABEL_LONG_DOT);
    s_shell.header_back = make_button(header, 640, 19, 48, 48, FRIDGE_UI_COLOR_SURFACE,
                                       FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                                       "", FRIDGE_FONT_BODY_SMALL, page_back_event, NULL,
                                       &s_shell.header_back_label);
    register_icon(s_shell.header_back, FRIDGE_UI_ICON_BACK, FRIDGE_UI_COLOR_SAGE_DARK,
                  10, 10, 24, "<");
    center_header_icon(s_shell.header_back);
    lv_obj_add_flag(s_shell.header_back, LV_OBJ_FLAG_HIDDEN);
    s_shell.header_search = make_button(header, 584, 18, 48, 48, FRIDGE_UI_COLOR_SURFACE,
                                         FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                                         "", FRIDGE_FONT_BODY_SMALL, header_search_event,
                                         NULL, NULL);
    register_icon(s_shell.header_search, FRIDGE_UI_ICON_SEARCH, FRIDGE_UI_COLOR_SAGE_DARK,
                  10, 10, 24, "?");
    center_header_icon(s_shell.header_search);
    s_shell.header_plus = make_button(header, 640, 18, 48, 48, FRIDGE_UI_COLOR_SURFACE,
                                       FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                                       "", FRIDGE_FONT_BODY_SMALL, header_plus_event,
                                       NULL, NULL);
    register_icon(s_shell.header_plus, FRIDGE_UI_ICON_PLUS, FRIDGE_UI_COLOR_SAGE_DARK,
                  10, 10, 24, "+");
    center_header_icon(s_shell.header_plus);
    s_shell.header_grid = make_button(header, 640, 18, 48, 48, FRIDGE_UI_COLOR_SURFACE,
                                      FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                                      "", FRIDGE_FONT_BODY_SMALL, header_grid_event,
                                      NULL, NULL);
    register_icon(s_shell.header_grid, FRIDGE_UI_ICON_GRID, FRIDGE_UI_COLOR_SAGE_DARK,
                  10, 10, 24, "格");
    center_header_icon(s_shell.header_grid);
    lv_obj_add_flag(s_shell.header_grid, LV_OBJ_FLAG_HIDDEN);
    // 闹钟管理页采用参考稿中的文字“新建”按钮，和首页的图标快速添加分开，
    // 避免两种语义共用一个控件导致误触或文案叠加。
    s_shell.header_new = make_button(header, 552, 19, 80, 48, FRIDGE_UI_COLOR_SAGE,
                                     0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "新建",
                                     FRIDGE_FONT_BODY, header_new_event, NULL, NULL);
    lv_obj_add_flag(s_shell.header_new, LV_OBJ_FLAG_HIDDEN);
    // 对话页右上角使用独立垃圾桶按钮，不复用库存/首页的新增入口。
    s_shell.header_chat_clear = make_button(header, 640, 18, 48, 48,
                                            FRIDGE_UI_COLOR_DANGER_BG,
                                            FRIDGE_UI_COLOR_TOMATO,
                                            FRIDGE_UI_COLOR_DANGER_BG, "",
                                            FRIDGE_FONT_BODY_SMALL,
                                            header_chat_clear_event, NULL, NULL);
    register_icon(s_shell.header_chat_clear, FRIDGE_UI_ICON_TRASH, FRIDGE_UI_COLOR_TOMATO,
                  10, 10, 24, "删");
    center_header_icon(s_shell.header_chat_clear);
    lv_obj_add_flag(s_shell.header_chat_clear, LV_OBJ_FLAG_HIDDEN);
    // 显式绘制 2px 强分隔线，避免浅色 1px border 经 RGB565 量化后在实屏不可见。
    lv_obj_t *divider = lv_obj_create(header);
    clear_container_style(divider);
    lv_obj_set_pos(divider, 0, UI_HEADER_H - 2);
    lv_obj_set_size(divider, UI_MAIN_W, 2);
    lv_obj_set_style_bg_color(divider, color_hex(FRIDGE_UI_COLOR_LINE_STRONG), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
}

// 摄像头接管双缓冲前的短暂占位反馈。预览启动后由摄像头任务直接接管取景区与底栏热区。
static void camera_placeholder_event(lv_event_t *event)
{
    (void)event;
    fridge_ui_shell_show_toast("正在启动摄像头，请稍候");
}

static void camera_reshoot_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.camera_review_active && s_shell.actions.camera_reshoot) {
        s_shell.actions.camera_reshoot(s_shell.actions.user_ctx);
        return;
    }
    camera_placeholder_event(event);
}

static void camera_capture_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.camera_review_active && s_shell.actions.camera_upload) {
        s_shell.actions.camera_upload(s_shell.actions.user_ctx);
        return;
    }
    camera_placeholder_event(event);
}

static void make_camera_corner(lv_obj_t *parent, int x, int y, lv_border_side_t sides)
{
    lv_obj_t *corner = lv_obj_create(parent);
    clear_container_style(corner);
    lv_obj_set_pos(corner, x, y);
    lv_obj_set_size(corner, 40, 40);
    lv_obj_set_style_border_color(corner, color_hex(FRIDGE_UI_COLOR_YOLK), 0);
    lv_obj_set_style_border_width(corner, 3, 0);
    lv_obj_set_style_border_side(corner, sides, 0);
}

static void create_camera_page(lv_obj_t *main)
{
    s_shell.camera_icon_slot_start = s_shell.icon_slot_count;
    // INV-05 只承载共享顶栏下方的取景与操作区，页头复用首页/更多页的同一对象。
    s_shell.camera_page = make_box(main, 0, UI_HEADER_H, UI_MAIN_W, UI_SCREEN_H - UI_HEADER_H,
                                   FRIDGE_UI_COLOR_PAPER, FRIDGE_UI_COLOR_PAPER, 0);
    lv_obj_set_style_border_width(s_shell.camera_page, 0, 0);

    lv_obj_t *frame = make_box(s_shell.camera_page, 0, 0, UI_MAIN_W,
                               UI_CAMERA_FRAME_H, UI_CAMERA_FRAME_BG, UI_CAMERA_FRAME_BG, 0);
    lv_obj_set_style_border_width(frame, 0, 0);

    // JPEG 继续保留给云端识别；屏幕回看使用抓拍时同步生成的 RGB565 图，
    // 避免在唯一 LVGL 任务里做 JPEG 解码。四角取景框随后创建，保持覆盖在照片上方。
    s_shell.camera_photo = lv_image_create(frame);
    lv_obj_remove_style_all(s_shell.camera_photo);
    lv_obj_set_pos(s_shell.camera_photo, 0, 0);
    lv_obj_set_size(s_shell.camera_photo, UI_MAIN_W, 396);
    lv_image_set_inner_align(s_shell.camera_photo, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(s_shell.camera_photo, LV_OBJ_FLAG_HIDDEN);

    make_camera_corner(frame, 16, 16, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    make_camera_corner(frame, UI_MAIN_W - 56, 16, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT);
    make_camera_corner(frame, 16, UI_CAMERA_FRAME_H - UI_CAMERA_STATUS_H - 48,
                       LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT);
    make_camera_corner(frame, UI_MAIN_W - 56, UI_CAMERA_FRAME_H - UI_CAMERA_STATUS_H - 48,
                       LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);

    // 共享顶栏占用的高度从取景区扣除；该占位只在高清流异步启动前短暂显示。
    s_shell.camera_placeholder = make_box(frame, UI_CAMERA_PLACEHOLDER_X,
                                          UI_CAMERA_PLACEHOLDER_Y, UI_CAMERA_PLACEHOLDER_SIZE,
                                          UI_CAMERA_PLACEHOLDER_SIZE, UI_CAMERA_PLACEHOLDER_BG,
                                          UI_CAMERA_PLACEHOLDER_LINE, 0);
    register_icon(s_shell.camera_placeholder, FRIDGE_UI_ICON_CAMERA, UI_CAMERA_PLACEHOLDER_TEXT,
                  UI_CAMERA_PLACEHOLDER_SIZE / 2 - 16,
                  UI_CAMERA_PLACEHOLDER_SIZE / 2 - 48, 32, "C");
    lv_obj_t *placeholder_text = make_label(s_shell.camera_placeholder, "正在连接摄像头",
                                            FRIDGE_FONT_BODY_SMALL,
                                            UI_CAMERA_PLACEHOLDER_TEXT, 0,
                                            UI_CAMERA_PLACEHOLDER_SIZE / 2 + 8,
                                            UI_CAMERA_PLACEHOLDER_SIZE,
                                            LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(placeholder_text, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *network = make_box(frame, 0, UI_CAMERA_FRAME_H - UI_CAMERA_STATUS_H,
                                 UI_MAIN_W, UI_CAMERA_STATUS_H,
                                 UI_CAMERA_FRAME_BG, UI_CAMERA_FRAME_BG, 0);
    lv_obj_set_style_border_width(network, 0, 0);
    lv_obj_set_style_bg_opa(network, 184, 0);
    s_shell.camera_network_dot = make_box(network, 150, 16, 8, 8, FRIDGE_UI_COLOR_OFFLINE,
                                           0xFFFFFF, 4);
    lv_obj_set_style_border_width(s_shell.camera_network_dot, 1, 0);
    s_shell.camera_network_text = make_label(network, "正在读取网络状态",
                                              FRIDGE_FONT_BODY_SMALL, 0xFFFDF8, 168, 8, 390,
                                              LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    set_single_line_content_height(s_shell.camera_network_text);
    lv_obj_set_style_text_align(s_shell.camera_network_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_shell.camera_network_text, LV_ALIGN_LEFT_MID, 168, 0);

    lv_obj_t *footer = make_box(s_shell.camera_page, 0, UI_CAMERA_FRAME_H, UI_MAIN_W,
                                UI_CAMERA_FOOTER_H, FRIDGE_UI_COLOR_PAPER,
                                FRIDGE_UI_COLOR_LINE, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_t *reshoot_text = NULL;
    lv_obj_t *reshoot = make_button(footer, 24, 12, 132, 48, FRIDGE_UI_COLOR_SURFACE,
                                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                                    "重拍", FRIDGE_FONT_BODY, camera_reshoot_event, NULL,
                                    &reshoot_text);
    register_icon(reshoot, FRIDGE_UI_ICON_RETRY, FRIDGE_UI_COLOR_SAGE_DARK, 24, 10, 22, "R");
    if (reshoot_text) {
        lv_obj_set_width(reshoot_text, 48);
        set_single_line_content_height(reshoot_text);
        lv_obj_set_style_text_align(reshoot_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(reshoot_text, LV_ALIGN_LEFT_MID, 60, 0);
    }
    s_shell.camera_capture_button = make_button(footer, 284, 12, 136, 48,
                                                FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                                                FRIDGE_UI_COLOR_SAGE, "拍摄", FRIDGE_FONT_BODY,
                                                camera_capture_event, NULL,
                                                &s_shell.camera_capture_label);
    register_icon(s_shell.camera_capture_button, FRIDGE_UI_ICON_CAMERA, 0xFFFFFF,
                  26, 10, 22, "C");
    if (s_shell.camera_capture_label) {
        lv_obj_set_width(s_shell.camera_capture_label, 48);
        set_single_line_content_height(s_shell.camera_capture_label);
        lv_obj_set_style_text_align(s_shell.camera_capture_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_shell.camera_capture_label, LV_ALIGN_LEFT_MID, 62, 0);
    }
    lv_obj_t *cancel_text = NULL;
    lv_obj_t *cancel = make_button(footer, 580, 12, 100, 48, FRIDGE_UI_COLOR_SURFACE,
                                   FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消",
                                   FRIDGE_FONT_BODY, page_back_event, NULL, &cancel_text);
    register_icon(cancel, FRIDGE_UI_ICON_BACK, FRIDGE_UI_COLOR_SAGE_DARK, 8, 10, 22, "<");
    if (cancel_text) {
        lv_obj_set_width(cancel_text, 48);
        set_single_line_content_height(cancel_text);
        lv_obj_set_style_text_align(cancel_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(cancel_text, LV_ALIGN_LEFT_MID, 44, 0);
    }
}

static void create_stat_cards(lv_obj_t *content)
{
    const fridge_ui_icon_id_t icons[] = {
        FRIDGE_UI_ICON_BOX, FRIDGE_UI_ICON_CLOCK, FRIDGE_UI_ICON_WARN, FRIDGE_UI_ICON_LOCATION,
    };
    const char *fallback[] = {"I", "T", "!", "@"};
    for (size_t i = 0; i < FRIDGE_UI_HOME_STATS_MAX; i++) {
        int x = (int)i * 170;
        lv_obj_t *card = make_box(content, x, 0, 162, UI_STAT_CARD_H, FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        if (i == 1U || i == 2U) {
            add_click_event(card, nav_event, (void *)(uintptr_t)SHELL_PAGE_HOME_DETAIL);
        } else {
            add_click_event(card, unsupported_event,
                            i == 0U ? "库存食材详情" : "待补充位置详情");
        }
        register_icon(card, icons[i], tone_color(s_shell.home.stats[i].tone),
                      12, 6, 24, fallback[i]);
        s_shell.stats[i].number = make_label(card, "0", FRIDGE_FONT_SECTION_STRONG,
                                              FRIDGE_UI_COLOR_INK, 12, 31, 136,
                                              LV_SIZE_CONTENT,
                                              LV_LABEL_LONG_CLIP);
        s_shell.stats[i].label = make_label(card, "", FRIDGE_FONT_BODY_SMALL,
                                            FRIDGE_UI_COLOR_MUTED, 12, 77, 138,
                                             LV_SIZE_CONTENT,
                                             LV_LABEL_LONG_DOT);
        // 网页参考稿的数字字形底部到说明字形顶部约 16px；
        // LVGL 字体度量需把标签对象再下移 8px 才能取得相同视觉间隔。
        align_card_bottom_label(s_shell.stats[i].label, 12, 138,
                                UI_STAT_LABEL_BOTTOM_INSET);
    }
}

static void create_expiring_card(lv_obj_t *content)
{
    lv_obj_t *card = make_box(content, 0, UI_HOME_COLUMNS_Y, 353, 330,
                              FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_LINE, 8);
    add_card_heading(card, "临期优先", "全部 7 项 >", &s_shell.expiring_side);
    lv_obj_add_flag(s_shell.expiring_side, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(s_shell.expiring_side, nav_event,
                    (void *)(uintptr_t)SHELL_PAGE_HOME_DETAIL);
    for (size_t i = 0; i < FRIDGE_UI_HOME_EXPIRING_MAX; i++) {
        int y = 50 + (int)i * 80;
        expiring_view_t *view = &s_shell.expiring[i];
        view->row = make_box(card, 12, y, 329, 76, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(view->row, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(view->row, unsupported_event, "食材详情");
        lv_obj_t *food_box = make_box(view->row, 8, 16, 40, 40, FRIDGE_UI_COLOR_CREAM,
                                      FRIDGE_UI_COLOR_CREAM, 8);
        register_icon(food_box, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_SAGE_DARK,
                      6, 6, 22, "F");
        view->name = make_label(view->row, "", FRIDGE_FONT_BODY_EMPHASIS,
                                FRIDGE_UI_COLOR_INK,
                                56, 4, 100, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        view->meta = make_label(view->row, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                 56, 39, 150, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        // 网页参考稿正文距行卡底边约 10～11px；FreeType 字形在对象内自带下缘。
        // 保留 1px 对象边界可维持 PPA 快照偶数高度，视觉误差不超过 1px。
        align_card_bottom_label(view->meta, 56, 150,
                                UI_EXPIRING_META_BOTTOM_INSET);
        view->badge_box = make_box(view->row, 160, 3, 48, 30, FRIDGE_UI_COLOR_WARN_BG,
                                  FRIDGE_UI_COLOR_WARN_BG, 15);
        view->badge = make_label(view->badge_box, "", FRIDGE_FONT_BODY_SMALL,
                                 FRIDGE_UI_COLOR_WARN_INK, 3, 0, 42, LV_SIZE_CONTENT,
                                 LV_LABEL_LONG_DOT);
        center_single_line_label(view->badge, 42);
        view->expiry_box = make_box(view->row, 211, 20, 84, 32, FRIDGE_UI_COLOR_DANGER_BG,
                                   FRIDGE_UI_COLOR_DANGER_BG, 16);
        view->expiry = make_label(view->expiry_box, "",
                                  FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                  FRIDGE_UI_COLOR_TOMATO, 2, 0, 80, LV_SIZE_CONTENT,
                                  LV_LABEL_LONG_DOT);
        center_single_line_label(view->expiry, 80);
        register_icon(view->row, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED, 297, 22, 18, ">");
    }
}

static void create_recommendation_card(lv_obj_t *content)
{
    lv_obj_t *card = make_box(content, 365, UI_HOME_COLUMNS_Y, 307, 330,
                              FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *menu_side = NULL;
    add_card_heading(card, "今日推荐", "本周菜单 >", &menu_side);
    lv_obj_add_flag(menu_side, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(menu_side, nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN);
    lv_obj_t *recipe = make_box(card, 12, 50, 283, 130, FRIDGE_UI_COLOR_SURFACE,
                                FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(recipe, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(recipe, nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN);
    lv_obj_t *placeholder = make_box(recipe, 12, 12, 72, 104, FRIDGE_UI_COLOR_CREAM,
                                     FRIDGE_UI_COLOR_LINE_STRONG, 8);
    register_icon(placeholder, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_MUTED, 22, 12, 28, "F");
    lv_obj_t *placeholder_text = make_label(placeholder, "番茄", FRIDGE_FONT_BODY_SMALL,
                                             FRIDGE_UI_COLOR_MUTED, 6, 72, 60,
                                             LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(placeholder_text, LV_TEXT_ALIGN_CENTER, 0);
    align_card_bottom_label(placeholder_text, 6, 60, 8);
    lv_obj_set_style_text_align(placeholder_text, LV_TEXT_ALIGN_CENTER, 0);
    s_shell.rec_title = make_label(recipe, "", FRIDGE_FONT_BODY_EMPHASIS,
                                   FRIDGE_UI_COLOR_INK,
                                   96, 7, 150, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(recipe, FRIDGE_UI_ICON_STAR, FRIDGE_UI_COLOR_YOLK, 246, 10, 18, "*");
    // 推荐摘要拆成三个单行标签，固定 25px 节奏，避免 FreeType 多行行框与状态行挤压。
    s_shell.rec_meta = make_label(recipe, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                   96, 43, 180, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.rec_coverage = make_label(recipe, "", FRIDGE_FONT_BODY_SMALL,
                                       FRIDGE_UI_COLOR_MUTED, 96, 69, 180,
                                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.rec_ready = make_label(recipe, "", FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_SAGE_DARK, 96, 95, 170,
                                    LV_SIZE_CONTENT,
                                    LV_LABEL_LONG_DOT);
    lv_obj_t *reason_box = make_box(card, 12, 190, 283, 76, FRIDGE_UI_COLOR_CREAM,
                                    FRIDGE_UI_COLOR_CREAM, 8);
    register_icon(reason_box, FRIDGE_UI_ICON_INFO, FRIDGE_UI_COLOR_SAGE_DARK, 10, 8, 22, "i");
    s_shell.rec_reason = make_label(reason_box, "", FRIDGE_FONT_BODY_SMALL,
                                     FRIDGE_UI_COLOR_MUTED, 40, 8, 233, 60,
                                     LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.rec_reason);
    make_button(card, 12, 274, 137, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN, &s_shell.rec_primary);
    make_button(card, 157, 274, 138, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG, "",
                FRIDGE_FONT_BODY_SMALL_EMPHASIS, nav_event,
                (void *)(uintptr_t)SHELL_PAGE_PLAN,
                &s_shell.rec_secondary);
}

static void create_agent_tips(lv_obj_t *content)
{
    lv_obj_t *card = make_box(content, 0, UI_HOME_TIPS_Y, 672, 226,
                              FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_LINE, 8);
    add_card_heading(card, "小精灵建议", "标准模式", NULL);
    for (size_t i = 0; i < FRIDGE_UI_HOME_TIPS_MAX; i++) {
        int y = 48 + (int)i * 84;
        lv_obj_t *row = make_box(card, 12, y, 648, 76, FRIDGE_UI_COLOR_CREAM,
                                 FRIDGE_UI_COLOR_CREAM, 8);
        lv_obj_t *icon = make_box(row, 10, 20, 36, 36, FRIDGE_UI_COLOR_MINT,
                                  FRIDGE_UI_COLOR_MINT, 18);
        register_icon(icon, FRIDGE_UI_ICON_ZAP, FRIDGE_UI_COLOR_SAGE_DARK, 4, 4, 22, "!");
        s_shell.tip_text[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_INK, 56, 8, 404, 60,
                                         LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(s_shell.tip_text[i]);
        make_button(row, 468, 14, 128, 48, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG, "",
                    FRIDGE_FONT_BODY_SMALL_EMPHASIS, nav_event,
                    (void *)(uintptr_t)FRIDGE_UI_P4_PAGE_CHAT,
                    &s_shell.tip_action[i]);
        lv_obj_t *close = make_button(row, 604, 14, 36, 48, FRIDGE_UI_COLOR_CREAM,
                                      FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_CREAM, "",
                                      FRIDGE_FONT_BODY_SMALL, unsupported_event, "忽略", NULL);
        register_icon(close, FRIDGE_UI_ICON_CLOSE, FRIDGE_UI_COLOR_MUTED, 4, 10, 20, "x");
    }
}

static void create_home_summaries(lv_obj_t *content)
{
    const fridge_ui_icon_id_t icons[] = {FRIDGE_UI_ICON_PLAN, FRIDGE_UI_ICON_CART};
    const char *titles[] = {"今日菜单", "购物清单"};
    const char *fallback[] = {"P", "C"};
    for (size_t i = 0; i < 2; i++) {
        int x = (int)i * 342;
        lv_obj_t *card = make_box(content, x, UI_HOME_SUMMARIES_Y, 330, 80,
                                  FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(card, nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN);
        register_icon(card, icons[i], FRIDGE_UI_COLOR_SAGE_DARK, 12, 24, 24, fallback[i]);
        make_label(card, titles[i], FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                   48, 6, 230, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_t *value = make_label(card, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                      48, 42, 240, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        align_card_bottom_label(value, 48, 240, UI_CARD_BOTTOM_INSET);
        if (i == 0) s_shell.dinner = value;
        else s_shell.shopping = value;
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED, 294, 24, 18, ">");
    }
    lv_obj_t *dot = make_box(content, 4, 798, 10, 10, FRIDGE_UI_COLOR_SAGE,
                             FRIDGE_UI_COLOR_SAGE, 5);
    (void)dot;
    s_shell.sync_line = make_label(content, "", FRIDGE_FONT_BODY_SMALL,
                                   FRIDGE_UI_COLOR_MUTED, 20, 790, 640,
                                   LV_SIZE_CONTENT,
                                   LV_LABEL_LONG_DOT);
    // 末尾透明占位撑开滚动高度，HOME 主区只有这一层可滚动。
    lv_obj_t *spacer = lv_obj_create(content);
    clear_container_style(spacer);
    lv_obj_set_pos(spacer, 0, 834);
    lv_obj_set_size(spacer, 1, 8);
}

// HOME-01 竖屏统计区：按 ver2 的 2x2 网格排布，卡片高度保留真实字体行高和 48px 触控余量。
static void create_portrait_home_stats(lv_obj_t *content)
{
    const fridge_ui_icon_id_t icons[] = {
        FRIDGE_UI_ICON_BOX, FRIDGE_UI_ICON_CLOCK, FRIDGE_UI_ICON_WARN, FRIDGE_UI_ICON_LOCATION,
    };
    const char *fallback[] = {"I", "T", "!", "@"};
    for (size_t i = 0; i < FRIDGE_UI_HOME_STATS_MAX; i++) {
        int x = (int)(i % 2U) * 288;
        int y = (int)(i / 2U) * (UI_PORTRAIT_HOME_STAT_H + UI_PORTRAIT_HOME_STAT_GAP);
        lv_obj_t *card = make_box(content, x, y, 280, UI_PORTRAIT_HOME_STAT_H,
                                  FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        if (i == 1U || i == 2U) {
            // 临期和过期仍在两个已移植首页之间切换，保持竖屏隐私模式不闪回横屏。
            add_click_event(card, portrait_nav_event,
                            (void *)(uintptr_t)SHELL_PAGE_HOME_DETAIL);
        } else {
            add_click_event(card, unsupported_event,
                            i == 0U ? "库存食材详情" : "待补充位置详情");
        }
        register_icon(card, icons[i], tone_color(s_shell.home.stats[i].tone),
                      12, 8, 24, fallback[i]);
        s_shell.portrait_home.stats[i].number =
            make_label(card, "0", FRIDGE_FONT_SECTION_STRONG, FRIDGE_UI_COLOR_INK,
                       12, 42, 256, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        s_shell.portrait_home.stats[i].label =
            make_label(card, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                       12, 80, 256, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        align_card_bottom_label(s_shell.portrait_home.stats[i].label, 12, 256, 8);
    }
}

// HOME-01 竖屏临期卡：宽屏单行仍保留 ver2 的食材、位置、标签、到期胶囊和箭头层级。
static void create_portrait_home_expiring(lv_obj_t *content)
{
    lv_obj_t *card = make_box(content, 0, UI_PORTRAIT_HOME_EXP_Y,
                              UI_PORTRAIT_CONTENT_W, UI_PORTRAIT_HOME_EXP_H,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    add_card_heading(card, "临期优先", "全部 7 项 >",
                     &s_shell.portrait_home.expiring_side);
    lv_obj_add_flag(s_shell.portrait_home.expiring_side, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(s_shell.portrait_home.expiring_side, portrait_nav_event,
                    (void *)(uintptr_t)SHELL_PAGE_HOME_DETAIL);
    for (size_t i = 0; i < FRIDGE_UI_HOME_EXPIRING_MAX; i++) {
        int y = 50 + (int)i * 84;
        expiring_view_t *view = &s_shell.portrait_home.expiring[i];
        view->row = make_box(card, 12, y, 544, 76, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(view->row, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(view->row, unsupported_event, "食材详情");
        lv_obj_t *food_box = make_box(view->row, 8, 16, 40, 40,
                                      FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 8);
        register_icon(food_box, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_SAGE_DARK,
                      6, 6, 22, "F");
        view->name = make_label(view->row, "", FRIDGE_FONT_BODY_EMPHASIS,
                                FRIDGE_UI_COLOR_INK, 56, 4, 100, LV_SIZE_CONTENT,
                                LV_LABEL_LONG_DOT);
        view->meta = make_label(view->row, "", FRIDGE_FONT_BODY_SMALL,
                                FRIDGE_UI_COLOR_MUTED, 56, 39, 340,
                                LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        align_card_bottom_label(view->meta, 56, 340, 1);
        view->badge_box = make_box(view->row, 160, 3, 48, 30,
                                   FRIDGE_UI_COLOR_WARN_BG, FRIDGE_UI_COLOR_WARN_BG, 15);
        view->badge = make_label(view->badge_box, "", FRIDGE_FONT_BODY_SMALL,
                                 FRIDGE_UI_COLOR_WARN_INK, 3, 0, 42,
                                 LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        center_single_line_label(view->badge, 42);
        view->expiry_box = make_box(view->row, 408, 22, 100, 32,
                                    FRIDGE_UI_COLOR_DANGER_BG,
                                    FRIDGE_UI_COLOR_DANGER_BG, 16);
        view->expiry = make_label(view->expiry_box, "",
                                  FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                  FRIDGE_UI_COLOR_TOMATO, 2, 0, 96,
                                  LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        center_single_line_label(view->expiry, 96);
        register_icon(view->row, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      518, 24, 18, ">");
    }
}

// HOME-01 竖屏推荐卡：菜谱缩略图收窄为 72px，理由和两个小按钮按 ver2 单列堆叠。
static void create_portrait_home_recommendation(lv_obj_t *content)
{
    lv_obj_t *card = make_box(content, 0, UI_PORTRAIT_HOME_REC_Y,
                              UI_PORTRAIT_CONTENT_W, UI_PORTRAIT_HOME_REC_H,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *menu_side = NULL;
    add_card_heading(card, "今日推荐", "本周菜单 >", &menu_side);
    lv_obj_add_flag(menu_side, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(menu_side, portrait_nav_event,
                    (void *)(uintptr_t)SHELL_PAGE_PLAN);

    lv_obj_t *recipe = make_box(card, 12, 50, 544, 96,
                                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(recipe, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(recipe, portrait_nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN);
    lv_obj_t *placeholder = make_box(recipe, 12, 12, 72, 72,
                                     FRIDGE_UI_COLOR_CREAM,
                                     FRIDGE_UI_COLOR_LINE_STRONG, 8);
    register_icon(placeholder, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_MUTED,
                  22, 8, 28, "F");
    lv_obj_t *placeholder_text = make_label(placeholder, "番茄", FRIDGE_FONT_BODY_SMALL,
                                            FRIDGE_UI_COLOR_MUTED, 6, 44, 60,
                                            LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(placeholder_text, LV_TEXT_ALIGN_CENTER, 0);
    align_card_bottom_label(placeholder_text, 6, 60, 6);
    lv_obj_set_style_text_align(placeholder_text, LV_TEXT_ALIGN_CENTER, 0);
    s_shell.portrait_home.rec_title = make_label(
        recipe, "", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
        96, 6, 380, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(recipe, FRIDGE_UI_ICON_STAR, FRIDGE_UI_COLOR_YOLK,
                  500, 10, 18, "*");
    // 按 ver2 菜谱卡规则把时间、难度和库存覆盖率合并为一行，给状态行留出底部安全区。
    // 不增加卡片高度，避免推荐卡下方的理由与按钮整体下移，保持当前滚动节奏和触摸区域。
    s_shell.portrait_home.rec_meta = make_label(
        recipe, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        96, 34, 430, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.portrait_home.rec_coverage = NULL;
    s_shell.portrait_home.rec_ready = make_label(
        recipe, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_SAGE_DARK,
        96, 58, 430, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    lv_obj_t *reason_box = make_box(card, 12, 154, 544, 60,
                                    FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 8);
    register_icon(reason_box, FRIDGE_UI_ICON_INFO, FRIDGE_UI_COLOR_SAGE_DARK,
                  10, 8, 22, "i");
    s_shell.portrait_home.rec_reason = make_label(
        reason_box, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        40, 7, 492, 48, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.portrait_home.rec_reason);

    make_button(card, 12, 226, 266, 40, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                portrait_nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN,
                &s_shell.portrait_home.rec_primary);
    make_button(card, 290, 226, 266, 40, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG, "",
                FRIDGE_FONT_BODY_SMALL_EMPHASIS, portrait_nav_event,
                (void *)(uintptr_t)SHELL_PAGE_PLAN,
                &s_shell.portrait_home.rec_secondary);
}

// HOME-01 竖屏建议、摘要和同步信息全部放进同一个主滚动区，禁止嵌套滚动容器。
static void create_portrait_home_tips_and_summaries(lv_obj_t *content)
{
    lv_obj_t *tips = make_box(content, 0, UI_PORTRAIT_HOME_TIPS_Y,
                              UI_PORTRAIT_CONTENT_W, UI_PORTRAIT_HOME_TIPS_H,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    add_card_heading(tips, "Agent 建议", "标准模式", NULL);
    for (size_t i = 0; i < FRIDGE_UI_HOME_TIPS_MAX; i++) {
        int y = 48 + (int)i * 84;
        lv_obj_t *row = make_box(tips, 12, y, 544, 76,
                                 FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 8);
        lv_obj_t *icon = make_box(row, 10, 20, 36, 36,
                                  FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 18);
        register_icon(icon, FRIDGE_UI_ICON_ZAP, FRIDGE_UI_COLOR_SAGE_DARK,
                      4, 4, 22, "!");
        s_shell.portrait_home.tip_text[i] = make_label(
            row, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK,
            56, 8, 292, 60, LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(s_shell.portrait_home.tip_text[i]);
        make_button(row, 356, 14, 128, 48, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG, "",
                    FRIDGE_FONT_BODY_SMALL_EMPHASIS, portrait_nav_event,
                    (void *)(uintptr_t)FRIDGE_UI_P4_PAGE_CHAT,
                    &s_shell.portrait_home.tip_action[i]);
        lv_obj_t *close = make_button(row, 496, 14, 36, 48,
                                      FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_MUTED,
                                      FRIDGE_UI_COLOR_CREAM, "", FRIDGE_FONT_BODY_SMALL,
                                      unsupported_event, "忽略", NULL);
        register_icon(close, FRIDGE_UI_ICON_CLOSE, FRIDGE_UI_COLOR_MUTED,
                      4, 10, 20, "x");
    }

    const fridge_ui_icon_id_t icons[] = {FRIDGE_UI_ICON_PLAN, FRIDGE_UI_ICON_CART};
    const char *titles[] = {"今日菜单", "购物清单"};
    const char *fallback[] = {"P", "C"};
    for (size_t i = 0; i < 2; i++) {
        int x = (int)i * 290;
        lv_obj_t *card = make_box(content, x, UI_PORTRAIT_HOME_MINI_Y, 278,
                                  UI_PORTRAIT_HOME_MINI_H, FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(card, portrait_nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN);
        register_icon(card, icons[i], FRIDGE_UI_COLOR_SAGE_DARK, 12, 24, 24, fallback[i]);
        make_label(card, titles[i], FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                   48, 6, 190, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_t *value = make_label(card, "", FRIDGE_FONT_BODY_SMALL,
                                     FRIDGE_UI_COLOR_MUTED, 48, 42, 190,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        align_card_bottom_label(value, 48, 190, UI_CARD_BOTTOM_INSET);
        if (i == 0) s_shell.portrait_home.dinner = value;
        else s_shell.portrait_home.shopping = value;
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED, 242, 24, 18, ">");
    }

    lv_obj_t *dot = make_box(content, 4, UI_PORTRAIT_HOME_SYNC_Y + 7, 10, 10,
                             FRIDGE_UI_COLOR_SAGE, FRIDGE_UI_COLOR_SAGE, 5);
    (void)dot;
    s_shell.portrait_home.sync_line = make_label(
        content, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        20, UI_PORTRAIT_HOME_SYNC_Y, 544, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *spacer = lv_obj_create(content);
    clear_container_style(spacer);
    lv_obj_set_pos(spacer, 0, 1248);
    lv_obj_set_size(spacer, 1, 8);
}

static void create_portrait_home_page(lv_obj_t *content)
{
    create_portrait_home_stats(content);
    create_portrait_home_expiring(content);
    create_portrait_home_recommendation(content);
    create_portrait_home_tips_and_summaries(content);
}

// PLAN-01 竖屏统计只从现有计划模型按 group 计算，不补写 ver2 中不存在的购物业务数据。
static size_t count_plan_shopping_group(const plan_business_model_t *model,
                                        const char *group)
{
    if (!model || !group) return 0;
    size_t count = 0;
    for (size_t i = 0; i < model->shopping_count && i < FRIDGE_UI_PLAN_SHOPPING_MAX; i++) {
        if (strcmp(model->shopping[i].group, group) == 0) count++;
    }
    return count;
}

// PLAN-01 竖屏使用 ver2 的单列结构：今日菜单、购物摘要、临期利用、本周菜单、Agent 建议。
// 所有未移植子页只显示提示，不在隐私竖屏中切换方向或伪造页面内容。
static void create_portrait_plan_page(lv_obj_t *content)
{
    const plan_business_model_t *m = &s_shell.plan;
    char side_text[64];
    int y = 0;

    // 今日菜单卡：四行纵向排布，长菜名允许在行内换行但预留完整行高。
    lv_obj_t *today = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W,
                               UI_PORTRAIT_PLAN_TODAY_H,
                               FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_clear_flag(today, LV_OBJ_FLAG_CLICKABLE);
    snprintf(side_text, sizeof(side_text), "%s · 编辑 >",
             m->week_range[0] ? m->week_range : "本周菜单");
    lv_obj_t *today_side = NULL;
    add_card_heading(today, "今日菜单", side_text, &today_side);
    if (today_side) {
        lv_obj_add_flag(today_side, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(today_side, unsupported_event, (void *)"日/周菜单");
    }
    for (size_t i = 0; i < FRIDGE_UI_PLAN_MEAL_SLOTS; i++) {
        const int row_y = 44 + (int)i * 52;
        lv_obj_t *row = make_box(today, 12, row_y, 544, 44,
                                 FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        make_label(row, m->today_meals[i].slot, FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 12, 8, 70, 28, LV_LABEL_LONG_DOT);
        s_shell.portrait_plan.today_dishes[i] = make_label(
            row, m->today_meals[i].dish, FRIDGE_FONT_BODY_EMPHASIS,
            FRIDGE_UI_COLOR_INK, 94, 6, 430, 32, LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(s_shell.portrait_plan.today_dishes[i]);
    }

    // 购物摘要卡：待购买/建议购买数量按 shopping[].group 实时计算。
    y += UI_PORTRAIT_PLAN_TODAY_H + UI_PORTRAIT_PLAN_GAP;
    lv_obj_t *shopping = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W,
                                   UI_PORTRAIT_PLAN_SHOP_H,
                                   FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_clear_flag(shopping, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *shopping_side = NULL;
    add_card_heading(shopping, "购物清单", "全部 >", &shopping_side);
    if (shopping_side) {
        lv_obj_add_flag(shopping_side, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(shopping_side, unsupported_event, (void *)"购物清单");
    }
    const fridge_ui_icon_id_t shopping_icons[] = {
        FRIDGE_UI_ICON_CART, FRIDGE_UI_ICON_INFO,
    };
    const uint32_t shopping_colors[] = {
        FRIDGE_UI_COLOR_YOLK, FRIDGE_UI_COLOR_SAGE,
    };
    const char *shopping_labels[] = {"待购买", "建议购买"};
    const char *shopping_groups[] = {"待购买", "建议购买"};
    for (size_t i = 0; i < 2; i++) {
        // 统计卡底部保留 8px 安全区；TF 字体真实行高较浏览器回退字体更高，
        // 不能让“待购买/建议购买”贴到下边框，否则实屏会出现文字和边线裁切。
        lv_obj_t *stat = make_box(shopping, 12 + (int)i * 280, 44, 264, 116,
                                  FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(stat, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(stat, unsupported_event, (void *)"购物清单");
        register_icon(stat, shopping_icons[i], shopping_colors[i], 14, 12, 24,
                      i == 0 ? "C" : "i");
        char count_text[12];
        snprintf(count_text, sizeof(count_text), "%u",
                 (unsigned)count_plan_shopping_group(m, shopping_groups[i]));
        s_shell.portrait_plan.shopping_values[i] = make_label(
            stat, count_text, FRIDGE_FONT_SECTION_STRONG, FRIDGE_UI_COLOR_INK,
            14, 43, 236, 38, LV_LABEL_LONG_CLIP);
        make_label(stat, shopping_labels[i], FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 14, 82, 236, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
    }

    // 临期利用计划卡：只展示当前模型已有的名称和到期标签，不拼接虚构的菜谱安排。
    y += UI_PORTRAIT_PLAN_SHOP_H + UI_PORTRAIT_PLAN_GAP;
    lv_obj_t *expiring = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W,
                                  UI_PORTRAIT_PLAN_EXPIRE_H,
                                  FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_clear_flag(expiring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *expiring_side = NULL;
    add_card_heading(expiring, "临期利用计划", "优先消耗", &expiring_side);
    if (expiring_side) {
        lv_obj_add_flag(expiring_side, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(expiring_side, unsupported_event, (void *)"菜谱详情");
    }
    for (size_t i = 0; i < FRIDGE_UI_PLAN_EXPIRING_MAX; i++) {
        const int row_y = 46 + (int)i * 66;
        lv_obj_t *row = make_box(expiring, 12, row_y, 544, 58,
                                 FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(row, unsupported_event, (void *)"菜谱详情");
        lv_obj_t *food = make_box(row, 8, 11, 36, 36,
                                  FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 8);
        register_icon(food, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_SAGE_DARK,
                      6, 6, 22, "F");
        s_shell.portrait_plan.expiring_names[i] = make_label(
            row, i < m->expiring_count ? m->expiring[i].name : "",
            FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
            56, 7, 300, 42, LV_LABEL_LONG_DOT);
        s_shell.portrait_plan.expiring_pills[i] = make_label(
            row, i < m->expiring_count ? m->expiring[i].badge : "",
            FRIDGE_FONT_BODY_SMALL_EMPHASIS, FRIDGE_UI_COLOR_TOMATO,
            374, 14, 132, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_shell.portrait_plan.expiring_pills[i],
                                    LV_TEXT_ALIGN_RIGHT, 0);
        register_icon(row, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      518, 20, 18, ">");
    }

    // 本周菜单卡：7 个单行 mini day 在 568px 正文中完整保留；不嵌套竖向滚动。
    y += UI_PORTRAIT_PLAN_EXPIRE_H + UI_PORTRAIT_PLAN_GAP;
    lv_obj_t *week = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W,
                              UI_PORTRAIT_PLAN_WEEK_H,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_clear_flag(week, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *week_side = NULL;
    add_card_heading(week, "本周菜单", m->week_range, &week_side);
    if (week_side) {
        lv_obj_add_flag(week_side, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(week_side, unsupported_event, (void *)"日/周菜单");
    }
    const int mini_width = 72;
    const int mini_gap = 4;
    for (size_t i = 0; i < FRIDGE_UI_PLAN_WEEK_DAYS; i++) {
        const bool today_flag = m->week[i].today;
        const bool conflict = m->week[i].conflict;
        lv_obj_t *day = make_box(week, 12 + (int)i * (mini_width + mini_gap),
                                 44, mini_width, 78,
                                 today_flag ? FRIDGE_UI_COLOR_MINT : FRIDGE_UI_COLOR_SURFACE,
                                 today_flag ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE,
                                 8);
        lv_obj_add_flag(day, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(day, unsupported_event, (void *)"日/周菜单");
        s_shell.portrait_plan.week_boxes[i] = day;
        s_shell.portrait_plan.week_dates[i] = make_label(
            day, m->week[i].date, FRIDGE_FONT_BODY_SMALL_EMPHASIS,
            today_flag ? FRIDGE_UI_COLOR_SAGE_DARK : FRIDGE_UI_COLOR_MUTED,
            2, 6, 68, 20, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_shell.portrait_plan.week_dates[i],
                                    LV_TEXT_ALIGN_CENTER, 0);
        s_shell.portrait_plan.week_labels[i] = make_label(
            day, m->week[i].summary, FRIDGE_FONT_BODY_SMALL,
            FRIDGE_UI_COLOR_INK, 3, 30, 66, 40, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_shell.portrait_plan.week_labels[i],
                                    LV_TEXT_ALIGN_CENTER, 0);
        s_shell.portrait_plan.week_conflict_dots[i] = make_box(
            day, 58, 4, 8, 8, FRIDGE_UI_COLOR_TOMATO,
            FRIDGE_UI_COLOR_TOMATO, 4);
        if (!conflict) lv_obj_add_flag(s_shell.portrait_plan.week_conflict_dots[i],
                                       LV_OBJ_FLAG_HIDDEN);
    }

    // Agent 建议卡只保留已有 tip 文本和现有“提示”入口；没有计划采纳 Runtime 时不伪造成功。
    y += UI_PORTRAIT_PLAN_WEEK_H + UI_PORTRAIT_PLAN_GAP;
    lv_obj_t *agent = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W,
                               UI_PORTRAIT_PLAN_AGENT_H,
                               FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 8);
    lv_obj_clear_flag(agent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *agent_icon = make_box(agent, 12, 16, 44, 44,
                                    FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SURFACE, 22);
    register_icon(agent_icon, FRIDGE_UI_ICON_ZAP, FRIDGE_UI_COLOR_SAGE_DARK,
                  10, 10, 24, "!");
    s_shell.portrait_plan.agent_tip = make_label(
        agent, m->agent_tip.text, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK,
        68, 14, 480, 58, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.portrait_plan.agent_tip);
    make_button(agent, 388, 108, 156, 44, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE,
                m->agent_tip.action[0] ? m->agent_tip.action : "去确认",
                FRIDGE_FONT_BODY_SMALL_EMPHASIS, unsupported_event,
                (void *)"Agent 计划操作", NULL);
}

// HOME-02 仅保存参考稿演示数据；数组有界且不接入库存写入，避免页面移植扩大业务范围。
typedef struct {
    const char *name;
    const char *meta;
    const char *badge;
    const char *expiry;
    bool danger;
} home_detail_food_t;

static const home_detail_food_t s_home_detail_expired[] = {
    {"剩米饭", "左冷藏/第1层 · 1盒", NULL, "已过期1天", true},
    {"吐司面包", "左冷藏/第1层 · 6片", "未同步", "已过期3天", true},
};

static const home_detail_food_t s_home_detail_today[] = {
    {"原味酸奶", "右冷藏/第2层/B2+B3 奶制品位 · 4杯", "估算", "今天到期", true},
    {"卤牛肉", "左冷藏/第2层 · 200g", NULL, "今天到期", true},
};

static const home_detail_food_t s_home_detail_soon[] = {
    {"菠菜", "果蔬抽屉/抽屉 · 1把", NULL, "明天到期", false},
    {"内酯豆腐", "未定位 · 1盒", NULL, "明天到期", false},
    {"鲜牛奶", "右冷藏/第2层/B2+B3 奶制品位 · 950ml", NULL, "2天后到期", false},
    {"某绿叶菜", "未定位 · 1把", "低置信", "2天后到期", false},
    {"番茄", "果蔬抽屉/抽屉 · 3个", NULL, "3天后到期", false},
};

// 分组标题复刻网页 sec-title：22px 强调字，右侧 18px 辅助说明。
static void create_home_detail_section_title(lv_obj_t *parent, int y,
                                             const char *title, const char *side,
                                             uint32_t side_color, int width)
{
    make_label(parent, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, y, 360, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (side && side[0]) {
        lv_obj_t *label = make_label(parent, side, FRIDGE_FONT_BODY_SMALL, side_color,
                                     width - 300, y + 2, 300, LV_SIZE_CONTENT,
                                     LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

// 食材行使用同一 76px 几何与胶囊色板；点击只提示库存详情尚未移植。
static void create_home_detail_food_row(lv_obj_t *parent, int y,
                                        const home_detail_food_t *food, int width)
{
    lv_obj_t *row = make_box(parent, 0, y, width, 76, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(row, unsupported_event, "食材详情");

    lv_obj_t *icon = make_box(row, 12, 18, 40, 40, FRIDGE_UI_COLOR_CREAM,
                              FRIDGE_UI_COLOR_CREAM, 8);
    register_icon(icon, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_SAGE_DARK,
                  6, 6, 22, "F");

    int name_width = food->badge ? 150 : 300;
    make_label(row, food->name, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               64, 4, name_width, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (food->badge) {
        int badge_x = strcmp(food->badge, "低置信") == 0 ? 178 : 164;
        int badge_w = strcmp(food->badge, "低置信") == 0 ? 76 : 68;
        uint32_t badge_bg = strcmp(food->badge, "未同步") == 0 ?
                                FRIDGE_UI_COLOR_OFFLINE_BG :
                            strcmp(food->badge, "低置信") == 0 ?
                                FRIDGE_UI_COLOR_DANGER_BG : FRIDGE_UI_COLOR_WARN_BG;
        uint32_t badge_fg = strcmp(food->badge, "未同步") == 0 ?
                                FRIDGE_UI_COLOR_MUTED :
                            strcmp(food->badge, "低置信") == 0 ?
                                FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_WARN_INK;
        lv_obj_t *badge = make_box(row, badge_x, 5, badge_w, 26, badge_bg, badge_bg, 13);
        lv_obj_t *badge_label = make_label(badge, food->badge, FRIDGE_FONT_BODY_SMALL,
                                           badge_fg, 2, 0, badge_w - 4,
                                           LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        center_single_line_label(badge_label, badge_w - 4);
    }
    make_label(row, food->meta, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               64, 40, width - 244, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    uint32_t pill_bg = food->danger ? FRIDGE_UI_COLOR_DANGER_BG : FRIDGE_UI_COLOR_WARN_BG;
    uint32_t pill_fg = food->danger ? FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_WARN_INK;
    lv_obj_t *pill = make_box(row, width - 172, 22, 124, 32, pill_bg, pill_bg, 16);
    lv_obj_t *pill_label = make_label(pill, food->expiry, FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                      pill_fg, 2, 0, 120, LV_SIZE_CONTENT,
                                      LV_LABEL_LONG_DOT);
    center_single_line_label(pill_label, 120);
    register_icon(row, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                  width - 36, 24, 18, ">");
}

// HOME-02 通用信息行：图标、主副文与可选“缺”胶囊保持 64px 触控高度。
static void create_home_detail_info_row(lv_obj_t *card, int y,
                                        fridge_ui_icon_id_t icon_id,
                                        const char *title, const char *sub,
                                        bool missing, lv_event_cb_t event_cb,
                                        void *event_data, int card_width)
{
    const int row_width = card_width - 32;
    lv_obj_t *row = make_box(card, 16, y, row_width, 64, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(row, event_cb, event_data);
    lv_obj_t *icon = make_box(row, 12, 12, 40, 40, FRIDGE_UI_COLOR_MINT,
                              FRIDGE_UI_COLOR_MINT, 8);
    register_icon(icon, icon_id, FRIDGE_UI_COLOR_SAGE_DARK, 6, 6, 22, "·");
    make_label(row, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               64, 1, row_width - 144, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(row, sub, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               64, 34, row_width - 110, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (missing) {
        lv_obj_t *pill = make_box(row, row_width - 76, 17, 42, 30,
                                  FRIDGE_UI_COLOR_DANGER_BG,
                                  FRIDGE_UI_COLOR_DANGER_BG, 15);
        lv_obj_t *label = make_label(pill, "缺", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                     FRIDGE_UI_COLOR_TOMATO, 2, 0, 38,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        center_single_line_label(label, 38);
        register_icon(row, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      row_width - 30, 19, 18, ">");
    }
}

// HOME-02 横竖屏共用同一组有界演示数据；只按内容宽度调整卡片和操作区。
static void create_home_detail_page_width(lv_obj_t *page, int width, bool portrait)
{
    create_home_detail_section_title(page, 8, "已过期（2）", "建议尽快处理",
                                     FRIDGE_UI_COLOR_TOMATO, width);
    for (size_t i = 0; i < sizeof(s_home_detail_expired) / sizeof(s_home_detail_expired[0]); i++) {
        create_home_detail_food_row(page, 50 + (int)i * 84, &s_home_detail_expired[i], width);
    }

    create_home_detail_section_title(page, 222, "今天到期（2）", NULL,
                                     FRIDGE_UI_COLOR_MUTED, width);
    for (size_t i = 0; i < sizeof(s_home_detail_today) / sizeof(s_home_detail_today[0]); i++) {
        create_home_detail_food_row(page, 264 + (int)i * 84, &s_home_detail_today[i], width);
    }

    create_home_detail_section_title(page, 436, "3天内到期（5）", NULL,
                                     FRIDGE_UI_COLOR_MUTED, width);
    for (size_t i = 0; i < sizeof(s_home_detail_soon) / sizeof(s_home_detail_soon[0]); i++) {
        create_home_detail_food_row(page, 478 + (int)i * 84, &s_home_detail_soon[i], width);
    }

    lv_obj_t *recommend = make_box(page, 0, 902, width, 194, FRIDGE_UI_COLOR_SURFACE,
                                   FRIDGE_UI_COLOR_LINE, 8);
    make_label(recommend, "今日推荐：番茄炒蛋 + 菠菜鸡蛋汤",
               FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               16, 8, width - 32, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *reason = make_box(recommend, 16, 48, width - 32, 76,
                                FRIDGE_UI_COLOR_CREAM,
                                 FRIDGE_UI_COLOR_CREAM, 8);
    register_icon(reason, FRIDGE_UI_ICON_INFO, FRIDGE_UI_COLOR_SAGE_DARK,
                  12, 10, 22, "i");
    lv_obj_t *reason_text = make_label(
        reason,
        "菠菜明天到期、番茄 3 天后到期，优先消耗；两道菜共用鸡蛋，20 分钟内可完成。",
        FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 44, 7, width - 92, 62,
        LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(reason_text);
    const int button_width = (width - 40) / 2;
    make_button(recommend, 16, 134, button_width, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "生成完整菜谱", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                portrait ? portrait_nav_event : nav_event,
                (void *)(uintptr_t)SHELL_PAGE_PLAN, NULL);
    make_button(recommend, 24 + button_width, 134, button_width, 48,
                FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG,
                "继续和小精灵讨论", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                portrait ? portrait_nav_event : nav_event,
                (void *)(uintptr_t)FRIDGE_UI_P4_PAGE_CHAT, NULL);

    lv_obj_t *missing = make_box(page, 0, 1108, width, 252, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_LINE, 8);
    add_card_heading(missing, "菜单计划缺料", NULL, NULL);
    create_home_detail_info_row(missing, 48, FRIDGE_UI_ICON_CART,
                                "面条 ×1把", "周三 · 卤牛肉拌面 缺料", true,
                                portrait ? portrait_nav_event : nav_event,
                                (void *)(uintptr_t)SHELL_PAGE_PLAN, width);
    create_home_detail_info_row(missing, 120, FRIDGE_UI_ICON_CART,
                                "黑胡椒 ×1瓶", "周五 · 香煎鸡胸肉 缺料", true,
                                portrait ? portrait_nav_event : nav_event,
                                (void *)(uintptr_t)SHELL_PAGE_PLAN, width);
    make_button(missing, 16, 196, 220, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "全部加入购物清单",
                FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                portrait ? portrait_nav_event : nav_event,
                (void *)(uintptr_t)SHELL_PAGE_PLAN, NULL);

    lv_obj_t *activity = make_box(page, 0, 1372, width, 272, FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
    add_card_heading(activity, "今日库存动态", NULL, NULL);
    create_home_detail_info_row(activity, 48, FRIDGE_UI_ICON_PLUS,
                                "新增 内酯豆腐 ×1盒",
                                "07-22 14:02 · 语音登记 · 位置待补充", false,
                                unsupported_event, "库存动态详情", width);
    create_home_detail_info_row(activity, 120, FRIDGE_UI_ICON_CAMERA,
                                "拍照识别 某绿叶菜",
                                "07-22 11:38 · 低置信度待确认", false,
                                unsupported_event, "库存动态详情", width);
    create_home_detail_info_row(activity, 192, FRIDGE_UI_ICON_BOX,
                                "消耗 鸡蛋 ×2", "07-22 09:15 · 早餐", false,
                                unsupported_event, "库存动态详情", width);
}

// HOME-02 横屏正文单层纵向滚动，继续复用原 HOME PPA 与 8px 点击取消保护。
static void create_home_detail_page(lv_obj_t *page)
{
    create_home_detail_page_width(page, 672, false);
}

static void create_home(lv_obj_t *main)
{
    s_shell.main_scroll = lv_obj_create(main);
    lv_obj_remove_style_all(s_shell.main_scroll);
    lv_obj_set_pos(s_shell.main_scroll, 0, UI_HEADER_H);
    lv_obj_set_size(s_shell.main_scroll, UI_MAIN_W, UI_PAGE_H);
    lv_obj_set_style_bg_color(s_shell.main_scroll, color_hex(FRIDGE_UI_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(s_shell.main_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_shell.main_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_shell.main_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_shell.main_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(s_shell.main_scroll, main_scroll_activity_event,
                        LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_shell.main_scroll, main_scroll_activity_event,
                        LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_shell.main_scroll, main_scroll_activity_event,
                        LV_EVENT_SCROLL_END, NULL);
    lv_obj_set_style_bg_color(s_shell.main_scroll, color_hex(FRIDGE_UI_COLOR_LINE_STRONG),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_shell.main_scroll, 5, LV_PART_SCROLLBAR);

    s_shell.home_content = lv_obj_create(s_shell.main_scroll);
    clear_container_style(s_shell.home_content);
    lv_obj_set_pos(s_shell.home_content, 16, 12);
    lv_obj_set_size(s_shell.home_content, 672, 842);
    create_stat_cards(s_shell.home_content);
    create_expiring_card(s_shell.home_content);
    create_recommendation_card(s_shell.home_content);
    create_agent_tips(s_shell.home_content);
    create_home_summaries(s_shell.home_content);

}

static void create_nav(lv_obj_t *root)
{
    lv_obj_t *nav = make_box(root, UI_MAIN_W, 0, UI_NAV_W, UI_SCREEN_H,
                             FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_SAGE_DARK, 0);
    s_shell.nav = nav;
    const char *labels[] = {"首页", "库存", "对话", "计划", "更多"};
    const char *fallback[] = {"H", "I", "C", "P", "..."};
    const fridge_ui_icon_id_t icons[] = {
        FRIDGE_UI_ICON_HOME, FRIDGE_UI_ICON_BOX, FRIDGE_UI_ICON_CHAT,
        FRIDGE_UI_ICON_PLAN, FRIDGE_UI_ICON_MORE,
    };
    for (size_t i = 0; i < 5; i++) {
        int y = 12 + (int)i * 72;
        lv_obj_t *item = lv_button_create(nav);
        lv_obj_remove_style_all(item);
        lv_obj_set_pos(item, 6, y);
        lv_obj_set_size(item, 60, 64);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_bg_opa(item, i == 0 ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(item, color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_30, LV_STATE_PRESSED);
        if (i == 0) {
            add_click_event(item, nav_event, (void *)(uintptr_t)SHELL_PAGE_HOME);
        } else if (i == 1) {
            add_click_event(item, nav_event, (void *)(uintptr_t)SHELL_PAGE_INVENTORY);
        } else if (i == 2) {
            add_click_event(item, nav_event, (void *)(uintptr_t)FRIDGE_UI_P4_PAGE_CHAT);
        } else if (i == 3) {
            add_click_event(item, nav_event, (void *)(uintptr_t)SHELL_PAGE_PLAN);
        } else if (i == 4) {
            add_click_event(item, nav_event, (void *)(uintptr_t)SHELL_PAGE_MORE);
        } else {
            add_click_event(item, unsupported_event, (void *)labels[i]);
        }
        register_icon(item, icons[i], i == 0 ? 0xFFFFFF : 0xC6D5CD,
                      16, 5, 24, fallback[i]);
        lv_obj_t *label = make_label(item, labels[i], FRIDGE_FONT_BODY_SMALL,
                                     i == 0 ? 0xFFFFFF : 0xC6D5CD,
                                     2, 35, 56, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
        s_shell.nav_items[i] = item;
        s_shell.nav_labels[i] = label;
    }
    // 红点属于“更多”项内部：top=6/right=8，对齐网页参考稿而不是落在条目底部。
    lv_obj_t *badge = make_box(nav, 38, 306, 22, 22, FRIDGE_UI_COLOR_TOMATO,
                               FRIDGE_UI_COLOR_TOMATO, 11);
    lv_obj_t *badge_text = make_label(badge, "1", FRIDGE_FONT_BODY_SMALL, 0xFFFFFF,
                                       2, 0, 18, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(badge_text, 18);
}

static bool portrait_page_supported(shell_page_t page)
{
    return page == SHELL_PAGE_HOME || page == SHELL_PAGE_HOME_DETAIL ||
           page == SHELL_PAGE_PLAN ||
           page == SHELL_PAGE_MORE || page == SHELL_PAGE_TIMER ||
           page == SHELL_PAGE_ALARM || page == SHELL_PAGE_STOPWATCH ||
           page == SHELL_PAGE_SAFETY || page == SHELL_PAGE_SETTINGS ||
           page == SHELL_PAGE_MAINTENANCE ||
           page == FRIDGE_UI_P4_PAGE_CHAT ||
           page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
           page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
           page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS;
}

// 竖屏页面重建前先删除旧对象和资源槽引用；PPA 重新绑定前不能保留旧滚动对象指针。
static void clear_portrait_shell(void)
{
    close_chat_clear_confirm();
    // 先解除 PPA 动态覆盖层，再删除其所属 LVGL 对象，避免下一帧读取已释放区域。
    fridge_ui_scroll_ppa_set_dynamic_overlay(NULL);
    if (s_shell.portrait_root) lv_obj_delete(s_shell.portrait_root);
    s_shell.portrait_root = NULL;
    s_shell.portrait_scroll = NULL;
    s_shell.portrait_header_sub = NULL;
    s_shell.portrait_time = NULL;
    s_shell.portrait_date = NULL;
    s_shell.portrait_temperature = NULL;
    s_shell.portrait_humidity = NULL;
    s_shell.portrait_camera_label = NULL;
    memset(s_shell.portrait_health_boxes, 0, sizeof(s_shell.portrait_health_boxes));
    memset(s_shell.portrait_health_dots, 0, sizeof(s_shell.portrait_health_dots));
    memset(s_shell.portrait_health_icons, 0, sizeof(s_shell.portrait_health_icons));
    memset(&s_shell.portrait_home, 0, sizeof(s_shell.portrait_home));
    memset(&s_shell.portrait_plan, 0, sizeof(s_shell.portrait_plan));
    memset(&s_shell.portrait_more, 0, sizeof(s_shell.portrait_more));
    memset(&s_shell.portrait_timer, 0, sizeof(s_shell.portrait_timer));
    memset(&s_shell.portrait_alarm, 0, sizeof(s_shell.portrait_alarm));
    memset(&s_shell.portrait_stopwatch, 0, sizeof(s_shell.portrait_stopwatch));
    memset(&s_shell.portrait_safety, 0, sizeof(s_shell.portrait_safety));
    // CHAT 竖屏对象挂在 portrait_root 或 portrait_scroll 下；根节点删除后这些
    // 引用必须同步清空，避免异步 Agent/IME 刷新访问已释放控件。
    s_shell.chat_page = NULL;
    s_shell.chat_scroll = NULL;
    s_shell.chat_message_flow = NULL;
    s_shell.chat_status_row = NULL;
    s_shell.chat_permission_chip = NULL;
    s_shell.chat_permission_label = NULL;
    s_shell.chat_sync_dot = NULL;
    s_shell.chat_sync_label = NULL;
    s_shell.chat_agent_label = NULL;
    s_shell.chat_tts_label = NULL;
    s_shell.chat_input_area = NULL;
    s_shell.chat_textarea = NULL;
    s_shell.chat_voice_btn = NULL;
    s_shell.chat_send_stop_btn = NULL;
    s_shell.chat_send_stop_label = NULL;
    s_shell.chat_ime_panel = NULL;
    s_shell.chat_ime_underline = NULL;
    memset(s_shell.chat_ime_candidates, 0, sizeof(s_shell.chat_ime_candidates));
    s_shell.chat_initialized = false;
    s_shell.chat_ime_open = false;
    s_shell.icon_slot_count = s_shell.portrait_icon_slot_start;
}

// 竖屏 HOME-01 的 CTX-01 只展示确定性快照，不直接访问计时器或库存后端。
static void create_portrait_home_context(lv_obj_t *ctx)
{
    make_label(ctx, "CTX-01", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               404, 8, 58, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *today = make_box(ctx, 16, 34, 52, 30, FRIDGE_UI_COLOR_MINT,
                               FRIDGE_UI_COLOR_MINT, 15);
    lv_obj_t *today_label = make_label(today, "今日", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                       FRIDGE_UI_COLOR_SAGE_DARK, 2, 0, 48,
                                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(today_label, 48);
    s_shell.portrait_home.ctx_next_title = make_label(
        ctx, "", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
        80, 32, 360, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.portrait_home.ctx_detail = make_label(
        ctx, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        80, 68, 380, 42, LV_LABEL_LONG_DOT);

    const uint32_t backgrounds[] = {
        FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_WARN_BG, FRIDGE_UI_COLOR_DANGER_BG,
    };
    const uint32_t foregrounds[] = {
        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_WARN_INK, FRIDGE_UI_COLOR_TOMATO,
    };
    const char *prefixes[] = {"库存", "临期", "过期"};
    const int widths[] = {76, 76, 76};
    int x = 16;
    for (size_t i = 0; i < 3; i++) {
        lv_obj_t *pill = make_box(ctx, x, 128, widths[i], 30,
                                  backgrounds[i], backgrounds[i], 15);
        s_shell.portrait_home.ctx_summary_values[i] = make_label(
            pill, prefixes[i], FRIDGE_FONT_BODY_SMALL, foregrounds[i],
            2, 0, widths[i] - 4, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        center_single_line_label(s_shell.portrait_home.ctx_summary_values[i], widths[i] - 4);
        x += widths[i] + 8;
    }
}

// MORE-01/02/07/08 的 CTX-07 只读取真实设备和厨房工具快照，
// 不复用 HOME-02 的演示摘要，避免竖屏工具页出现虚构库存或菜谱状态。
static void create_portrait_more_context(lv_obj_t *ctx)
{
    lv_obj_t *flow = lv_obj_create(ctx);
    clear_container_style(flow);
    lv_obj_set_pos(flow, 10, 8);
    lv_obj_set_size(flow, 448, 150);
    make_label(flow, "CTX-07 · 工具状态", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 300, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    make_solid_line(flow, 0, 28, 448, 1, FRIDGE_UI_COLOR_LINE);
    s_shell.portrait_more.context_status = make_label(
        flow, "Wi-Fi：-- · TF：-- · 时间：--", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_MUTED, 0, 42, 448, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.portrait_more.context_detail = make_label(
        flow, "厨房工具状态由设备快照提供", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_INK, 0, 78, 448, 52, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.portrait_more.context_detail);
}

static void refresh_portrait_more_context(void)
{
    if (!s_shell.portrait_more.context_status) return;
    char status[96];
    snprintf(status, sizeof(status), "Wi-Fi：%s · TF：%s · 时间：%s",
             s_shell.device.wifi == FRIDGE_UI_HEALTH_OK ? "已连接" : "未连接",
             s_shell.device.tfcard == FRIDGE_UI_HEALTH_OK ? "就绪" : "降级",
             s_shell.kitchen.time_ready ? "已同步" : "未同步");
    set_label_if_changed(s_shell.portrait_more.context_status, status);
    const fridge_kitchen_timer_snapshot_t *timer = &s_shell.kitchen.timer;
    char detail[96];
    if (timer->id != 0 && timer->state != FRIDGE_KITCHEN_TIMER_IDLE &&
        timer->state != FRIDGE_KITCHEN_TIMER_DONE) {
        char remain[16];
        format_clock_duration(timer->remaining_seconds, remain, sizeof(remain));
        snprintf(detail, sizeof(detail), "当前倒计时：%s · %s", remain,
                 timer->label[0] ? timer->label : timer_default_name());
    } else if (s_shell.kitchen.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        strlcpy(detail, "秒表正在计时", sizeof(detail));
    } else {
        strlcpy(detail, "当前没有运行中的厨房工具", sizeof(detail));
    }
    set_label_if_changed(s_shell.portrait_more.context_detail, detail);
}

// 竖屏隐私页采用 600x1024 独立对象树：上主区 672、中导航 64、下状态舱 288。
// 横屏对象保留但隐藏，切回时无需重建成熟页面或丢失横屏业务状态。
static void create_portrait_shell(lv_obj_t *root)
{
    const bool home = s_shell.page == SHELL_PAGE_HOME;
    const bool home_detail = s_shell.page == SHELL_PAGE_HOME_DETAIL;
    const bool plan = s_shell.page == SHELL_PAGE_PLAN;
    const bool more = s_shell.page == SHELL_PAGE_MORE;
    const bool timer = s_shell.page == SHELL_PAGE_TIMER;
    const bool alarm = s_shell.page == SHELL_PAGE_ALARM;
    const bool stopwatch = s_shell.page == SHELL_PAGE_STOPWATCH;
    const bool safety = s_shell.page == SHELL_PAGE_SAFETY;
    const bool settings = s_shell.page == SHELL_PAGE_SETTINGS;
    const bool maintenance = s_shell.page == SHELL_PAGE_MAINTENANCE;
    const bool chat = s_shell.page == FRIDGE_UI_P4_PAGE_CHAT ||
                      s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
                      s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
                      s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS;
    const bool chat_main = s_shell.page == FRIDGE_UI_P4_PAGE_CHAT;
    const char *portrait_title = home ? "首页" :
                                  home_detail ? "今日摘要详情" :
                                  plan ? "计划" :
                                  more ? "更多" :
                                  timer ? "定时器" :
                                  alarm ? "闹钟" :
                                  stopwatch ? "秒表" :
                                  safety ? "设备与厨房安全" :
                                  settings ? "设置中心" :
                                  maintenance ? "维护中心" :
                                  s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ? "会话历史" :
                                  s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ? "工具执行详情" :
                                  s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS ? "待确认行动" :
                                  "对话";
    const char *portrait_id = home ? "HOME-01" :
                               home_detail ? "HOME-02" :
                               plan ? "PLAN-01" :
                               more ? "MORE-01" :
                               timer ? "MORE-02" :
                               alarm ? "MORE-07" :
                               stopwatch ? "MORE-08" :
                               safety ? "MORE-04" :
                               settings ? "MORE-05" :
                               maintenance ? "MORE-06" :
                               s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ? "CHAT-02" :
                               s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ? "CHAT-03" :
                               s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS ? "CHAT-04" :
                               "CHAT-01";
    const char *portrait_sub = home ? "未校时 · 冰箱一切正常" :
                               home_detail ? "临期 · 推荐 · 缺料 · 动态一览" :
                               plan ? "本周菜单 · 临期利用 · 购物摘要" :
                               more ? "时间工具 · 设备 · 设置" :
                               timer ? "倒计时 · 厨房计时" :
                               alarm ? "固定时刻 · 周期提醒" :
                               stopwatch ? "计次 · 复位" :
                               safety ? "硬件健康 · 连接 · 传感器 · 安全事件" :
                               settings ? "网络 · AI 模型 · 权限 · 语音显示" :
                               maintenance ? "存储 · 日志 · 更新 · 关于" :
                               s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ?
                                   "查看、恢复和清理历史会话" :
                               s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ?
                                   "当前工具执行状态" :
                               s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS ?
                                   "确认后 Agent 才会执行这些操作" :
                               "与 Agent 讨论库存、菜谱和计划";
    lv_obj_t *portrait = make_box(root, 0, 0, UI_PORTRAIT_W, UI_PORTRAIT_H,
                                  FRIDGE_UI_COLOR_PAPER,
                                  FRIDGE_UI_COLOR_PAPER, 0);
    s_shell.portrait_root = portrait;

    lv_obj_t *main = make_box(portrait, 0, 0, UI_PORTRAIT_W, UI_PORTRAIT_MAIN_H,
                              FRIDGE_UI_COLOR_PAPER,
                              FRIDGE_UI_COLOR_PAPER, 0);
    lv_obj_t *header = make_box(main, 0, 0, UI_PORTRAIT_W, UI_HEADER_H,
                                FRIDGE_UI_COLOR_PAPER,
                                FRIDGE_UI_COLOR_LINE, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    const int title_width = home ? 180 : home_detail ? 260 :
                            (safety || settings || maintenance) ? 270 :
                            chat ? 220 : 180;
    make_label(header, portrait_title, FRIDGE_FONT_TITLE_STRONG,
               FRIDGE_UI_COLOR_INK, 24, 10, title_width,
               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    // CHAT-03/04 的中文标题较长，页面 ID 胶囊按实际标题长度右移，避免遮住末字。
    const int id_x = home ? 86 : home_detail ? 202 :
                     plan ? 86 :
                     more ? 86 :
                     (timer || alarm || stopwatch) ? 122 :
                     safety ? 230 :
                     (settings || maintenance) ? 150 :
                     s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ? 220 :
                     s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS ? 190 :
                     chat ? 150 : 86;
    lv_obj_t *id_box = make_box(header, id_x, 27, 78, 22,
                                FRIDGE_UI_COLOR_MINT,
                                FRIDGE_UI_COLOR_MINT, 4);
    lv_obj_t *id = make_label(id_box, portrait_id,
                              FRIDGE_FONT_BODY_SMALL,
                              FRIDGE_UI_COLOR_SAGE_DARK, 2, 0, 74,
                              LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(id, 74);
    s_shell.portrait_header_sub = make_label(
        header, portrait_sub, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 24, 47,
        chat ? 390 : (safety || settings || maintenance) ? 350 : 470,
        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (home) {
        lv_obj_t *search = make_button(header, 480, 19, 48, 48,
                                       FRIDGE_UI_COLOR_SURFACE,
                                       FRIDGE_UI_COLOR_SAGE_DARK,
                                       FRIDGE_UI_COLOR_LINE, "",
                                       FRIDGE_FONT_BODY_SMALL, header_search_event,
                                       NULL, NULL);
        register_icon(search, FRIDGE_UI_ICON_SEARCH, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "?");
        center_header_icon(search);
        lv_obj_t *plus = make_button(header, 536, 19, 48, 48,
                                     FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_SAGE_DARK,
                                     FRIDGE_UI_COLOR_LINE, "",
                                     FRIDGE_FONT_BODY_SMALL, header_plus_event,
                                     NULL, NULL);
        register_icon(plus, FRIDGE_UI_ICON_PLUS, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "+");
        center_header_icon(plus);
    } else if (plan) {
        // PLAN-03/PLAN-06 尚未移植竖屏；入口保留 ver2 的视觉位置，但只提示不切页。
        lv_obj_t *search = make_button(header, 480, 19, 48, 48,
                                       FRIDGE_UI_COLOR_SURFACE,
                                       FRIDGE_UI_COLOR_SAGE_DARK,
                                       FRIDGE_UI_COLOR_LINE, "",
                                       FRIDGE_FONT_BODY_SMALL, unsupported_event,
                                       (void *)"菜谱库", NULL);
        register_icon(search, FRIDGE_UI_ICON_SEARCH, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "?");
        center_header_icon(search);
        lv_obj_t *shopping = make_button(header, 536, 19, 48, 48,
                                         FRIDGE_UI_COLOR_SURFACE,
                                         FRIDGE_UI_COLOR_SAGE_DARK,
                                         FRIDGE_UI_COLOR_LINE, "",
                                         FRIDGE_FONT_BODY_SMALL, unsupported_event,
                                         (void *)"购物清单", NULL);
        register_icon(shopping, FRIDGE_UI_ICON_CART, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "C");
        center_header_icon(shopping);
    } else if (more) {
        // MORE-01 没有额外顶栏动作；入口卡承担工具导航。
    } else if (timer || alarm) {
        // MORE-03 编辑页明确留在横屏；竖屏只保留入口位置并提示边界。
        make_button(header, 414, 19, 106, 48, FRIDGE_UI_COLOR_SAGE,
                    0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "新建",
                    FRIDGE_FONT_BODY_SMALL, portrait_unsupported_event, NULL, NULL);
        lv_obj_t *back = make_button(header, 536, 19, 48, 48,
                                     FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_SAGE_DARK,
                                     FRIDGE_UI_COLOR_LINE, "",
                                     FRIDGE_FONT_BODY_SMALL, page_back_event,
                                     NULL, NULL);
        register_icon(back, FRIDGE_UI_ICON_BACK, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "<");
        center_header_icon(back);
    } else if (stopwatch) {
        lv_obj_t *back = make_button(header, 536, 19, 48, 48,
                                     FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_SAGE_DARK,
                                     FRIDGE_UI_COLOR_LINE, "",
                                     FRIDGE_FONT_BODY_SMALL, page_back_event,
                                     NULL, NULL);
        register_icon(back, FRIDGE_UI_ICON_BACK, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "<");
        center_header_icon(back);
    } else if (chat && chat_main) {
        make_button(header, 414, 19, 106, 48, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "历史",
                    FRIDGE_FONT_BODY_SMALL, portrait_chat_history_event, NULL, NULL);
        make_button(header, 536, 19, 48, 48, FRIDGE_UI_COLOR_DANGER_BG,
                    FRIDGE_UI_COLOR_TOMATO, FRIDGE_UI_COLOR_DANGER_BG, "清空",
                    FRIDGE_FONT_BODY_SMALL, header_chat_clear_event, NULL, NULL);
    } else {
        lv_obj_t *back = make_button(header, 536, 19, 48, 48,
                                     FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_SAGE_DARK,
                                     FRIDGE_UI_COLOR_LINE, "",
                                     FRIDGE_FONT_BODY_SMALL, page_back_event,
                                     NULL, NULL);
        register_icon(back, FRIDGE_UI_ICON_BACK, FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 24, "<");
        center_header_icon(back);
    }
    make_solid_line(header, 0, UI_HEADER_H - 1, 600, 1,
                    FRIDGE_UI_COLOR_LINE_STRONG);

    s_shell.portrait_scroll = lv_obj_create(main);
    lv_obj_remove_style_all(s_shell.portrait_scroll);
    lv_obj_set_pos(s_shell.portrait_scroll, 0, UI_HEADER_H);
    lv_obj_set_size(s_shell.portrait_scroll, UI_PORTRAIT_W,
                    UI_PORTRAIT_MAIN_H - UI_HEADER_H);
    lv_obj_set_style_bg_color(s_shell.portrait_scroll, color_hex(FRIDGE_UI_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(s_shell.portrait_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_shell.portrait_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_shell.portrait_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_shell.portrait_scroll, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_event_cb(s_shell.portrait_scroll, main_scroll_activity_event,
                        LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_shell.portrait_scroll, main_scroll_activity_event,
                        LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_shell.portrait_scroll, main_scroll_activity_event,
                        LV_EVENT_SCROLL_END, NULL);
    lv_obj_set_style_bg_color(s_shell.portrait_scroll,
                              color_hex(FRIDGE_UI_COLOR_LINE_STRONG),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_shell.portrait_scroll, 5, LV_PART_SCROLLBAR);
    if (chat) {
        // CHAT-01～04 共用同一个竖屏滚动视口；正文、输入法和固定按钮由专用
        // builder 创建，避免把二级页面错误折叠回横屏 CHAT-01。
        create_portrait_chat_page();
    } else {
        fridge_ui_scroll_ppa_set_viewport_height(0);
        lv_obj_t *content = lv_obj_create(s_shell.portrait_scroll);
        clear_container_style(content);
        // 内容包装层只负责撑高正文，不能成为透明可点击遮罩，否则会拦截外层滚动。
        lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(content, UI_PORTRAIT_CONTENT_X, 12);
        lv_obj_set_size(content, UI_PORTRAIT_CONTENT_W,
                        home ? UI_PORTRAIT_HOME_CONTENT_H :
                        plan ? UI_PORTRAIT_PLAN_CONTENT_H :
                        more ? UI_PORTRAIT_MORE_CONTENT_H :
                        timer ? UI_PORTRAIT_TIMER_CONTENT_H :
                        alarm ? UI_PORTRAIT_ALARM_CONTENT_H :
                        stopwatch ? UI_PORTRAIT_STOPWATCH_CONTENT_H :
                        (safety || settings || maintenance) ? UI_PORTRAIT_SPLIT_CONTENT_H : 1656);
        if (home) create_portrait_home_page(content);
        else if (plan) create_portrait_plan_page(content);
        else if (more) create_portrait_more_page(content);
        else if (timer) create_portrait_timer_page(content);
        else if (alarm) create_portrait_alarm_page(content);
        else if (stopwatch) create_portrait_stopwatch_page(content);
        else if (safety) create_portrait_safety_page(content);
        else if (settings) create_portrait_settings_page(content);
        else if (maintenance) create_portrait_maintenance_page(content);
        else create_home_detail_page_width(content, UI_PORTRAIT_CONTENT_W, true);
    }

    lv_obj_t *nav = make_box(portrait, 0, UI_PORTRAIT_NAV_Y, UI_PORTRAIT_W,
                             UI_PORTRAIT_NAV_H, FRIDGE_UI_COLOR_SAGE_DARK,
                             FRIDGE_UI_COLOR_SAGE_DARK, 0);
    const char *labels[] = {"首页", "库存", "对话", "计划", "更多"};
    const char *fallback[] = {"H", "I", "C", "P", "..."};
    const fridge_ui_icon_id_t icons[] = {
        FRIDGE_UI_ICON_HOME, FRIDGE_UI_ICON_BOX, FRIDGE_UI_ICON_CHAT,
        FRIDGE_UI_ICON_PLAN, FRIDGE_UI_ICON_MORE,
    };
    const shell_page_t pages[] = {
        SHELL_PAGE_HOME, SHELL_PAGE_CAMERA, FRIDGE_UI_P4_PAGE_CHAT,
        SHELL_PAGE_PLAN, SHELL_PAGE_MORE,
    };
    for (size_t i = 0; i < 5; i++) {
        lv_obj_t *item = lv_button_create(nav);
        lv_obj_remove_style_all(item);
        lv_obj_set_pos(item, 6 + (int)i * 118, 4);
        lv_obj_set_size(item, 112, 56);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_bg_color(item, color_hex(0xFFFFFF), 0);
        const bool active = (i == 0 && (home || home_detail)) ||
                            (i == 2 && chat) ||
                            (i == 3 && s_shell.page == SHELL_PAGE_PLAN) ||
                            (i == 4 && (more || timer || alarm || stopwatch ||
                                        safety || settings || maintenance));
        lv_obj_set_style_bg_opa(item, active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_30, LV_STATE_PRESSED);
        if (i == 1U) {
            add_click_event(item, unsupported_event, "库存页面");
        } else {
            add_click_event(item, portrait_nav_event, (void *)(uintptr_t)pages[i]);
        }
        register_icon(item, icons[i], active ? 0xFFFFFF : 0xC6D5CD,
                      42, 1, 22, fallback[i]);
        lv_obj_t *label = make_label(item, labels[i], FRIDGE_FONT_BODY_SMALL,
                                     active ? 0xFFFFFF : 0xC6D5CD,
                                     2, 31, 108, LV_SIZE_CONTENT,
                                     LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -1);
    }
    lv_obj_t *nav_badge = make_box(nav, 548, 5, 22, 22, FRIDGE_UI_COLOR_TOMATO,
                                   FRIDGE_UI_COLOR_TOMATO, 11);
    lv_obj_t *nav_badge_text = make_label(nav_badge, "1", FRIDGE_FONT_BODY_SMALL,
                                          0xFFFFFF, 2, 0, 18, LV_SIZE_CONTENT,
                                          LV_LABEL_LONG_CLIP);
    center_single_line_label(nav_badge_text, 18);

    lv_obj_t *cabin = make_box(portrait, 0, UI_PORTRAIT_CABIN_Y, UI_PORTRAIT_W,
                               UI_PORTRAIT_CABIN_H, FRIDGE_UI_COLOR_CREAM,
                               FRIDGE_UI_COLOR_LINE, 16);
    lv_obj_set_style_border_side(cabin, LV_BORDER_SIDE_TOP, 0);
    lv_obj_t *status = make_box(cabin, 10, 10, 474, 81, FRIDGE_UI_COLOR_SURFACE,
                                FRIDGE_UI_COLOR_LINE, 8);
    s_shell.portrait_time = make_label(status, "--:--", FRIDGE_FONT_STATUS_TIME_STRONG,
                                        FRIDGE_UI_COLOR_INK, 12, 0, 116,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_shell.portrait_time, LV_ALIGN_LEFT_MID, 12, 0);
    s_shell.portrait_date = make_label(status, "未校时", FRIDGE_FONT_CAPTION_EMPHASIS,
                                        FRIDGE_UI_COLOR_MUTED, 136, 9, 110,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(status, FRIDGE_UI_ICON_TEMP, FRIDGE_UI_COLOR_SAGE_DARK,
                  126, 40, 16, "°");
    s_shell.portrait_temperature =
        make_label(status, "--.-°C", FRIDGE_FONT_CAPTION_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 148, 43, 56,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(status, FRIDGE_UI_ICON_DROP, FRIDGE_UI_COLOR_SAGE_DARK,
                  201, 40, 16, "%");
    s_shell.portrait_humidity =
        make_label(status, "--%", FRIDGE_FONT_CAPTION_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 223, 43, 48,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    const fridge_ui_icon_id_t health_icons[] = {
        FRIDGE_UI_ICON_WIFI, FRIDGE_UI_ICON_CLOUD, FRIDGE_UI_ICON_TF,
        FRIDGE_UI_ICON_CAMERA, FRIDGE_UI_ICON_MIC,
    };
    for (size_t i = 0; i < 5; i++) {
        const bool camera_off = i == 3U;
        s_shell.portrait_health_boxes[i] =
            make_box(status, 280 + (int)i * 34, 40, 28, 28,
                     camera_off ? FRIDGE_UI_COLOR_OFFLINE_BG : FRIDGE_UI_COLOR_CREAM,
                     FRIDGE_UI_COLOR_LINE, 6);
        register_icon(s_shell.portrait_health_boxes[i], health_icons[i],
                      camera_off ? FRIDGE_UI_COLOR_MUTED : FRIDGE_UI_COLOR_SAGE_DARK,
                      0, 0, 16, "·");
        s_shell.portrait_health_icons[i] =
            &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
        s_shell.portrait_health_dots[i] =
            make_box(s_shell.portrait_health_boxes[i], 19, 0, 7, 7,
                     FRIDGE_UI_COLOR_OFFLINE, FRIDGE_UI_COLOR_PAPER, 4);
    }
    lv_obj_t *privacy = make_box(status, 260, 3, 116, 30,
                                 FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 15);
    lv_obj_t *privacy_content = lv_obj_create(privacy);
    clear_container_style(privacy_content);
    // 锁与四字文案使用固定槽位；约 4px 可见间距兼顾分离度和紧凑感。
    lv_obj_set_pos(privacy_content, 9, 0);
    lv_obj_set_size(privacy_content, 94, 30);
    register_icon(privacy_content, FRIDGE_UI_ICON_LOCK, FRIDGE_UI_COLOR_SAGE_DARK,
                  0, 1, 14, "·");
    lv_obj_t *privacy_text =
        make_label(privacy_content, "隐私模式", FRIDGE_FONT_CAPTION_EMPHASIS,
                                        FRIDGE_UI_COLOR_SAGE_DARK, 25, 0, 66,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    set_single_line_content_height(privacy_text);
    lv_obj_set_style_text_align(privacy_text, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(privacy_text, LV_ALIGN_LEFT_MID, 25, 0);

    lv_obj_t *ctx = make_box(cabin, 10, 99, 474, 179, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(ctx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ctx, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(ctx, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_event_cb(ctx, main_scroll_activity_event, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(ctx, main_scroll_activity_event, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(ctx, main_scroll_activity_event, LV_EVENT_SCROLL_END, NULL);
    if (home) {
        // HOME-01 的 CTX-01 采用 ver2 首页卡片：下一事项在上、统计胶囊在下。
        create_portrait_home_context(ctx);
    } else if (more || timer || alarm || stopwatch) {
        create_portrait_more_context(ctx);
    } else if (safety) {
        make_label(ctx, "CTX-08 · 设备与厨房安全", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 10, 8, 360, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_solid_line(ctx, 10, 36, 448, 1, FRIDGE_UI_COLOR_LINE);
        make_label(ctx, "设备健康与传感器均为只读快照；当前没有可确认的安全事件。",
                   FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 10, 52, 440, 70,
                   LV_LABEL_LONG_WRAP);
    } else if (settings) {
        make_label(ctx, "CTX-09 · 设置状态", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 10, 8, 360, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_solid_line(ctx, 10, 36, 448, 1, FRIDGE_UI_COLOR_LINE);
        make_label(ctx, "网络、权限、WakeNet 与亮度由现有设置后台提供状态。",
                   FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 10, 52, 440, 70,
                   LV_LABEL_LONG_WRAP);
    } else if (maintenance) {
        make_label(ctx, "CTX-10 · 系统状态", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 10, 8, 360, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_solid_line(ctx, 10, 36, 448, 1, FRIDGE_UI_COLOR_LINE);
        make_label(ctx, "存储、启动、固件版本和设备信息来自当前运行时快照。",
                   FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 10, 52, 440, 70,
                   LV_LABEL_LONG_WRAP);
    } else {
        lv_obj_t *ctx_flow = lv_obj_create(ctx);
        clear_container_style(ctx_flow);
        lv_obj_set_pos(ctx_flow, 10, 8);
        lv_obj_set_size(ctx_flow, 650, 148);
        make_label(ctx_flow, "今日摘要", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 180, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_solid_line(ctx_flow, 0, 28, 190, 1, FRIDGE_UI_COLOR_LINE);
        lv_obj_t *summary_labels =
            make_label(ctx_flow, "库存\n临期\n过期\n晚餐", FRIDGE_FONT_BODY_SMALL,
                       FRIDGE_UI_COLOR_MUTED, 0, 34, 80, 112, LV_LABEL_LONG_WRAP);
        lv_obj_t *summary_values = make_label(
            ctx_flow, "20 项\n7 项\n2 项\n番茄炒蛋+菠菜汤", FRIDGE_FONT_BODY_SMALL,
            FRIDGE_UI_COLOR_INK, 64, 34, 132, 112, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(summary_labels, -5, 0);
        lv_obj_set_style_text_line_space(summary_values, -5, 0);
        lv_obj_set_style_text_align(summary_values, LV_TEXT_ALIGN_RIGHT, 0);

        make_solid_line(ctx_flow, 200, 0, 1, 140, FRIDGE_UI_COLOR_LINE);
        make_label(ctx_flow, "下一事项", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 214, 0, 170, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_solid_line(ctx_flow, 214, 28, 182, 1, FRIDGE_UI_COLOR_LINE);
        register_icon(ctx_flow, FRIDGE_UI_ICON_TIMER, FRIDGE_UI_COLOR_SAGE_DARK,
                      214, 36, 18, "·");
        make_label(ctx_flow, "煮蛋  还剩 08:00", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_INK, 238, 38, 156, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_label(ctx_flow, "来源：菜谱·番茄炒蛋", FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 214, 70, 180, 56, LV_LABEL_LONG_WRAP);

        make_solid_line(ctx_flow, 410, 0, 1, 140, FRIDGE_UI_COLOR_LINE);
        make_label(ctx_flow, "最近同步", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, 424, 0, 160, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        make_solid_line(ctx_flow, 424, 28, 160, 1, FRIDGE_UI_COLOR_LINE);
        make_label(ctx_flow, "今天 14:02\n20 项已同步", FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 424, 38, 170, 70, LV_LABEL_LONG_WRAP);
    }

    lv_obj_t *portrait_voice_label = NULL;
    lv_obj_t *voice = make_button(cabin, 494, 10, 96, 129, FRIDGE_UI_COLOR_SAGE,
                                  0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "语音",
                                  FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                  voice_event, NULL, &portrait_voice_label);
    register_icon(voice, FRIDGE_UI_ICON_MIC, 0xFFFFFF, 34, 29, 28, "M");
    if (portrait_voice_label) {
        lv_obj_align(portrait_voice_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
    lv_obj_t *camera = make_button(cabin, 494, 149, 96, 129,
                                   FRIDGE_UI_COLOR_OFFLINE_BG,
                                   FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE,
                                   "摄像关", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                   camera_event, NULL, &s_shell.portrait_camera_label);
    register_icon(camera, FRIDGE_UI_ICON_LOCK, FRIDGE_UI_COLOR_MUTED,
                  34, 29, 28, "C");
    if (s_shell.portrait_camera_label) {
        lv_obj_align(s_shell.portrait_camera_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    }

    lv_obj_add_flag(portrait, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *page_card(lv_obj_t *page, int y, int height, uint32_t background)
{
    return make_box(page, 0, y, 672, height, background,
                    FRIDGE_UI_COLOR_LINE, 8);
}

// MORE-01 角标只统计真实业务状态：运行/暂停中的倒计时，以及已启用闹钟。
// 秒表参考稿没有数量角标，因此不为第三张卡伪造数字。
static uint8_t more_running_timer_count(const fridge_kitchen_tools_snapshot_t *snapshot)
{
    uint8_t count = 0;
    if (!snapshot) return 0;
    for (size_t i = 0; i < snapshot->timer_count && i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        fridge_kitchen_timer_state_t state = snapshot->timers[i].state;
        if (state == FRIDGE_KITCHEN_TIMER_RUNNING || state == FRIDGE_KITCHEN_TIMER_PAUSED) {
            count++;
        }
    }
    return count;
}

static uint8_t more_enabled_alarm_count(const fridge_kitchen_tools_snapshot_t *snapshot)
{
    uint8_t count = 0;
    if (!snapshot) return 0;
    for (size_t i = 0; i < snapshot->alarm_count && i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        if (snapshot->alarms[i].enabled) count++;
    }
    return count;
}

static void refresh_more_badges(void)
{
    const uint8_t counts[2] = {
        more_running_timer_count(&s_shell.kitchen),
        more_enabled_alarm_count(&s_shell.kitchen),
    };
    for (size_t i = 0; i < 2; i++) {
        lv_obj_t *box = s_shell.more_badge_boxes[i];
        lv_obj_t *label = s_shell.more_badge_labels[i];
        if (!box || !label) continue;
        if (counts[i] == 0) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char text[4];
        snprintf(text, sizeof(text), "%u", (unsigned)counts[i]);
        set_label_if_changed(label, text);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_more_page(lv_obj_t *page)
{
    static const char *const fallback_titles[3] = {"定时器", "闹钟", "秒表"};
    static const char *const fallback_descriptions[3] = {
        "厨房倒计时，支持快捷预设", "固定时间提醒，最多 5 个", "毫秒计时，支持计次记录",
    };
    static const char *const placeholder_titles[3] = {
        "设备与厨房安全", "设置中心", "维护中心",
    };
    static const char *const placeholder_descriptions[3] = {
        "硬件健康 · 连接 · 传感器 · 安全事件", "网络 · AI 模型 · 权限 · 语音显示",
        "存储 · 插件 · 日志 · 系统更新",
    };
    const shell_page_t destinations[3] = {
        SHELL_PAGE_TIMER, SHELL_PAGE_ALARM, SHELL_PAGE_STOPWATCH,
    };
    // 系统与维护区三张卡的目标页；MORE-04～06 均保持当前竖屏方向进入。
    const shell_page_t system_destinations[3] = {
        SHELL_PAGE_SAFETY, SHELL_PAGE_SETTINGS, SHELL_PAGE_MAINTENANCE,
    };
    // MORE-01 使用参考稿的两列入口网格；第三张卡自然落在下一行左侧。
    make_label(page, "时间工具", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 2, 0, 200, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 3; i++) {
        int x = (int)(i % 2U) * 342;
        int y = 36 + (int)(i / 2U) * 160;
        lv_obj_t *card = make_box(page, x, y, 330, 148, FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(card, more_card_event, (void *)(uintptr_t)destinations[i]);
        const char *title = s_shell.more_initialized && s_shell.more.cards[i].title[0] ?
            s_shell.more.cards[i].title : fallback_titles[i];
        const char *description = s_shell.more_initialized && s_shell.more.cards[i].description[0] ?
            s_shell.more.cards[i].description : fallback_descriptions[i];
        lv_obj_t *icon = make_box(card, 16, 16, 48, 48, FRIDGE_UI_COLOR_MINT,
                                  FRIDGE_UI_COLOR_MINT, 8);
        register_icon(icon, more_card_icon(i), FRIDGE_UI_COLOR_SAGE_DARK, 10, 10, 28,
                      i == 0 ? "T" : (i == 1 ? "A" : "S"));
        if (i < 2U) {
            // 参考稿 .me-badge：top/right=12px、24px 高、全圆角、offline 灰底、Medium 白字。
            lv_obj_t *badge = lv_obj_create(card);
            lv_obj_remove_style_all(badge);
            lv_obj_set_pos(badge, 294, 12);
            lv_obj_set_size(badge, 24, 24);
            lv_obj_set_style_bg_color(badge, color_hex(FRIDGE_UI_COLOR_OFFLINE), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(badge, 12, 0);
            lv_obj_set_style_pad_all(badge, 0, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *badge_label = make_label(badge, "", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                               0xFFFFFF, 0, 0, 24,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
            center_single_line_label(badge_label, 24);
            s_shell.more_badge_boxes[i] = badge;
            s_shell.more_badge_labels[i] = badge_label;
        }
        make_label(card, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                   16, 72, 278, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(card, description, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                   16, 104, 270, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      298, 116, 20, ">");
    }
    refresh_more_badges();

    make_label(page, "系统与维护", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 2, 372, 220, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 3; i++) {
        int x = (int)(i % 2U) * 342;
        int y = 408 + (int)(i / 2U) * 160;
        lv_obj_t *card = make_box(page, x, y, 330, 148, FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        // 系统与维护三卡分别进入设备安全、设置中心和维护中心。
        add_click_event(card, more_card_event, (void *)(uintptr_t)system_destinations[i]);
        lv_obj_t *icon = make_box(card, 16, 16, 48, 48, FRIDGE_UI_COLOR_MINT,
                                  FRIDGE_UI_COLOR_MINT, 8);
        register_icon(icon, more_card_icon(i + 3U), FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 28, "I");
        make_label(card, placeholder_titles[i], FRIDGE_FONT_BODY_EMPHASIS,
                   FRIDGE_UI_COLOR_INK, 16, 72, 278,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(card, placeholder_descriptions[i], FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 16, 104, 270, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      298, 116, 20, ">");
    }

    lv_obj_t *summary = make_box(page, 0, 728, 672, 50, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_LINE, 8);
    s_shell.more_device = make_label(summary, "", FRIDGE_FONT_BODY_SMALL,
                                     FRIDGE_UI_COLOR_MUTED, 16, 10, 640,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
}

// 设备二维码绑定页：二维码只承载短期 claim token，长期设备密钥和 MQTT 密码不得显示。
// LVGL QRCode 的画布属于大块 UI 内存，当前 P4 配置会让大于阈值的普通分配优先进入 PSRAM。
static void create_binding_page(lv_obj_t *page)
{
    lv_obj_t *card = page_card(page, 0, 430, FRIDGE_UI_COLOR_SURFACE);
    make_label(card, "设备与小程序连接", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_SAGE_DARK, 24, 20, 360,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(card, "在小程序中打开“绑定设备”并扫描下方二维码",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               24, 62, 390, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

#if LV_USE_QRCODE
    s_shell.binding_qr = lv_qrcode_create(card);
    lv_qrcode_set_size(s_shell.binding_qr, 280);
    lv_qrcode_set_dark_color(s_shell.binding_qr, color_hex(FRIDGE_UI_COLOR_INK));
    lv_qrcode_set_light_color(s_shell.binding_qr, color_hex(0xFFFFFF));
    lv_obj_set_pos(s_shell.binding_qr, 24, 108);
    lv_obj_set_style_border_width(s_shell.binding_qr, 8, 0);
    lv_obj_set_style_border_color(s_shell.binding_qr, color_hex(0xFFFFFF), 0);
#endif

    s_shell.binding_qr_placeholder =
        make_box(card, 24, 108, 280, 280, FRIDGE_UI_COLOR_CREAM,
                 FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *placeholder = make_label(
        s_shell.binding_qr_placeholder, "正在向服务器申请二维码…",
        FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED, 20, 112, 240,
        LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);

    s_shell.binding_status = make_label(
        card, "尚未预置设备身份", FRIDGE_FONT_BODY_EMPHASIS,
        FRIDGE_UI_COLOR_INK, 336, 122, 300, LV_SIZE_CONTENT,
        LV_LABEL_LONG_WRAP);
    s_shell.binding_detail = make_label(
        card, "请先通过 USB 写入 deviceSn、bootstrap key 和 API 地址。",
        FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        336, 174, 300, 120, LV_LABEL_LONG_WRAP);
    s_shell.binding_expiry = make_label(
        card, "", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
        FRIDGE_UI_COLOR_SAGE_DARK, 336, 316, 300,
        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    refresh_binding_page();
}

static void refresh_binding_page(void)
{
    if (!s_shell.binding_status) return;

    set_label_if_changed(s_shell.binding_status,
                         s_shell.binding.status[0] ?
                             s_shell.binding.status :
                             (s_shell.binding.bound ? "设备已绑定" :
                              (s_shell.binding.provisioned ? "等待扫码绑定" :
                               "尚未预置设备身份")));
    set_label_if_changed(s_shell.binding_detail,
                         s_shell.binding.detail[0] ?
                             s_shell.binding.detail :
                             "二维码由服务器签发，过期后设备会自动刷新。");

    char expiry[96] = {0};
    if (s_shell.binding.bound) {
        snprintf(expiry, sizeof(expiry), "设备：%s · 已连接家庭",
                 s_shell.binding.device_sn[0] ? s_shell.binding.device_sn : "--");
    } else if (s_shell.binding.qr_ready) {
        snprintf(expiry, sizeof(expiry), "设备：%s · 二维码剩余 %lu 秒",
                 s_shell.binding.device_sn[0] ? s_shell.binding.device_sn : "--",
                 (unsigned long)s_shell.binding.expires_in_seconds);
    } else if (s_shell.binding.device_sn[0]) {
        snprintf(expiry, sizeof(expiry), "设备：%s", s_shell.binding.device_sn);
    }
    set_label_if_changed(s_shell.binding_expiry, expiry);

    const bool show_qr = s_shell.binding.qr_ready &&
                         s_shell.binding.qr_payload[0] != '\0';
#if LV_USE_QRCODE
    if (s_shell.binding_qr) {
        if (show_qr) {
            lv_qrcode_update(s_shell.binding_qr,
                             s_shell.binding.qr_payload,
                             strlen(s_shell.binding.qr_payload));
            lv_obj_remove_flag(s_shell.binding_qr, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_shell.binding_qr, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
    if (s_shell.binding_qr_placeholder) {
        if (show_qr) lv_obj_add_flag(s_shell.binding_qr_placeholder, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_shell.binding_qr_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// MORE-05 设置中心 / MORE-06 维护中心：参考稿 renderSplitShell 分区模板
// 横屏使用 180px 分区导航；竖屏收窄为 150px 并扩大右侧内容区；切分区只重建右侧内容，不切换页面。
// 本模板只做只读展示与已实现入口跳转，不写 NVS、不触发 OTA、不操作 GPIO/背光。
// ============================================================================

// 分区行的控件类型。参考稿的 switch / seg / slider 在 P4 尚无对应可写能力，
// 统一降级为只读展示，避免用户以为点了就生效。
typedef enum {
    SPLIT_ROW_VALUE = 0,     // 右侧只读取值文本
    SPLIT_ROW_STATE_ON,      // 已启用状态胶囊（绿）
    SPLIT_ROW_STATE_OFF,     // 已关闭状态胶囊（灰）
    SPLIT_ROW_PENDING,       // 未移植能力，显示“后续开放”灰胶囊
    SPLIT_ROW_LINK,          // 可点击行，跳转到已实现页面
} split_row_kind_t;

typedef struct {
    const char *label;       // 行标题
    const char *desc;        // 行说明，可为 NULL
    const char *value;       // 取值文本；为 NULL 时由 provider 动态填充
    split_row_kind_t kind;
    shell_page_t link_page;  // 仅 SPLIT_ROW_LINK 使用
} split_row_t;

typedef struct {
    const char *name;                  // 分区名（左栏按钮文案）
    fridge_ui_icon_id_t icon;          // 分区图标
    const char *icon_fallback;         // 无 TF 图标时的文字回退
    const split_row_t *rows;
    size_t row_count;
    const char *note;                  // 分区底部说明，可为 NULL
} split_section_t;

// 分区行高与参考稿 .form-row 一致：48px 触控高 + 说明行，统一按 78px 排布。
#define UI_SPLIT_ROW_H 78

// 动态取值：把设备真实状态填进分区行。返回 NULL 表示该行使用静态 value。
typedef const char *(*split_value_provider_t)(const split_row_t *row, char *buffer,
                                              size_t buffer_size);

// 横屏和竖屏共用分区行渲染，但正文几何不同；统一从这里取值，避免竖屏控件
// 误用 476px 横屏宽度而越过 600px 逻辑画布。
static int split_content_x(void)
{
    return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
           UI_PORTRAIT_SPLIT_CONTENT_X : UI_SPLIT_CONTENT_X;
}

static int split_side_width(void)
{
    return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
           UI_PORTRAIT_SPLIT_SIDE_W : UI_SPLIT_SIDE_W;
}

static int split_content_width(void)
{
    return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
           UI_PORTRAIT_SPLIT_CONTENT_W : UI_SPLIT_CONTENT_W;
}

static int split_row_height(void)
{
    return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
           UI_PORTRAIT_SPLIT_ROW_H : UI_SPLIT_ROW_H;
}

static void split_tab_event(lv_event_t *event);
static void split_row_link_event(lv_event_t *event);
static void build_split_page(lv_obj_t *page, const split_section_t *sections,
                             size_t section_count, uint8_t current,
                             split_value_provider_t provider);

// 左栏分区按钮：选中态使用薄荷绿底 + 鼠尾草深绿边框，与参考稿 .more-tab.on 对齐。
static void create_split_tab(lv_obj_t *page, size_t index, const split_section_t *section,
                             bool selected)
{
    const bool portrait = s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    const int side_width = split_side_width();
    const int icon_x = portrait ? 10 : 12;
    const int label_x = portrait ? 36 : 44;
    const int label_width = side_width - label_x - (portrait ? 8 : 12);
    const fridge_font_role_t label_role = portrait ?
        (selected ? FRIDGE_FONT_BODY_SMALL_EMPHASIS : FRIDGE_FONT_BODY_SMALL) :
        (selected ? FRIDGE_FONT_BODY_EMPHASIS : FRIDGE_FONT_BODY);
    int y = (int)index * (UI_SPLIT_TAB_H + UI_SPLIT_TAB_GAP);
    lv_obj_t *tab = make_box(page, 0, y, side_width, UI_SPLIT_TAB_H,
                             selected ? FRIDGE_UI_COLOR_MINT : FRIDGE_UI_COLOR_SURFACE,
                             selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(tab, split_tab_event, (void *)(uintptr_t)index);
    register_icon(tab, section->icon, FRIDGE_UI_COLOR_SAGE_DARK, icon_x, 14, 20,
                  section->icon_fallback);
    // 48px 分区触控行的图标视觉中心此前落在 y=28（比中线低 4px）。图像和
    // 文字回退都使用 LEFT_MID，使 TF 图标缺失时两种呈现保持同一基线。
    if (s_shell.icon_slot_count > 0) {
        icon_slot_t *icon = &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
        lv_obj_align(icon->image, LV_ALIGN_LEFT_MID, icon_x, 0);
        lv_obj_align(icon->fallback, LV_ALIGN_LEFT_MID, icon_x, 0);
    }
    lv_obj_t *label = make_label(tab, section->name,
                                 label_role,
                                 selected ? FRIDGE_UI_COLOR_SAGE_DARK : FRIDGE_UI_COLOR_INK,
                                 label_x, 0, label_width, LV_SIZE_CONTENT,
                                 LV_LABEL_LONG_DOT);
    set_single_line_content_height(label);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, label_x, 0);
    if (index < UI_SPLIT_SECTION_MAX) {
        s_shell.split_tabs[index] = tab;
        s_shell.split_tab_labels[index] = label;
    }
}

// 状态胶囊：只读展示当前能力状态，不接受点击，避免误解为可写开关。
static void create_split_state_pill(lv_obj_t *parent, int x, int y, const char *text,
                                    uint32_t background, uint32_t foreground)
{
    int width = 92;
    lv_obj_t *pill = make_box(parent, x, y, width, 32, background, background, 16);
    lv_obj_t *label = make_label(pill, text, FRIDGE_FONT_BODY_SMALL, foreground,
                                 2, 0, width - 4, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(label, width - 4);
}

// 右侧内容区单行：左标题 + 下方说明，右侧按控件类型渲染取值或状态。
static void create_split_row(lv_obj_t *card, int y, const split_row_t *row,
                             split_value_provider_t provider)
{
    const int card_w = split_content_width();
    const int pad = 16;
    const bool portrait = s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    const int value_w = portrait ? 144 : 200;
    const int text_w = card_w - pad * 2 - value_w - (portrait ? 12 : 8);
    const int value_y = y + (portrait ? (split_row_height() - 30) / 2 : 28);

    make_label(card, row->label, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               pad, y + 12, text_w, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (row->desc && row->desc[0]) {
        lv_obj_t *desc = make_label(card, row->desc, FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_MUTED, pad, y + 42, text_w,
                                    LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        apply_compact_body_lines(desc);
    }

    char buffer[64] = {0};
    const char *value = row->value;
    if (provider) {
        const char *dynamic = provider(row, buffer, sizeof(buffer));
        if (dynamic) value = dynamic;
    }

    switch (row->kind) {
    case SPLIT_ROW_STATE_ON:
        create_split_state_pill(card, card_w - pad - 92,
                                y + (portrait ? (split_row_height() - 32) / 2 : 20),
                                value ? value : "已启用",
                                FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_SAGE_DARK);
        break;
    case SPLIT_ROW_STATE_OFF:
        create_split_state_pill(card, card_w - pad - 92,
                                y + (portrait ? (split_row_height() - 32) / 2 : 20),
                                value ? value : "已关闭",
                                FRIDGE_UI_COLOR_OFFLINE_BG, FRIDGE_UI_COLOR_MUTED);
        break;
    case SPLIT_ROW_PENDING:
        create_split_state_pill(card, card_w - pad - 92,
                                y + (portrait ? (split_row_height() - 32) / 2 : 20),
                                "后续开放",
                                FRIDGE_UI_COLOR_OFFLINE_BG, FRIDGE_UI_COLOR_MUTED);
        break;
    case SPLIT_ROW_LINK: {
        // 可点击行覆盖整行区域；行内按钮同样接入 8px 滑动取消保护。
        lv_obj_t *hit = make_box(card, pad - 8, y + 6, card_w - pad * 2 + 16,
                                 split_row_height() - 12, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_SURFACE, 8);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_set_style_outline_opa(hit, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(hit, split_row_link_event, (void *)(uintptr_t)row->link_page);
        if (value && value[0]) {
            lv_obj_t *label = make_label(card, value, FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_SAGE_DARK,
                                         card_w - pad - 128, value_y, 112,
                                         portrait ? 30 : LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        }
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      card_w - pad - 22,
                      y + (portrait ? (split_row_height() - 20) / 2 : 30), 20, ">");
        break;
    }
    case SPLIT_ROW_VALUE:
    default:
        if (value && value[0]) {
            lv_obj_t *label = make_label(card, value, FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_SAGE_DARK,
                                         card_w - pad - value_w, value_y, value_w,
                                         portrait ? 30 : LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        }
        break;
    }
}

// 右侧内容：分区标题 + 行卡片 + 可选说明。内容整体高度按行数计算，超出时随主区滚动。
static void create_split_content(lv_obj_t *page, const split_section_t *section,
                                 split_value_provider_t provider)
{
    s_shell.split_content_icon_slot_start = s_shell.icon_slot_count;
    lv_obj_t *host = lv_obj_create(page);
    clear_container_style(host);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(host, split_content_x(), 0);
    int rows_h = (int)section->row_count * split_row_height();
    int total_h = 34 + rows_h + (section->note ? 60 : 0);
    lv_obj_set_size(host, split_content_width(), total_h);
    s_shell.split_content = host;

    make_label(host, section->name, FRIDGE_FONT_BODY_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, split_content_width(),
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    lv_obj_t *card = make_box(host, 0, 30, split_content_width(), rows_h,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    for (size_t i = 0; i < section->row_count; i++) {
        int y = (int)i * split_row_height();
        create_split_row(card, y, &section->rows[i], provider);
        // 行间 1px 分隔线，末行不画，和参考稿 .form-row:last-child 一致。
        if (i + 1 < section->row_count) {
            make_solid_line(card, 16, y + split_row_height() - 1,
                            split_content_width() - 32, 1, FRIDGE_UI_COLOR_LINE);
        }
    }

    if (section->note) {
        lv_obj_t *note = make_label(host, section->note, FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_MUTED, 0, 30 + rows_h + 12,
                                    split_content_width(), LV_SIZE_CONTENT,
                                    LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(note);
    }
}

static void build_split_page(lv_obj_t *page, const split_section_t *sections,
                             size_t section_count, uint8_t current,
                             split_value_provider_t provider)
{
    if (section_count > UI_SPLIT_SECTION_MAX) section_count = UI_SPLIT_SECTION_MAX;
    if (current >= section_count) current = 0;
    for (size_t i = 0; i < section_count; i++) {
        create_split_tab(page, i, &sections[i], i == current);
    }
    create_split_content(page, &sections[current], provider);
}

// 分区切换：只重建当前页面内容，不进入 switch_page，避免整页销毁重来。
static void split_tab_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_shell.page == SHELL_PAGE_SETTINGS) {
        if (s_shell.settings_section == index) return;
        s_shell.settings_section = index;
    } else if (s_shell.page == SHELL_PAGE_MAINTENANCE) {
        if (s_shell.maintenance_section == index) return;
        s_shell.maintenance_section = index;
    } else if (s_shell.page == SHELL_PAGE_SAFETY) {
        if (s_shell.safety_section == index) return;
        s_shell.safety_section = index;
    } else {
        return;
    }
    // 左侧导航、页头与主滚动容器保持不变，只替换右侧分区内容，避免整页删除导致
    // PPA 快照、图标绑定与触控状态反复重建。
    rebuild_current_split_content();
}

static void split_row_link_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    shell_page_t page = (shell_page_t)(uintptr_t)lv_event_get_user_data(event);
    if (page >= FRIDGE_UI_P4_PAGE_COUNT) return;
    switch_page(page);
}

// ---------------------------------------------------------------------------
// MORE-05 设置中心：五个分区与参考稿一致。当前 P4 只做只读展示 +
// 已实现入口跳转；云同步、AI Provider 切换、亮度、夜间模式等写操作尚未移植。
// ---------------------------------------------------------------------------
static const split_row_t k_settings_network_rows[] = {
    { "Wi-Fi 网络", "由板载 ESP32-C6 经 SDIO 提供", NULL, SPLIT_ROW_VALUE, 0 },
    { "云端同步", "库存 / 菜单 / 会话与家庭云端同步", NULL, SPLIT_ROW_VALUE, 0 },
    { "MQTT 推送", "状态变化实时推送到小程序", NULL, SPLIT_ROW_VALUE, 0 },
    { "离线模式", "仅本地运行，不发起任何网络请求", NULL, SPLIT_ROW_PENDING, 0 },
};

static const split_row_t k_settings_ai_rows[] = {
    { "对话模型", "OpenAI 兼容接口，经 Web 面板配置", NULL, SPLIT_ROW_VALUE, 0 },
    { "视觉识别模型", "拍照识别食材使用", "尚未接入", SPLIT_ROW_PENDING, 0 },
    { "语音合成", "到点播报与小精灵朗读", "由 Web 面板管理", SPLIT_ROW_VALUE, 0 },
    { "API Key", "仅保存在设备 NVS，设备 UI 不显示", "不在设备 UI 显示", SPLIT_ROW_VALUE, 0 },
};

static const split_row_t k_settings_agent_rows[] = {
    { "唤醒词", "板载麦克风 + WakeNet 本地识别", "小冰小冰", SPLIT_ROW_VALUE, 0 },
    { "危险操作", "删除、清空与密钥修改等", "由权限模式决定", SPLIT_ROW_VALUE, 0 },
};

static const split_row_t k_settings_voice_rows[] = {
    { "语音唤醒", "本地 WakeNet，不上传音频", NULL, SPLIT_ROW_VALUE, 0 },
    { "屏幕亮度", "MIPI-DSI 背光调节尚未移植", "固定 100%", SPLIT_ROW_PENDING, 0 },
    { "夜间模式", "夜间自动降低亮度并熄屏", NULL, SPLIT_ROW_PENDING, 0 },
    { "竖屏隐私模式", "旋转到竖屏并关闭摄像头", NULL, SPLIT_ROW_VALUE, 0 },
};

static const split_row_t k_settings_privacy_rows[] = {
    { "拍照本地留存", "识别完成后是否保留本地照片", NULL, SPLIT_ROW_PENDING, 0 },
    { "家庭成员", "通过小程序管理", NULL, SPLIT_ROW_VALUE, 0 },
    { "设备绑定", "扫码连接后端与小程序", NULL, SPLIT_ROW_LINK, SHELL_PAGE_BINDING },
};

static const split_section_t k_settings_sections[] = {
    { "网络与同步", FRIDGE_UI_ICON_WIFI, "W", k_settings_network_rows,
      sizeof(k_settings_network_rows) / sizeof(k_settings_network_rows[0]),
      "Wi-Fi 由板载 ESP32-C6 提供；配网与凭据修改当前在 Web 运维面板完成。" },
    { "AI 模型", FRIDGE_UI_ICON_ZAP, "A", k_settings_ai_rows,
      sizeof(k_settings_ai_rows) / sizeof(k_settings_ai_rows[0]),
      "模型与 API Key 经 Web 面板写入设备 NVS；串口与界面均不回显明文 Key。" },
    { "小精灵权限", FRIDGE_UI_ICON_LOCK, "P", k_settings_agent_rows,
      sizeof(k_settings_agent_rows) / sizeof(k_settings_agent_rows[0]),
      "确认模式会询问写入；标准模式自动普通写入；自动模式会放行全部工具。" },
    { "语音与显示", FRIDGE_UI_ICON_MIC, "V", k_settings_voice_rows,
      sizeof(k_settings_voice_rows) / sizeof(k_settings_voice_rows[0]),
      "背光与夜间模式涉及硬件调光，移植前不提供界面开关。" },
    { "隐私与家庭", FRIDGE_UI_ICON_CAMERA, "S", k_settings_privacy_rows,
      sizeof(k_settings_privacy_rows) / sizeof(k_settings_privacy_rows[0]),
      "二维码只承载 5 分钟一次性 claim token，不包含设备密钥或 MQTT 密码。" },
};

// MORE-04 设备与厨房安全分区。所有行都只消费已有设备/传感器快照，安全事件区在
// Runtime 尚无事件队列时明确展示空态，不填入 ver2 的演示告警。
static const split_row_t k_safety_device_rows[] = {
    { "主控", "当前固件运行的 P4 主控", "ESP32-P4NRW32", SPLIT_ROW_VALUE, 0 },
    { "协处理器", "板载 C6 通过 SDIO 提供联网能力", NULL, SPLIT_ROW_VALUE, 0 },
    { "TF 卡", "字体、图标和本地数据存储", NULL, SPLIT_ROW_VALUE, 0 },
    { "摄像头", "隐私模式下保持硬件关闭", NULL, SPLIT_ROW_VALUE, 0 },
    { "音频输入", "板载麦克风与 ES8311 状态", NULL, SPLIT_ROW_VALUE, 0 },
};

static const split_row_t k_safety_connection_rows[] = {
    { "Wi-Fi", "由板载 ESP32-C6 经 SDIO 提供", NULL, SPLIT_ROW_VALUE, 0 },
    { "云端", "互联网与 AI 服务连通状态", NULL, SPLIT_ROW_VALUE, 0 },
    { "TF 卡", "本地资源挂载状态", NULL, SPLIT_ROW_VALUE, 0 },
    { "时间同步", "SNTP 校时状态", NULL, SPLIT_ROW_VALUE, 0 },
};

static const split_row_t k_safety_sensor_rows[] = {
    { "温度", "SHT4x 最近一次有效读数", NULL, SPLIT_ROW_VALUE, 0 },
    { "湿度", "SHT4x 最近一次有效读数", NULL, SPLIT_ROW_VALUE, 0 },
    { "光照", "光敏 ADC 换算后的亮度百分比", NULL, SPLIT_ROW_VALUE, 0 },
    { "IMU", "MPU6050 就绪与姿态变化", NULL, SPLIT_ROW_VALUE, 0 },
    { "振动峰值", "最近传感器快照中的变化幅度", NULL, SPLIT_ROW_VALUE, 0 },
    { "人体雷达", "存在检测与置信度", NULL, SPLIT_ROW_VALUE, 0 },
};

static const split_row_t k_safety_kitchen_rows[] = {
    { "安全事件", "当前 Runtime 未提供安全事件队列", "暂无事件", SPLIT_ROW_VALUE, 0 },
    { "门体告警", "待接入门磁/状态机事件后展示", "尚未接入", SPLIT_ROW_PENDING, 0 },
    { "确认告警", "当前没有可确认的真实告警", NULL, SPLIT_ROW_PENDING, 0 },
};

// MQ-5 燃气模块只读行：raw/value/percent 均为相对 ADC 读数，不是 ppm，不承载报警阈值。
// 行序把“状态/预热/老化/基线/ADC 占比/错误”放在前 6 行，保证竖屏分区 6 行上限内
// 也能完整覆盖合同要求的展示项；原始/相对读数作为补充行放最后。
static const split_row_t k_safety_gas_rows[] = {
    { "MQ-5 状态", "燃气传感器工作状态", NULL, SPLIT_ROW_VALUE, 0 },
    { "预热进度", "上电 60 秒预热状态", NULL, SPLIT_ROW_VALUE, 0 },
    { "首次老化", "首次长时间通电老化确认", NULL, SPLIT_ROW_VALUE, 0 },
    { "基线标定", "洁净空气基线状态", NULL, SPLIT_ROW_VALUE, 0 },
    { "ADC 占比", "ADC 满量程相对百分比（非 ppm）", NULL, SPLIT_ROW_VALUE, 0 },
    { "错误状态", "最近一次采样/初始化错误", NULL, SPLIT_ROW_VALUE, 0 },
    { "原始读数", "12-bit ADC 均值（0..4095）", NULL, SPLIT_ROW_VALUE, 0 },
    { "相对读数", "缩放到 0..1023 的相对值", NULL, SPLIT_ROW_VALUE, 0 },
};

static const split_section_t k_safety_sections[] = {
    { "设备概览", FRIDGE_UI_ICON_INFO, "D", k_safety_device_rows,
      sizeof(k_safety_device_rows) / sizeof(k_safety_device_rows[0]),
      "只读展示当前 P4、C6、TF、摄像头和音频健康状态。" },
    { "连接状态", FRIDGE_UI_ICON_WIFI, "C", k_safety_connection_rows,
      sizeof(k_safety_connection_rows) / sizeof(k_safety_connection_rows[0]),
      "不在设备端伪造 SSID、MQTT 主题或同步时间；状态以现有网络快照为准。" },
    { "传感器", FRIDGE_UI_ICON_INFO, "S", k_safety_sensor_rows,
      sizeof(k_safety_sensor_rows) / sizeof(k_safety_sensor_rows[0]),
      "传感器读取在各自后台任务完成，页面只读最近一次有界快照。" },
    { "厨房安全", FRIDGE_UI_ICON_SHIELD, "!", k_safety_kitchen_rows,
      sizeof(k_safety_kitchen_rows) / sizeof(k_safety_kitchen_rows[0]),
      "没有真实安全事件时保持中性空态；重新初始化、校准和确认告警暂不执行。" },
    { "燃气检测", FRIDGE_UI_ICON_WARN, "G", k_safety_gas_rows,
      sizeof(k_safety_gas_rows) / sizeof(k_safety_gas_rows[0]),
      "相对 ADC 读数，不是 ppm；当前不触发正式燃气报警，不能替代认证报警器。" },
};

static const char *safety_health_text(fridge_ui_health_t health)
{
    switch (health) {
    case FRIDGE_UI_HEALTH_OK: return "正常";
    case FRIDGE_UI_HEALTH_WARN: return "等待";
    case FRIDGE_UI_HEALTH_DANGER: return "异常";
    case FRIDGE_UI_HEALTH_OFFLINE:
    default: return "未就绪";
    }
}

static const char *settings_wifi_status(char *buffer, size_t buffer_size);

// MORE-04 分区的动态取值只读取 Shell 已有副本，避免在 LVGL 绘制期间访问硬件。
static const char *safety_value_provider(const split_row_t *row, char *buffer,
                                         size_t buffer_size)
{
    if (!row || !row->label || !buffer || buffer_size == 0) return NULL;
    if (strcmp(row->label, "协处理器") == 0) {
        return s_shell.device.wifi == FRIDGE_UI_HEALTH_OK ? "已在线" : "未在线";
    }
    if (strcmp(row->label, "TF 卡") == 0) {
        return safety_health_text(s_shell.device.tfcard);
    }
    if (strcmp(row->label, "摄像头") == 0) {
        return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
               "隐私态关闭" : safety_health_text(s_shell.device.camera);
    }
    if (strcmp(row->label, "音频输入") == 0) return safety_health_text(s_shell.device.microphone);
    if (strcmp(row->label, "Wi-Fi") == 0) {
        return settings_wifi_status(buffer, buffer_size);
    }
    if (strcmp(row->label, "云端") == 0) return safety_health_text(s_shell.device.cloud);
    if (strcmp(row->label, "时间同步") == 0) {
        return s_shell.kitchen.time_ready ? "已同步" : "未同步";
    }
    if (strcmp(row->label, "温度") == 0) return s_shell.device.temperature;
    if (strcmp(row->label, "湿度") == 0) return s_shell.device.humidity;
    if (strcmp(row->label, "光照") == 0) {
        if (!s_shell.safety.light_ready) return "未接入";
        snprintf(buffer, buffer_size, "%u%%", (unsigned)s_shell.safety.light_percent);
        return buffer;
    }
    if (strcmp(row->label, "IMU") == 0) {
        if (!s_shell.safety.imu_ready) return "未接入";
        snprintf(buffer, buffer_size, "已就绪 · Δ%.1f°", (double)s_shell.safety.angle_delta);
        return buffer;
    }
    if (strcmp(row->label, "振动峰值") == 0) {
        if (!s_shell.safety.imu_ready) return "未接入";
        snprintf(buffer, buffer_size, "%.2f g", (double)s_shell.safety.vibration_peak);
        return buffer;
    }
    if (strcmp(row->label, "人体雷达") == 0) {
        if (!s_shell.safety.radar_ready) return "未就绪";
        snprintf(buffer, buffer_size, "%s · %u%%",
                 s_shell.safety.radar_presence ? "有人" : "无人",
                 (unsigned)s_shell.safety.radar_confidence);
        return buffer;
    }
    if (strcmp(row->label, "MQ-5 状态") == 0) {
        if (!s_shell.safety.mq5_ready) {
            return s_shell.safety.mq5_error ? "未接入 · 异常" : "未接入";
        }
        switch (s_shell.safety.mq5_state) {
        case FRIDGE_MQ5_STATE_DISABLED: return "已关闭";
        case FRIDGE_MQ5_STATE_ERROR: return "异常";
        case FRIDGE_MQ5_STATE_WARMING_UP: return "预热中";
        case FRIDGE_MQ5_STATE_NEEDS_BURN_IN: return "等待老化确认";
        case FRIDGE_MQ5_STATE_READY_TO_CALIBRATE: return "等待基线标定";
        case FRIDGE_MQ5_STATE_MONITORING: return "监测中";
        default: return "未知";
        }
    }
    if (strcmp(row->label, "原始读数") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        snprintf(buffer, buffer_size, "%u", (unsigned)s_shell.safety.mq5_raw_12bit);
        return buffer;
    }
    if (strcmp(row->label, "相对读数") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        snprintf(buffer, buffer_size, "%u / 1023", (unsigned)s_shell.safety.mq5_value_10bit);
        return buffer;
    }
    if (strcmp(row->label, "预热进度") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        if (s_shell.safety.mq5_daily_warmup_complete) {
            return "已完成（60 秒）";
        }
        snprintf(buffer, buffer_size, "进行中 · %lus",
                 (unsigned long)s_shell.safety.mq5_warmup_elapsed_sec);
        return buffer;
    }
    if (strcmp(row->label, "首次老化") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        return s_shell.safety.mq5_burn_in_confirmed ? "已确认" : "未确认";
    }
    if (strcmp(row->label, "基线标定") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        if (s_shell.safety.mq5_baseline_ready) {
            snprintf(buffer, buffer_size, "已标定 · %u",
                     (unsigned)s_shell.safety.mq5_baseline_raw_12bit);
            return buffer;
        }
        return "未标定";
    }
    if (strcmp(row->label, "ADC 占比") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        snprintf(buffer, buffer_size, "%u%%", (unsigned)s_shell.safety.mq5_adc_percent);
        return buffer;
    }
    if (strcmp(row->label, "错误状态") == 0) {
        if (!s_shell.safety.mq5_ready) return "未接入";
        return s_shell.safety.mq5_error ? "最近采样异常" : "正常";
    }
    return NULL;
}

// 设置中心动态取值：只读设备真实状态，全部来自已有快照，不新增硬件访问。
static const char *settings_value_provider(const split_row_t *row, char *buffer,
                                           size_t buffer_size)
{
    if (!row || !row->label) return NULL;

    if (strcmp(row->label, "Wi-Fi 网络") == 0) {
        if (s_shell.device.wifi == FRIDGE_UI_HEALTH_OK) {
            snprintf(buffer, buffer_size, "已连接 · %ddBm", (int)s_shell.device.wifi_rssi);
        } else {
            snprintf(buffer, buffer_size, "未连接");
        }
        return buffer;
    }
    if (strcmp(row->label, "云端同步") == 0 || strcmp(row->label, "MQTT 推送") == 0) {
        return s_shell.binding.bound ? "已连接家庭" :
               (s_shell.binding.provisioned ? "等待扫码绑定" : "尚未绑定");
    }
    if (strcmp(row->label, "对话模型") == 0) {
        return s_shell.device.cloud == FRIDGE_UI_HEALTH_OK ? "已配置 · 在线" : "已配置 · 离线";
    }
    if (strcmp(row->label, "API Key") == 0) {
        return "不在设备 UI 显示";
    }
    if (strcmp(row->label, "语音唤醒") == 0) {
        if (!s_shell.settings_initialized) {
            return s_shell.device.microphone == FRIDGE_UI_HEALTH_OK ? "等待设置状态" : "麦克风异常";
        }
        return s_shell.settings.wake_word_user_enabled ?
               (s_shell.settings.wake_word_listening ? "已开启 · 监听中" : "已开启 · 暂停") :
               "用户已关闭";
    }
    if (strcmp(row->label, "竖屏隐私模式") == 0) {
        return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
               "当前已开启" : "在首页详情切换";
    }
    if (strcmp(row->label, "家庭成员") == 0) {
        return s_shell.binding.bound ? "已加入家庭" : "绑定后可管理";
    }
    if (strcmp(row->label, "设备绑定") == 0) {
        return s_shell.binding.bound ? "已绑定" :
               (s_shell.binding.qr_ready ? "可扫码" : "未绑定");
    }
    return NULL;
}

static bool settings_dispatch(fridge_ui_shell_settings_action_t action,
                              const fridge_ui_shell_settings_request_t *request)
{
    if (!s_shell.actions.settings_action) {
        fridge_ui_shell_show_toast("设置服务尚未就绪");
        return false;
    }
    esp_err_t ret = s_shell.actions.settings_action(s_shell.actions.user_ctx, action, request);
    if (ret == ESP_OK) return true;
    fridge_ui_shell_show_toast("设置操作未提交，请重试");
    return false;
}

static uint8_t settings_brightness_percent(void)
{
    if (!s_shell.settings_initialized || s_shell.settings.brightness_percent < 10U ||
        s_shell.settings.brightness_percent > 100U) {
        return 100U;
    }
    return s_shell.settings.brightness_percent;
}

static const char *settings_wake_status(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return "";
    if (!s_shell.settings_initialized) {
        snprintf(buffer, buffer_size, "等待唤醒服务状态");
    } else if (!s_shell.settings.wake_word_user_enabled) {
        snprintf(buffer, buffer_size, "用户已关闭");
    } else if (s_shell.settings.wake_word_listening) {
        snprintf(buffer, buffer_size, "用户已开启 · 正在监听");
    } else if (s_shell.settings.wake_word_detail[0]) {
        snprintf(buffer, buffer_size, "用户已开启 · %s", s_shell.settings.wake_word_detail);
    } else {
        snprintf(buffer, buffer_size, "用户已开启 · 等待策略恢复");
    }
    return buffer;
}

static const char *settings_wifi_status(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return "";
    if (s_shell.settings_initialized && s_shell.settings.wifi_status[0]) {
        return s_shell.settings.wifi_status;
    }
    if (s_shell.device.wifi == FRIDGE_UI_HEALTH_OK) {
        snprintf(buffer, buffer_size, "已连接 · %ddBm", (int)s_shell.device.wifi_rssi);
    } else {
        snprintf(buffer, buffer_size, "未连接");
    }
    return buffer;
}

static const char *settings_agent_permission_name(uint8_t mode)
{
    switch (mode) {
    case 0: return "确认模式";
    case 1: return "标准模式";
    case 2: return "自动模式";
    default: return "确认模式";
    }
}

static void refresh_settings_visible(void)
{
    char text[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
    set_label_if_changed(s_shell.settings_wifi_summary,
                         settings_wifi_status(text, sizeof(text)));

    uint8_t brightness = settings_brightness_percent();
    snprintf(text, sizeof(text), "%u%%", (unsigned)brightness);
    set_label_if_changed(s_shell.settings_brightness_value, text);
    if (s_shell.settings_brightness_slider && !s_shell.settings_brightness_dragging) {
        lv_slider_set_value(s_shell.settings_brightness_slider, brightness, LV_ANIM_OFF);
    }

    char wake[FRIDGE_UI_SHELL_SETTINGS_WAKE_DETAIL_LEN + 24] = {0};
    set_label_if_changed(s_shell.settings_wake_status, settings_wake_status(wake, sizeof(wake)));
    if (s_shell.settings_wake_switch) {
        const bool enabled = s_shell.settings_initialized &&
                             s_shell.settings.wake_word_user_enabled;
        if (enabled) lv_obj_add_state(s_shell.settings_wake_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_shell.settings_wake_switch, LV_STATE_CHECKED);
    }

    for (uint8_t i = 0; i < 3; i++) {
        lv_obj_t *button = s_shell.settings_agent_mode_buttons[i];
        if (!button) continue;
        if (s_shell.settings_initialized && s_shell.settings.agent_permission_mode == i) {
            lv_obj_add_state(button, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(button, LV_STATE_CHECKED);
        }
    }
    if (s_shell.settings_agent_mode_summary) {
        snprintf(text, sizeof(text), "当前：%s", settings_agent_permission_name(
                     s_shell.settings.agent_permission_mode));
        set_label_if_changed(s_shell.settings_agent_mode_summary, text);
    }
}

static void settings_agent_apply_permission_mode(uint8_t mode)
{
    fridge_ui_shell_settings_request_t request = {
        .agent_permission_mode = mode,
    };
    if (settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_SET_AGENT_PERMISSION_MODE, &request)) {
        fridge_ui_shell_show_toast("正在保存小精灵权限模式…");
    }
}

static void settings_agent_auto_cancel_event(lv_event_t *event)
{
    (void)event;
    if (!s_shell.settings_agent_auto_dialog) return;
    lv_obj_delete_async(s_shell.settings_agent_auto_dialog);
    s_shell.settings_agent_auto_dialog = NULL;
}

static void settings_agent_auto_confirm_event(lv_event_t *event)
{
    (void)event;
    settings_agent_auto_cancel_event(NULL);
    settings_agent_apply_permission_mode(2);
}

static void settings_agent_mode_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const uint8_t mode = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (mode > 2) return;
    if (mode != 2 || s_shell.settings.agent_permission_mode == 2) {
        settings_agent_apply_permission_mode(mode);
        return;
    }
    if (s_shell.settings_agent_auto_dialog || !s_shell.split_content) return;

    // 自动模式等价于用户显式选择 full access。确认层只负责说明风险；真正权限修改
    // 仍经 settings_worker 持久化并由 Tool Broker 统一生效。
    const int split_w = split_content_width();
    s_shell.settings_agent_auto_dialog = make_box(s_shell.split_content, 0, 30,
                                                   split_w, 272,
                                                   FRIDGE_UI_COLOR_SURFACE,
                                                   FRIDGE_UI_COLOR_TOMATO, 8);
    make_label(s_shell.settings_agent_auto_dialog, "启用自动模式？", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_TOMATO, 18, 18, 380, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.settings_agent_auto_dialog,
               "此模式会直接执行全部小精灵工具，包括删除、清空和密钥修改。"
               "只应在你完全信任当前使用者和对话内容时启用。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 18, 62,
               split_w - 36, 94, LV_LABEL_LONG_WRAP);
    const int action_w = (split_w - 66) / 2;
    make_button(s_shell.settings_agent_auto_dialog, 22, 204, action_w, 48,
                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                "取消", FRIDGE_FONT_BODY, settings_agent_auto_cancel_event, NULL, NULL);
    make_button(s_shell.settings_agent_auto_dialog, 44 + action_w, 204, action_w, 48,
                FRIDGE_UI_COLOR_TOMATO, 0xFFFFFF, FRIDGE_UI_COLOR_TOMATO,
                "仍然启用", FRIDGE_FONT_BODY, settings_agent_auto_confirm_event, NULL, NULL);
    lv_obj_move_foreground(s_shell.settings_agent_auto_dialog);
}

static void settings_brightness_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *slider = lv_event_get_target(event);
    if (!slider) return;
    if (code == LV_EVENT_PRESSED) {
        s_shell.settings_brightness_dragging = true;
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        s_shell.settings_brightness_dragging = false;
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;

    uint8_t brightness = (uint8_t)lv_slider_get_value(slider);
    s_shell.settings.brightness_percent = brightness;
    s_shell.settings_initialized = true;
    char text[12] = {0};
    snprintf(text, sizeof(text), "%u%%", (unsigned)brightness);
    set_label_if_changed(s_shell.settings_brightness_value, text);

    fridge_ui_shell_settings_request_t request = {
        .brightness_percent = brightness,
        // 拖动中只实时更新 PWM；真实抬手时才让后台任务写 brightness_pct NVS。
        .persist = code == LV_EVENT_RELEASED,
    };
    (void)settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_SET_BRIGHTNESS, &request);
    if (code == LV_EVENT_RELEASED) {
        s_shell.settings_brightness_dragging = false;
    }
    fridge_ui_scroll_ppa_invalidate_cache();
}

static void settings_wake_switch_event(lv_event_t *event)
{
    lv_obj_t *toggle = lv_event_get_target(event);
    if (!toggle) return;
    const bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    s_shell.settings.wake_word_user_enabled = enabled;
    s_shell.settings_initialized = true;
    fridge_ui_shell_settings_request_t request = {
        .wake_word_enabled = enabled,
        .persist = true,
    };
    (void)settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_SET_WAKE_WORD_ENABLED, &request);
    refresh_settings_visible();
    fridge_ui_scroll_ppa_invalidate_cache();
}

static void settings_wifi_modal_refresh_list(void);
static void settings_wifi_show_password_panel(void);
static void settings_wifi_submit_connect(const char *password);

static void settings_wifi_close_event(lv_event_t *event)
{
    (void)event;
    close_settings_wifi_modal();
}

static void settings_wifi_scan_event(lv_event_t *event)
{
    (void)event;
    fridge_ui_shell_settings_request_t request = {0};
    if (!settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN, &request)) return;
    s_shell.settings.wifi_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_SCANNING;
    strlcpy(s_shell.settings.wifi_status, "正在扫描附近 Wi-Fi…",
            sizeof(s_shell.settings.wifi_status));
    settings_wifi_modal_refresh_list();
}

static void settings_wifi_password_back_event(lv_event_t *event)
{
    (void)event;
    if (!s_shell.settings_wifi_dialog) return;
    if (s_shell.settings_wifi_password_input) {
        lv_textarea_set_text(s_shell.settings_wifi_password_input, "");
    }
    if (s_shell.settings_wifi_list_panel) {
        lv_obj_remove_flag(s_shell.settings_wifi_list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shell.settings_wifi_password_panel) {
        lv_obj_add_flag(s_shell.settings_wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_shell.settings_wifi_modal_mode = SETTINGS_WIFI_MODAL_LIST;
    settings_wifi_modal_refresh_list();
}

static void settings_wifi_connect_event(lv_event_t *event)
{
    (void)event;
    const char *password = s_shell.settings_wifi_password_input ?
                           lv_textarea_get_text(s_shell.settings_wifi_password_input) : "";
    if (s_shell.settings_wifi_selected_secured && (!password || !password[0])) {
        fridge_ui_shell_show_toast("请输入 Wi-Fi 密码");
        return;
    }
    settings_wifi_submit_connect(password ? password : "");
}

static void settings_wifi_keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) settings_wifi_connect_event(event);
    else if (code == LV_EVENT_CANCEL) settings_wifi_password_back_event(event);
}

static void settings_wifi_ap_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const settings_wifi_row_ctx_t *row = lv_event_get_user_data(event);
    if (!row || !row->ssid[0]) return;
    strlcpy(s_shell.settings_wifi_selected_ssid, row->ssid,
            sizeof(s_shell.settings_wifi_selected_ssid));
    s_shell.settings_wifi_selected_secured = row->secured;
    if (row->saved) {
        fridge_ui_shell_settings_request_t request = {0};
        strlcpy(request.ssid, row->ssid, sizeof(request.ssid));
        if (settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT_SAVED, &request)) {
            s_shell.settings.wifi_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTING;
            strlcpy(s_shell.settings.wifi_status, "正在使用已保存凭据并等待 DHCP…",
                    sizeof(s_shell.settings.wifi_status));
            settings_wifi_modal_refresh_list();
        }
        return;
    }
    if (row->secured) settings_wifi_show_password_panel();
    else settings_wifi_submit_connect("");
}

static void pause_settings_ppa_for_modal(void)
{
    // 先把可能正在运行的手势帧收尾，再停止 PPA。这样旧快照不会在弹窗创建后
    // 回写到双缓冲表面并覆盖动态对话框。
    fridge_ui_scroll_ppa_request_finish();
    fridge_ui_scroll_ppa_process();
    fridge_ui_scroll_ppa_set_enabled(false);
}

static void close_settings_wifi_modal(void)
{
    if (!s_shell.settings_wifi_scrim) return;
    if (s_shell.settings_wifi_password_input) {
        // 密码仅在 LVGL 文本框和一次后台请求中短暂存在；关闭时立即清空控件文本。
        lv_textarea_set_text(s_shell.settings_wifi_password_input, "");
    }
    lv_obj_add_flag(s_shell.settings_wifi_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(s_shell.settings_wifi_scrim);
    s_shell.settings_wifi_scrim = NULL;
    s_shell.settings_wifi_dialog = NULL;
    s_shell.settings_wifi_list_panel = NULL;
    s_shell.settings_wifi_password_panel = NULL;
    s_shell.settings_wifi_status = NULL;
    s_shell.settings_wifi_list = NULL;
    s_shell.settings_wifi_selected_label = NULL;
    s_shell.settings_wifi_password_input = NULL;
    s_shell.settings_wifi_keyboard = NULL;
    s_shell.settings_wifi_modal_mode = SETTINGS_WIFI_MODAL_NONE;
    s_shell.settings_wifi_modal_ap_count = 0;
    s_shell.settings_wifi_modal_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_IDLE;
    s_shell.settings_wifi_modal_status[0] = '\0';
    s_shell.settings_wifi_selected_ssid[0] = '\0';
    s_shell.settings_wifi_selected_secured = false;
    if (s_shell.page == SHELL_PAGE_SETTINGS) {
        // 横屏/竖屏都在弹层关闭后重新失效静态缓存；竖屏滚动对象由 ui_p4 已绑定的
        // portrait_scroll 继续承接，不能把旧横屏对象重新绑定回来。
        fridge_ui_scroll_ppa_set_enabled(true);
        fridge_ui_scroll_ppa_invalidate_cache();
        lv_obj_invalidate(s_shell.root);
    }
}

static void settings_wifi_modal_refresh_list(void)
{
    if (!s_shell.settings_wifi_dialog) return;
    char status[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN] = {0};
    set_label_if_changed(s_shell.settings_wifi_status,
                         settings_wifi_status(status, sizeof(status)));
    if (!s_shell.settings_wifi_list ||
        s_shell.settings_wifi_modal_mode != SETTINGS_WIFI_MODAL_LIST) return;

    const bool changed = s_shell.settings_wifi_modal_ap_count != s_shell.settings.wifi_ap_count ||
                         s_shell.settings_wifi_modal_phase != s_shell.settings.wifi_phase ||
                         strcmp(s_shell.settings_wifi_modal_status,
                                s_shell.settings.wifi_status) != 0 ||
                         memcmp(s_shell.settings_wifi_modal_aps, s_shell.settings.wifi_aps,
                                s_shell.settings.wifi_ap_count *
                                    sizeof(s_shell.settings.wifi_aps[0])) != 0;
    if (!changed) return;
    s_shell.settings_wifi_modal_ap_count = s_shell.settings.wifi_ap_count;
    s_shell.settings_wifi_modal_phase = s_shell.settings.wifi_phase;
    strlcpy(s_shell.settings_wifi_modal_status, s_shell.settings.wifi_status,
            sizeof(s_shell.settings_wifi_modal_status));
    memcpy(s_shell.settings_wifi_modal_aps, s_shell.settings.wifi_aps,
           s_shell.settings.wifi_ap_count * sizeof(s_shell.settings.wifi_aps[0]));
    lv_obj_clean(s_shell.settings_wifi_list);

    size_t count = s_shell.settings.wifi_ap_count;
    if (count > FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS) {
        count = FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS;
    }
    if (count == 0) {
        const char *empty = s_shell.settings.wifi_phase == FRIDGE_UI_SHELL_SETTINGS_WIFI_SCANNING ?
                            "正在扫描附近 2.4GHz 热点…" : "暂无扫描结果，点击“重新扫描”后重试。";
        const int list_width = (int)lv_obj_get_width(s_shell.settings_wifi_list);
        lv_obj_t *label = make_label(s_shell.settings_wifi_list, empty, FRIDGE_FONT_BODY,
                                     FRIDGE_UI_COLOR_MUTED, 12, 20, list_width - 24,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(label);
        return;
    }
    const int list_width = (int)lv_obj_get_width(s_shell.settings_wifi_list);
    for (size_t i = 0; i < count; i++) {
        const fridge_ui_shell_settings_wifi_ap_t *ap = &s_shell.settings.wifi_aps[i];
        settings_wifi_row_ctx_t *ctx = &s_shell.settings_wifi_rows[i];
        strlcpy(ctx->ssid, ap->ssid, sizeof(ctx->ssid));
        ctx->secured = ap->secured;
        ctx->saved = ap->saved;
        lv_obj_t *row = lv_button_create(s_shell.settings_wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, (int)i * UI_SETTINGS_WIFI_ROW_H);
        lv_obj_set_size(row, list_width - 4, UI_SETTINGS_WIFI_ROW_H - 4);
        style_box(row, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8, 1);
        lv_obj_set_style_bg_color(row, color_hex(FRIDGE_UI_COLOR_MINT), LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        add_click_event(row, settings_wifi_ap_event, ctx);
        make_label(row, ap->ssid[0] ? ap->ssid : "隐藏网络", FRIDGE_FONT_BODY_EMPHASIS,
                   FRIDGE_UI_COLOR_INK, 12, 8, list_width - 170,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        char meta[72] = {0};
        snprintf(meta, sizeof(meta), "%ddBm · Ch%u · %s", (int)ap->rssi,
                 (unsigned)ap->channel,
                 ap->secured ? (ap->authmode[0] ? ap->authmode : "加密") : "开放");
        make_label(row, meta, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                   12, 34, list_width - 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(row, ap->saved ? "已保存" : (ap->secured ? "输入密码" : "连接"),
                   FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                   FRIDGE_UI_COLOR_SAGE_DARK, list_width - 112, 22, 96, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
    }
}

static void settings_wifi_show_password_panel(void)
{
    if (!s_shell.settings_wifi_dialog || !s_shell.settings_wifi_password_panel) return;
    if (s_shell.settings_wifi_list_panel) {
        lv_obj_add_flag(s_shell.settings_wifi_list_panel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_shell.settings_wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    s_shell.settings_wifi_modal_mode = SETTINGS_WIFI_MODAL_PASSWORD;
    char selected[80] = {0};
    snprintf(selected, sizeof(selected), "网络：%s", s_shell.settings_wifi_selected_ssid);
    set_label_if_changed(s_shell.settings_wifi_selected_label, selected);
    if (s_shell.settings_wifi_password_input) {
        lv_textarea_set_text(s_shell.settings_wifi_password_input, "");
        lv_obj_add_state(s_shell.settings_wifi_password_input, LV_STATE_FOCUSED);
    }
}

static void settings_wifi_submit_connect(const char *password)
{
    if (!s_shell.settings_wifi_selected_ssid[0]) return;
    fridge_ui_shell_settings_request_t request = {0};
    strlcpy(request.ssid, s_shell.settings_wifi_selected_ssid, sizeof(request.ssid));
    strlcpy(request.password, password ? password : "", sizeof(request.password));
    request.save_after_verified = true;
    if (settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT, &request)) {
        s_shell.settings.wifi_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTING;
        strlcpy(s_shell.settings.wifi_status, "正在连接，随后验证 DHCP 与 SNTP…",
                sizeof(s_shell.settings.wifi_status));
        settings_wifi_modal_refresh_list();
    }
    // callback 已按接口约定同步复制请求；Shell 不长期保留或记录密码。
    memset(&request, 0, sizeof(request));
    if (s_shell.settings_wifi_password_input) {
        lv_textarea_set_text(s_shell.settings_wifi_password_input, "");
    }
}

static void settings_wifi_open_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event) || s_shell.settings_wifi_scrim) return;
    fridge_ui_shell_settings_request_t request = {0};
    if (!settings_dispatch(FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN, &request)) return;
    s_shell.settings.wifi_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_SCANNING;
    strlcpy(s_shell.settings.wifi_status, "正在扫描附近 Wi-Fi…",
            sizeof(s_shell.settings.wifi_status));
    pause_settings_ppa_for_modal();

    const bool portrait = s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    const int root_width = portrait ? UI_PORTRAIT_W : 1024;
    const int root_height = portrait ? UI_PORTRAIT_MAIN_H : UI_SCREEN_H;
    const int modal_width = portrait ? 568 : UI_SETTINGS_MODAL_W;
    const int modal_height = portrait ? 620 : UI_SETTINGS_MODAL_H;
    const int panel_width = modal_width - 40;
    const int panel_height = portrait ? 506 : 430;
    const int list_height = portrait ? 400 : 350;
    const int action_y = portrait ? 444 : 370;
    const int keyboard_height = portrait ? 288 : 232;

    s_shell.settings_wifi_scrim = make_box(s_shell.root, 0, 0, root_width, root_height,
                                            FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.settings_wifi_scrim, LV_OPA_50, 0);
    s_shell.settings_wifi_dialog = make_box(s_shell.settings_wifi_scrim,
                                             (root_width - modal_width) / 2,
                                             (root_height - modal_height) / 2,
                                             modal_width, modal_height,
                                             FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.settings_wifi_dialog, "连接 Wi-Fi", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_INK, 20, 16, modal_width - 132,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(s_shell.settings_wifi_dialog, modal_width - 92, 12, 72, 40,
                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                "关闭", FRIDGE_FONT_BODY_SMALL, settings_wifi_close_event, NULL, NULL);
    s_shell.settings_wifi_status = make_label(s_shell.settings_wifi_dialog, "", 
                                               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                               20, 54, panel_width,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    s_shell.settings_wifi_list_panel = lv_obj_create(s_shell.settings_wifi_dialog);
    clear_container_style(s_shell.settings_wifi_list_panel);
    lv_obj_set_pos(s_shell.settings_wifi_list_panel, 20, 86);
    lv_obj_set_size(s_shell.settings_wifi_list_panel, panel_width, panel_height);
    s_shell.settings_wifi_list = lv_obj_create(s_shell.settings_wifi_list_panel);
    lv_obj_remove_style_all(s_shell.settings_wifi_list);
    lv_obj_set_pos(s_shell.settings_wifi_list, 0, 0);
    lv_obj_set_size(s_shell.settings_wifi_list, panel_width, list_height);
    lv_obj_set_scroll_dir(s_shell.settings_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_shell.settings_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(s_shell.settings_wifi_list, 8, 0);
    make_button(s_shell.settings_wifi_list_panel, 0, action_y, 170, 44,
                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                "重新扫描", FRIDGE_FONT_BODY, settings_wifi_scan_event, NULL, NULL);
    make_label(s_shell.settings_wifi_list_panel, "选择热点后仅在验证成功时保存凭据",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 188, action_y + 12,
               panel_width - 188,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    s_shell.settings_wifi_password_panel = lv_obj_create(s_shell.settings_wifi_dialog);
    clear_container_style(s_shell.settings_wifi_password_panel);
    lv_obj_set_pos(s_shell.settings_wifi_password_panel, 20, 86);
    lv_obj_set_size(s_shell.settings_wifi_password_panel, panel_width, panel_height);
    lv_obj_add_flag(s_shell.settings_wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    s_shell.settings_wifi_selected_label = make_label(s_shell.settings_wifi_password_panel, "",
                                                       FRIDGE_FONT_BODY_EMPHASIS,
                                                       FRIDGE_UI_COLOR_INK, 0, 0, panel_width,
                                                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.settings_wifi_password_panel, "密码仅用于本次连接测试，不会显示或写入日志。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 0, 28, panel_width,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.settings_wifi_password_input = lv_textarea_create(s_shell.settings_wifi_password_panel);
    lv_obj_set_pos(s_shell.settings_wifi_password_input, 0, 60);
    lv_obj_set_size(s_shell.settings_wifi_password_input, panel_width, 48);
    lv_textarea_set_one_line(s_shell.settings_wifi_password_input, true);
    lv_textarea_set_password_mode(s_shell.settings_wifi_password_input, true);
    lv_textarea_set_max_length(s_shell.settings_wifi_password_input,
                               FRIDGE_UI_SHELL_SETTINGS_WIFI_PASSWORD_LEN);
    // 密码输入框和键盘不依赖全局 LVGL 默认主题。设置页切换时默认主题的内边距会让
    // buttonmatrix 的按键区域重新计算到错误位置，因此这里固定完整的视觉与几何参数。
    lv_obj_set_style_bg_color(s_shell.settings_wifi_password_input,
                              color_hex(FRIDGE_UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_shell.settings_wifi_password_input, LV_OPA_COVER,
                            LV_PART_MAIN);
    lv_obj_set_style_border_color(s_shell.settings_wifi_password_input,
                                  color_hex(FRIDGE_UI_COLOR_LINE_STRONG), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_shell.settings_wifi_password_input, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_shell.settings_wifi_password_input, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_shell.settings_wifi_password_input, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_shell.settings_wifi_password_input, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_shell.settings_wifi_password_input, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_shell.settings_wifi_password_input, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_shell.settings_wifi_password_input,
                                color_hex(FRIDGE_UI_COLOR_INK), LV_PART_MAIN);
    lv_obj_add_style(s_shell.settings_wifi_password_input,
                     fridge_ui_theme_font_style(FRIDGE_FONT_BODY), 0);
    s_shell.settings_wifi_keyboard = lv_keyboard_create(s_shell.settings_wifi_password_panel);
    lv_obj_remove_style_all(s_shell.settings_wifi_keyboard);
    lv_obj_set_size(s_shell.settings_wifi_keyboard, panel_width, keyboard_height);
    // lv_keyboard 构造时会先按父容器底部对齐；这里在固定尺寸后再次锚定到密码页，
    // 防止在列表/密码面板显隐切换后沿用构造时的偏移坐标。
    lv_obj_align(s_shell.settings_wifi_keyboard, LV_ALIGN_TOP_LEFT, 0, 120);
    lv_obj_set_style_bg_color(s_shell.settings_wifi_keyboard,
                              color_hex(FRIDGE_UI_COLOR_CREAM), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_shell.settings_wifi_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_shell.settings_wifi_keyboard,
                                  color_hex(FRIDGE_UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_shell.settings_wifi_keyboard, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_shell.settings_wifi_keyboard, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_shell.settings_wifi_keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_shell.settings_wifi_keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_shell.settings_wifi_keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_shell.settings_wifi_keyboard,
                              color_hex(FRIDGE_UI_COLOR_SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_shell.settings_wifi_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_shell.settings_wifi_keyboard,
                                  color_hex(FRIDGE_UI_COLOR_LINE), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_shell.settings_wifi_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_shell.settings_wifi_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_shell.settings_wifi_keyboard,
                                color_hex(FRIDGE_UI_COLOR_INK), LV_PART_ITEMS);
    // 键盘的退格、模式切换和确认键使用 LV_SYMBOL 私有字形；正文 CJK 字体没有这些
    // codepoint，必须显式使用 LVGL 默认符号字体，避免显示成方框或斜线。
    lv_obj_set_style_text_font(s_shell.settings_wifi_keyboard, LV_FONT_DEFAULT, LV_PART_ITEMS);
    lv_keyboard_set_mode(s_shell.settings_wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_shell.settings_wifi_keyboard, s_shell.settings_wifi_password_input);
    lv_obj_add_event_cb(s_shell.settings_wifi_keyboard, settings_wifi_keyboard_event,
                        LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_shell.settings_wifi_keyboard, settings_wifi_keyboard_event,
                        LV_EVENT_CANCEL, NULL);
    make_button(s_shell.settings_wifi_password_panel, 0, action_y, 150, 44,
                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                "返回热点", FRIDGE_FONT_BODY, settings_wifi_password_back_event, NULL, NULL);
    make_button(s_shell.settings_wifi_password_panel, panel_width - 170, action_y, 170, 44,
                FRIDGE_UI_COLOR_SAGE, 0xFFFFFF, FRIDGE_UI_COLOR_SAGE,
                "连接并测试", FRIDGE_FONT_BODY, settings_wifi_connect_event, NULL, NULL);

    s_shell.settings_wifi_modal_mode = SETTINGS_WIFI_MODAL_LIST;
    s_shell.settings_wifi_modal_ap_count = SIZE_MAX;
    s_shell.settings_wifi_modal_phase = FRIDGE_UI_SHELL_SETTINGS_WIFI_IDLE;
    s_shell.settings_wifi_modal_status[0] = '\0';
    lv_obj_move_foreground(s_shell.settings_wifi_scrim);
    settings_wifi_modal_refresh_list();
}

static lv_obj_t *create_settings_content_host(lv_obj_t *page, const char *title, int total_h)
{
    s_shell.split_content_icon_slot_start = s_shell.icon_slot_count;
    lv_obj_t *host = lv_obj_create(page);
    clear_container_style(host);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(host, split_content_x(), 0);
    lv_obj_set_size(host, split_content_width(), total_h);
    s_shell.split_content = host;
    make_label(host, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, 0, split_content_width(), LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    return host;
}

static void create_settings_network_content(lv_obj_t *page)
{
    const int width = split_content_width();
    const bool portrait = s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    lv_obj_t *host = create_settings_content_host(page, "网络与同步", portrait ? 390 : 340);
    lv_obj_t *card = make_box(host, 0, 30, width, portrait ? 282 : 234,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    make_label(card, "Wi-Fi 网络", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, 12, 210, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.settings_wifi_summary = make_label(card, "", FRIDGE_FONT_BODY_SMALL,
                                                FRIDGE_UI_COLOR_MUTED, 16, 42,
                                                portrait ? width - 156 : 300,
                                                LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(card, portrait ? width - 132 : 342, 16, 118, 44, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "管理 Wi-Fi",
                FRIDGE_FONT_BODY_SMALL, settings_wifi_open_event, NULL, NULL);
    make_solid_line(card, 16, 77, width - 32, 1, FRIDGE_UI_COLOR_LINE);
    create_split_row(card, 78, &k_settings_network_rows[1], settings_value_provider);
    make_solid_line(card, 16, 77 + split_row_height(), width - 32, 1, FRIDGE_UI_COLOR_LINE);
    create_split_row(card, 78 + split_row_height(), &k_settings_network_rows[2], settings_value_provider);
    make_label(host, "扫描、连接与 NTP 验证均在后台执行；验证失败不会覆盖原已保存网络。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 0,
               portrait ? 326 : 278, width,
               LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
}

static void create_settings_voice_content(lv_obj_t *page)
{
    const int width = split_content_width();
    const bool portrait = s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    lv_obj_t *host = create_settings_content_host(page, "语音与显示", portrait ? 330 : 286);
    lv_obj_t *card = make_box(host, 0, 30, width, 184,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    make_label(card, "语音唤醒", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, 12, 210, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(card, "本地 WakeNet，不上传音频", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 16, 40, 300, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.settings_wake_status = make_label(card, "", FRIDGE_FONT_BODY_SMALL,
                                               FRIDGE_UI_COLOR_SAGE_DARK, 16, 64, 356,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.settings_wake_switch = lv_switch_create(card);
    lv_obj_set_pos(s_shell.settings_wake_switch, portrait ? width - 70 : 402, 24);
    lv_obj_set_size(s_shell.settings_wake_switch, 54, 30);
    lv_obj_set_style_bg_color(s_shell.settings_wake_switch,
                              color_hex(FRIDGE_UI_COLOR_OFFLINE_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_shell.settings_wake_switch,
                              color_hex(FRIDGE_UI_COLOR_SAGE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_shell.settings_wake_switch, scroll_press_track_event,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_shell.settings_wake_switch, scroll_press_track_event,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_shell.settings_wake_switch, scroll_press_track_event,
                        LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_shell.settings_wake_switch, settings_wake_switch_event,
                        LV_EVENT_VALUE_CHANGED, NULL);
    make_solid_line(card, 16, 91, width - 32, 1, FRIDGE_UI_COLOR_LINE);
    make_label(card, "屏幕亮度", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, 104, 210, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    // 100% 使用强调字体时实际字宽超过原先 52px，导致只显示首字符。保留右边距，
    // 将数值区扩至 90px，并与左侧说明留出 50px 空隙，避免视觉拥挤。
    s_shell.settings_brightness_value = make_label(card, "", FRIDGE_FONT_BODY_EMPHASIS,
                                                    FRIDGE_UI_COLOR_SAGE_DARK,
                                                    portrait ? width - 94 : 366, 104,
                                                    portrait ? 78 : 90,
                                                    LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.settings_brightness_value, LV_TEXT_ALIGN_RIGHT, 0);
    s_shell.settings_brightness_slider = lv_slider_create(card);
    lv_obj_set_pos(s_shell.settings_brightness_slider, 16, 158);
    lv_obj_set_size(s_shell.settings_brightness_slider, width - 32, 12);
    lv_slider_set_range(s_shell.settings_brightness_slider, 10, 100);
    lv_slider_set_value(s_shell.settings_brightness_slider, settings_brightness_percent(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_shell.settings_brightness_slider,
                              color_hex(FRIDGE_UI_COLOR_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_shell.settings_brightness_slider,
                              color_hex(FRIDGE_UI_COLOR_SAGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_shell.settings_brightness_slider,
                              color_hex(FRIDGE_UI_COLOR_SAGE), LV_PART_KNOB);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, scroll_press_track_event,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, scroll_press_track_event,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, scroll_press_track_event,
                        LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, settings_brightness_event,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, settings_brightness_event,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, settings_brightness_event,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_shell.settings_brightness_slider, settings_brightness_event,
                        LV_EVENT_PRESS_LOST, NULL);
    make_label(host, "拖动时实时调节背光；松手后才由后台保存设置。WakeNet 的加载与停止不阻塞界面。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 0, 228, width,
               LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
    refresh_settings_visible();
}

static lv_obj_t *create_settings_agent_mode_button(lv_obj_t *parent, int y, uint8_t mode,
                                                    const char *title, const char *detail)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, 16, y);
    const int width = split_content_width();
    lv_obj_set_size(button, width - 32, 58);
    style_box(button, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8, 1);
    lv_obj_set_style_bg_color(button, color_hex(FRIDGE_UI_COLOR_MINT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, color_hex(FRIDGE_UI_COLOR_MINT), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(button, color_hex(FRIDGE_UI_COLOR_SAGE), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(button, 2, LV_STATE_CHECKED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    add_click_event(button, settings_agent_mode_event, (void *)(uintptr_t)mode);
    make_label(button, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               12, 6, 132, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(button, detail, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               12, 31, width - 56, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    return button;
}

static void create_settings_agent_content(lv_obj_t *page)
{
    const int width = split_content_width();
    lv_obj_t *host = create_settings_content_host(page, "小精灵权限", 376);
    lv_obj_t *card = make_box(host, 0, 30, width, 310,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    make_label(card, "选择执行权限", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, 12, 210, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.settings_agent_mode_summary = make_label(card, "", FRIDGE_FONT_BODY_SMALL,
                                                      FRIDGE_UI_COLOR_SAGE_DARK,
                                                      width - 152 - 16, 14, 152, LV_SIZE_CONTENT,
                                                      LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.settings_agent_mode_summary, LV_TEXT_ALIGN_RIGHT, 0);
    s_shell.settings_agent_mode_buttons[0] = create_settings_agent_mode_button(
        card, 48, 0, "确认模式", "写入、删除、清空与密钥修改均需确认");
    s_shell.settings_agent_mode_buttons[1] = create_settings_agent_mode_button(
        card, 112, 1, "标准模式", "普通写入自动执行；危险操作仍需确认");
    s_shell.settings_agent_mode_buttons[2] = create_settings_agent_mode_button(
        card, 176, 2, "自动模式", "全部操作直接执行，等同完全访问权限");
    make_label(host, "模式会保存到设备 NVS，并同时作用于对话页、连续语音和本地工具调用。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 0, 348,
               width, LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
    refresh_settings_visible();
}

static void create_settings_split_content(lv_obj_t *page)
{
    const size_t count = sizeof(k_settings_sections) / sizeof(k_settings_sections[0]);
    if (s_shell.settings_section >= count) s_shell.settings_section = 0;
    // 删除旧内容后这些指针必须只指向当前右栏，后台状态更新才能保持 LVGL 所有权正确。
    s_shell.settings_wifi_summary = NULL;
    s_shell.settings_brightness_value = NULL;
    s_shell.settings_brightness_slider = NULL;
    s_shell.settings_wake_switch = NULL;
    s_shell.settings_wake_status = NULL;
    memset(s_shell.settings_agent_mode_buttons, 0, sizeof(s_shell.settings_agent_mode_buttons));
    s_shell.settings_agent_mode_summary = NULL;
    s_shell.settings_agent_auto_dialog = NULL;
    if (s_shell.settings_section == 0) {
        create_settings_network_content(page);
    } else if (s_shell.settings_section == 2) {
        create_settings_agent_content(page);
    } else if (s_shell.settings_section == 3) {
        create_settings_voice_content(page);
    } else {
        create_split_content(page, &k_settings_sections[s_shell.settings_section],
                             settings_value_provider);
    }
}

static void create_settings_page(lv_obj_t *page)
{
    const size_t count = sizeof(k_settings_sections) / sizeof(k_settings_sections[0]);
    if (s_shell.settings_section >= count) s_shell.settings_section = 0;
    for (size_t i = 0; i < count && i < UI_SPLIT_SECTION_MAX; i++) {
        create_split_tab(page, i, &k_settings_sections[i], i == s_shell.settings_section);
    }
    create_settings_split_content(page);
    refresh_settings_visible();
}

// ---------------------------------------------------------------------------
// MORE-06 维护中心：存储与关于分区展示真实容量、版本和硬件信息；
// 插件、日志、系统更新当前只读占位，界面不触发 Flash 写入、日志导出或 OTA。
// ---------------------------------------------------------------------------
static const split_row_t k_maintenance_storage_rows[] = {
    { "TF 卡状态", "UI 资源、字体与会话记录均存放于此", NULL, SPLIT_ROW_VALUE, 0 },
    { "总容量", "SDMMC Slot0，与 C6 共用 LDO4 电源域", NULL, SPLIT_ROW_VALUE, 0 },
    { "可用空间", "低于 10% 时建议清理缓存", NULL, SPLIT_ROW_VALUE, 0 },
    { "清理缓存", "删除临时截图与过期缓存", NULL, SPLIT_ROW_PENDING, 0 },
};

static const split_row_t k_maintenance_plugin_rows[] = {
    { "临期提醒", "读库存 / 发通知", "内置能力", SPLIT_ROW_STATE_ON, 0 },
    { "菜谱生成器", "读库存 / 联网", "内置能力", SPLIT_ROW_STATE_ON, 0 },
    { "插件管理", "第三方插件安装与权限控制", NULL, SPLIT_ROW_PENDING, 0 },
};

static const split_row_t k_maintenance_log_rows[] = {
    { "运行日志", "记录到 TF 卡，串口可实时查看", NULL, SPLIT_ROW_VALUE, 0 },
    { "启动次数", "含正常重启与异常复位", NULL, SPLIT_ROW_VALUE, 0 },
    { "导出诊断包", "打包日志与状态到 TF 卡", NULL, SPLIT_ROW_PENDING, 0 },
};

static const split_row_t k_maintenance_ota_rows[] = {
    { "当前固件", "运行中的应用分区镜像", NULL, SPLIT_ROW_VALUE, 0 },
    { "启动槽位", "主固件使用 6MiB ota_0", "ota_0", SPLIT_ROW_VALUE, 0 },
    { "检查更新", "在线检查与下载新固件", NULL, SPLIT_ROW_PENDING, 0 },
    { "开始 OTA", "升级中断电存在变砖风险", NULL, SPLIT_ROW_PENDING, 0 },
};

static const split_row_t k_maintenance_about_rows[] = {
    { "设备型号", NULL, "P4 CB V1.2", SPLIT_ROW_VALUE, 0 },
    { "主控", NULL, "P4NRW32", SPLIT_ROW_VALUE, 0 },
    { "内存与存储", NULL, "32M PSRAM/16M Flash", SPLIT_ROW_VALUE, 0 },
    { "协处理器", "esp-hosted SDIO", "C6-MINI-1", SPLIT_ROW_VALUE, 0 },
    { "设备编号", "绑定后由后端签发", NULL, SPLIT_ROW_VALUE, 0 },
};

static const split_section_t k_maintenance_sections[] = {
    { "存储", FRIDGE_UI_ICON_TF, "T", k_maintenance_storage_rows,
      sizeof(k_maintenance_storage_rows) / sizeof(k_maintenance_storage_rows[0]),
      "TF 卡承载字体、图标与会话记录；拔卡会导致界面回退到 Flash 降级资源。" },
    { "插件", FRIDGE_UI_ICON_LIST, "G", k_maintenance_plugin_rows,
      sizeof(k_maintenance_plugin_rows) / sizeof(k_maintenance_plugin_rows[0]),
      "当前能力由固件内置 Tool 提供；第三方插件框架尚未移植到 P4。" },
    { "日志", FRIDGE_UI_ICON_INFO, "L", k_maintenance_log_rows,
      sizeof(k_maintenance_log_rows) / sizeof(k_maintenance_log_rows[0]),
      "完整日志与诊断包导出当前经 Web 运维面板的 TF 卡工作台完成。" },
    { "系统更新", FRIDGE_UI_ICON_CLOUD, "O", k_maintenance_ota_rows,
      sizeof(k_maintenance_ota_rows) / sizeof(k_maintenance_ota_rows[0]),
      "设备端 OTA 尚未开放；升级仍需通过 USB 串口烧录，避免升级中断电变砖。" },
    { "关于", FRIDGE_UI_ICON_STAR, "I", k_maintenance_about_rows,
      sizeof(k_maintenance_about_rows) / sizeof(k_maintenance_about_rows[0]),
      "恢复出厂设置会清空全部本地数据，风险较高，暂不在设备端提供入口。" },
};

// 维护中心动态取值：TF 容量与固件版本来自已有运行时接口，不新增硬件访问。
static const char *maintenance_value_provider(const split_row_t *row, char *buffer,
                                              size_t buffer_size)
{
    if (!row || !row->label) return NULL;

    if (strcmp(row->label, "TF 卡状态") == 0) {
        return s_shell.device.tfcard == FRIDGE_UI_HEALTH_OK ? "已挂载" : "未挂载";
    }
    if (strcmp(row->label, "总容量") == 0 || strcmp(row->label, "可用空间") == 0) {
        fridge_tfcard_status_t tf = {0};
        fridge_tfcard_get_status(&tf);
        uint64_t mb = (strcmp(row->label, "总容量") == 0) ? tf.capacity_mb : tf.free_mb;
        if (!tf.mounted || mb == 0) return "--";
        if (mb >= 1024ULL) {
            snprintf(buffer, buffer_size, "%.1f GB", (double)mb / 1024.0);
        } else {
            snprintf(buffer, buffer_size, "%llu MB", (unsigned long long)mb);
        }
        return buffer;
    }
    if (strcmp(row->label, "运行日志") == 0) {
        return s_shell.device.tfcard == FRIDGE_UI_HEALTH_OK ? "记录中" : "仅串口输出";
    }
    if (strcmp(row->label, "启动次数") == 0) {
        fridge_diagnostic_snapshot_t diag = {0};
        if (fridge_diagnostics_get_snapshot(&diag) == ESP_OK) {
            snprintf(buffer, buffer_size, "第 %lu 次启动",
                     (unsigned long)diag.boot_sequence);
            return buffer;
        }
        return "--";
    }
    if (strcmp(row->label, "当前固件") == 0) {
        const esp_app_desc_t *desc = esp_app_get_description();
        if (desc && desc->version[0]) {
            snprintf(buffer, buffer_size, "%s", desc->version);
            return buffer;
        }
        return "--";
    }
    if (strcmp(row->label, "设备编号") == 0) {
        return s_shell.binding.device_sn[0] ? s_shell.binding.device_sn : "未预置";
    }
    return NULL;
}

static void create_maintenance_page(lv_obj_t *page)
{
    build_split_page(page, k_maintenance_sections,
                     sizeof(k_maintenance_sections) / sizeof(k_maintenance_sections[0]),
                     s_shell.maintenance_section, maintenance_value_provider);
}

static void create_safety_page(lv_obj_t *page)
{
    build_split_page(page, k_safety_sections,
                     sizeof(k_safety_sections) / sizeof(k_safety_sections[0]),
                     s_shell.safety_section, safety_value_provider);
}

// 竖屏 MORE-04 右栏用独立值标签承接传感器刷新；内容宿主仍是 portrait_scroll 的
// 唯一纵向滚动对象，左侧分区栏只负责切换当前分区。
static void create_portrait_safety_content(lv_obj_t *page)
{
    memset(s_shell.portrait_safety.values, 0, sizeof(s_shell.portrait_safety.values));
    const size_t section_count = sizeof(k_safety_sections) / sizeof(k_safety_sections[0]);
    if (s_shell.safety_section >= section_count) s_shell.safety_section = 0;
    const split_section_t *section = &k_safety_sections[s_shell.safety_section];
    const int width = UI_PORTRAIT_SPLIT_CONTENT_W;
    const int row_h = UI_PORTRAIT_SPLIT_ROW_H;
    const int value_w = 144;
    const int rows_h = (int)section->row_count * row_h;
    const int total_h = 34 + rows_h + (section->note ? 72 : 0);

    s_shell.split_content_icon_slot_start = s_shell.icon_slot_count;
    lv_obj_t *host = lv_obj_create(page);
    clear_container_style(host);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(host, UI_PORTRAIT_SPLIT_CONTENT_X, 0);
    lv_obj_set_size(host, width, total_h);
    s_shell.split_content = host;
    make_label(host, section->name, FRIDGE_FONT_BODY_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, width,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    lv_obj_t *card = make_box(host, 0, 30, width, rows_h,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    for (size_t i = 0; i < section->row_count && i < 6U; i++) {
        const split_row_t *row = &section->rows[i];
        const int y = (int)i * row_h;
        const int text_w = width - 16 * 2 - value_w - 12;
        make_label(card, row->label, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                   16, y + 12, text_w, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        if (row->desc && row->desc[0]) {
            lv_obj_t *desc = make_label(card, row->desc, FRIDGE_FONT_BODY_SMALL,
                                        FRIDGE_UI_COLOR_MUTED, 16, y + 42, text_w,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            apply_compact_body_lines(desc);
        }
        if (row->kind == SPLIT_ROW_PENDING) {
            create_split_state_pill(card, width - 16 - 92, y + (row_h - 32) / 2,
                                    "后续开放",
                                    FRIDGE_UI_COLOR_OFFLINE_BG, FRIDGE_UI_COLOR_MUTED);
        } else {
            char value_buffer[64] = {0};
            const char *value = row->value;
            const char *dynamic = safety_value_provider(row, value_buffer,
                                                        sizeof(value_buffer));
            if (dynamic) value = dynamic;
            s_shell.portrait_safety.values[s_shell.safety_section][i] = make_label(
                card, value ? value : "--", FRIDGE_FONT_BODY_SMALL,
                FRIDGE_UI_COLOR_SAGE_DARK, width - 16 - value_w,
                y + (row_h - 30) / 2, value_w, 30, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(
                s_shell.portrait_safety.values[s_shell.safety_section][i],
                LV_TEXT_ALIGN_RIGHT, 0);
        }
        if (i + 1U < section->row_count) {
            make_solid_line(card, 16, y + row_h - 1, width - 32, 1,
                            FRIDGE_UI_COLOR_LINE);
        }
    }
    if (section->note) {
        lv_obj_t *note = make_label(host, section->note, FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_MUTED, 0, 30 + rows_h + 12,
                                    width, LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(note);
    }
}

static void create_portrait_safety_page(lv_obj_t *content)
{
    memset(s_shell.portrait_safety.values, 0, sizeof(s_shell.portrait_safety.values));
    const size_t count = sizeof(k_safety_sections) / sizeof(k_safety_sections[0]);
    if (s_shell.safety_section >= count) s_shell.safety_section = 0;
    for (size_t i = 0; i < count && i < UI_SPLIT_SECTION_MAX; i++) {
        create_split_tab(content, i, &k_safety_sections[i], i == s_shell.safety_section);
    }
    create_portrait_safety_content(content);
}

static void create_portrait_settings_page(lv_obj_t *content)
{
    // 设置页的真实 Wi-Fi、权限、WakeNet 和亮度控件复用同一套分区回调；几何函数
    // 根据当前 portrait orientation 使用 150px 左栏、12px 间距和约 406px 右栏。
    create_settings_page(content);
}

static void create_portrait_maintenance_page(lv_obj_t *content)
{
    create_maintenance_page(content);
}

static void refresh_portrait_safety_page(void)
{
    if (s_shell.orientation != FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ||
        s_shell.page != SHELL_PAGE_SAFETY) return;
    const size_t count = sizeof(k_safety_sections) / sizeof(k_safety_sections[0]);
    if (s_shell.safety_section >= count) return;
    const split_section_t *section = &k_safety_sections[s_shell.safety_section];
    for (size_t i = 0; i < section->row_count && i < 6U; i++) {
        lv_obj_t *label = s_shell.portrait_safety.values[s_shell.safety_section][i];
        if (!label) continue;
        char value_buffer[64] = {0};
        const char *value = section->rows[i].value;
        const char *dynamic = safety_value_provider(&section->rows[i], value_buffer,
                                                    sizeof(value_buffer));
        if (dynamic) value = dynamic;
        set_label_if_changed(label, value ? value : "--");
    }
}

static void rebuild_current_split_content(void)
{
    if (!s_shell.page_content || !s_shell.split_content) return;
    // split_content 不是当前点击目标，隐藏后异步删除可避免在 LVGL 事件分发期间释放
    // 对象树；右栏图标槽随旧容器一并回收。
    lv_obj_add_flag(s_shell.split_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(s_shell.split_content);
    s_shell.split_content = NULL;
    s_shell.icon_slot_count = s_shell.split_content_icon_slot_start;
    s_shell.settings_wifi_summary = NULL;
    s_shell.settings_brightness_value = NULL;
    s_shell.settings_brightness_slider = NULL;
    s_shell.settings_wake_switch = NULL;
    s_shell.settings_wake_status = NULL;

    for (size_t i = 0; i < UI_SPLIT_SECTION_MAX; i++) {
        const bool selected = s_shell.page == SHELL_PAGE_SETTINGS ?
                              i == s_shell.settings_section :
                              s_shell.page == SHELL_PAGE_MAINTENANCE ?
                                  i == s_shell.maintenance_section :
                                  i == s_shell.safety_section;
        if (s_shell.split_tabs[i]) {
            lv_obj_set_style_bg_color(s_shell.split_tabs[i],
                                      color_hex(selected ? FRIDGE_UI_COLOR_MINT :
                                                           FRIDGE_UI_COLOR_SURFACE), 0);
            lv_obj_set_style_border_color(s_shell.split_tabs[i],
                                          color_hex(selected ? FRIDGE_UI_COLOR_SAGE :
                                                               FRIDGE_UI_COLOR_LINE), 0);
        }
        if (s_shell.split_tab_labels[i]) {
            lv_obj_set_style_text_color(s_shell.split_tab_labels[i],
                                        color_hex(selected ? FRIDGE_UI_COLOR_SAGE_DARK :
                                                             FRIDGE_UI_COLOR_INK), 0);
        }
    }
    if (s_shell.page == SHELL_PAGE_SETTINGS) {
        create_settings_split_content(s_shell.page_content);
        refresh_settings_visible();
    } else if (s_shell.page == SHELL_PAGE_MAINTENANCE) {
        const size_t count = sizeof(k_maintenance_sections) / sizeof(k_maintenance_sections[0]);
        if (s_shell.maintenance_section >= count) s_shell.maintenance_section = 0;
        create_split_content(s_shell.page_content,
                             &k_maintenance_sections[s_shell.maintenance_section],
                             maintenance_value_provider);
    } else if (s_shell.page == SHELL_PAGE_SAFETY) {
        if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
            create_portrait_safety_content(s_shell.page_content);
            refresh_portrait_safety_page();
        } else {
            create_split_content(s_shell.page_content,
                                 &k_safety_sections[s_shell.safety_section],
                                 safety_value_provider);
        }
    }
    fridge_ui_scroll_ppa_invalidate_cache();
}

static void create_timer_page(lv_obj_t *page)
{
    // MORE-02：主计时面 + 快捷预设 + 其他运行中 + 已完成（对象池，update 只改文案/显隐）。
    s_shell.timer_hero = page_card(page, 0, 220, FRIDGE_UI_COLOR_CREAM);
    s_shell.timer_label = make_label(s_shell.timer_hero, "厨房倒计时", FRIDGE_FONT_TITLE,
                                     FRIDGE_UI_COLOR_SAGE_DARK, 18, 14, 420,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.timer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(s_shell.timer_label, 80);
    s_shell.timer_chip = make_label(s_shell.timer_hero, "", FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_MUTED, 500, 18, 140,
                                    LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.timer_face = make_label(s_shell.timer_hero, "00:00", FRIDGE_FONT_HUGE,
                                    FRIDGE_UI_COLOR_MUTED, 18, 52, 620,
                                    LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_shell.timer_face, LV_TEXT_ALIGN_CENTER, 0);
    s_shell.timer_state = make_label(s_shell.timer_hero, "点下方预设立即开始，或新建自定义时长",
                                     FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED, 18, 150, 620,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.timer_state, LV_TEXT_ALIGN_CENTER, 0);

    s_shell.timer_ops = make_box(s_shell.timer_hero, 16, 168, 640, 44,
                                 FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 0);
    lv_obj_add_flag(s_shell.timer_ops, LV_OBJ_FLAG_HIDDEN);
    s_shell.timer_pause = make_button(s_shell.timer_ops, 20, 0, 170, 44,
                                      FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                      FRIDGE_UI_COLOR_LINE_STRONG, "暂停", FRIDGE_FONT_BODY,
                                      timer_pause_event, NULL, &s_shell.timer_pause_label);
    s_shell.timer_plus = make_button(s_shell.timer_ops, 210, 0, 170, 44,
                                     FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                     FRIDGE_UI_COLOR_LINE_STRONG, "+1 分钟", FRIDGE_FONT_BODY,
                                     timer_plus_event, NULL, NULL);
    s_shell.timer_cancel = make_button(s_shell.timer_ops, 400, 0, 170, 44,
                                       FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_TOMATO,
                                       FRIDGE_UI_COLOR_LINE_STRONG, "取消", FRIDGE_FONT_BODY,
                                       timer_cancel_event, NULL, NULL);
    s_shell.timer_dismiss = make_button(s_shell.timer_ops, 400, 0, 170, 44,
                                        FRIDGE_UI_COLOR_TOMATO, 0xFFFFFF,
                                        FRIDGE_UI_COLOR_TOMATO, "结束", FRIDGE_FONT_BODY,
                                        timer_dismiss_event, NULL, NULL);
    lv_obj_add_flag(s_shell.timer_dismiss, LV_OBJ_FLAG_HIDDEN);

    // 参考稿空闲态不放独立「开始」大按钮：一键预设 + 右上角「新建」即可。
    // 仍保留隐藏控件以兼容旧 refresh 路径，默认不显示避免压住预设格。
    s_shell.timer_start = make_button(page, 492, 232, 160, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                                      FRIDGE_UI_COLOR_SAGE, "开始", FRIDGE_FONT_BODY,
                                      timer_start_event, NULL, &s_shell.timer_start_label);
    lv_obj_add_flag(s_shell.timer_start, LV_OBJ_FLAG_HIDDEN);

    make_label(page, "快捷开始", FRIDGE_FONT_BODY_SMALL_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, 236, 150, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < UI_TIMER_PRESET_MAX; i++) {
        uint16_t minutes = timer_preset_minutes(i);
        char text[8];
        snprintf(text, sizeof(text), "%u", (unsigned)minutes);
        lv_obj_t *preset = make_button(page, (int)i * 112, 264, 104, 72,
                                       FRIDGE_UI_COLOR_SURFACE,
                                       FRIDGE_UI_COLOR_SAGE_DARK,
                                       FRIDGE_UI_COLOR_LINE, "",
                                       FRIDGE_FONT_BODY, timer_preset_event,
                                       (void *)(uintptr_t)minutes, NULL);
        lv_obj_t *number = make_label(preset, text, FRIDGE_FONT_TITLE,
                                      FRIDGE_UI_COLOR_SAGE_DARK, 0, 6, 104,
                                      LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *unit = make_label(preset, "分钟", FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_MUTED, 0, 42, 104,
                                    LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    }

    s_shell.timer_other_title = make_label(page, "其他运行中", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                           FRIDGE_UI_COLOR_SAGE_DARK, 0, 352, 300,
                                           LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_shell.timer_other_title, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < UI_TIMER_OTHER_MAX; i++) {
        int y = 380 + (int)i * 118;
        lv_obj_t *row = make_box(page, 0, y, 672, 110, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_LINE, 8);
        s_shell.timer_other_rows[i] = row;
        s_shell.timer_other_names[i] = make_label(row, "", FRIDGE_FONT_BODY_EMPHASIS,
                                                  FRIDGE_UI_COLOR_INK, 16, 10, 420,
                                                  LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.timer_other_remains[i] = make_label(row, "", FRIDGE_FONT_TITLE,
                                                    FRIDGE_UI_COLOR_SAGE_DARK, 450, 10, 200,
                                                    LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(s_shell.timer_other_remains[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_shell.timer_other_meta[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                                 FRIDGE_UI_COLOR_MUTED, 16, 42, 640,
                                                 LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.timer_other_pause[i] = make_button(row, 16, 68, 120, 34,
                                                   FRIDGE_UI_COLOR_SURFACE,
                                                   FRIDGE_UI_COLOR_SAGE_DARK,
                                                   FRIDGE_UI_COLOR_LINE, "暂停",
                                                   FRIDGE_FONT_BODY_SMALL,
                                                   timer_other_pause_event, NULL,
                                                   &s_shell.timer_other_pause_labels[i]);
        s_shell.timer_other_plus[i] = make_button(row, 148, 68, 120, 34,
                                                  FRIDGE_UI_COLOR_SURFACE,
                                                  FRIDGE_UI_COLOR_SAGE_DARK,
                                                  FRIDGE_UI_COLOR_LINE, "+1分钟",
                                                  FRIDGE_FONT_BODY_SMALL,
                                                  timer_other_plus_event, NULL, NULL);
        s_shell.timer_other_cancel[i] = make_button(row, 280, 68, 100, 34,
                                                    FRIDGE_UI_COLOR_SURFACE,
                                                    FRIDGE_UI_COLOR_TOMATO,
                                                    FRIDGE_UI_COLOR_LINE, "取消",
                                                    FRIDGE_FONT_BODY_SMALL,
                                                    timer_other_cancel_event, NULL, NULL);
        s_shell.timer_other_ids[i] = 0;
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    s_shell.timer_done_title = make_label(page, "已完成", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                          FRIDGE_UI_COLOR_SAGE_DARK, 0, 380, 300,
                                          LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(s_shell.timer_done_title, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < UI_TIMER_DONE_MAX; i++) {
        int y = 408 + (int)i * 76;
        lv_obj_t *row = make_box(page, 0, y, 672, 68, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_set_style_opa(row, LV_OPA_60, 0);
        s_shell.timer_done_rows[i] = row;
        s_shell.timer_done_names[i] = make_label(row, "", FRIDGE_FONT_BODY,
                                                 FRIDGE_UI_COLOR_INK, 16, 10, 420,
                                                 LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.timer_done_meta[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                                FRIDGE_UI_COLOR_MUTED, 16, 38, 640,
                                                LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void timer_edit_hour_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int next = (int)s_shell.timer_edit_hour + delta;
    if (next < 0) next = 0;
    if (next > 23) next = 23;
    s_shell.timer_edit_hour = (uint8_t)next;
    if (s_shell.timer_edit_hour_label) {
        char text[8];
        snprintf(text, sizeof(text), "%u", (unsigned)s_shell.timer_edit_hour);
        set_label_if_changed(s_shell.timer_edit_hour_label, text);
    }
}

static void timer_edit_min_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int next = (int)s_shell.timer_edit_minute + delta;
    if (next < 0) next = 59;
    if (next > 59) next = 0;
    s_shell.timer_edit_minute = (uint8_t)next;
    if (s_shell.timer_edit_min_label) {
        char text[8];
        snprintf(text, sizeof(text), "%02u", (unsigned)s_shell.timer_edit_minute);
        set_label_if_changed(s_shell.timer_edit_min_label, text);
    }
}

static void timer_edit_name_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.timer_edit_name_idx = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    static const char *names[] = {"煮蛋", "焯水", "蒸鱼", "厨房计时"};
    if (s_shell.timer_edit_name_idx > 3) s_shell.timer_edit_name_idx = 3;
    if (s_shell.timer_edit_name_label) {
        set_label_if_changed(s_shell.timer_edit_name_label, names[s_shell.timer_edit_name_idx]);
    }
}

static void timer_edit_sound_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.timer_edit_sound =
        (fridge_kitchen_timer_sound_t)(uintptr_t)lv_event_get_user_data(event);
}

static void timer_edit_start_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint32_t seconds = (uint32_t)s_shell.timer_edit_hour * 3600U +
                       (uint32_t)s_shell.timer_edit_minute * 60U;
    if (seconds == 0) {
        fridge_ui_shell_show_toast("时长至少 1 分钟");
        return;
    }
    static const char *names[] = {"煮蛋", "焯水", "蒸鱼", "厨房计时"};
    uint8_t idx = s_shell.timer_edit_name_idx > 3 ? 3 : s_shell.timer_edit_name_idx;
    timer_start_with(seconds, names[idx], "手动", s_shell.timer_edit_sound);
    switch_page(SHELL_PAGE_TIMER);
}

static void timer_edit_cancel_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    switch_page(SHELL_PAGE_TIMER);
}

static void create_timer_editor_page(lv_obj_t *page)
{
    s_shell.timer_edit_hour = 0;
    s_shell.timer_edit_minute = 10;
    s_shell.timer_edit_name_idx = 0;
    s_shell.timer_edit_sound = FRIDGE_KITCHEN_TIMER_SOUND_VOICE;

    lv_obj_t *card = make_box(page, 0, 0, 672, 360, FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_LINE, 8);
    make_label(card, "类型", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               20, 16, 100, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(card, "倒计时", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               20, 40, 200, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    make_label(card, "时长（时 / 分）", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               20, 84, 220, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(card, 20, 112, 72, 48, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                FRIDGE_UI_COLOR_LINE, "时-", FRIDGE_FONT_BODY, timer_edit_hour_event,
                (void *)(intptr_t)-1, NULL);
    s_shell.timer_edit_hour_label = make_label(card, "0", FRIDGE_FONT_TITLE,
                                               FRIDGE_UI_COLOR_INK, 110, 120, 40,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    make_button(card, 160, 112, 72, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "时+", FRIDGE_FONT_BODY, timer_edit_hour_event,
                (void *)(intptr_t)1, NULL);
    make_label(card, ":", FRIDGE_FONT_TITLE, FRIDGE_UI_COLOR_INK, 250, 120, 20,
               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    make_button(card, 280, 112, 72, 48, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                FRIDGE_UI_COLOR_LINE, "分-", FRIDGE_FONT_BODY, timer_edit_min_event,
                (void *)(intptr_t)-1, NULL);
    s_shell.timer_edit_min_label = make_label(card, "10", FRIDGE_FONT_TITLE,
                                              FRIDGE_UI_COLOR_INK, 370, 120, 50,
                                              LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    make_button(card, 430, 112, 72, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "分+", FRIDGE_FONT_BODY, timer_edit_min_event,
                (void *)(intptr_t)1, NULL);

    make_label(card, "名称", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               20, 180, 100, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.timer_edit_name_label = make_label(card, "煮蛋", FRIDGE_FONT_BODY,
                                               FRIDGE_UI_COLOR_INK, 20, 204, 200,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    static const char *names[] = {"煮蛋", "焯水", "蒸鱼", "厨房计时"};
    for (uint8_t i = 0; i < 4; i++) {
        make_button(card, 20 + (int)i * 156, 236, 148, 44, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, names[i],
                    FRIDGE_FONT_BODY_SMALL, timer_edit_name_event,
                    (void *)(uintptr_t)i, NULL);
    }

    make_label(card, "提醒声音", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               20, 294, 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(card, 20, 318, 160, 40, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                FRIDGE_UI_COLOR_LINE, "铃声", FRIDGE_FONT_BODY_SMALL,
                timer_edit_sound_event,
                (void *)(uintptr_t)FRIDGE_KITCHEN_TIMER_SOUND_CHIME, NULL);
    make_button(card, 196, 318, 160, 40, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "语音播报", FRIDGE_FONT_BODY_SMALL,
                timer_edit_sound_event,
                (void *)(uintptr_t)FRIDGE_KITCHEN_TIMER_SOUND_VOICE, NULL);

    make_button(page, 120, 390, 200, 56, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                FRIDGE_UI_COLOR_LINE, "取消", FRIDGE_FONT_BODY, timer_edit_cancel_event,
                NULL, NULL);
    make_button(page, 360, 390, 280, 56, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "立即开始", FRIDGE_FONT_BODY, timer_edit_start_event,
                NULL, NULL);
}

static void create_timer_ring_overlay(lv_obj_t *root)
{
    if (!root || s_shell.timer_ring_root) return;
    s_shell.timer_ring_root = lv_obj_create(root);
    clear_container_style(s_shell.timer_ring_root);
    lv_obj_set_size(s_shell.timer_ring_root, 1024, 600);
    lv_obj_set_pos(s_shell.timer_ring_root, 0, 0);
    lv_obj_set_style_bg_color(s_shell.timer_ring_root, color_hex(0x24342D), 0);
    lv_obj_set_style_bg_opa(s_shell.timer_ring_root, LV_OPA_50, 0);
    lv_obj_add_flag(s_shell.timer_ring_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_shell.timer_ring_root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = make_box(s_shell.timer_ring_root, 232, 120, 560, 300,
                               FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    make_label(panel, "定时器到时", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 16, 560, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *face = make_label(panel, "00:00", FRIDGE_FONT_HUGE, FRIDGE_UI_COLOR_SAGE_DARK,
                                0, 40, 560, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(face, LV_TEXT_ALIGN_CENTER, 0);
    s_shell.timer_ring_name = make_label(panel, "时间到！", FRIDGE_FONT_TITLE,
                                         FRIDGE_UI_COLOR_INK, 20, 150, 520,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.timer_ring_name, LV_TEXT_ALIGN_CENTER, 0);
    s_shell.timer_ring_src = make_label(panel, "来源：手动", FRIDGE_FONT_BODY_SMALL,
                                        FRIDGE_UI_COLOR_MUTED, 20, 190, 520,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.timer_ring_src, LV_TEXT_ALIGN_CENTER, 0);
    make_button(panel, 30, 230, 150, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "结束", FRIDGE_FONT_BODY, timer_dismiss_event,
                NULL, NULL);
    make_button(panel, 195, 230, 170, 48, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                FRIDGE_UI_COLOR_LINE, "延长1分钟", FRIDGE_FONT_BODY, timer_ring_plus_event,
                NULL, NULL);
    make_button(panel, 380, 230, 150, 48, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                FRIDGE_UI_COLOR_LINE, "稍后提醒", FRIDGE_FONT_BODY, timer_ring_plus_event,
                NULL, NULL);
    s_shell.timer_ring_id = 0;
}

static void refresh_timer_ring_overlay(void)
{
    if (!s_shell.timer_ring_root) return;
    const fridge_kitchen_timer_snapshot_t *ring = NULL;
    for (size_t i = 0; i < s_shell.kitchen.timer_count; i++) {
        const fridge_kitchen_timer_snapshot_t *t = &s_shell.kitchen.timers[i];
        if (t->state != FRIDGE_KITCHEN_TIMER_RINGING) continue;
        if (!ring || t->target_at_ms < ring->target_at_ms) ring = t;
    }
    if (!ring) {
        lv_obj_add_flag(s_shell.timer_ring_root, LV_OBJ_FLAG_HIDDEN);
        s_shell.timer_ring_id = 0;
        return;
    }
    s_shell.timer_ring_id = ring->id;
    char name[64];
    snprintf(name, sizeof(name), "%s 时间到！",
             ring->label[0] ? ring->label : "厨房计时");
    set_label_if_changed(s_shell.timer_ring_name, name);
    char src[48];
    snprintf(src, sizeof(src), "来源：%s", ring->source[0] ? ring->source : "手动");
    set_label_if_changed(s_shell.timer_ring_src, src);
    lv_obj_remove_flag(s_shell.timer_ring_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shell.timer_ring_root);
}

static const char *alarm_kind_text(fridge_kitchen_alarm_kind_t kind)
{
    return kind == FRIDGE_KITCHEN_ALARM_KIND_CYCLE ? "周期提醒" : "闹钟";
}

static const char *alarm_repeat_text(fridge_kitchen_alarm_repeat_t repeat)
{
    switch (repeat) {
    case FRIDGE_KITCHEN_ALARM_REPEAT_WEEKDAYS:
        return "工作日";
    case FRIDGE_KITCHEN_ALARM_REPEAT_SUNDAY:
        return "每周日";
    case FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM:
        return "自定义";
    case FRIDGE_KITCHEN_ALARM_REPEAT_DAILY:
    default:
        return "每天";
    }
}

static const char *alarm_source_text(fridge_kitchen_alarm_source_t source)
{
    return source == FRIDGE_KITCHEN_ALARM_SOURCE_AGENT ? "小精灵" : "手动";
}

static void alarm_draft_from_alarm(const fridge_kitchen_alarm_t *alarm)
{
    if (!alarm) return;
    memset(&s_shell.alarm_draft, 0, sizeof(s_shell.alarm_draft));
    s_shell.alarm_draft.kind = alarm->kind;
    s_shell.alarm_draft.repeat = alarm->repeat;
    s_shell.alarm_draft.weekday_mask = alarm->weekday_mask;
    s_shell.alarm_draft.sound = alarm->sound;
    s_shell.alarm_draft.source = alarm->source;
    s_shell.alarm_draft.hour = alarm->hour;
    s_shell.alarm_draft.minute = alarm->minute;
    strlcpy(s_shell.alarm_draft.label, alarm->label, sizeof(s_shell.alarm_draft.label));
    strlcpy(s_shell.alarm_draft.speech_text, alarm->speech_text,
            sizeof(s_shell.alarm_draft.speech_text));
}

static const fridge_kitchen_alarm_t *alarm_snapshot_by_id(uint8_t id)
{
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        if (s_shell.kitchen.alarms[i].id == id) return &s_shell.kitchen.alarms[i];
    }
    return NULL;
}

static void refresh_alarm_card(size_t index, const fridge_kitchen_alarm_t *alarm)
{
    if (index >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS || !alarm) return;
    char time_text[12];
    char meta[96];
    snprintf(time_text, sizeof(time_text), "%02u:%02u", (unsigned)alarm->hour,
             (unsigned)alarm->minute);
    snprintf(meta, sizeof(meta), "%s · 来源 %s%s", alarm_repeat_text(alarm->repeat),
             alarm_source_text(alarm->source),
             alarm->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE ? " · 语音" : "");
    set_label_if_changed(s_shell.alarm_card_times[index], time_text);
    set_label_if_changed(s_shell.alarm_card_names[index],
                         alarm->label[0] ? alarm->label : alarm_default_name());
    set_label_if_changed(s_shell.alarm_card_meta[index], meta);
    set_label_if_changed(s_shell.alarm_card_kind[index], alarm_kind_text(alarm->kind));
    if (s_shell.alarm_card_toggles[index]) {
        lv_obj_set_style_bg_color(s_shell.alarm_card_toggles[index],
                                  color_hex(alarm->enabled ? FRIDGE_UI_COLOR_SAGE :
                                                             FRIDGE_UI_COLOR_OFFLINE), 0);
        lv_obj_set_style_border_color(s_shell.alarm_card_toggles[index],
                                      color_hex(alarm->enabled ? FRIDGE_UI_COLOR_SAGE :
                                                                 FRIDGE_UI_COLOR_OFFLINE), 0);
    }
    if (s_shell.alarm_card_knobs[index]) {
        lv_obj_set_x(s_shell.alarm_card_knobs[index], alarm->enabled ? 32 : 4);
    }
    if (s_shell.alarm_card_main[index]) {
        lv_obj_set_style_opa(s_shell.alarm_card_main[index],
                             alarm->enabled ? LV_OPA_COVER : LV_OPA_60, 0);
    }
}

static void create_alarm_card(lv_obj_t *page, int y, size_t slot,
                              const fridge_kitchen_alarm_t *alarm)
{
    if (!page || !alarm || slot >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) return;
    lv_obj_t *card = make_box(page, 0, y, 672, 96, FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_LINE, 8);
    s_shell.alarm_cards[slot] = card;
    s_shell.alarm_card_ids[slot] = alarm->id;
    lv_obj_t *main = make_button(card, 1, 1, 574, 94, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_SURFACE, "",
                                 FRIDGE_FONT_BODY, alarm_card_event,
                                 (void *)(uintptr_t)alarm->id, NULL);
    s_shell.alarm_card_main[slot] = main;
    s_shell.alarm_card_times[slot] = make_label(main, "", FRIDGE_FONT_SECTION_STRONG,
                                                FRIDGE_UI_COLOR_INK, 16, 10, 108,
                                                LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    s_shell.alarm_card_names[slot] = make_label(main, "", FRIDGE_FONT_TITLE,
                                                FRIDGE_UI_COLOR_INK, 130, 12, 282,
                                                LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *chip = make_box(main, 416, 12, 88, 28, FRIDGE_UI_COLOR_MINT,
                              FRIDGE_UI_COLOR_MINT, 14);
    s_shell.alarm_card_kind[slot] = make_label(chip, "", FRIDGE_FONT_BODY_SMALL,
                                               FRIDGE_UI_COLOR_SAGE_DARK, 4, 0, 80,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(s_shell.alarm_card_kind[slot], 80);
    s_shell.alarm_card_meta[slot] = make_label(main, "", FRIDGE_FONT_BODY_SMALL,
                                               FRIDGE_UI_COLOR_MUTED, 130, 54, 384,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *toggle = make_button(card, 588, 30, 64, 36, FRIDGE_UI_COLOR_SAGE,
                                   0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "",
                                   FRIDGE_FONT_BODY_SMALL, alarm_toggle_event,
                                   (void *)(uintptr_t)alarm->id, NULL);
    lv_obj_set_style_radius(toggle, 18, 0);
    s_shell.alarm_card_toggles[slot] = toggle;
    s_shell.alarm_card_knobs[slot] = make_box(toggle, 32, 4, 28, 28,
                                               FRIDGE_UI_COLOR_SURFACE,
                                               FRIDGE_UI_COLOR_SURFACE, 14);
    refresh_alarm_card(slot, alarm);
}

static void create_alarm_page(lv_obj_t *page)
{
    s_shell.alarm_status = make_label(page, "时间已同步，可设置固定时刻和周期提醒",
                                      FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED, 0, 0, 660,
                                      LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    size_t fixed_count = 0;
    size_t cycle_count = 0;
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        if (s_shell.kitchen.alarms[i].kind == FRIDGE_KITCHEN_ALARM_KIND_CYCLE) cycle_count++;
        else fixed_count++;
    }
    char section[32];
    int y = 38;
    snprintf(section, sizeof(section), "闹钟（%u）", (unsigned)fixed_count);
    make_label(page, section, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, y, 180, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    y += 32;
    size_t slot = 0;
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        const fridge_kitchen_alarm_t *alarm = &s_shell.kitchen.alarms[i];
        if (alarm->kind != FRIDGE_KITCHEN_ALARM_KIND_FIXED) continue;
        create_alarm_card(page, y, slot++, alarm);
        y += 108;
    }
    snprintf(section, sizeof(section), "周期提醒（%u）", (unsigned)cycle_count);
    make_label(page, section, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, y, 180, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    y += 32;
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        const fridge_kitchen_alarm_t *alarm = &s_shell.kitchen.alarms[i];
        if (alarm->kind != FRIDGE_KITCHEN_ALARM_KIND_CYCLE) continue;
        create_alarm_card(page, y, slot++, alarm);
        y += 108;
    }
    if (slot == 0) {
        s_shell.alarm_empty = make_box(page, 0, y + 4, 672, 86, FRIDGE_UI_COLOR_SURFACE,
                                       FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_t *empty = make_label(s_shell.alarm_empty, "暂无提醒，点击右上角“新建”添加",
                                     FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED, 8, 0, 656,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        center_single_line_label(empty, 656);
    }
}

static lv_obj_t *make_alarm_choice(lv_obj_t *parent, int x, int y, int width,
                                   const char *text, bool selected, lv_event_cb_t event_cb,
                                   void *event_data)
{
    return make_button(parent, x, y, width, 42,
                       selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_SURFACE,
                       selected ? 0xFFFFFF : FRIDGE_UI_COLOR_SAGE_DARK,
                       selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE,
                       text, FRIDGE_FONT_BODY_SMALL, event_cb, event_data, NULL);
}

static void add_alarm_form_divider(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    clear_container_style(line);
    lv_obj_set_pos(line, 16, y);
    lv_obj_set_size(line, 640, 1);
    lv_obj_set_style_bg_color(line, color_hex(FRIDGE_UI_COLOR_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

static void create_alarm_editor_footer(void)
{
    s_shell.alarm_editor_footer = make_box(s_shell.main, 0, 530, UI_MAIN_W, 70,
                                           FRIDGE_UI_COLOR_PAPER, FRIDGE_UI_COLOR_LINE, 0);
    lv_obj_set_style_border_side(s_shell.alarm_editor_footer, LV_BORDER_SIDE_TOP, 0);
    int x = s_shell.alarm_editing_id ? 24 : 72;
    if (s_shell.alarm_editing_id) {
        make_button(s_shell.alarm_editor_footer, x, 12, 88, 46, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_TOMATO, FRIDGE_UI_COLOR_LINE, "删除",
                    FRIDGE_FONT_BODY_SMALL, alarm_editor_delete_event, NULL, NULL);
        x += 96;
    }
    make_button(s_shell.alarm_editor_footer, x, 12, 92, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消",
                FRIDGE_FONT_BODY_SMALL, alarm_editor_cancel_event, NULL, NULL);
    x += 100;
    make_button(s_shell.alarm_editor_footer, x, 12, 100, 46, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "预览",
                FRIDGE_FONT_BODY_SMALL, alarm_editor_preview_event, NULL, NULL);
    x += 108;
    make_button(s_shell.alarm_editor_footer, x, 12, 118, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "保存",
                FRIDGE_FONT_BODY_SMALL, alarm_editor_save_event, NULL, NULL);
    x += 126;
    make_button(s_shell.alarm_editor_footer, x, 12, 160, 46, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "保存并启用",
                FRIDGE_FONT_BODY_SMALL, alarm_editor_save_enable_event, NULL, NULL);
}

static void create_alarm_editor_page(lv_obj_t *page)
{
    lv_obj_t *form = make_box(page, 0, 0, 672, 650, FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_LINE, 8);
    int y = 0;
    make_label(form, "类型", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 110, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_alarm_choice(form, 154, y + 18, 112, "定时", s_shell.alarm_draft.kind ==
                      FRIDGE_KITCHEN_ALARM_KIND_FIXED, alarm_editor_kind_event,
                      (void *)(uintptr_t)FRIDGE_KITCHEN_ALARM_KIND_FIXED);
    make_alarm_choice(form, 276, y + 18, 112, "周期", s_shell.alarm_draft.kind ==
                      FRIDGE_KITCHEN_ALARM_KIND_CYCLE, alarm_editor_kind_event,
                      (void *)(uintptr_t)FRIDGE_KITCHEN_ALARM_KIND_CYCLE);
    add_alarm_form_divider(form, y += UI_ALARM_FORM_ROW_H);

    make_label(form, "响铃时间", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char time_text[12];
    snprintf(time_text, sizeof(time_text), "%02u:%02u", (unsigned)s_shell.alarm_draft.hour,
             (unsigned)s_shell.alarm_draft.minute);
    make_label(form, time_text, FRIDGE_FONT_SECTION_STRONG, FRIDGE_UI_COLOR_INK,
               154, y + 15, 112, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    make_button(form, 282, y + 18, 68, 42, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "时-",
                FRIDGE_FONT_BODY_SMALL, alarm_editor_adjust_event, (void *)(intptr_t)-60, NULL);
    make_button(form, 358, y + 18, 68, 42, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "时+", FRIDGE_FONT_BODY_SMALL,
                alarm_editor_adjust_event, (void *)(intptr_t)60, NULL);
    make_button(form, 434, y + 18, 68, 42, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "分-",
                FRIDGE_FONT_BODY_SMALL, alarm_editor_adjust_event, (void *)(intptr_t)-1, NULL);
    make_button(form, 510, y + 18, 68, 42, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "分+", FRIDGE_FONT_BODY_SMALL,
                alarm_editor_adjust_event, (void *)(intptr_t)1, NULL);
    add_alarm_form_divider(form, y += UI_ALARM_FORM_ROW_H);

    make_label(form, "重复规则", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    const char *repeats[] = {"每天", "工作日", "每周日", "自定义"};
    for (size_t i = 0; i < 4; i++) {
        make_alarm_choice(form, 154 + (int)i * 122, y + 18, 114, repeats[i],
                          s_shell.alarm_draft.repeat == (fridge_kitchen_alarm_repeat_t)i,
                          alarm_editor_repeat_event, (void *)(uintptr_t)i);
    }
    y += UI_ALARM_FORM_ROW_H;
    if (s_shell.alarm_draft.repeat == FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM) {
        add_alarm_form_divider(form, y);
        make_label(form, "重复日", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                   16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        static const char *days[] = {"日", "一", "二", "三", "四", "五", "六"};
        for (size_t i = 0; i < 7; i++) {
            make_alarm_choice(form, 154 + (int)i * 66, y + 18, 58, days[i],
                              (s_shell.alarm_draft.weekday_mask & (1U << i)) != 0,
                              alarm_editor_weekday_event, (void *)(uintptr_t)(1U << i));
        }
        y += UI_ALARM_FORM_ROW_H;
    }
    add_alarm_form_divider(form, y);

    make_label(form, "名称", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(form, 154, y + 18, 486, 42, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE,
                s_shell.alarm_draft.label[0] ? s_shell.alarm_draft.label : alarm_default_name(),
                FRIDGE_FONT_BODY, alarm_editor_name_event, NULL, NULL);
    add_alarm_form_divider(form, y += UI_ALARM_FORM_ROW_H);

    make_label(form, "提醒声音", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_alarm_choice(form, 154, y + 18, 160, "铃声", s_shell.alarm_draft.sound ==
                      FRIDGE_KITCHEN_ALARM_SOUND_CHIME, alarm_editor_sound_event,
                      (void *)(uintptr_t)FRIDGE_KITCHEN_ALARM_SOUND_CHIME);
    make_alarm_choice(form, 324, y + 18, 160, "语音播报", s_shell.alarm_draft.sound ==
                      FRIDGE_KITCHEN_ALARM_SOUND_VOICE, alarm_editor_sound_event,
                      (void *)(uintptr_t)FRIDGE_KITCHEN_ALARM_SOUND_VOICE);
    add_alarm_form_divider(form, y += UI_ALARM_FORM_ROW_H);

    make_label(form, "播报文字", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(form, 154, y + 18, 486, 42, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE,
                s_shell.alarm_draft.speech_text[0] ? s_shell.alarm_draft.speech_text :
                                                      "请输入播报文字",
                FRIDGE_FONT_BODY, alarm_editor_speech_event, NULL, NULL);
    y += UI_ALARM_FORM_ROW_H;
    add_alarm_form_divider(form, y);
    make_label(form, "来源", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, y + 27, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(form, s_shell.alarm_draft.source == FRIDGE_KITCHEN_ALARM_SOURCE_AGENT ?
               "小精灵创建" : "手动创建", FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED,
               154, y + 27, 300, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    create_alarm_editor_footer();
}

static void close_alarm_popups(void)
{
    if (s_shell.alarm_delete_dialog) {
        lv_obj_delete_async(s_shell.alarm_delete_dialog);
        s_shell.alarm_delete_dialog = NULL;
    }
    if (s_shell.alarm_delete_scrim) {
        lv_obj_delete_async(s_shell.alarm_delete_scrim);
        s_shell.alarm_delete_scrim = NULL;
    }
    if (s_shell.alarm_text_dialog) {
        lv_obj_delete_async(s_shell.alarm_text_dialog);
        s_shell.alarm_text_dialog = NULL;
    }
    if (s_shell.alarm_text_scrim) {
        lv_obj_delete_async(s_shell.alarm_text_scrim);
        s_shell.alarm_text_scrim = NULL;
    }
    s_shell.alarm_textarea = NULL;
    s_shell.alarm_ime_preedit = NULL;
    memset(s_shell.alarm_ime_candidates, 0, sizeof(s_shell.alarm_ime_candidates));
}

static void alarm_delete_close_event(lv_event_t *event)
{
    (void)event;
    close_alarm_popups();
}

static void alarm_delete_confirm_event(lv_event_t *event)
{
    (void)event;
    fridge_ui_shell_kitchen_request_t request = {.alarm_id = s_shell.alarm_editing_id};
    bool deleted = dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_ALARM_CANCEL, &request,
                                           "闹钟已删除", "闹钟删除失败");
    close_alarm_popups();
    if (deleted) switch_page(SHELL_PAGE_ALARM);
}

static void alarm_show_delete_confirm(void)
{
    if (!s_shell.root || s_shell.alarm_delete_dialog) return;
    s_shell.alarm_delete_scrim = make_box(s_shell.root, 0, 0, 1024, UI_SCREEN_H,
                                          FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.alarm_delete_scrim, LV_OPA_50, 0);
    lv_obj_add_flag(s_shell.alarm_delete_scrim, LV_OBJ_FLAG_CLICKABLE);
    s_shell.alarm_delete_dialog = make_box(s_shell.root, 176, 196, 352, 198,
                                           FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.alarm_delete_dialog, "删除这个提醒？", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_INK, 20, 20, 312, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.alarm_delete_dialog, "删除后无法恢复，周期规则和播报内容也会一并移除。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 20, 66, 312, 48,
               LV_LABEL_LONG_WRAP);
    make_button(s_shell.alarm_delete_dialog, 20, 138, 146, 44, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消",
                FRIDGE_FONT_BODY, alarm_delete_close_event, NULL, NULL);
    make_button(s_shell.alarm_delete_dialog, 186, 138, 146, 44, FRIDGE_UI_COLOR_TOMATO,
                0xFFFFFF, FRIDGE_UI_COLOR_TOMATO, "确认删除", FRIDGE_FONT_BODY,
                alarm_delete_confirm_event, NULL, NULL);
    lv_obj_move_foreground(s_shell.alarm_delete_scrim);
    lv_obj_move_foreground(s_shell.alarm_delete_dialog);
}

static void refresh_alarm_ime(void)
{
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    set_label_if_changed(s_shell.alarm_ime_preedit,
                         snapshot.preedit[0] ? snapshot.preedit : "输入拼音后选择候选词");
    for (size_t i = 0; i < FRIDGE_IME_MAX_CANDIDATES; i++) {
        set_label_if_changed(s_shell.alarm_ime_candidates[i],
                             i < snapshot.candidate_count ? snapshot.candidates[i] : "");
    }
}

static void alarm_text_finish(bool save)
{
    if (save && s_shell.alarm_textarea) {
        const char *text = lv_textarea_get_text(s_shell.alarm_textarea);
        if (s_shell.alarm_editing_speech) {
            strlcpy(s_shell.alarm_draft.speech_text, text ? text : "",
                    sizeof(s_shell.alarm_draft.speech_text));
        } else {
            strlcpy(s_shell.alarm_draft.label, text ? text : "",
                    sizeof(s_shell.alarm_draft.label));
        }
    }
    fridge_ime_clear_preedit();
    close_alarm_popups();
    switch_page(SHELL_PAGE_ALARM_EDITOR);
}

static void alarm_text_cancel_event(lv_event_t *event)
{
    (void)event;
    alarm_text_finish(false);
}

static void alarm_text_save_event(lv_event_t *event)
{
    (void)event;
    alarm_text_finish(true);
}

static void alarm_text_clear_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.alarm_textarea) lv_textarea_set_text(s_shell.alarm_textarea, "");
    fridge_ime_clear_preedit();
    refresh_alarm_ime();
}

static void alarm_text_key_event(lv_event_t *event)
{
    char key = (char)(uintptr_t)lv_event_get_user_data(event);
    fridge_ime_feed_letter(key);
    refresh_alarm_ime();
}

static void alarm_text_backspace_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_backspace();
    refresh_alarm_ime();
}

static void alarm_text_candidate_event(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (fridge_ime_select_candidate(index) == ESP_OK) refresh_alarm_ime();
}

static void alarm_open_text_editor(bool speech)
{
    if (!s_shell.root || s_shell.alarm_text_dialog) return;
    s_shell.alarm_editing_speech = speech;
    // 全局 IME 由 ui_p4 初始化并在 TF 挂载后延迟补全；这里只切换目标文本框，
    // 禁止重复释放和重建 6.5 万条词库及简拼索引。
    s_shell.alarm_text_scrim = make_box(s_shell.root, 0, 0, 1024, UI_SCREEN_H,
                                        FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.alarm_text_scrim, LV_OPA_50, 0);
    lv_obj_add_flag(s_shell.alarm_text_scrim, LV_OBJ_FLAG_CLICKABLE);
    s_shell.alarm_text_dialog = make_box(s_shell.root, 48, 62, 608, 470,
                                         FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.alarm_text_dialog, speech ? "编辑播报文字" : "编辑提醒名称",
               FRIDGE_FONT_TITLE, FRIDGE_UI_COLOR_INK, 20, 16, 568,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.alarm_textarea = lv_textarea_create(s_shell.alarm_text_dialog);
    lv_obj_set_pos(s_shell.alarm_textarea, 20, 52);
    lv_obj_set_size(s_shell.alarm_textarea, 568, 52);
    lv_textarea_set_one_line(s_shell.alarm_textarea, true);
    lv_textarea_set_max_length(s_shell.alarm_textarea,
                               speech ? FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN :
                                        FRIDGE_KITCHEN_TOOL_MAX_LABEL_LEN);
    lv_textarea_set_text(s_shell.alarm_textarea,
                         speech ? s_shell.alarm_draft.speech_text : s_shell.alarm_draft.label);
    lv_obj_add_style(s_shell.alarm_textarea, fridge_ui_theme_font_style(FRIDGE_FONT_BODY), 0);
    // 闹钟编辑器仍显示完整的 12 个候选；CHAT-01 会在打开时切成单行 6 个候选。
    (void)fridge_ime_set_candidate_page_size(FRIDGE_IME_MAX_CANDIDATES);
    (void)fridge_ime_attach_textarea(s_shell.alarm_textarea);
    fridge_ime_clear_preedit();
    s_shell.alarm_ime_preedit = make_label(s_shell.alarm_text_dialog, "", FRIDGE_FONT_BODY_SMALL,
                                           FRIDGE_UI_COLOR_MUTED, 20, 112, 568,
                                           LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < FRIDGE_IME_MAX_CANDIDATES; i++) {
        int row = (int)(i / 6U);
        int column = (int)(i % 6U);
        lv_obj_t *candidate = make_button(s_shell.alarm_text_dialog, 20 + column * 94,
                                          142 + row * 34, 88, 30,
                                          FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_SAGE_DARK,
                                          FRIDGE_UI_COLOR_CREAM, "", FRIDGE_FONT_BODY_SMALL,
                                          alarm_text_candidate_event,
                                          (void *)(uintptr_t)i,
                                          &s_shell.alarm_ime_candidates[i]);
        (void)candidate;
    }
    static const char *rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (size_t row = 0; row < 3; row++) {
        const char *letters = rows[row];
        size_t count = strlen(letters);
        int x = row == 0 ? 20 : row == 1 ? 48 : 84;
        for (size_t i = 0; i < count; i++) {
            char letter[2] = {letters[i], '\0'};
            make_button(s_shell.alarm_text_dialog, x + (int)i * 56, 226 + (int)row * 42,
                        52, 34, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                        FRIDGE_UI_COLOR_LINE, letter, FRIDGE_FONT_BODY_SMALL,
                        alarm_text_key_event, (void *)(uintptr_t)letters[i], NULL);
        }
    }
    make_button(s_shell.alarm_text_dialog, 486, 310, 102, 34, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "退格",
                FRIDGE_FONT_BODY_SMALL, alarm_text_backspace_event, NULL, NULL);
    make_button(s_shell.alarm_text_dialog, 20, 392, 96, 46, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_CREAM, "清空",
                FRIDGE_FONT_BODY_SMALL, alarm_text_clear_event, NULL, NULL);
    make_button(s_shell.alarm_text_dialog, 330, 392, 116, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消",
                FRIDGE_FONT_BODY, alarm_text_cancel_event, NULL, NULL);
    make_button(s_shell.alarm_text_dialog, 458, 392, 130, 46, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "确定",
                FRIDGE_FONT_BODY, alarm_text_save_event, NULL, NULL);
    refresh_alarm_ime();
    lv_obj_move_foreground(s_shell.alarm_text_scrim);
    lv_obj_move_foreground(s_shell.alarm_text_dialog);
}

static void create_stopwatch_page(lv_obj_t *page)
{
    // 参考稿秒表主卡为约 234px 高；内部数字、状态和按钮按浏览器首屏基线定位。
    lv_obj_t *face = page_card(page, 0, 234, FRIDGE_UI_COLOR_SURFACE);
    s_shell.stopwatch_face_card = face;
    // PPA 动态覆盖必须带不透明底色，先覆盖静态快照中的旧数字，再绘制当前数字。
    // 区域颜色与主卡一致，不改变参考稿的字体、字号和视觉层级。
    lv_obj_t *face_region = lv_obj_create(face);
    lv_obj_remove_style_all(face_region);
    lv_obj_set_pos(face_region, 18, 8);
    lv_obj_set_size(face_region, 620, 84);
    lv_obj_set_style_bg_color(face_region, color_hex(FRIDGE_UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(face_region, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(face_region, 0, 0);
    lv_obj_clear_flag(face_region, LV_OBJ_FLAG_SCROLLABLE);
    s_shell.stopwatch_face_region = face_region;
    s_shell.stopwatch_face = make_label(face_region, "00:00.000", FRIDGE_FONT_HUGE,
                                        FRIDGE_UI_COLOR_INK, 0, 0, 620,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_shell.stopwatch_face, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_shell.stopwatch_face, LV_ALIGN_CENTER, 0, 0);
    s_shell.stopwatch_state = make_label(face, "就绪", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED,
                                         18, 100, 620, LV_SIZE_CONTENT,
                                         LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.stopwatch_state, LV_TEXT_ALIGN_CENTER, 0);

    // 参考稿操作区最大宽度 520px，三列比例为 1 : 1.3 : 1；两侧按钮不再横向铺满。
    lv_obj_t *ops = lv_obj_create(face);
    clear_container_style(ops);
    lv_obj_set_pos(ops, 76, 153);
    lv_obj_set_size(ops, 520, 64);
    s_shell.stopwatch_reset = make_stopwatch_button(
        ops, 0, 0, 150, 64, FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_SAGE_DARK,
        FRIDGE_UI_COLOR_LINE_STRONG, "复位", FRIDGE_UI_ICON_RETRY, "↻",
        stopwatch_reset_event, NULL, NULL);
    s_shell.stopwatch_toggle = make_stopwatch_button(
        ops, 162, 0, 196, 64, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF, FRIDGE_UI_COLOR_SAGE,
        "开始", FRIDGE_UI_ICON_PLAY, ">", stopwatch_toggle_event,
        &s_shell.stopwatch_toggle_label, NULL);
    if (s_shell.icon_slot_count > 0) {
        icon_slot_t *play_slot = &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
        s_shell.stopwatch_toggle_play_image = play_slot->image;
        s_shell.stopwatch_toggle_play_fallback = play_slot->fallback;
    }
    // 开始/暂停共用一个按钮；两个图标仅切换可见性，避免运行时重建 LVGL 对象。
    register_icon(s_shell.stopwatch_toggle, FRIDGE_UI_ICON_PAUSE, 0xFFFFFF,
                  84, 4, 28, "||");
    if (s_shell.icon_slot_count > 0) {
        icon_slot_t *pause_slot = &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
        s_shell.stopwatch_toggle_pause_image = pause_slot->image;
        s_shell.stopwatch_toggle_pause_fallback = pause_slot->fallback;
        lv_obj_add_flag(pause_slot->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pause_slot->fallback, LV_OBJ_FLAG_HIDDEN);
    }
    s_shell.stopwatch_lap = make_stopwatch_button(
        ops, 370, 0, 150, 64, FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_SAGE_DARK,
        FRIDGE_UI_COLOR_LINE_STRONG, "计次", FRIDGE_UI_ICON_LIST, "≡",
        stopwatch_lap_event, NULL, NULL);

    s_shell.stopwatch_laps_count = make_label(page, "计次（0）", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                               FRIDGE_UI_COLOR_SAGE_DARK, 2, 250, 200,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.stopwatch_laps_empty = make_box(page, 0, 286, 672, 56,
                                             FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    s_shell.stopwatch_laps_empty_label = make_label(
        s_shell.stopwatch_laps_empty, "计次记录会出现在这里", FRIDGE_FONT_BODY,
        FRIDGE_UI_COLOR_MUTED, 16, 0, 640, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(s_shell.stopwatch_laps_empty_label, 640);

    s_shell.stopwatch_laps_list = make_box(page, 0, 286, 672,
                                           FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS * 48,
                                           FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(s_shell.stopwatch_laps_list, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS; i++) {
        lv_obj_t *row = lv_obj_create(s_shell.stopwatch_laps_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 1, (int)i * 48);
        lv_obj_set_size(row, 670, 48);
        lv_obj_set_style_bg_color(row, color_hex(FRIDGE_UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, color_hex(FRIDGE_UI_COLOR_LINE), 0);
        lv_obj_set_style_border_width(row, i + 1U < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS ? 1 : 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_shell.stopwatch_lap_rows[i] = row;
        s_shell.stopwatch_lap_labels[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                                       FRIDGE_UI_COLOR_MUTED, 16, 0, 200,
                                                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_align(s_shell.stopwatch_lap_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        s_shell.stopwatch_lap_times[i] = make_label(row, "", FRIDGE_FONT_BODY,
                                                     FRIDGE_UI_COLOR_INK, 350, 0, 290,
                                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_shell.stopwatch_lap_times[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_shell.stopwatch_lap_times[i], LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

// ==================== MORE 竖屏隐私页 ====================
// 以下四个页面使用 portrait_scroll 唯一纵向滚动容器；透明内容宿主不带 CLICKABLE，
// 避免它在 GT911/LVGL 手势链上截获滚动。固定页头、导航和状态舱不属于该缓存。

static void create_portrait_more_page(lv_obj_t *content)
{
    static const char *const tool_titles[3] = {"定时器", "闹钟", "秒表"};
    static const char *const tool_fallback_desc[3] = {
        "厨房倒计时 · 快捷预设", "固定闹钟 · 周期提醒", "计次 · 复位 · 大数字面",
    };
    static const shell_page_t tool_pages[3] = {
        SHELL_PAGE_TIMER, SHELL_PAGE_ALARM, SHELL_PAGE_STOPWATCH,
    };
    static const fridge_ui_icon_id_t tool_icons[3] = {
        FRIDGE_UI_ICON_TIMER, FRIDGE_UI_ICON_BELL, FRIDGE_UI_ICON_STOPWATCH,
    };
    static const char *const tool_fallback_icons[3] = {"T", "A", "S"};
    static const char *const system_titles[3] = {
        "设备与厨房安全", "设置中心", "维护中心",
    };
    static const char *const system_descriptions[3] = {
        "硬件健康 · 连接 · 传感器 · 安全事件",
        "网络 · AI 模型 · 权限 · 语音显示",
        "存储 · 插件 · 日志 · 系统更新",
    };
    static const fridge_ui_icon_id_t system_icons[3] = {
        FRIDGE_UI_ICON_SHIELD, FRIDGE_UI_ICON_INFO, FRIDGE_UI_ICON_CLOCK,
    };
    static const char *const system_fallback_icons[3] = {"!", "I", "M"};
    static const shell_page_t system_pages[3] = {
        SHELL_PAGE_SAFETY, SHELL_PAGE_SETTINGS, SHELL_PAGE_MAINTENANCE,
    };

    make_label(content, "时间工具", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 2, 0, 220, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 3; i++) {
        int x = (int)(i % 2U) * 290;
        int y = 28 + (int)(i / 2U) * (UI_PORTRAIT_MORE_CARD_H + UI_PORTRAIT_TOOL_GAP);
        lv_obj_t *card = make_box(content, x, y, 278, UI_PORTRAIT_MORE_CARD_H,
                                  FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        add_click_event(card, portrait_nav_event, (void *)(uintptr_t)tool_pages[i]);
        lv_obj_t *icon = make_box(card, 16, 16, 48, 48, FRIDGE_UI_COLOR_MINT,
                                  FRIDGE_UI_COLOR_MINT, 8);
        register_icon(icon, tool_icons[i], FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 28, tool_fallback_icons[i]);
        if (i < 2U) {
            lv_obj_t *badge = make_box(card, 242, 12, 24, 24,
                                       FRIDGE_UI_COLOR_OFFLINE,
                                       FRIDGE_UI_COLOR_OFFLINE, 12);
            lv_obj_t *badge_label = make_label(badge, "", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                               0xFFFFFF, 0, 0, 24, LV_SIZE_CONTENT,
                                               LV_LABEL_LONG_CLIP);
            center_single_line_label(badge_label, 24);
            s_shell.portrait_more.badges[i] = badge;
            s_shell.portrait_more.badge_labels[i] = badge_label;
            lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        }
        s_shell.portrait_more.titles[i] = make_label(
            card, tool_titles[i], FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
            16, 72, 238, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.portrait_more.descriptions[i] = make_label(
            card, tool_fallback_desc[i], FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
            16, 100, 238, 28, LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(s_shell.portrait_more.descriptions[i]);
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      244, 106, 20, ">");
    }

    const int system_y = 28 + 2 * (UI_PORTRAIT_MORE_CARD_H + UI_PORTRAIT_TOOL_GAP) + 12;
    make_label(content, "系统与维护", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 2, system_y, 220, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 3; i++) {
        size_t index = i + 3U;
        int x = (int)(i % 2U) * 290;
        int y = system_y + 28 + (int)(i / 2U) *
                (UI_PORTRAIT_MORE_CARD_H + UI_PORTRAIT_TOOL_GAP);
        lv_obj_t *card = make_box(content, x, y, 278, UI_PORTRAIT_MORE_CARD_H,
                                  FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        add_click_event(card, portrait_nav_event,
                        (void *)(uintptr_t)system_pages[i]);
        lv_obj_t *icon = make_box(card, 16, 16, 48, 48, FRIDGE_UI_COLOR_MINT,
                                  FRIDGE_UI_COLOR_MINT, 8);
        register_icon(icon, system_icons[i], FRIDGE_UI_COLOR_SAGE_DARK,
                      10, 10, 28, system_fallback_icons[i]);
        s_shell.portrait_more.titles[index] = make_label(
            card, system_titles[i], FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
            16, 72, 238, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.portrait_more.descriptions[index] = make_label(
            card, system_descriptions[i], FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
            16, 100, 238, 28, LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(s_shell.portrait_more.descriptions[index]);
        register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                      244, 106, 20, ">");
    }

    s_shell.portrait_more.summary = make_box(
        content, 0, system_y + 2 * (UI_PORTRAIT_MORE_CARD_H + UI_PORTRAIT_TOOL_GAP) + 12,
        UI_PORTRAIT_CONTENT_W, 84, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    make_label(s_shell.portrait_more.summary, "设备状态", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 16, 10, 120, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    s_shell.portrait_more.summary_wifi = make_label(
        s_shell.portrait_more.summary, "Wi-Fi：--", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_MUTED, 16, 42, 166, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.portrait_more.summary_tf = make_label(
        s_shell.portrait_more.summary, "TF：--", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_MUTED, 194, 42, 150, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.portrait_more.summary_time = make_label(
        s_shell.portrait_more.summary, "时间：--", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_MUTED, 370, 42, 166, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
}

static void create_portrait_timer_page(lv_obj_t *content)
{
    portrait_timer_view_t *view = &s_shell.portrait_timer;
    view->content = content;
    view->hero = make_box(content, 0, 0, UI_PORTRAIT_CONTENT_W, 220,
                          FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_LINE, 8);
    view->label = make_label(view->hero, "厨房倒计时", FRIDGE_FONT_BODY_EMPHASIS,
                             FRIDGE_UI_COLOR_SAGE_DARK, 60, 14, 448,
                             LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->label, LV_TEXT_ALIGN_CENTER, 0);
    view->chip = make_label(view->hero, "", FRIDGE_FONT_BODY_SMALL,
                            FRIDGE_UI_COLOR_MUTED, 420, 18, 120,
                            LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->chip, LV_TEXT_ALIGN_RIGHT, 0);
    view->face_region = make_box(view->hero, 16, 50, 536, 86,
                                 FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 0);
    view->face = make_label(view->face_region, "00:00", FRIDGE_FONT_HUGE,
                            FRIDGE_UI_COLOR_MUTED, 0, 0, 536,
                            LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(view->face, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->face, LV_ALIGN_CENTER, 0, 0);
    view->state = make_label(view->hero, "点下方预设立即开始，或新建自定义时长",
                             FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                             20, 140, 528, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->state, LV_TEXT_ALIGN_CENTER, 0);

    view->ops = lv_obj_create(view->hero);
    clear_container_style(view->ops);
    lv_obj_set_pos(view->ops, 4, 166);
    lv_obj_set_size(view->ops, 560, 46);
    view->pause = make_button(view->ops, 0, 0, 174, 46, FRIDGE_UI_COLOR_SURFACE,
                              FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG,
                              "暂停", FRIDGE_FONT_BODY_SMALL, timer_pause_event, NULL,
                              &view->pause_label);
    view->plus = make_button(view->ops, 188, 0, 174, 46, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG,
                             "+1 分钟", FRIDGE_FONT_BODY_SMALL, timer_plus_event, NULL, NULL);
    view->cancel = make_button(view->ops, 376, 0, 174, 46, FRIDGE_UI_COLOR_SURFACE,
                               FRIDGE_UI_COLOR_TOMATO, FRIDGE_UI_COLOR_LINE_STRONG,
                               "取消", FRIDGE_FONT_BODY_SMALL, timer_cancel_event, NULL, NULL);
    view->dismiss = make_button(view->ops, 376, 0, 174, 46, FRIDGE_UI_COLOR_TOMATO,
                                0xFFFFFF, FRIDGE_UI_COLOR_TOMATO, "结束",
                                FRIDGE_FONT_BODY_SMALL, timer_dismiss_event, NULL, NULL);
    lv_obj_add_flag(view->ops, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->dismiss, LV_OBJ_FLAG_HIDDEN);

    make_label(content, "快捷开始", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 2, 244, 180, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < UI_TIMER_PRESET_MAX; i++) {
        uint16_t minutes = timer_preset_minutes(i);
        char text[8];
        snprintf(text, sizeof(text), "%u", (unsigned)minutes);
        int x = (int)(i % 3U) * 194;
        int y = 272 + (int)(i / 3U) * 80;
        lv_obj_t *preset = make_button(content, x, y, 180, 72,
                                       FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                       FRIDGE_UI_COLOR_LINE, "", FRIDGE_FONT_BODY,
                                       timer_preset_event, (void *)(uintptr_t)minutes, NULL);
        lv_obj_t *number = make_label(preset, text, FRIDGE_FONT_TITLE,
                                      FRIDGE_UI_COLOR_SAGE_DARK, 0, 6, 180,
                                      LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *unit = make_label(preset, "分钟", FRIDGE_FONT_BODY_SMALL,
                                    FRIDGE_UI_COLOR_MUTED, 0, 42, 180,
                                    LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    }

    view->other_title = make_label(content, "其他运行中", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                   FRIDGE_UI_COLOR_SAGE_DARK, 2, 448, 300,
                                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(view->other_title, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < UI_TIMER_OTHER_MAX; i++) {
        int y = 478 + (int)i * 120;
        lv_obj_t *row = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W, 108,
                                 FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        view->other_rows[i] = row;
        view->other_names[i] = make_label(row, "", FRIDGE_FONT_BODY_EMPHASIS,
                                          FRIDGE_UI_COLOR_INK, 16, 10, 330,
                                          LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        view->other_remains[i] = make_label(row, "", FRIDGE_FONT_BODY,
                                            FRIDGE_UI_COLOR_SAGE_DARK, 360, 10, 190,
                                            LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(view->other_remains[i], LV_TEXT_ALIGN_RIGHT, 0);
        view->other_meta[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED, 16, 42, 536,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        view->other_pause[i] = make_button(row, 16, 68, 134, 34,
                                           FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                           FRIDGE_UI_COLOR_LINE, "暂停", FRIDGE_FONT_BODY_SMALL,
                                           timer_other_pause_event, NULL,
                                           &view->other_pause_labels[i]);
        view->other_plus[i] = make_button(row, 160, 68, 134, 34,
                                          FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                          FRIDGE_UI_COLOR_LINE, "+1分钟", FRIDGE_FONT_BODY_SMALL,
                                          timer_other_plus_event, NULL, NULL);
        view->other_cancel[i] = make_button(row, 304, 68, 110, 34,
                                            FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_TOMATO,
                                            FRIDGE_UI_COLOR_LINE, "取消", FRIDGE_FONT_BODY_SMALL,
                                            timer_other_cancel_event, NULL, NULL);
        view->other_ids[i] = 0;
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    view->done_title = make_label(content, "已完成", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                  FRIDGE_UI_COLOR_SAGE_DARK, 2, 970, 300,
                                  LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(view->done_title, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < UI_TIMER_DONE_MAX; i++) {
        int y = 1000 + (int)i * 76;
        lv_obj_t *row = make_box(content, 0, y, UI_PORTRAIT_CONTENT_W, 68,
                                 FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
        lv_obj_set_style_opa(row, LV_OPA_60, 0);
        view->done_rows[i] = row;
        view->done_names[i] = make_label(row, "", FRIDGE_FONT_BODY,
                                         FRIDGE_UI_COLOR_INK, 16, 10, 330,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        view->done_meta[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                        FRIDGE_UI_COLOR_MUTED, 16, 38, 536,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *create_portrait_alarm_card(lv_obj_t *content, size_t slot)
{
    lv_obj_t *card = make_box(content, 0, 60, UI_PORTRAIT_CONTENT_W,
                               UI_PORTRAIT_ALARM_CARD_H,
                               FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    portrait_alarm_view_t *view = &s_shell.portrait_alarm;
    view->cards[slot] = card;
    view->card_main[slot] = make_button(card, 1, 1, 478, 102,
                                        FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_INK,
                                        FRIDGE_UI_COLOR_SURFACE, "", FRIDGE_FONT_BODY,
                                        portrait_unsupported_event, NULL, NULL);
    view->card_times[slot] = make_label(view->card_main[slot], "--:--",
                                        FRIDGE_FONT_SECTION_STRONG, FRIDGE_UI_COLOR_INK,
                                        16, 10, 112, LV_SIZE_CONTENT,
                                        LV_LABEL_LONG_CLIP);
    view->card_names[slot] = make_label(view->card_main[slot], "",
                                        FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                                        136, 12, 190, LV_SIZE_CONTENT,
                                        LV_LABEL_LONG_DOT);
    lv_obj_t *chip = make_box(view->card_main[slot], 336, 12, 112, 28,
                              FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 14);
    view->card_kind[slot] = make_label(chip, "", FRIDGE_FONT_BODY_SMALL,
                                       FRIDGE_UI_COLOR_SAGE_DARK, 4, 0, 104,
                                       LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(view->card_kind[slot], 104);
    view->card_meta[slot] = make_label(view->card_main[slot], "",
                                       FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                       136, 56, 330, LV_SIZE_CONTENT,
                                       LV_LABEL_LONG_DOT);
    view->card_toggles[slot] = make_button(
        card, 490, 34, 64, 36, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
        FRIDGE_UI_COLOR_SAGE, "", FRIDGE_FONT_BODY_SMALL, alarm_toggle_event,
        NULL, NULL);
    lv_obj_set_style_radius(view->card_toggles[slot], 18, 0);
    view->card_knobs[slot] = make_box(view->card_toggles[slot], 32, 4, 28, 28,
                                      FRIDGE_UI_COLOR_SURFACE,
                                      FRIDGE_UI_COLOR_SURFACE, 14);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    return card;
}

static void create_portrait_alarm_page(lv_obj_t *content)
{
    portrait_alarm_view_t *view = &s_shell.portrait_alarm;
    view->content = content;
    view->status = make_label(content, "时间已同步，可设置固定时刻和周期提醒",
                              FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                              0, 0, UI_PORTRAIT_CONTENT_W, LV_SIZE_CONTENT,
                              LV_LABEL_LONG_DOT);
    view->fixed_title = make_label(content, "闹钟（0）", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                   FRIDGE_UI_COLOR_SAGE_DARK, 2, 32, 220,
                                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    view->fixed_empty = make_box(content, 0, 60, UI_PORTRAIT_CONTENT_W, 72,
                                 FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *fixed_empty_label = make_label(view->fixed_empty, "还没有闹钟",
                                             FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                             8, 0, UI_PORTRAIT_CONTENT_W - 16,
                                             LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(fixed_empty_label, UI_PORTRAIT_CONTENT_W - 16);
    view->cycle_title = make_label(content, "周期提醒（0）",
                                   FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                   FRIDGE_UI_COLOR_SAGE_DARK, 2, 148, 240,
                                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    view->cycle_empty = make_box(content, 0, 176, UI_PORTRAIT_CONTENT_W, 72,
                                 FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *cycle_empty_label = make_label(view->cycle_empty, "还没有周期提醒",
                                             FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                             8, 0, UI_PORTRAIT_CONTENT_W - 16,
                                             LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(cycle_empty_label, UI_PORTRAIT_CONTENT_W - 16);
    for (size_t i = 0; i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        (void)create_portrait_alarm_card(content, i);
        view->card_ids[i] = 0;
    }
}

static void create_portrait_stopwatch_page(lv_obj_t *content)
{
    portrait_stopwatch_view_t *view = &s_shell.portrait_stopwatch;
    view->content = content;
    view->stage = make_box(content, 0, 0, UI_PORTRAIT_CONTENT_W, 276,
                           FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    view->face_region = make_box(view->stage, 16, 16, 536, 88,
                                 FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SURFACE, 0);
    view->face = make_label(view->face_region, "00:00.000", FRIDGE_FONT_HUGE,
                            FRIDGE_UI_COLOR_INK, 0, 0, 536,
                            LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(view->face, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(view->face, LV_ALIGN_CENTER, 0, 0);
    view->state = make_label(view->stage, "就绪", FRIDGE_FONT_BODY_SMALL,
                             FRIDGE_UI_COLOR_MUTED, 16, 112, 536,
                             LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->state, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *ops = lv_obj_create(view->stage);
    clear_container_style(ops);
    lv_obj_set_pos(ops, 24, 158);
    lv_obj_set_size(ops, 520, 64);
    view->reset = make_stopwatch_button(
        ops, 0, 0, 150, 64, FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_SAGE_DARK,
        FRIDGE_UI_COLOR_LINE_STRONG, "复位", FRIDGE_UI_ICON_RETRY, "↻",
        stopwatch_reset_event, NULL, NULL);
    view->toggle = make_stopwatch_button(
        ops, 162, 0, 196, 64, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
        FRIDGE_UI_COLOR_SAGE, "开始", FRIDGE_UI_ICON_PLAY, ">",
        stopwatch_toggle_event, &view->toggle_label, &view->toggle_play_slot);
    register_icon(view->toggle, FRIDGE_UI_ICON_PAUSE, 0xFFFFFF, 84, 4, 28, "||");
    if (s_shell.icon_slot_count > 0) {
        view->toggle_pause_slot = &s_shell.icon_slots[s_shell.icon_slot_count - 1U];
        lv_obj_add_flag(view->toggle_pause_slot->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->toggle_pause_slot->fallback, LV_OBJ_FLAG_HIDDEN);
    }
    view->lap = make_stopwatch_button(
        ops, 370, 0, 150, 64, FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_SAGE_DARK,
        FRIDGE_UI_COLOR_LINE_STRONG, "计次", FRIDGE_UI_ICON_LIST, "≡",
        stopwatch_lap_event, NULL, NULL);

    view->laps_count = make_label(content, "计次（0）", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                  FRIDGE_UI_COLOR_SAGE_DARK, 2, 294, 200,
                                  LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    view->laps_empty = make_box(content, 0, 330, UI_PORTRAIT_CONTENT_W, 64,
                                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    view->laps_empty_label = make_label(view->laps_empty, "计次记录会出现在这里",
                                        FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                        16, 0, UI_PORTRAIT_CONTENT_W - 32,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    center_single_line_label(view->laps_empty_label, UI_PORTRAIT_CONTENT_W - 32);
    view->laps_list = make_box(content, 0, 330, UI_PORTRAIT_CONTENT_W,
                               FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS * 48,
                               FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_add_flag(view->laps_list, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS; i++) {
        lv_obj_t *row = lv_obj_create(view->laps_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 1, (int)i * 48);
        lv_obj_set_size(row, UI_PORTRAIT_CONTENT_W - 2, 48);
        lv_obj_set_style_bg_color(row, color_hex(FRIDGE_UI_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, color_hex(FRIDGE_UI_COLOR_LINE), 0);
        lv_obj_set_style_border_width(row, i + 1U < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS ? 1 : 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        view->lap_rows[i] = row;
        view->lap_labels[i] = make_label(row, "", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED, 16, 0, 220,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_align(view->lap_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        view->lap_times[i] = make_label(row, "", FRIDGE_FONT_BODY,
                                        FRIDGE_UI_COLOR_INK, 300, 0, 230,
                                        LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(view->lap_times[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(view->lap_times[i], LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
}

// ==================== INV-01..08 原生库存页面 ====================
// 所有页面固定运行在 1024x600 横屏 Shell 中；主区 672px 内容宽度与 ver2 参考稿 1:1 对齐。

static const fridge_ui_inventory_zone_t *inventory_current_zone(void)
{
    return s_shell.inventory_zone_index < s_shell.inventory.zone_count ?
        &s_shell.inventory.zones[s_shell.inventory_zone_index] : NULL;
}

static const fridge_ui_inventory_layer_t *inventory_current_layer(void)
{
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    if (!zone) return NULL;
    size_t seen = 0;
    for (size_t i = 0; i < s_shell.inventory.layer_count; i++) {
        if (strcmp(s_shell.inventory.layers[i].zone_id, zone->id) != 0) continue;
        if (seen++ == s_shell.inventory_layer_index) return &s_shell.inventory.layers[i];
    }
    return NULL;
}

static size_t inventory_scope_count(int *expiring, int *expired, int *unlocated)
{
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
    size_t count = 0;
    if (expiring) *expiring = 0;
    if (expired) *expired = 0;
    if (unlocated) *unlocated = 0;
    for (size_t i = 0; i < s_shell.inventory.item_count; i++) {
        const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[i];
        if (s_shell.inventory_scope_level >= 1 && (!zone || strcmp(item->zone_id, zone->id) != 0)) continue;
        if (s_shell.inventory_scope_level >= 2 && (!layer || strcmp(item->layer_id, layer->id) != 0)) continue;
        count++;
        if (!item->zone_id[0] && unlocated) (*unlocated)++;
        if (item->days_left < 0 && expired) (*expired)++;
        else if (item->days_left <= 3 && expiring) (*expiring)++;
    }
    return count;
}

// 查询指定区域、层和格位是否已有食材。新增与识别写入共用该检查，
// 避免真实库存出现同一格位叠放多个条目的情况。
static int inventory_item_at_location(const char *zone_id, const char *layer_id, uint8_t cell)
{
    if (!zone_id || !zone_id[0] || !layer_id || !layer_id[0]) return -1;
    for (size_t i = 0; i < s_shell.inventory.item_count; i++) {
        const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[i];
        if (item->cell == cell && strcmp(item->zone_id, zone_id) == 0 &&
            strcmp(item->layer_id, layer_id) == 0) return (int)i;
    }
    return -1;
}

static int inventory_item_at_cell(uint8_t cell)
{
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
    if (!zone || !layer) return -1;
    return inventory_item_at_location(zone->id, layer->id, cell);
}

// 从首选格位开始寻找该层的空位；九宫格已满时返回 -1，由调用方保留为“未定位”。
static int inventory_first_free_cell(const char *zone_id, const char *layer_id,
                                     uint8_t preferred)
{
    for (uint8_t offset = 0; offset < FRIDGE_UI_INVENTORY_GRID_CELLS; offset++) {
        uint8_t cell = (uint8_t)((preferred + offset) % FRIDGE_UI_INVENTORY_GRID_CELLS);
        if (inventory_item_at_location(zone_id, layer_id, cell) < 0) return cell;
    }
    return -1;
}

static bool inventory_place_first_free(fridge_ui_inventory_item_t *item,
                                       const char *zone_id, const char *layer_id,
                                       const char *zone_name, const char *layer_name,
                                       uint8_t preferred)
{
    if (!item) return false;
    int cell = inventory_first_free_cell(zone_id, layer_id, preferred);
    if (cell < 0) return false;
    strlcpy(item->zone_id, zone_id, sizeof(item->zone_id));
    strlcpy(item->layer_id, layer_id, sizeof(item->layer_id));
    item->cell = (uint8_t)cell;
    snprintf(item->location, sizeof(item->location), "%s / %s / %c%u",
             zone_name, layer_name, 'A' + cell / 3,
             (unsigned)(cell % 3 + 1));
    return true;
}

static void inventory_open_selected_item(size_t index)
{
    if (index >= s_shell.inventory.item_count) return;
    s_shell.inventory_selected_item = (uint8_t)index;
    switch_page(SHELL_PAGE_INVENTORY_DETAIL);
}

static void inventory_zone_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_shell.inventory.zone_count) return;
    s_shell.inventory_zone_index = (uint8_t)index;
    s_shell.inventory_layer_index = 0;
    s_shell.inventory_scope_level = 1;
    switch_page(SHELL_PAGE_INVENTORY);
}

static void inventory_layer_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_layer_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    s_shell.inventory_scope_level = 2;
    switch_page(SHELL_PAGE_INVENTORY);
}

static void inventory_cell_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    uint8_t cell = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    int item = inventory_item_at_cell(cell);
    if (s_shell.inventory_pick_location) {
        if (item >= 0 &&
            (!s_shell.inventory_edit_existing ||
             (size_t)item != s_shell.inventory_selected_item)) {
            fridge_ui_shell_show_toast("这个格位已有食材");
            return;
        }
        const fridge_ui_inventory_zone_t *pick_zone = inventory_current_zone();
        const fridge_ui_inventory_layer_t *pick_layer = inventory_current_layer();
        if (!pick_zone || !pick_layer) return;
        strlcpy(s_shell.inventory_draft.zone_id, pick_zone->id,
                sizeof(s_shell.inventory_draft.zone_id));
        strlcpy(s_shell.inventory_draft.layer_id, pick_layer->id,
                sizeof(s_shell.inventory_draft.layer_id));
        s_shell.inventory_draft.cell = cell;
        snprintf(s_shell.inventory_draft.location,
                 sizeof(s_shell.inventory_draft.location), "%s / %s / %c%u",
                 pick_zone->name, pick_layer->name, 'A' + cell / 3U,
                 (unsigned)(cell % 3U + 1U));
        s_shell.inventory_pick_location = false;
        switch_page(SHELL_PAGE_INVENTORY_EDIT);
        return;
    }
    if (item >= 0) {
        inventory_open_selected_item((size_t)item);
        return;
    }
    memset(&s_shell.inventory_draft, 0, sizeof(s_shell.inventory_draft));
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
    if (zone) strlcpy(s_shell.inventory_draft.zone_id, zone->id, sizeof(s_shell.inventory_draft.zone_id));
    if (layer) strlcpy(s_shell.inventory_draft.layer_id, layer->id, sizeof(s_shell.inventory_draft.layer_id));
    s_shell.inventory_draft.cell = cell;
    strlcpy(s_shell.inventory_draft.quantity, "1份", sizeof(s_shell.inventory_draft.quantity));
    s_shell.inventory_edit_existing = false;
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

static void inventory_toggle_view_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_list_view = !s_shell.inventory_list_view;
    switch_page(SHELL_PAGE_INVENTORY);
}

static void inventory_back_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.page == SHELL_PAGE_INVENTORY) {
        if (s_shell.inventory_pick_location) {
            s_shell.inventory_pick_location = false;
            switch_page(SHELL_PAGE_INVENTORY_EDIT);
            return;
        }
        if (s_shell.inventory_scope_level == 2) s_shell.inventory_scope_level = 1;
        else if (s_shell.inventory_scope_level == 1) s_shell.inventory_scope_level = 0;
        else { switch_page(SHELL_PAGE_HOME); return; }
        switch_page(SHELL_PAGE_INVENTORY);
        return;
    }
    if (s_shell.page == SHELL_PAGE_INVENTORY_LAYOUT &&
        s_shell.inventory_layout_level > 0) {
        s_shell.inventory_layout_level--;
        switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
        return;
    }
    switch_page(SHELL_PAGE_INVENTORY);
}

static void inventory_open_new_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    memset(&s_shell.inventory_draft, 0, sizeof(s_shell.inventory_draft));
    strlcpy(s_shell.inventory_draft.quantity, "1份", sizeof(s_shell.inventory_draft.quantity));
    strlcpy(s_shell.inventory_draft.expire_date, "待填写",
            sizeof(s_shell.inventory_draft.expire_date));
    s_shell.inventory_draft.days_left = 3;
    s_shell.inventory_draft.cell = FRIDGE_UI_INVENTORY_CELL_UNLOCATED;
    if (s_shell.inventory_scope_level >= 2) {
        const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
        const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
        if (zone && layer) {
            (void)inventory_place_first_free(&s_shell.inventory_draft,
                                             zone->id, layer->id,
                                             zone->name, layer->name, 0);
        }
    }
    s_shell.inventory_edit_existing = false;
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

esp_err_t fridge_ui_shell_open_inventory_prefill(const char *name,
                                                 const char *quantity,
                                                 const char *note)
{
    if (!s_shell.created || !name || !name[0]) return ESP_ERR_INVALID_ARG;

    // AI 识别结果只能形成 INV-04 草稿，禁止在这里调用库存持久化接口；
    // 用户仍需核对名称、数量、保质期和位置后手动保存。
    memset(&s_shell.inventory_draft, 0, sizeof(s_shell.inventory_draft));
    strlcpy(s_shell.inventory_draft.name, name, sizeof(s_shell.inventory_draft.name));
    strlcpy(s_shell.inventory_draft.quantity,
            quantity && quantity[0] ? quantity : "1份",
            sizeof(s_shell.inventory_draft.quantity));
    strlcpy(s_shell.inventory_draft.expire_date, "3天后",
            sizeof(s_shell.inventory_draft.expire_date));
    s_shell.inventory_draft.days_left = 3;
    s_shell.inventory_draft.cell = FRIDGE_UI_INVENTORY_CELL_UNLOCATED;
    if (note && note[0]) {
        strlcpy(s_shell.inventory_draft.note, note,
                sizeof(s_shell.inventory_draft.note));
    }
    s_shell.inventory_category_index = 5;  // 视觉结果未提供可靠分类，默认“其他”供用户确认。
    s_shell.inventory_edit_existing = false;
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
    return ESP_OK;
}

static void inventory_open_edit_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event) ||
        s_shell.inventory_selected_item >= s_shell.inventory.item_count) return;
    s_shell.inventory_draft = s_shell.inventory.items[s_shell.inventory_selected_item];
    s_shell.inventory_edit_existing = true;
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

static void inventory_move_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event) ||
        s_shell.inventory_selected_item >= s_shell.inventory.item_count) return;
    s_shell.inventory_draft = s_shell.inventory.items[s_shell.inventory_selected_item];
    s_shell.inventory_edit_existing = true;
    s_shell.inventory_pick_location = true;
    s_shell.inventory_scope_level = 0;
    switch_page(SHELL_PAGE_INVENTORY);
}

static void inventory_delete_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event) ||
        s_shell.inventory_selected_item >= s_shell.inventory.item_count) return;
    const char *id = s_shell.inventory.items[s_shell.inventory_selected_item].id;
    if (fridge_ui_inventory_model_remove_item(&s_shell.inventory, id) == ESP_OK) {
        fridge_ui_shell_show_toast("已从库存移除");
        switch_page(SHELL_PAGE_INVENTORY);
    } else {
        fridge_ui_shell_show_toast("删除失败，已保留原数据");
    }
}

static void inventory_save_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (!s_shell.inventory_draft.name[0]) {
        fridge_ui_shell_show_toast("请先填写食材名称");
        return;
    }
    if (fridge_ui_inventory_model_upsert_item(&s_shell.inventory, &s_shell.inventory_draft) == ESP_OK) {
        fridge_ui_shell_show_toast("已保存到本地库存");
        switch_page(SHELL_PAGE_INVENTORY);
    } else {
        fridge_ui_shell_show_toast("保存失败，原库存未改变");
    }
}

static void inventory_item_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    inventory_open_selected_item((size_t)(uintptr_t)lv_event_get_user_data(event));
}

static void inventory_search_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    switch_page(SHELL_PAGE_INVENTORY_SEARCH);
}

static void inventory_layout_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_layout_level = 0;
    switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
}

static void inventory_pick_location_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_pick_location = true;
    s_shell.inventory_scope_level = 0;
    switch_page(SHELL_PAGE_INVENTORY);
}

static void inventory_quantity_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    int value = 1;
    (void)sscanf(s_shell.inventory_draft.quantity, "%d", &value);
    value += (int)(intptr_t)lv_event_get_user_data(event);
    if (value < 1) value = 1;
    if (value > 99) value = 99;
    snprintf(s_shell.inventory_draft.quantity,
             sizeof(s_shell.inventory_draft.quantity), "%d份", value);
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

static void inventory_expiry_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    int days = (int)(intptr_t)lv_event_get_user_data(event);
    s_shell.inventory_draft.days_left = (int16_t)days;
    snprintf(s_shell.inventory_draft.expire_date,
             sizeof(s_shell.inventory_draft.expire_date), "%d天后", days);
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

static void inventory_category_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_category_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

static void inventory_autofill_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (!s_shell.inventory_draft.quantity[0]) {
        strlcpy(s_shell.inventory_draft.quantity, "1份",
                sizeof(s_shell.inventory_draft.quantity));
    }
    if (!s_shell.inventory_draft.expire_date[0] ||
        strcmp(s_shell.inventory_draft.expire_date, "待填写") == 0) {
        strlcpy(s_shell.inventory_draft.expire_date, "3天后",
                sizeof(s_shell.inventory_draft.expire_date));
        s_shell.inventory_draft.days_left = 3;
    }
    fridge_ui_shell_show_toast("已按食材类别补全常用信息");
    switch_page(SHELL_PAGE_INVENTORY_EDIT);
}

static void inventory_search_filter_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_search_filter = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    switch_page(SHELL_PAGE_INVENTORY_SEARCH);
}

static void inventory_search_sort_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_search_sort = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    switch_page(SHELL_PAGE_INVENTORY_SEARCH);
}

static void inventory_search_clear_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_search_text[0] = '\0';
    s_shell.inventory_search_filter = 0;
    s_shell.inventory_search_sort = 0;
    switch_page(SHELL_PAGE_INVENTORY_SEARCH);
}

static void inventory_layout_zone_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_shell.inventory.zone_count) return;
    s_shell.inventory_zone_index = (uint8_t)index;
    s_shell.inventory_layer_index = 0;
    s_shell.inventory_layout_level = 1;
    switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
}

static void inventory_layout_layer_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.inventory_layer_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
    if (!zone || !layer) return;
    memset(&s_shell.inventory_layout_draft, 0,
           sizeof(s_shell.inventory_layout_draft));
    strlcpy(s_shell.inventory_layout_draft.zone_id, zone->id,
            sizeof(s_shell.inventory_layout_draft.zone_id));
    strlcpy(s_shell.inventory_layout_draft.layer_id, layer->id,
            sizeof(s_shell.inventory_layout_draft.layer_id));
    strlcpy(s_shell.inventory_layout_draft.name, layer->name,
            sizeof(s_shell.inventory_layout_draft.name));
    strlcat(s_shell.inventory_layout_draft.name, "格位",
            sizeof(s_shell.inventory_layout_draft.name));
    for (size_t i = 0; i < FRIDGE_UI_INVENTORY_GRID_CELLS; i++) {
        snprintf(s_shell.inventory_layout_draft.cell_labels[i],
                 sizeof(s_shell.inventory_layout_draft.cell_labels[i]),
                 "%c%u", 'A' + (int)(i / 3U), (unsigned)(i % 3U + 1U));
    }
    for (size_t i = 0; i < s_shell.inventory.layout_draft_count; i++) {
        const fridge_ui_inventory_layout_draft_t *saved =
            &s_shell.inventory.layout_drafts[i];
        if (strcmp(saved->zone_id, zone->id) == 0 &&
            strcmp(saved->layer_id, layer->id) == 0) {
            s_shell.inventory_layout_draft = *saved;
            break;
        }
    }
    s_shell.inventory_layout_level = 2;
    s_shell.inventory_layout_dirty = false;
    switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
}

static void inventory_layout_cell_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    size_t cell = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (cell >= FRIDGE_UI_INVENTORY_GRID_CELLS) return;
    char *label = s_shell.inventory_layout_draft.cell_labels[cell];
    if (label[0]) label[0] = '\0';
    else snprintf(label, 24, "%c%u", 'A' + (int)(cell / 3U),
                  (unsigned)(cell % 3U + 1U));
    s_shell.inventory_layout_dirty = true;
    switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
}

static void inventory_layout_submit_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (s_shell.inventory_layout_level < 2) {
        fridge_ui_shell_show_toast("请进入具体层后再提交格位设置");
        return;
    }
    if (fridge_ui_inventory_model_upsert_layout_draft(
            &s_shell.inventory, &s_shell.inventory_layout_draft) == ESP_OK) {
        s_shell.inventory_layout_dirty = false;
        fridge_ui_shell_show_toast("空间设置已保存到本机");
        switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
    } else {
        fridge_ui_shell_show_toast("保存失败，原布局未改变");
    }
}

static void inventory_layout_undo_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (s_shell.inventory_layout_level == 2) {
        const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
        const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
        if (!zone || !layer) return;
        memset(&s_shell.inventory_layout_draft, 0,
               sizeof(s_shell.inventory_layout_draft));
        strlcpy(s_shell.inventory_layout_draft.zone_id, zone->id,
                sizeof(s_shell.inventory_layout_draft.zone_id));
        strlcpy(s_shell.inventory_layout_draft.layer_id, layer->id,
                sizeof(s_shell.inventory_layout_draft.layer_id));
        strlcpy(s_shell.inventory_layout_draft.name, layer->name,
                sizeof(s_shell.inventory_layout_draft.name));
        strlcat(s_shell.inventory_layout_draft.name, "格位",
                sizeof(s_shell.inventory_layout_draft.name));
        for (size_t i = 0; i < FRIDGE_UI_INVENTORY_GRID_CELLS; i++) {
            snprintf(s_shell.inventory_layout_draft.cell_labels[i],
                     sizeof(s_shell.inventory_layout_draft.cell_labels[i]),
                     "%c%u", 'A' + (int)(i / 3U),
                     (unsigned)(i % 3U + 1U));
        }
        for (size_t i = 0; i < s_shell.inventory.layout_draft_count; i++) {
            const fridge_ui_inventory_layout_draft_t *saved =
                &s_shell.inventory.layout_drafts[i];
            if (strcmp(saved->zone_id, zone->id) == 0 &&
                strcmp(saved->layer_id, layer->id) == 0) {
                s_shell.inventory_layout_draft = *saved;
                break;
            }
        }
        s_shell.inventory_layout_dirty = false;
        switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
    } else {
        fridge_ui_shell_show_toast("当前没有未提交修改");
    }
}

static void inventory_add_layer_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    if (!zone) return;
    size_t count = 0;
    for (size_t i = 0; i < s_shell.inventory.layer_count; i++) {
        if (strcmp(s_shell.inventory.layers[i].zone_id, zone->id) == 0) count++;
    }
    fridge_ui_inventory_layer_t layer = {0};
    strlcpy(layer.zone_id, zone->id, sizeof(layer.zone_id));
    snprintf(layer.name, sizeof(layer.name), "第%u层", (unsigned)(count + 1U));
    layer.order = (uint8_t)count;
    if (fridge_ui_inventory_model_upsert_layer(&s_shell.inventory, &layer) == ESP_OK) {
        fridge_ui_shell_show_toast("已新增一层");
        switch_page(SHELL_PAGE_INVENTORY_LAYOUT);
    } else {
        fridge_ui_shell_show_toast("新增层失败");
    }
}

static void inventory_recognize_confirm_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    static const char *names[] = {"西红柿", "原味酸奶", "某绿叶菜"};
    static const char *quantities[] = {"3个", "2杯", "1把"};
    static const int16_t days[] = {3, 2, 2};
    size_t saved = 0;
    for (size_t i = 0; i < 3; i++) {
        if (!s_shell.inventory_recognize_keep[i]) continue;
        fridge_ui_inventory_item_t item = {0};
        strlcpy(item.name, names[i], sizeof(item.name));
        strlcpy(item.quantity, quantities[i], sizeof(item.quantity));
        snprintf(item.expire_date, sizeof(item.expire_date), "%d天后", days[i]);
        item.days_left = days[i];
        item.cell = FRIDGE_UI_INVENTORY_CELL_UNLOCATED;
        if (i == 0) {
            (void)inventory_place_first_free(&item, "drawer", "v1",
                                             "果蔬抽屉", "抽屉", 3);
        } else if (i == 1) {
            (void)inventory_place_first_free(&item, "right", "r2",
                                             "右冷藏", "第2层", 2);
            strlcpy(item.note, "日期为估算；存在同名批次", sizeof(item.note));
        } else {
            strlcpy(item.note, "低置信度识别，名称待确认", sizeof(item.note));
        }
        if (fridge_ui_inventory_model_upsert_item(&s_shell.inventory, &item) == ESP_OK) {
            saved++;
        }
    }
    if (saved > 0) {
        char toast[48];
        snprintf(toast, sizeof(toast), "已确认并写入 %u 项库存", (unsigned)saved);
        fridge_ui_shell_show_toast(toast);
        switch_page(SHELL_PAGE_INVENTORY);
    } else {
        fridge_ui_shell_show_toast("请先勾选要写入的识别结果");
    }
}

static void inventory_recognize_toggle_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= 3) return;
    s_shell.inventory_recognize_keep[index] = !s_shell.inventory_recognize_keep[index];
    switch_page(SHELL_PAGE_INVENTORY_RECOGNIZE);
}

// INV-04/06/07 文本输入复用聊天页底层 fridge_ime：同一份拼音词典、候选顺序和 26 键布局。
static void inventory_close_text_input(void)
{
    fridge_ime_clear_preedit();
    if (s_shell.inventory_input_scrim) lv_obj_delete_async(s_shell.inventory_input_scrim);
    if (s_shell.inventory_input_dialog) lv_obj_delete_async(s_shell.inventory_input_dialog);
    s_shell.inventory_input_scrim = NULL;
    s_shell.inventory_input_dialog = NULL;
    s_shell.inventory_textarea = NULL;
    s_shell.inventory_ime_preedit = NULL;
    memset(s_shell.inventory_ime_candidates, 0, sizeof(s_shell.inventory_ime_candidates));
}

static void inventory_refresh_ime(void)
{
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    set_label_if_changed(s_shell.inventory_ime_preedit,
                         snapshot.preedit[0] ? snapshot.preedit : "输入拼音后选择候选词");
    for (size_t i = 0; i < FRIDGE_IME_MAX_CANDIDATES; i++) {
        set_label_if_changed(s_shell.inventory_ime_candidates[i],
                             i < snapshot.candidate_count ? snapshot.candidates[i] : "");
    }
}

static void inventory_ime_key_event(lv_event_t *event)
{
    char key = (char)(uintptr_t)lv_event_get_user_data(event);
    if (!s_shell.inventory_textarea) return;
    lv_textarea_set_cursor_pos(s_shell.inventory_textarea, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_char(s_shell.inventory_textarea, (uint32_t)key);
    fridge_ime_feed_letter(key);
    inventory_refresh_ime();
}

static void inventory_ime_backspace_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (snapshot.preedit[0] && s_shell.inventory_textarea) {
        lv_textarea_set_cursor_pos(s_shell.inventory_textarea, LV_TEXTAREA_CURSOR_LAST);
        lv_textarea_delete_char(s_shell.inventory_textarea);
    } else if (s_shell.inventory_textarea) {
        lv_textarea_delete_char(s_shell.inventory_textarea);
    }
    fridge_ime_backspace();
    inventory_refresh_ime();
}

static void inventory_ime_candidate_event(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (index >= snapshot.candidate_count) return;
    if (snapshot.preedit[0] && s_shell.inventory_textarea) {
        lv_textarea_set_cursor_pos(s_shell.inventory_textarea, LV_TEXTAREA_CURSOR_LAST);
        for (size_t i = 0; snapshot.preedit[i]; i++) {
            lv_textarea_delete_char(s_shell.inventory_textarea);
        }
    }
    if (fridge_ime_select_candidate(index) == ESP_OK) inventory_refresh_ime();
}

static void inventory_ime_digit_event(lv_event_t *event)
{
    char key = (char)(uintptr_t)lv_event_get_user_data(event);
    fridge_ime_clear_preedit();
    if (s_shell.inventory_textarea) lv_textarea_add_char(s_shell.inventory_textarea, key);
    inventory_refresh_ime();
}

static void inventory_ime_space_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (snapshot.preedit[0] && snapshot.candidate_count > 0) {
        if (s_shell.inventory_textarea) {
            lv_textarea_set_cursor_pos(s_shell.inventory_textarea, LV_TEXTAREA_CURSOR_LAST);
            for (size_t i = 0; snapshot.preedit[i]; i++) {
                lv_textarea_delete_char(s_shell.inventory_textarea);
            }
        }
        (void)fridge_ime_select_candidate(0);
    } else if (s_shell.inventory_textarea) {
        lv_textarea_add_text(s_shell.inventory_textarea, " ");
        fridge_ime_clear_preedit();
    }
    inventory_refresh_ime();
}

static void inventory_ime_punctuation_event(lv_event_t *event)
{
    const char *text = lv_event_get_user_data(event);
    fridge_ime_clear_preedit();
    if (text && s_shell.inventory_textarea) lv_textarea_add_text(s_shell.inventory_textarea, text);
    inventory_refresh_ime();
}

static void inventory_ime_previous_page_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (fridge_ime_previous_candidate_page() == ESP_OK) inventory_refresh_ime();
}

static void inventory_ime_next_page_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (fridge_ime_next_candidate_page() == ESP_OK) inventory_refresh_ime();
}

static void inventory_text_cancel_event(lv_event_t *event)
{
    (void)event;
    inventory_close_text_input();
}

static void inventory_text_confirm_event(lv_event_t *event)
{
    (void)event;
    const char *text = s_shell.inventory_textarea ? lv_textarea_get_text(s_shell.inventory_textarea) : NULL;
    if (!text || !text[0]) {
        fridge_ui_shell_show_toast("请输入内容");
        return;
    }
    if (s_shell.inventory_input_target == INVENTORY_INPUT_NAME) {
        strlcpy(s_shell.inventory_draft.name, text, sizeof(s_shell.inventory_draft.name));
    } else if (s_shell.inventory_input_target == INVENTORY_INPUT_NOTE) {
        strlcpy(s_shell.inventory_draft.note, text, sizeof(s_shell.inventory_draft.note));
    } else {
        strlcpy(s_shell.inventory_search_text, text, sizeof(s_shell.inventory_search_text));
    }
    const shell_page_t target = s_shell.inventory_input_target == INVENTORY_INPUT_SEARCH ?
                                SHELL_PAGE_INVENTORY_SEARCH : SHELL_PAGE_INVENTORY_EDIT;
    inventory_close_text_input();
    // 当前页可能正是目标页，强制经库存浏览器重建一次，确保输入结果立即回显。
    switch_page(SHELL_PAGE_INVENTORY);
    switch_page(target);
}

static void inventory_open_text_input(inventory_input_target_t target)
{
    if (s_shell.inventory_input_dialog) return;
    // 输入法弹窗会一次创建候选按钮和文本对象；先完成残留 PPA 帧，避免旧快照
    // 在弹窗创建后继续直接写 DPI 双缓冲，造成编辑页蓝屏或看门狗复位。
    fridge_ui_scroll_ppa_request_finish();
    fridge_ui_scroll_ppa_process();
    fridge_ui_scroll_ppa_set_enabled(false);
    s_shell.inventory_input_target = target;
    const char *title = target == INVENTORY_INPUT_NAME ? "输入食材名称" :
                        target == INVENTORY_INPUT_NOTE ? "输入备注" : "搜索食材";
    const char *initial = target == INVENTORY_INPUT_NAME ? s_shell.inventory_draft.name :
                          target == INVENTORY_INPUT_NOTE ? s_shell.inventory_draft.note :
                          s_shell.inventory_search_text;
    s_shell.inventory_input_scrim = make_box(s_shell.root, 0, UI_HEADER_H, UI_MAIN_W,
                                              UI_PAGE_H,
                                              FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.inventory_input_scrim, LV_OPA_30, 0);
    s_shell.inventory_input_dialog = make_box(s_shell.root, 16, 103, 672, 493,
                                               FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.inventory_input_dialog, title, FRIDGE_FONT_TITLE, FRIDGE_UI_COLOR_INK,
               16, 10, 360, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.inventory_textarea = lv_textarea_create(s_shell.inventory_input_dialog);
    lv_obj_set_pos(s_shell.inventory_textarea, 16, 48);
    lv_obj_set_size(s_shell.inventory_textarea, 460, 48);
    lv_textarea_set_one_line(s_shell.inventory_textarea, true);
    lv_textarea_set_max_length(s_shell.inventory_textarea, target == INVENTORY_INPUT_NOTE ? 79 : 47);
    lv_obj_add_style(s_shell.inventory_textarea, fridge_ui_theme_font_style(FRIDGE_FONT_BODY), 0);
    lv_textarea_set_text(s_shell.inventory_textarea, initial ? initial : "");
    (void)fridge_ime_set_candidate_page_size(6);
    (void)fridge_ime_attach_textarea(s_shell.inventory_textarea);
    fridge_ime_clear_preedit();
    make_button(s_shell.inventory_input_dialog, 484, 48, 76, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消",
                FRIDGE_FONT_BODY_SMALL, inventory_text_cancel_event, NULL, NULL);
    make_button(s_shell.inventory_input_dialog, 568, 48, 88, 48, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "确定",
                FRIDGE_FONT_BODY_SMALL, inventory_text_confirm_event, NULL, NULL);
    s_shell.inventory_ime_preedit = make_label(s_shell.inventory_input_dialog, "",
                                                FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                                16, 100, 640, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 6 && i < FRIDGE_IME_MAX_CANDIDATES; i++) {
        int col = (int)i;
        lv_obj_t *candidate = make_button(
                    s_shell.inventory_input_dialog, 12 + col * 98, 116, 92, 32,
                    FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                    FRIDGE_UI_COLOR_SURFACE,
                    "", FRIDGE_FONT_BODY_SMALL, inventory_ime_candidate_event,
                    (void *)(uintptr_t)i, &s_shell.inventory_ime_candidates[i]);
        lv_obj_set_style_bg_opa(candidate, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(candidate, 0, 0);
    }
    static const char *const rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (size_t row = 0; row < 4; row++) {
        size_t count = strlen(rows[row]);
        int width = row <= 1 ? 60 : row == 2 ? 62 : 64;
        int gap = row <= 1 ? 4 : row == 2 ? 5 : 6;
        int x = row <= 1 ? 18 : row == 2 ? 37 : 59;
        for (size_t i = 0; i < count; i++) {
            char letter[2] = {rows[row][i], '\0'};
            make_button(s_shell.inventory_input_dialog,
                        x + (int)i * (width + gap), 156 + (int)row * 42,
                        width, 38, FRIDGE_UI_COLOR_SURFACE,
                        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                        letter, FRIDGE_FONT_BODY_SMALL,
                        row == 0 ? inventory_ime_digit_event : inventory_ime_key_event,
                        (void *)(uintptr_t)rows[row][i], NULL);
        }
    }
    make_button(s_shell.inventory_input_dialog, 549, 282, 64, 38, FRIDGE_UI_COLOR_CREAM,
                 FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "退格", FRIDGE_FONT_BODY_SMALL,
                 inventory_ime_backspace_event, NULL, NULL);
    make_button(s_shell.inventory_input_dialog, 24, 332, 64, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "，",
                FRIDGE_FONT_BODY, inventory_ime_punctuation_event, (void *)"，", NULL);
    make_button(s_shell.inventory_input_dialog, 96, 332, 64, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "。",
                FRIDGE_FONT_BODY, inventory_ime_punctuation_event, (void *)"。", NULL);
    make_button(s_shell.inventory_input_dialog, 168, 332, 196, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "空格/首选",
                FRIDGE_FONT_BODY_SMALL, inventory_ime_space_event, NULL, NULL);
    make_button(s_shell.inventory_input_dialog, 372, 332, 72, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "上一页",
                FRIDGE_FONT_BODY_SMALL, inventory_ime_previous_page_event, NULL, NULL);
    make_button(s_shell.inventory_input_dialog, 452, 332, 72, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "下一页",
                FRIDGE_FONT_BODY_SMALL, inventory_ime_next_page_event, NULL, NULL);
    make_button(s_shell.inventory_input_dialog, 532, 332, 112, 48, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "完成",
                FRIDGE_FONT_BODY_SMALL, inventory_text_confirm_event, NULL, NULL);
    inventory_refresh_ime();
    lv_obj_move_foreground(s_shell.inventory_input_scrim);
    lv_obj_move_foreground(s_shell.inventory_input_dialog);
}

static void inventory_edit_name_event(lv_event_t *event)
{
    if (!scroll_click_is_suppressed(event)) inventory_open_text_input(INVENTORY_INPUT_NAME);
}

static void inventory_edit_note_event(lv_event_t *event)
{
    if (!scroll_click_is_suppressed(event)) inventory_open_text_input(INVENTORY_INPUT_NOTE);
}

static void inventory_search_input_event(lv_event_t *event)
{
    if (!scroll_click_is_suppressed(event)) inventory_open_text_input(INVENTORY_INPUT_SEARCH);
}

static void inventory_add_food_row(lv_obj_t *parent, int y, size_t index)
{
    const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[index];
    uint32_t tone = item->days_left < 0 ? FRIDGE_UI_COLOR_TOMATO :
                    item->days_left <= 3 ? FRIDGE_UI_COLOR_YOLK : FRIDGE_UI_COLOR_SAGE;
    lv_obj_t *row = make_button(parent, 0, y, 656, 64, FRIDGE_UI_COLOR_SURFACE,
                                FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, "",
                                FRIDGE_FONT_BODY, inventory_item_event,
                                (void *)(uintptr_t)index, NULL);
    register_icon(row, FRIDGE_UI_ICON_FOOD, tone, 14, 18, 24, "食");
    make_label(row, item->name, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               50, 8, 210, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(row, item->location[0] ? item->location : "位置待补充",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 50, 34, 280,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(row, item->quantity, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_SAGE_DARK,
               340, 10, 100, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char expiry[32];
    snprintf(expiry, sizeof(expiry), item->days_left < 0 ? "已过期 %d天" : "剩余 %d天", item->days_left < 0 ? -item->days_left : item->days_left);
    make_label(row, expiry, FRIDGE_FONT_BODY_SMALL, tone, 468, 10, 128,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(row, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED, 614, 20, 18, ">");
}

static size_t inventory_zone_stats(size_t zone_index, int *expiring, int *expired)
{
    if (expiring) *expiring = 0;
    if (expired) *expired = 0;
    if (zone_index >= s_shell.inventory.zone_count) return 0;
    const char *zone_id = s_shell.inventory.zones[zone_index].id;
    size_t count = 0;
    for (size_t i = 0; i < s_shell.inventory.item_count; i++) {
        const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[i];
        if (strcmp(item->zone_id, zone_id) != 0) continue;
        count++;
        if (item->days_left < 0) {
            if (expired) (*expired)++;
        } else if (item->days_left <= 3 && expiring) {
            (*expiring)++;
        }
    }
    return count;
}

static const char *inventory_zone_type(size_t index)
{
    static const char *types[] = {"冷冻", "冷藏", "冷藏", "门架", "抽屉"};
    return index < 5 ? types[index] : "分区";
}

static fridge_ui_icon_id_t inventory_zone_icon(size_t index)
{
    if (index == 0) return FRIDGE_UI_ICON_TEMP;
    if (index == 3) return FRIDGE_UI_ICON_DOOR;
    if (index == 4) return FRIDGE_UI_ICON_BOX;
    return FRIDGE_UI_ICON_FRIDGE;
}

static uint32_t inventory_tone_bg(int expiring, int expired)
{
    if (expired > 0) return 0xFBE1DD;
    if (expiring > 0) return 0xFFF0C9;
    return FRIDGE_UI_COLOR_MINT;
}

static void inventory_make_stat_chip(lv_obj_t *parent, int x, int width,
                                     const char *text, uint32_t bg, uint32_t fg)
{
    lv_obj_t *chip = make_box(parent, x, 8, width, 32, bg, bg, 16);
    lv_obj_t *label = make_label(chip, text, FRIDGE_FONT_BODY_SMALL, fg, 6, 5,
                                 width - 12, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(label, width - 12);
}

static void inventory_make_toolbar(lv_obj_t *page)
{
    int expiring = 0, expired = 0, unlocated = 0;
    size_t count = inventory_scope_count(&expiring, &expired, &unlocated);
    const fridge_ui_inventory_zone_t *zone =
        s_shell.inventory_scope_level >= 1 ? inventory_current_zone() : NULL;
    const fridge_ui_inventory_layer_t *layer =
        s_shell.inventory_scope_level >= 2 ? inventory_current_layer() : NULL;

    lv_obj_t *seg = make_box(page, 0, 0, 226, 48, FRIDGE_UI_COLOR_CREAM,
                             FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *space = make_button(seg, 4, 4, 110, 40,
                                  s_shell.inventory_list_view ? FRIDGE_UI_COLOR_CREAM :
                                                               FRIDGE_UI_COLOR_SURFACE,
                                  s_shell.inventory_list_view ? FRIDGE_UI_COLOR_MUTED :
                                                               FRIDGE_UI_COLOR_SAGE_DARK,
                                  s_shell.inventory_list_view ? FRIDGE_UI_COLOR_CREAM :
                                                               FRIDGE_UI_COLOR_LINE,
                                  "空间视图", FRIDGE_FONT_BODY_EMPHASIS,
                                  inventory_toggle_view_event, NULL, NULL);
    lv_obj_t *list = make_button(seg, 114, 4, 108, 40,
                                 s_shell.inventory_list_view ? FRIDGE_UI_COLOR_SURFACE :
                                                              FRIDGE_UI_COLOR_CREAM,
                                 s_shell.inventory_list_view ? FRIDGE_UI_COLOR_SAGE_DARK :
                                                              FRIDGE_UI_COLOR_MUTED,
                                 s_shell.inventory_list_view ? FRIDGE_UI_COLOR_LINE :
                                                              FRIDGE_UI_COLOR_CREAM,
                                 "列表视图", FRIDGE_FONT_BODY_EMPHASIS,
                                 inventory_toggle_view_event, NULL, NULL);
    lv_obj_set_style_shadow_width(space, 0, 0);
    lv_obj_set_style_shadow_width(list, 0, 0);

    lv_obj_t *scope = make_box(page, 238, 0, 418, 48,
                               inventory_tone_bg(expiring, expired),
                               FRIDGE_UI_COLOR_LINE_STRONG, 8);
    char crumb[96];
    snprintf(crumb, sizeof(crumb), "%s%s%s",
             zone ? zone->name : "冰箱", layer ? " · " : "", layer ? layer->name : "");
    lv_obj_t *crumb_label = make_label(
        scope, crumb, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
        14, 13, zone || layer ? 132 : 76, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    // 标题在统计卡预留区内水平居中；根层“冰箱”与右侧统计胶囊保持同一中线。
    lv_obj_set_style_text_align(crumb_label, LV_TEXT_ALIGN_CENTER, 0);
    int x = zone || layer ? 154 : 84;
    char value[24];
    snprintf(value, sizeof(value), "%u 项", (unsigned)count);
    inventory_make_stat_chip(scope, x, 66, value, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_INK);
    x += 70;
    if (expired) {
        snprintf(value, sizeof(value), "%d 过期", expired);
        inventory_make_stat_chip(scope, x, 72, value, 0xFBE1DD,
                                 FRIDGE_UI_COLOR_TOMATO);
        x += 76;
    }
    if (expiring) {
        snprintf(value, sizeof(value), "%d 临期", expiring);
        inventory_make_stat_chip(scope, x, 72, value, 0xFFF0C9, 0x765500);
        x += 76;
    }
    if (unlocated) {
        snprintf(value, sizeof(value), "未定位 %d", unlocated);
        inventory_make_stat_chip(scope, x, 86, value, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_INK);
    }
}

static void inventory_make_zone_card(lv_obj_t *parent, size_t index,
                                     int x, int y, int width, int height)
{
    if (index >= s_shell.inventory.zone_count) return;
    int expiring = 0, expired = 0;
    size_t count = inventory_zone_stats(index, &expiring, &expired);
    uint32_t bg = (index == 1 || index == 2) ? 0xFBE1DD :
                  index == 4 ? 0xFFF0C9 : FRIDGE_UI_COLOR_MINT;
    lv_obj_t *card = make_button(parent, x, y, width, height, bg,
                                 FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE_STRONG,
                                 "", FRIDGE_FONT_BODY, inventory_zone_event,
                                 (void *)(uintptr_t)index, NULL);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_t *stripe = make_box(card, 0, 0, 4, height,
                                (index == 1 || index == 2) ? FRIDGE_UI_COLOR_TOMATO :
                                index == 4 ? FRIDGE_UI_COLOR_YOLK : FRIDGE_UI_COLOR_SAGE,
                                (index == 1 || index == 2) ? FRIDGE_UI_COLOR_TOMATO :
                                index == 4 ? FRIDGE_UI_COLOR_YOLK : FRIDGE_UI_COLOR_SAGE, 0);
    lv_obj_clear_flag(stripe, LV_OBJ_FLAG_CLICKABLE);
    const bool tall = height >= 200;
    make_label(card, s_shell.inventory.zones[index].name, FRIDGE_FONT_BODY_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 14, tall ? 64 : 12, width - 118,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(card, inventory_zone_icon(index), FRIDGE_UI_COLOR_MUTED,
                  width - 104, tall ? 67 : 15, 18, "");
    make_label(card, inventory_zone_type(index), FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, width - 80, tall ? 66 : 14, 70,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char amount[24];
    snprintf(amount, sizeof(amount), "%u", (unsigned)count);
    const int amount_y = tall ? 100 : (height >= 150 ? 72 : 36);
    lv_obj_t *amount_label = make_label(
        card, amount, FRIDGE_FONT_TITLE_STRONG, FRIDGE_UI_COLOR_INK,
        14, amount_y, LV_SIZE_CONTENT, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_t *amount_unit = make_label(
        card, "项", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    // 使用对象实际尺寸对齐，单/双数字都让“项”的字框底边贴齐数量字框底边。
    lv_obj_update_layout(card);
    lv_obj_align_to(amount_unit, amount_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, 0);
    char note[48];
    if (expired > 0 && expiring > 0) {
        snprintf(note, sizeof(note), "%d 过期 · %d 临期", expired, expiring);
    } else if (expired > 0) {
        snprintf(note, sizeof(note), "%d 过期", expired);
    } else if (expiring > 0) {
        snprintf(note, sizeof(note), "%d 临期", expiring);
    } else {
        strlcpy(note, "保质充裕", sizeof(note));
    }
    make_label(card, note, FRIDGE_FONT_BODY_SMALL,
               expired ? FRIDGE_UI_COLOR_TOMATO :
               expiring ? 0x765500 : FRIDGE_UI_COLOR_MUTED,
               14, tall ? 146 : (height < 120 ? 72 : height - 34),
               tall ? width - 28 : width - 104,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(card, index == 4 ? "尽快吃" :
               (expired || expiring) ? "待处理" : "正常", FRIDGE_FONT_BODY_SMALL,
               expired ? FRIDGE_UI_COLOR_TOMATO :
               expiring ? 0x765500 : FRIDGE_UI_COLOR_SAGE_DARK,
               width - 92, tall ? height - 52 : height - 34, 76,
               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
}

static void create_inventory_page(lv_obj_t *page)
{
    const fridge_ui_inventory_zone_t *zone =
        s_shell.inventory_scope_level >= 1 ? inventory_current_zone() : NULL;
    const fridge_ui_inventory_layer_t *layer =
        s_shell.inventory_scope_level >= 2 ? inventory_current_layer() : NULL;
    inventory_make_toolbar(page);
    if (s_shell.inventory_list_view) {
        int y = 64;
        for (size_t i = 0; i < s_shell.inventory.item_count; i++) {
            const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[i];
            if (s_shell.inventory_scope_level >= 1 && (!zone || strcmp(item->zone_id, zone->id) != 0)) continue;
            if (s_shell.inventory_scope_level >= 2 && (!layer || strcmp(item->layer_id, layer->id) != 0)) continue;
            inventory_add_food_row(page, y, i);
            y += 72;
        }
        if (y == 64) make_label(page, "此范围暂无食材", FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED,
                                  0, y + 36, 656, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        return;
    }
    if (s_shell.inventory_scope_level == 0) {
        lv_obj_t *fridge = make_box(page, 0, 64, 656, 417, FRIDGE_UI_COLOR_SURFACE,
                                    FRIDGE_UI_COLOR_LINE_STRONG, 8);
        // 用户明确移除分区总览标题行；五张空间卡上移并填满原有 417px 内容区域。
        inventory_make_zone_card(fridge, 0, 12, 12, 632, 100);
        inventory_make_zone_card(fridge, 1, 12, 120, 206, 285);
        inventory_make_zone_card(fridge, 2, 226, 120, 418, 100);
        inventory_make_zone_card(fridge, 3, 226, 228, 202, 177);
        inventory_make_zone_card(fridge, 4, 436, 228, 208, 177);
    } else if (s_shell.inventory_scope_level == 1 && zone) {
        lv_obj_t *fridge = make_box(page, 0, 64, 656, 417, FRIDGE_UI_COLOR_SURFACE,
                                    FRIDGE_UI_COLOR_LINE_STRONG, 8);
        char title[64];
        size_t layer_count = 0;
        for (size_t i = 0; i < s_shell.inventory.layer_count; i++) {
            if (strcmp(s_shell.inventory.layers[i].zone_id, zone->id) == 0) layer_count++;
        }
        snprintf(title, sizeof(title), "%s · %u 层", zone->name, (unsigned)layer_count);
        make_label(fridge, title, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
                   14, 12, 310, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(fridge, "选择一层查看固定 3×3 格位", FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 338, 14, 298, LV_SIZE_CONTENT,
                   LV_LABEL_LONG_DOT);
        size_t seen = 0;
        for (size_t i = 0; i < s_shell.inventory.layer_count; i++) {
            const fridge_ui_inventory_layer_t *l = &s_shell.inventory.layers[i];
            if (strcmp(l->zone_id, zone->id) != 0) continue;
            int y = 48 + (int)seen * 96;
            int expiring = 0, expired = 0, layer_items = 0;
            for (size_t k = 0; k < s_shell.inventory.item_count; k++) {
                const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[k];
                if (strcmp(item->layer_id, l->id) != 0) continue;
                layer_items++;
                if (item->days_left < 0) expired++;
                else if (item->days_left <= 3) expiring++;
            }
            lv_obj_t *card = make_button(fridge, 12, y, 632, 88,
                                         inventory_tone_bg(expiring, expired),
                                         FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, "",
                                         FRIDGE_FONT_BODY, inventory_layer_event,
                                         (void *)(uintptr_t)seen, NULL);
            char ordinal[8]; snprintf(ordinal, sizeof(ordinal), "%u", (unsigned)(seen + 1U));
            lv_obj_t *ord = make_box(card, 14, 20, 44, 44, FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_LINE_STRONG, 8);
            lv_obj_t *ord_label = make_label(ord, ordinal, FRIDGE_FONT_BODY_EMPHASIS,
                                             FRIDGE_UI_COLOR_SAGE_DARK, 0, 8, 44,
                                             LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
            center_single_line_label(ord_label, 44);
            make_label(card, l->name, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                       74, 14, 300, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            char meta[80];
            snprintf(meta, sizeof(meta), "%d 项 · %d 个空格 · 固定九宫格",
                     layer_items, 9 - layer_items);
            make_label(card, meta, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                       74, 46, 440, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED, 596, 32, 20, ">");
            seen++;
        }
    } else if (zone && layer) {
        lv_obj_t *board = make_box(page, 0, 64, 656, 417, FRIDGE_UI_COLOR_SURFACE,
                                   FRIDGE_UI_COLOR_LINE_STRONG, 8);
        lv_obj_set_style_pad_all(board, 0, 0);
        for (uint8_t cell = 0; cell < FRIDGE_UI_INVENTORY_GRID_CELLS; cell++) {
            int col = cell % 3U, row = cell / 3U;
            int item_index = inventory_item_at_cell(cell);
            uint32_t bg = item_index < 0 ? FRIDGE_UI_COLOR_SURFACE : FRIDGE_UI_COLOR_MINT;
            int cell_w = col == 2 ? 218 : 219;
            int cell_h = row == 2 ? 139 : 138;
            lv_obj_t *grid = make_button(board, col * 219, row * 138, cell_w, cell_h, bg,
                                         FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, "",
                                         FRIDGE_FONT_BODY, inventory_cell_event,
                                         (void *)(uintptr_t)cell, NULL);
            lv_obj_set_style_radius(grid, 0, 0);
            lv_obj_set_style_shadow_width(grid, 0, 0);
            char cell_name[8]; snprintf(cell_name, sizeof(cell_name), "%c%u", 'A' + row, (unsigned)(col + 1));
            make_label(grid, cell_name, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                       12, 10, 46, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
            if (item_index >= 0) {
                const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[item_index];
                make_label(grid, item->name, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                           12, 46, 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
                make_label(grid, item->quantity, FRIDGE_FONT_BODY_SMALL,
                           FRIDGE_UI_COLOR_SAGE_DARK, 138, 46, 68,
                           LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
                char expiry[24];
                snprintf(expiry, sizeof(expiry), item->days_left < 0 ? "已过期 %d 天" :
                         "剩余 %d 天", item->days_left < 0 ? -item->days_left : item->days_left);
                lv_obj_t *pill = make_box(grid, 12, cell_h - 42, cell_w - 24, 30,
                                          item->days_left < 0 ? 0xFBE1DD :
                                          item->days_left <= 3 ? 0xFFF0C9 :
                                          FRIDGE_UI_COLOR_MINT,
                                          item->days_left < 0 ? 0xFBE1DD :
                                          item->days_left <= 3 ? 0xFFF0C9 :
                                          FRIDGE_UI_COLOR_MINT, 15);
                lv_obj_t *expiry_label = make_label(pill, expiry, FRIDGE_FONT_BODY_SMALL,
                                                    item->days_left < 0 ? FRIDGE_UI_COLOR_TOMATO :
                                                    item->days_left <= 3 ? 0x765500 :
                                                    FRIDGE_UI_COLOR_SAGE_DARK,
                                                    0, 4, cell_w - 24, LV_SIZE_CONTENT,
                                                    LV_LABEL_LONG_CLIP);
                center_single_line_label(expiry_label, cell_w - 24);
            } else {
                make_label(grid, "空格", FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED,
                           12, 52, 170, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
                lv_obj_t *pill = make_box(grid, 12, cell_h - 42, cell_w - 24, 30,
                                          FRIDGE_UI_COLOR_OFFLINE_BG,
                                          FRIDGE_UI_COLOR_OFFLINE_BG, 15);
                lv_obj_t *label = make_label(pill, "点击登记", FRIDGE_FONT_BODY_SMALL,
                                             FRIDGE_UI_COLOR_MUTED, 0, 4, cell_w - 24,
                                             LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
                center_single_line_label(label, cell_w - 24);
            }
        }
    }
}

static void create_inventory_detail_page(lv_obj_t *page)
{
    if (s_shell.inventory_selected_item >= s_shell.inventory.item_count) return;
    const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[s_shell.inventory_selected_item];
    lv_obj_t *hero = make_box(page, 0, 0, 656, 116,
                              item->days_left < 0 ? 0xFBE1DD :
                              item->days_left <= 3 ? 0xFFF0C9 : FRIDGE_UI_COLOR_MINT,
                              FRIDGE_UI_COLOR_LINE_STRONG, 8);
    register_icon(hero, FRIDGE_UI_ICON_FOOD, FRIDGE_UI_COLOR_SAGE_DARK, 18, 18, 32, "食");
    make_label(hero, item->name, FRIDGE_FONT_TITLE_STRONG, FRIDGE_UI_COLOR_INK,
               66, 14, 360, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(hero, item->quantity, FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_SAGE_DARK,
               66, 58, 230, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char left[32];
    snprintf(left, sizeof(left), item->days_left < 0 ? "已过期 %d 天" : "剩余 %d 天",
             item->days_left < 0 ? -item->days_left : item->days_left);
    lv_obj_t *left_box = make_box(hero, 458, 22, 178, 72,
                                  item->days_left < 0 ? 0xFBE1DD :
                                  item->days_left <= 3 ? 0xFFF0C9 : FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
    make_label(left_box, "保质状态", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               12, 8, 154, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(left_box, left, FRIDGE_FONT_BODY_EMPHASIS,
               item->days_left < 0 ? FRIDGE_UI_COLOR_TOMATO :
               item->days_left <= 3 ? 0x765500 : FRIDGE_UI_COLOR_SAGE_DARK,
               12, 36, 154, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    lv_obj_t *facts = make_box(page, 0, 128, 656, 204, FRIDGE_UI_COLOR_SURFACE,
                               FRIDGE_UI_COLOR_LINE, 8);
    const char *keys[] = {"到期日期", "存放位置", "登记来源", "备注"};
    const char *values[] = {item->expire_date[0] ? item->expire_date : "未填写",
                            item->location[0] ? item->location : "位置待补充",
                            "本机库存",
                            item->note[0] ? item->note : "—"};
    const fridge_ui_icon_id_t icons[] = {FRIDGE_UI_ICON_CLOCK, FRIDGE_UI_ICON_LOCATION,
                                         FRIDGE_UI_ICON_SHIELD, FRIDGE_UI_ICON_EDIT};
    for (size_t i = 0; i < 4; i++) {
        int y = 10 + (int)i * 48;
        register_icon(facts, icons[i], FRIDGE_UI_COLOR_MUTED, 14, y + 4, 20, "");
        make_label(facts, keys[i], FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                   46, y + 4, 112, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(facts, values[i], FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK,
                   166, y, 470, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        if (i < 3) add_divider(facts, y + 40);
    }
    lv_obj_t *tip = make_box(page, 0, 344, 656, 64, FRIDGE_UI_COLOR_CREAM,
                             FRIDGE_UI_COLOR_CREAM, 8);
    register_icon(tip, FRIDGE_UI_ICON_INFO, FRIDGE_UI_COLOR_SAGE, 14, 20, 22, "i");
    make_label(tip, item->days_left <= 3 ? "建议优先安排食用；处理后请及时更新库存。" :
               "保质期充裕，继续按当前位置保存。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 48, 20, 590,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    make_button(page, 0, 424, 112, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "编辑", FRIDGE_FONT_BODY,
                inventory_open_edit_event, NULL, NULL);
    make_button(page, 120, 424, 112, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "移动",
                FRIDGE_FONT_BODY, inventory_move_event, NULL, NULL);
    make_button(page, 240, 424, 184, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "加入购物清单",
                FRIDGE_FONT_BODY_SMALL, unsupported_event, "已加入购物清单", NULL);
    make_button(page, 528, 424, 128, 48, 0xFBE1DD, FRIDGE_UI_COLOR_TOMATO,
                FRIDGE_UI_COLOR_TOMATO, "删除", FRIDGE_FONT_BODY,
                inventory_delete_event, NULL, NULL);
}

static void create_inventory_edit_page(lv_obj_t *page)
{
    const fridge_ui_inventory_item_t *item = &s_shell.inventory_draft;
    const char *categories[] = {"蔬菜", "水果", "肉类", "蛋奶", "主食", "其他"};
    make_label(page, "名称", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 16, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *name = make_button(page, 88, 0, 568, 52, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, "",
                                 FRIDGE_FONT_BODY, inventory_edit_name_event, NULL, NULL);
    register_icon(name, FRIDGE_UI_ICON_KEYBOARD, FRIDGE_UI_COLOR_SAGE_DARK,
                  528, 14, 22, "");
    make_label(name, item->name[0] ? item->name : "点击输入食材名称",
               FRIDGE_FONT_BODY, item->name[0] ? FRIDGE_UI_COLOR_INK :
                                                FRIDGE_UI_COLOR_MUTED,
               16, 14, 492, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    make_label(page, "分类", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 78, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 6; i++) {
        make_button(page, 88 + (int)i * 94, 62, 86, 40,
                    i == s_shell.inventory_category_index ? FRIDGE_UI_COLOR_MINT :
                                                            FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK,
                    i == s_shell.inventory_category_index ? FRIDGE_UI_COLOR_SAGE :
                                                            FRIDGE_UI_COLOR_LINE,
                    categories[i], FRIDGE_FONT_BODY_SMALL, inventory_category_event,
                    (void *)(uintptr_t)i, NULL);
    }

    make_label(page, "数量", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 126, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(page, 88, 112, 48, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "−",
                FRIDGE_FONT_TITLE, inventory_quantity_event, (void *)(intptr_t)-1, NULL);
    lv_obj_t *quantity = make_box(page, 144, 112, 160, 48, FRIDGE_UI_COLOR_SURFACE,
                                  FRIDGE_UI_COLOR_LINE, 8);
    lv_obj_t *quantity_label = make_label(quantity,
                                          item->quantity[0] ? item->quantity : "1份",
                                          FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                                          0, 12, 160, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(quantity_label, 160);
    make_button(page, 312, 112, 48, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "+",
                FRIDGE_FONT_TITLE, inventory_quantity_event, (void *)(intptr_t)1, NULL);
    make_label(page, "来源  手动登记", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               392, 126, 240, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    make_label(page, "到期", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 184, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    const int expiry_days[] = {1, 3, 7, 30};
    const char *expiry_labels[] = {"明天", "3天", "7天", "30天"};
    for (size_t i = 0; i < 4; i++) {
        make_button(page, 88 + (int)i * 92, 170, 84, 40,
                    item->days_left == expiry_days[i] ? FRIDGE_UI_COLOR_MINT :
                                                       FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK,
                    item->days_left == expiry_days[i] ? FRIDGE_UI_COLOR_SAGE :
                                                       FRIDGE_UI_COLOR_LINE,
                    expiry_labels[i], FRIDGE_FONT_BODY_SMALL, inventory_expiry_event,
                    (void *)(intptr_t)expiry_days[i], NULL);
    }
    make_label(page, item->expire_date[0] ? item->expire_date : "待填写",
               FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK, 472, 180, 168,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    make_label(page, "位置", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 236, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *location = make_button(page, 88, 222, 568, 48,
                                     item->location[0] ? FRIDGE_UI_COLOR_MINT :
                                                        FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_SAGE_DARK,
                                     item->location[0] ? FRIDGE_UI_COLOR_SAGE :
                                                        FRIDGE_UI_COLOR_LINE,
                                     "", FRIDGE_FONT_BODY, inventory_pick_location_event,
                                     NULL, NULL);
    register_icon(location, FRIDGE_UI_ICON_LOCATION, FRIDGE_UI_COLOR_SAGE_DARK,
                  14, 13, 22, "");
    make_label(location, item->location[0] ? item->location : "选择位置",
               FRIDGE_FONT_BODY, item->location[0] ? FRIDGE_UI_COLOR_SAGE_DARK :
                                                    FRIDGE_UI_COLOR_MUTED,
               48, 13, 470, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(location, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED, 532, 14, 20, ">");

    make_label(page, "备注", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 294, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *note = make_button(page, 88, 280, 568, 56, FRIDGE_UI_COLOR_SURFACE,
                                 FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, "",
                                 FRIDGE_FONT_BODY, inventory_edit_note_event, NULL, NULL);
    make_label(note, item->note[0] ? item->note : "可选；点击使用同款键盘输入",
               FRIDGE_FONT_BODY_SMALL, item->note[0] ? FRIDGE_UI_COLOR_INK :
                                                      FRIDGE_UI_COLOR_MUTED,
               16, 17, 500, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    register_icon(note, FRIDGE_UI_ICON_KEYBOARD, FRIDGE_UI_COLOR_SAGE_DARK,
                  528, 17, 20, "");

    make_button(page, 0, 424, 150, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "保存", FRIDGE_FONT_BODY,
                inventory_save_event, NULL, NULL);
    make_button(page, 158, 424, 150, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "语音录入",
                FRIDGE_FONT_BODY, voice_event, NULL, NULL);
    make_button(page, 316, 424, 166, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "自动补全",
                FRIDGE_FONT_BODY, inventory_autofill_event, NULL, NULL);
    make_button(page, 506, 424, 150, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE, "取消",
                FRIDGE_FONT_BODY, inventory_back_event, NULL, NULL);
}

static void create_inventory_recognize_page(lv_obj_t *page)
{
    lv_obj_t *rule = make_box(page, 0, 0, 656, 48, 0xFFF0C9,
                              FRIDGE_UI_COLOR_YOLK, 8);
    register_icon(rule, FRIDGE_UI_ICON_WARN, 0x765500, 14, 13, 22, "!");
    make_label(rule, "识别结果不会自动入库，请逐项核对并勾选。",
               FRIDGE_FONT_BODY_SMALL, 0x765500, 48, 14, 590,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    static const char *names[] = {"西红柿", "原味酸奶", "某绿叶菜"};
    static const char *qtys[] = {"3 个 · 置信度 93%", "2 杯 · 置信度 87%",
                                 "1 把 · 置信度 61%"};
    static const char *locations[] = {"建议位置：果蔬抽屉 / 抽屉",
                                      "建议合并：右冷藏 / 第2层 / B3",
                                      "位置：稍后补充"};
    static const char *warnings[] = {"识别清晰",
                                     "日期为估算 · 存在同名批次",
                                     "置信度较低，请确认名称 · 日期为估算"};
    for (size_t i = 0; i < 3; i++) {
        int y = 60 + (int)i * 116;
        lv_obj_t *card = make_button(page, 0, y, 656, 104,
                                     s_shell.inventory_recognize_keep[i] ?
                                         FRIDGE_UI_COLOR_SURFACE : FRIDGE_UI_COLOR_OFFLINE_BG,
                                     FRIDGE_UI_COLOR_INK,
                                     s_shell.inventory_recognize_keep[i] ?
                                         FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE,
                                     "", FRIDGE_FONT_BODY, inventory_recognize_toggle_event,
                                     (void *)(uintptr_t)i, NULL);
        lv_obj_t *check = make_box(card, 14, 14, 28, 28,
                                   s_shell.inventory_recognize_keep[i] ?
                                       FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_SURFACE,
                                   s_shell.inventory_recognize_keep[i] ?
                                       FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE_STRONG, 6);
        if (s_shell.inventory_recognize_keep[i]) {
            register_icon(check, FRIDGE_UI_ICON_CHECK, 0xFFFFFF, 4, 4, 18, "√");
        }
        make_label(card, names[i], FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
                   58, 12, 210, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(card, qtys[i], FRIDGE_FONT_BODY_SMALL,
                   i == 2 ? 0x765500 : FRIDGE_UI_COLOR_SAGE_DARK,
                   282, 14, 334, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        register_icon(card, FRIDGE_UI_ICON_LOCATION, FRIDGE_UI_COLOR_SAGE,
                      58, 48, 18, "");
        make_label(card, locations[i], FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                   84, 48, 532, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_label(card, warnings[i], FRIDGE_FONT_BODY_SMALL,
                   i == 0 ? FRIDGE_UI_COLOR_SAGE_DARK : 0x765500,
                   58, 76, 558, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    }
    make_button(page, 0, 424, 190, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "确认并同步", FRIDGE_FONT_BODY,
                inventory_recognize_confirm_event, NULL, NULL);
    make_button(page, 198, 424, 180, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "存为待确认",
                FRIDGE_FONT_BODY, inventory_back_event, NULL, NULL);
    make_button(page, 506, 424, 150, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE, "取消",
                FRIDGE_FONT_BODY, inventory_back_event, NULL, NULL);
}

static void create_inventory_search_page(lv_obj_t *page)
{
    lv_obj_t *search = make_button(page, 0, 0, 548, 48, FRIDGE_UI_COLOR_SURFACE,
                                   FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE, "",
                                   FRIDGE_FONT_BODY, inventory_search_input_event, NULL, NULL);
    register_icon(search, FRIDGE_UI_ICON_SEARCH, FRIDGE_UI_COLOR_SAGE_DARK, 14, 12, 24, "?");
    make_label(search, s_shell.inventory_search_text[0] ? s_shell.inventory_search_text : "搜索食材名称（使用聊天页同款键盘）",
               FRIDGE_FONT_BODY, s_shell.inventory_search_text[0] ? FRIDGE_UI_COLOR_INK : FRIDGE_UI_COLOR_MUTED,
               48, 13, 476, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(page, 560, 0, 96, 48, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                FRIDGE_UI_COLOR_SAGE, "搜索", FRIDGE_FONT_BODY,
                inventory_search_input_event, NULL, NULL);
    const char *filters[] = {"全部", "临期", "过期", "未定位"};
    make_label(page, "筛选", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 72, 60, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 4; i++) make_button(page, 68 + (int)i * 106, 60, 98, 40,
        i == s_shell.inventory_search_filter ? FRIDGE_UI_COLOR_MINT : FRIDGE_UI_COLOR_SURFACE,
        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, filters[i], FRIDGE_FONT_BODY_SMALL,
        inventory_search_filter_event, (void *)(uintptr_t)i, NULL);
    const char *sorts[] = {"默认顺序", "最快到期"};
    make_label(page, "排序", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 124, 60, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 2; i++) make_button(page, 68 + (int)i * 132, 112, 124, 40,
        i == s_shell.inventory_search_sort ? FRIDGE_UI_COLOR_MINT : FRIDGE_UI_COLOR_SURFACE,
        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, sorts[i], FRIDGE_FONT_BODY_SMALL,
        inventory_search_sort_event, (void *)(uintptr_t)i, NULL);

    size_t indices[FRIDGE_UI_INVENTORY_ITEM_MAX] = {0};
    size_t result_count = 0;
    if (fridge_ui_inventory_model_search(&s_shell.inventory,
                                         s_shell.inventory_search_text,
                                         indices, FRIDGE_UI_INVENTORY_ITEM_MAX,
                                         &result_count) != ESP_OK) {
        result_count = 0;
    }
    size_t filtered[FRIDGE_UI_INVENTORY_ITEM_MAX] = {0};
    size_t filtered_count = 0;
    for (size_t i = 0; i < result_count; i++) {
        const fridge_ui_inventory_item_t *item = &s_shell.inventory.items[indices[i]];
        if (s_shell.inventory_search_filter == 1 && (item->days_left < 0 || item->days_left > 3)) continue;
        if (s_shell.inventory_search_filter == 2 && item->days_left >= 0) continue;
        if (s_shell.inventory_search_filter == 3 && item->zone_id[0]) continue;
        filtered[filtered_count++] = indices[i];
    }
    if (s_shell.inventory_search_sort == 1) {
        for (size_t i = 1; i < filtered_count; i++) {
            size_t value = filtered[i];
            size_t j = i;
            while (j > 0 &&
                   s_shell.inventory.items[filtered[j - 1]].days_left >
                       s_shell.inventory.items[value].days_left) {
                filtered[j] = filtered[j - 1];
                j--;
            }
            filtered[j] = value;
        }
    }
    char result_title[40];
    snprintf(result_title, sizeof(result_title), "搜索结果 · %u 项",
             (unsigned)filtered_count);
    make_label(page, result_title, FRIDGE_FONT_BODY_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 170, 300,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    int y = 204;
    for (size_t i = 0; i < filtered_count; i++) {
        inventory_add_food_row(page, y, filtered[i]);
        y += 72;
    }
    if (filtered_count == 0) {
        lv_obj_t *empty = make_box(page, 0, 204, 656, 96,
                                   FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 8);
        make_label(empty, "没有符合条件的食材", FRIDGE_FONT_BODY,
                   FRIDGE_UI_COLOR_MUTED, 16, 34, 624,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    }
    int footer_y = y + 12;
    make_button(page, 0, footer_y, 170, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "批量处理",
                FRIDGE_FONT_BODY, unsupported_event, "请先选择食材", NULL);
    make_button(page, 486, footer_y, 170, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE, "清除筛选",
                FRIDGE_FONT_BODY, inventory_search_clear_event, NULL, NULL);
}
static void create_inventory_layout_page(lv_obj_t *page)
{
    const fridge_ui_inventory_zone_t *zone = inventory_current_zone();
    const fridge_ui_inventory_layer_t *layer = inventory_current_layer();
    lv_obj_t *hint = make_box(page, 0, 0, 656, 44, 0xE9F1F7, 0xE9F1F7, 8);
    register_icon(hint, FRIDGE_UI_ICON_INFO, 0x35647A, 12, 11, 20, "i");
    make_label(hint,
               s_shell.inventory_layout_level == 0 ?
                   "选择一个分区，进入后可查看和调整层结构。" :
               s_shell.inventory_layout_level == 1 ?
                   "当前只修改所选分区的层结构，食材位置不会被静默迁移。" :
                   "点格位可启用或停用；提交后写入本机布局草稿。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 44, 12, 598,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char section[128];
    if (s_shell.inventory_layout_level == 0) {
        snprintf(section, sizeof(section), "分区形状 · %u 个分区",
                 (unsigned)s_shell.inventory.zone_count);
    } else if (s_shell.inventory_layout_level == 1) {
        snprintf(section, sizeof(section), "%s · 层结构",
                 zone ? zone->name : "分区");
    } else {
        snprintf(section, sizeof(section), "%s · %s · 固定格位",
                 zone ? zone->name : "分区", layer ? layer->name : "层");
    }
    make_label(page, section, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, 58, 430, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (s_shell.inventory_layout_dirty) {
        lv_obj_t *draft = make_box(page, 530, 52, 126, 32, 0xFFF0C9, 0xFFF0C9, 16);
        lv_obj_t *label = make_label(draft, "未提交草稿", FRIDGE_FONT_BODY_SMALL,
                                     0x765500, 0, 5, 126, LV_SIZE_CONTENT,
                                     LV_LABEL_LONG_CLIP);
        center_single_line_label(label, 126);
    }

    if (s_shell.inventory_layout_level == 0) {
        lv_obj_t *canvas = make_box(page, 0, 88, 656, 320,
                                    FRIDGE_UI_COLOR_SURFACE,
                                    FRIDGE_UI_COLOR_LINE_STRONG, 8);
        inventory_make_zone_card(canvas, 0, 8, 8, 640, 78);
        inventory_make_zone_card(canvas, 1, 8, 94, 208, 218);
        inventory_make_zone_card(canvas, 2, 224, 94, 424, 78);
        inventory_make_zone_card(canvas, 3, 224, 180, 204, 132);
        inventory_make_zone_card(canvas, 4, 436, 180, 212, 132);
        // 覆盖卡片的浏览回调，使空间编辑保持在 INV-08 内。
        for (size_t i = 0; i < lv_obj_get_child_count(canvas); i++) {
            lv_obj_t *child = lv_obj_get_child(canvas, i);
            if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
                lv_obj_remove_event_cb(child, inventory_zone_event);
            }
        }
        // 单独放置透明点击热区，几何与五个分区完全一致。
        const int rects[5][4] = {{8,8,640,78},{8,94,208,218},{224,94,424,78},
                                 {224,180,204,132},{436,180,212,132}};
        for (size_t i = 0; i < 5 && i < s_shell.inventory.zone_count; i++) {
            lv_obj_t *hit = lv_obj_create(canvas);
            clear_container_style(hit);
            lv_obj_set_pos(hit, rects[i][0], rects[i][1]);
            lv_obj_set_size(hit, rects[i][2], rects[i][3]);
            lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(hit, inventory_layout_zone_event, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)i);
        }
    } else if (s_shell.inventory_layout_level == 1 && zone) {
        size_t seen = 0;
        for (size_t i = 0; i < s_shell.inventory.layer_count; i++) {
            const fridge_ui_inventory_layer_t *candidate = &s_shell.inventory.layers[i];
            if (strcmp(candidate->zone_id, zone->id) != 0) continue;
            int y = 88 + (int)seen * 76;
            lv_obj_t *card = make_button(page, 0, y, 656, 68,
                                         FRIDGE_UI_COLOR_SURFACE,
                                         FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_LINE,
                                         "", FRIDGE_FONT_BODY,
                                         inventory_layout_layer_event,
                                         (void *)(uintptr_t)seen, NULL);
            char ordinal[16];
            snprintf(ordinal, sizeof(ordinal), "%u", (unsigned)(seen + 1U));
            make_label(card, ordinal, FRIDGE_FONT_BODY_EMPHASIS,
                       FRIDGE_UI_COLOR_SAGE_DARK, 18, 18, 36,
                       LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
            make_label(card, candidate->name, FRIDGE_FONT_BODY_EMPHASIS,
                       FRIDGE_UI_COLOR_INK, 66, 10, 300,
                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            make_label(card, "固定 3×3 · 点击进入设置", FRIDGE_FONT_BODY_SMALL,
                       FRIDGE_UI_COLOR_MUTED, 66, 38, 360,
                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            register_icon(card, FRIDGE_UI_ICON_FWD, FRIDGE_UI_COLOR_MUTED,
                          616, 24, 20, ">");
            seen++;
        }
    } else if (zone && layer) {
        lv_obj_t *board = make_box(page, 0, 88, 656, 320,
                                   FRIDGE_UI_COLOR_SURFACE,
                                   FRIDGE_UI_COLOR_LINE_STRONG, 8);
        for (size_t cell = 0; cell < FRIDGE_UI_INVENTORY_GRID_CELLS; cell++) {
            int col = (int)(cell % 3U), row = (int)(cell / 3U);
            bool enabled = s_shell.inventory_layout_draft.cell_labels[cell][0];
            lv_obj_t *grid = make_button(board, 8 + col * 214, 8 + row * 102,
                                         206, 94,
                                         enabled ? FRIDGE_UI_COLOR_MINT :
                                                   FRIDGE_UI_COLOR_OFFLINE_BG,
                                         FRIDGE_UI_COLOR_SAGE_DARK,
                                         enabled ? FRIDGE_UI_COLOR_SAGE :
                                                   FRIDGE_UI_COLOR_LINE,
                                         "", FRIDGE_FONT_BODY,
                                         inventory_layout_cell_event,
                                         (void *)(uintptr_t)cell, NULL);
            char code[8];
            snprintf(code, sizeof(code), "%c%u", 'A' + row, (unsigned)(col + 1));
            make_label(grid, code, FRIDGE_FONT_BODY_EMPHASIS,
                       enabled ? FRIDGE_UI_COLOR_SAGE_DARK : FRIDGE_UI_COLOR_MUTED,
                       14, 18, 64, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
            make_label(grid, enabled ? "已启用" : "已停用",
                       FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                       14, 52, 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        }
    }

    make_button(page, 0, 424, 160, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE,
                s_shell.inventory_layout_level == 1 ? "新增一层" : "撤销",
                FRIDGE_FONT_BODY,
                s_shell.inventory_layout_level == 1 ? inventory_add_layer_event :
                                                      inventory_layout_undo_event,
                NULL, NULL);
    make_button(page, 496, 424, 160, 48,
                s_shell.inventory_layout_level == 2 ? FRIDGE_UI_COLOR_SAGE :
                                                      FRIDGE_UI_COLOR_OFFLINE_BG,
                s_shell.inventory_layout_level == 2 ? 0xFFFFFF :
                                                      FRIDGE_UI_COLOR_MUTED,
                s_shell.inventory_layout_level == 2 ? FRIDGE_UI_COLOR_SAGE :
                                                      FRIDGE_UI_COLOR_LINE,
                "提交布局", FRIDGE_FONT_BODY, inventory_layout_submit_event, NULL, NULL);
}

static void clear_page_references(void)
{
    s_shell.page_content = NULL;
    s_shell.timer_hero = NULL;
    s_shell.timer_label = NULL;
    s_shell.timer_chip = NULL;
    s_shell.timer_face = NULL;
    s_shell.timer_state = NULL;
    s_shell.timer_ops = NULL;
    s_shell.timer_start = NULL;
    s_shell.timer_pause = NULL;
    s_shell.timer_plus = NULL;
    s_shell.timer_cancel = NULL;
    s_shell.timer_start_label = NULL;
    s_shell.timer_pause_label = NULL;
    s_shell.timer_dismiss = NULL;
    s_shell.timer_other_title = NULL;
    s_shell.timer_done_title = NULL;
    s_shell.timer_edit_hour_label = NULL;
    s_shell.timer_edit_min_label = NULL;
    s_shell.timer_edit_name_label = NULL;
    memset(s_shell.timer_other_rows, 0, sizeof(s_shell.timer_other_rows));
    memset(s_shell.timer_other_names, 0, sizeof(s_shell.timer_other_names));
    memset(s_shell.timer_other_remains, 0, sizeof(s_shell.timer_other_remains));
    memset(s_shell.timer_other_meta, 0, sizeof(s_shell.timer_other_meta));
    memset(s_shell.timer_other_pause, 0, sizeof(s_shell.timer_other_pause));
    memset(s_shell.timer_other_pause_labels, 0, sizeof(s_shell.timer_other_pause_labels));
    memset(s_shell.timer_other_plus, 0, sizeof(s_shell.timer_other_plus));
    memset(s_shell.timer_other_cancel, 0, sizeof(s_shell.timer_other_cancel));
    memset(s_shell.timer_other_ids, 0, sizeof(s_shell.timer_other_ids));
    memset(s_shell.timer_done_rows, 0, sizeof(s_shell.timer_done_rows));
    memset(s_shell.timer_done_names, 0, sizeof(s_shell.timer_done_names));
    memset(s_shell.timer_done_meta, 0, sizeof(s_shell.timer_done_meta));
    memset(s_shell.more_badge_boxes, 0, sizeof(s_shell.more_badge_boxes));
    memset(s_shell.more_badge_labels, 0, sizeof(s_shell.more_badge_labels));
    s_shell.more_device_dot = NULL;
    s_shell.more_device = NULL;
    s_shell.more_wifi = NULL;
    s_shell.more_tf = NULL;
    s_shell.alarm_status = NULL;
    s_shell.alarm_empty = NULL;
    // 绑定页与分区模板同样使用 page_content 承载；切页异步删除后必须解除引用，
    // 否则绑定状态回调或分区刷新会写入已释放的 LVGL 对象。
    s_shell.binding_qr = NULL;
    s_shell.binding_qr_placeholder = NULL;
    s_shell.binding_status = NULL;
    s_shell.binding_detail = NULL;
    s_shell.binding_expiry = NULL;
    s_shell.split_content = NULL;
    s_shell.split_content_icon_slot_start = 0;
    memset(s_shell.split_tabs, 0, sizeof(s_shell.split_tabs));
    memset(s_shell.split_tab_labels, 0, sizeof(s_shell.split_tab_labels));
    s_shell.settings_wifi_summary = NULL;
    s_shell.settings_brightness_value = NULL;
    s_shell.settings_brightness_slider = NULL;
    s_shell.settings_wake_switch = NULL;
    s_shell.settings_wake_status = NULL;
    s_shell.stopwatch_face = NULL;
    s_shell.stopwatch_face_region = NULL;
    s_shell.stopwatch_face_card = NULL;
    s_shell.stopwatch_state = NULL;
    s_shell.stopwatch_reset = NULL;
    s_shell.stopwatch_toggle = NULL;
    s_shell.stopwatch_toggle_label = NULL;
    s_shell.stopwatch_lap = NULL;
    s_shell.stopwatch_toggle_play_image = NULL;
    s_shell.stopwatch_toggle_play_fallback = NULL;
    s_shell.stopwatch_toggle_pause_image = NULL;
    s_shell.stopwatch_toggle_pause_fallback = NULL;
    s_shell.stopwatch_laps_count = NULL;
    s_shell.stopwatch_laps_empty = NULL;
    s_shell.stopwatch_laps_empty_label = NULL;
    s_shell.stopwatch_laps_list = NULL;
    memset(s_shell.stopwatch_lap_rows, 0, sizeof(s_shell.stopwatch_lap_rows));
    memset(s_shell.stopwatch_lap_labels, 0, sizeof(s_shell.stopwatch_lap_labels));
    memset(s_shell.stopwatch_lap_times, 0, sizeof(s_shell.stopwatch_lap_times));
    memset(s_shell.alarm_cards, 0, sizeof(s_shell.alarm_cards));
    memset(s_shell.alarm_card_main, 0, sizeof(s_shell.alarm_card_main));
    memset(s_shell.alarm_card_times, 0, sizeof(s_shell.alarm_card_times));
    memset(s_shell.alarm_card_names, 0, sizeof(s_shell.alarm_card_names));
    memset(s_shell.alarm_card_meta, 0, sizeof(s_shell.alarm_card_meta));
    memset(s_shell.alarm_card_kind, 0, sizeof(s_shell.alarm_card_kind));
    memset(s_shell.alarm_card_toggles, 0, sizeof(s_shell.alarm_card_toggles));
    memset(s_shell.alarm_card_knobs, 0, sizeof(s_shell.alarm_card_knobs));
    memset(s_shell.alarm_card_ids, 0, sizeof(s_shell.alarm_card_ids));
    memset(s_shell.plan_today_dishes, 0, sizeof(s_shell.plan_today_dishes));
    memset(s_shell.plan_expiring_names, 0, sizeof(s_shell.plan_expiring_names));
    memset(s_shell.plan_expiring_pills, 0, sizeof(s_shell.plan_expiring_pills));
    memset(s_shell.plan_shopping_items, 0, sizeof(s_shell.plan_shopping_items));
    memset(s_shell.plan_week_boxes, 0, sizeof(s_shell.plan_week_boxes));
    memset(s_shell.plan_week_labels, 0, sizeof(s_shell.plan_week_labels));
    s_shell.plan_shopping_count = NULL;
    s_shell.plan_week_range = NULL;
    s_shell.plan_agent_tip = NULL;
    memset(&s_shell.portrait_plan, 0, sizeof(s_shell.portrait_plan));
    s_shell.shopping_subtitle = NULL;
    memset(s_shell.shopping_rows, 0, sizeof(s_shell.shopping_rows));
    memset(s_shell.shopping_checks, 0, sizeof(s_shell.shopping_checks));
    memset(s_shell.shopping_check_icons, 0, sizeof(s_shell.shopping_check_icons));
    memset(s_shell.shopping_names, 0, sizeof(s_shell.shopping_names));
    memset(s_shell.shopping_meta, 0, sizeof(s_shell.shopping_meta));
    memset(s_shell.shopping_groups, 0, sizeof(s_shell.shopping_groups));
    s_shell.chat_page = NULL;
    s_shell.chat_scroll = NULL;
    s_shell.chat_message_flow = NULL;
    s_shell.chat_status_row = NULL;
    s_shell.chat_permission_chip = NULL;
    s_shell.chat_permission_label = NULL;
    s_shell.chat_sync_dot = NULL;
    s_shell.chat_sync_label = NULL;
    s_shell.chat_agent_label = NULL;
    s_shell.chat_tts_label = NULL;
    s_shell.chat_input_area = NULL;
    s_shell.chat_textarea = NULL;
    s_shell.chat_voice_btn = NULL;
    s_shell.chat_send_stop_btn = NULL;
    s_shell.chat_send_stop_label = NULL;
    s_shell.chat_ime_panel = NULL;
    s_shell.chat_ime_underline = NULL;
    memset(s_shell.chat_ime_candidates, 0, sizeof(s_shell.chat_ime_candidates));
    s_shell.chat_clear_scrim = NULL;
    s_shell.chat_clear_dialog = NULL;
    s_shell.chat_initialized = false;
    s_shell.chat_ime_open = false;
}

static void update_navigation(void)
{
    bool home = s_shell.page == SHELL_PAGE_HOME || s_shell.page == SHELL_PAGE_HOME_DETAIL;
    bool inventory = s_shell.page == SHELL_PAGE_INVENTORY ||
                     s_shell.page == SHELL_PAGE_INVENTORY_DETAIL ||
                     s_shell.page == SHELL_PAGE_INVENTORY_EDIT ||
                     s_shell.page == SHELL_PAGE_INVENTORY_RECOGNIZE ||
                     s_shell.page == SHELL_PAGE_INVENTORY_SEARCH ||
                     s_shell.page == SHELL_PAGE_INVENTORY_LAYOUT ||
                     s_shell.page == SHELL_PAGE_CAMERA;
    bool chat = s_shell.page == FRIDGE_UI_P4_PAGE_CHAT ||
                s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
                s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
                s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS;
    bool plan = s_shell.page == SHELL_PAGE_PLAN || s_shell.page == SHELL_PAGE_SHOPPING;
    bool more = !home && !inventory && !chat && !plan;
    for (size_t i = 0; i < 5; i++) {
        bool active = (i == 0 && home) || (i == 1 && inventory) ||
                      (i == 2 && chat) || (i == 3 && plan) || (i == 4 && more);
        if (s_shell.nav_items[i]) {
            lv_obj_set_style_bg_opa(s_shell.nav_items[i], active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        }
        if (s_shell.nav_labels[i]) {
            lv_obj_set_style_text_color(s_shell.nav_labels[i],
                                        color_hex(active ? 0xFFFFFF : 0xC6D5CD), 0);
        }
    }
}

static void switch_page(shell_page_t page)
{
    // 竖屏返回横屏后，计划/更多/工具页的旧正文已经被释放；同页重建一次，
    // 不能被常规的“同页请求直接返回”优化短路。
    const bool same_page_needs_rebuild =
        page == s_shell.page &&
        s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_LANDSCAPE &&
        !s_shell.page_content && page != SHELL_PAGE_HOME &&
        page != FRIDGE_UI_P4_PAGE_CHAT && !s_shell.camera_page;
    if (!s_shell.created ||
        (page == s_shell.page && !same_page_needs_rebuild &&
         page != SHELL_PAGE_ALARM_EDITOR && page != SHELL_PAGE_SHOPPING &&
         page != SHELL_PAGE_INVENTORY && page != SHELL_PAGE_INVENTORY_EDIT &&
         page != SHELL_PAGE_INVENTORY_RECOGNIZE &&
         page != SHELL_PAGE_INVENTORY_SEARCH &&
         page != SHELL_PAGE_INVENTORY_LAYOUT)) return;
    log_internal_dma_heap("page_switch_before", page);
    // PPA 直接操作 DPI 双缓冲。切页会销毁/重建滚动对象，必须先同步收尾旧手势，
    // 不能只置 enabled=false 后让下一帧继续写入已变更的 LVGL 对象树。
    fridge_ui_scroll_ppa_request_finish();
    fridge_ui_scroll_ppa_process();
    const bool inventory_page = page == SHELL_PAGE_INVENTORY ||
                                page == SHELL_PAGE_INVENTORY_DETAIL ||
                                page == SHELL_PAGE_INVENTORY_EDIT ||
                                page == SHELL_PAGE_INVENTORY_RECOGNIZE ||
                                page == SHELL_PAGE_INVENTORY_SEARCH ||
                                page == SHELL_PAGE_INVENTORY_LAYOUT;
    // UI 可能早于 LittleFS 完成挂载；首次打开库存页时再尝试一次，避免展示空白页。
    if (inventory_page && !s_shell.inventory_initialized &&
        fridge_ui_inventory_model_load(&s_shell.inventory) == ESP_OK) {
        s_shell.inventory_initialized = true;
    }
    cancel_pending_alarm_action();
    // 离开设置页前先隐藏并销毁 Wi-Fi 弹窗；close 会短暂恢复旧页 PPA，随后本次
    // 目标页面的白名单立即覆盖它，避免动态弹窗被快照带到下一页。
    close_settings_wifi_modal();
    close_chat_clear_confirm();
    // HOME/更多/闹钟管理使用静态整页快照；秒表和闹钟编辑页都额外叠加动态区域。
    // 编辑页的固定底栏不属于滚动容器，必须作为动态覆盖层回绘，避免滚动快照盖住操作栏。
    fridge_ui_scroll_ppa_set_dynamic_overlay(NULL);
    // PLAN-06 正文只露出 440px，底部 73px 是固定操作栏。缩小 PPA 复制区域后，
    // 底栏直接沿用双缓冲的静态像素，无需每帧 ARGB 快照和混合。
    fridge_ui_scroll_ppa_set_viewport_height(page == SHELL_PAGE_SHOPPING ? 440U : 0U);
    fridge_ui_scroll_ppa_set_enabled(page == SHELL_PAGE_HOME ||
                                      page == SHELL_PAGE_HOME_DETAIL ||
                                      page == SHELL_PAGE_MORE ||
                                      page == SHELL_PAGE_SETTINGS ||
                                      page == SHELL_PAGE_SAFETY ||
                                      page == SHELL_PAGE_MAINTENANCE ||
                                      page == SHELL_PAGE_ALARM ||
                                      page == SHELL_PAGE_ALARM_EDITOR ||
                                       page == SHELL_PAGE_STOPWATCH ||
                                       page == SHELL_PAGE_PLAN ||
                                       page == SHELL_PAGE_SHOPPING);
    close_alarm_popups();
    close_shopping_dialogs();
    if (s_shell.alarm_editor_footer) {
        lv_obj_delete_async(s_shell.alarm_editor_footer);
        s_shell.alarm_editor_footer = NULL;
    }
    if (s_shell.shopping_footer) {
        lv_obj_delete_async(s_shell.shopping_footer);
        s_shell.shopping_footer = NULL;
    }
    if (s_shell.page_content) {
        lv_obj_add_flag(s_shell.page_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_shell.page_content);
        s_shell.page_content = NULL;
        // 动态页销毁时同步回收其图标槽位，防止 TF 绑定遍历到已删除 LVGL 对象。
        s_shell.icon_slot_count = s_shell.page_icon_slot_start;
        clear_page_references();
    }
    if (s_shell.chat_page) {
        // CHAT-01 是独立固定布局层，不属于外层 main_scroll。切页时先关闭输入法并
        // 解除所有 LVGL 对象引用，避免异步删除后重入聊天页出现空白页面。
        close_chat_ime();
        lv_obj_add_flag(s_shell.chat_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_shell.chat_page);
        clear_page_references();
    }
    if (s_shell.camera_page) {
        if (s_shell.camera_photo) {
            lv_image_set_src(s_shell.camera_photo, NULL);
            lv_image_cache_drop(&s_shell.camera_photo_dsc);
        }
        // 侧边导航也可能直接切走拍照页；先解除 LVGL 图片源，再通知编排层释放
        // RGB565/JPEG，覆盖所有离页路径而不依赖某一个按钮回调。
        if (s_shell.actions.camera_release) {
            s_shell.actions.camera_release(s_shell.actions.user_ctx);
        }
        lv_obj_add_flag(s_shell.camera_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_shell.camera_page);
        s_shell.camera_page = NULL;
        s_shell.camera_photo = NULL;
        s_shell.camera_placeholder = NULL;
        s_shell.camera_capture_button = NULL;
        s_shell.camera_capture_label = NULL;
        memset(&s_shell.camera_photo_dsc, 0, sizeof(s_shell.camera_photo_dsc));
        s_shell.camera_review_active = false;
        s_shell.camera_network_dot = NULL;
        s_shell.camera_network_text = NULL;
        // 静态取景页也注册了 A8 图标；异步删除前先把待删除对象从资源绑定表移除。
        s_shell.icon_slot_count = s_shell.camera_icon_slot_start;
    }
    s_shell.page = page;
    bool home = page == SHELL_PAGE_HOME;
    bool home_detail = page == SHELL_PAGE_HOME_DETAIL;
    bool more = page == SHELL_PAGE_MORE;
    bool camera = page == SHELL_PAGE_CAMERA;
    bool chat = page == FRIDGE_UI_P4_PAGE_CHAT;
    if (camera) {
        // INV-05 复用首页/更多页的共享 87px 顶栏，确保标题、返回和底部分隔线完全对齐。
        if (s_shell.header) lv_obj_remove_flag(s_shell.header, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.main_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.home_content, LV_OBJ_FLAG_HIDDEN);
        create_camera_page(s_shell.main);
    } else if (chat) {
        // 对话页由内部消息流负责滚动；隐藏外层 main_scroll，且禁用 PPA，防止缓存
        // 复制覆盖动态气泡、输入框和键盘。
        if (s_shell.header) lv_obj_remove_flag(s_shell.header, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.main_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.home_content, LV_OBJ_FLAG_HIDDEN);
        create_chat_page(s_shell.main);
    } else {
        if (s_shell.header) lv_obj_remove_flag(s_shell.header, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_shell.main_scroll, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(s_shell.main_scroll, 0, LV_ANIM_OFF);
        if (home) {
            lv_obj_remove_flag(s_shell.home_content, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_shell.home_content, LV_OBJ_FLAG_HIDDEN);
            s_shell.page_content = lv_obj_create(s_shell.main_scroll);
            clear_container_style(s_shell.page_content);
            lv_obj_set_pos(s_shell.page_content, inventory_page ? 24 : 16,
                           inventory_page ? 16 : 12);
            int inventory_content_h = 497;
            if (page == SHELL_PAGE_INVENTORY && s_shell.inventory_list_view) {
                size_t visible = inventory_scope_count(NULL, NULL, NULL);
                int wanted = 64 + (int)visible * 72;
                if (wanted > inventory_content_h) inventory_content_h = wanted;
            }
            // 定时器页高度按最多 4 条其他运行中 + 5 条已完成预留，实际内容可滚动。
            int content_h = home_detail ? 1656 :
                            more ? 790 :
                            page == SHELL_PAGE_ALARM ? 900 :
                            page == SHELL_PAGE_ALARM_EDITOR ? 720 :
                            page == SHELL_PAGE_TIMER ? 980 :
                            page == SHELL_PAGE_TIMER_EDITOR ? 480 :
                            page == SHELL_PAGE_PLAN ? 900 :
                            page == SHELL_PAGE_SHOPPING ? 900 :
                            page == SHELL_PAGE_BINDING ? 460 :
                            // 分区模板高度取左栏与右侧内容的较大值，右侧最多 5 行 78px + 标题和说明。
                            page == SHELL_PAGE_SETTINGS ? 520 :
                            page == SHELL_PAGE_MAINTENANCE ? 520 :
                            page == SHELL_PAGE_SAFETY ? 520 :
                             page == SHELL_PAGE_INVENTORY ? inventory_content_h :
                             page == SHELL_PAGE_INVENTORY_DETAIL ? 680 :
                             page == SHELL_PAGE_INVENTORY_EDIT ? 720 :
                             page == SHELL_PAGE_INVENTORY_RECOGNIZE ? 620 :
                             page == SHELL_PAGE_INVENTORY_SEARCH ? 1780 :
                             page == SHELL_PAGE_INVENTORY_LAYOUT ? 620 : 360;
            lv_obj_set_size(s_shell.page_content, inventory_page ? 656 : 672, content_h);
            s_shell.page_icon_slot_start = s_shell.icon_slot_count;
            if (home_detail) create_home_detail_page(s_shell.page_content);
            else if (more) create_more_page(s_shell.page_content);
            else if (page == SHELL_PAGE_TIMER) create_timer_page(s_shell.page_content);
            else if (page == SHELL_PAGE_TIMER_EDITOR) create_timer_editor_page(s_shell.page_content);
            else if (page == SHELL_PAGE_ALARM) create_alarm_page(s_shell.page_content);
            else if (page == SHELL_PAGE_ALARM_EDITOR) create_alarm_editor_page(s_shell.page_content);
            else if (page == SHELL_PAGE_PLAN) create_plan_page(s_shell.page_content);
            else if (page == SHELL_PAGE_SHOPPING) create_shopping_page(s_shell.page_content);
            else if (page == SHELL_PAGE_BINDING) create_binding_page(s_shell.page_content);
            else if (page == SHELL_PAGE_SETTINGS) create_settings_page(s_shell.page_content);
            else if (page == SHELL_PAGE_MAINTENANCE) create_maintenance_page(s_shell.page_content);
            else if (page == SHELL_PAGE_SAFETY) create_safety_page(s_shell.page_content);
            else if (page == SHELL_PAGE_INVENTORY) create_inventory_page(s_shell.page_content);
            else if (page == SHELL_PAGE_INVENTORY_DETAIL) create_inventory_detail_page(s_shell.page_content);
            else if (page == SHELL_PAGE_INVENTORY_EDIT) create_inventory_edit_page(s_shell.page_content);
            else if (page == SHELL_PAGE_INVENTORY_RECOGNIZE) create_inventory_recognize_page(s_shell.page_content);
            else if (page == SHELL_PAGE_INVENTORY_SEARCH) create_inventory_search_page(s_shell.page_content);
            else if (page == SHELL_PAGE_INVENTORY_LAYOUT) create_inventory_layout_page(s_shell.page_content);
            else create_stopwatch_page(s_shell.page_content);
            if (page == SHELL_PAGE_STOPWATCH) {
                // 滚动期间只有毫秒数字需要持续变化；按钮、状态和卡片背景在
                // 状态/计次变化时重建静态快照，避免每帧 Blend 整张 704x234 主卡。
                fridge_ui_scroll_ppa_set_dynamic_overlay(s_shell.stopwatch_face_region);
            } else if (page == SHELL_PAGE_ALARM_EDITOR) {
                // 底栏固定在 main 上而非 main_scroll 内；PPA 复制滚动快照后需要重新混合它。
                fridge_ui_scroll_ppa_set_dynamic_overlay(s_shell.alarm_editor_footer);
            }
        }
    }
    if (s_shell.header_title) {
        const char *title = page == SHELL_PAGE_HOME ? "首页" :
                            page == SHELL_PAGE_HOME_DETAIL ? "今日摘要详情" :
                            page == SHELL_PAGE_MORE ? "更多" :
                            page == SHELL_PAGE_TIMER ? "定时器" :
                            page == SHELL_PAGE_TIMER_EDITOR ? "编辑定时器" :
                            page == SHELL_PAGE_ALARM ? "闹钟" :
                            page == SHELL_PAGE_ALARM_EDITOR ? "编辑闹钟" :
                            page == SHELL_PAGE_STOPWATCH ? "秒表" :
                            page == SHELL_PAGE_PLAN ? "计划" :
                            page == SHELL_PAGE_SHOPPING ? "购物清单" :
                            page == SHELL_PAGE_BINDING ? "设备绑定" :
                            page == SHELL_PAGE_SETTINGS ? "设置中心" :
                            page == SHELL_PAGE_MAINTENANCE ? "维护中心" :
                            page == SHELL_PAGE_SAFETY ? "设备与厨房安全" :
                            page == SHELL_PAGE_INVENTORY ? "库存浏览器" :
                            page == SHELL_PAGE_INVENTORY_DETAIL ? "食材详情" :
                            page == SHELL_PAGE_INVENTORY_EDIT ?
                                (s_shell.inventory_edit_existing ? "编辑食材" : "新增食材") :
                            page == SHELL_PAGE_INVENTORY_RECOGNIZE ? "识别结果确认" :
                            page == SHELL_PAGE_INVENTORY_SEARCH ? "搜索与筛选" :
                            page == SHELL_PAGE_INVENTORY_LAYOUT ? "空间形状编辑" :
                            page == FRIDGE_UI_P4_PAGE_CHAT ? "对话" : "摄影登记";
        set_label_if_changed(s_shell.header_title, title);
        // 四字标题（今日摘要详情/设备绑定/设置中心/维护中心）比两字页名更宽，
        // 需要右移页面 ID 角标，避免角标压住标题末字。
        s_shell.header_wide_title = home_detail || page == SHELL_PAGE_BINDING ||
                                    page == SHELL_PAGE_SETTINGS ||
                                    page == SHELL_PAGE_MAINTENANCE ||
                                    page == SHELL_PAGE_SAFETY ||
                                    page == SHELL_PAGE_INVENTORY ||
                                    page == SHELL_PAGE_INVENTORY_RECOGNIZE ||
                                    page == SHELL_PAGE_INVENTORY_SEARCH ||
                                    page == SHELL_PAGE_INVENTORY_LAYOUT;
        lv_obj_set_width(s_shell.header_title,
                         home_detail ? 260 : (page == SHELL_PAGE_SAFETY ? 250 : 180));
    }
    if (s_shell.header_id_box && s_shell.header_id) {
        const char *page_id = page == SHELL_PAGE_HOME ? "HOME-01" :
                              page == SHELL_PAGE_HOME_DETAIL ? "HOME-02" :
                              page == SHELL_PAGE_MORE ? "MORE-01" :
                              page == SHELL_PAGE_TIMER ? "MORE-02" :
                              page == SHELL_PAGE_TIMER_EDITOR ? "MORE-03" :
                              page == SHELL_PAGE_ALARM ? "MORE-07" :
                              page == SHELL_PAGE_ALARM_EDITOR ? "MORE-03" :
                              page == SHELL_PAGE_STOPWATCH ? "MORE-08" :
                              page == SHELL_PAGE_PLAN ? "PLAN-01" :
                              page == SHELL_PAGE_SHOPPING ? "PLAN-06" :
                              page == SHELL_PAGE_BINDING ? "BIND-01" :
                              page == SHELL_PAGE_SETTINGS ? "MORE-05" :
                              page == SHELL_PAGE_MAINTENANCE ? "MORE-06" :
                              page == SHELL_PAGE_SAFETY ? "MORE-04" :
                              page == SHELL_PAGE_INVENTORY ? "INV-01" :
                              page == SHELL_PAGE_INVENTORY_DETAIL ? "INV-03" :
                              page == SHELL_PAGE_INVENTORY_EDIT ? "INV-04" :
                              page == SHELL_PAGE_INVENTORY_RECOGNIZE ? "INV-06" :
                              page == SHELL_PAGE_INVENTORY_SEARCH ? "INV-07" :
                              page == SHELL_PAGE_INVENTORY_LAYOUT ? "INV-08" :
                              page == FRIDGE_UI_P4_PAGE_CHAT ? "CHAT-01" : "INV-05";
        set_label_if_changed(s_shell.header_id, page_id);
        lv_obj_set_x(s_shell.header_id_box, home_detail ? 202 :
                                            (s_shell.header_wide_title ? 150 : 86));
        // 首页与购物页按参考稿不显示页码胶囊；摄影登记保留 INV-05。
        if (home || page == SHELL_PAGE_SHOPPING) {
            lv_obj_add_flag(s_shell.header_id_box, LV_OBJ_FLAG_HIDDEN);
        }
        else lv_obj_remove_flag(s_shell.header_id_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shell.header_back) {
        if (home || more || page == SHELL_PAGE_PLAN ||
            (page == SHELL_PAGE_INVENTORY && s_shell.inventory_scope_level == 0) ||
            page == FRIDGE_UI_P4_PAGE_CHAT) {
            lv_obj_add_flag(s_shell.header_back, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_shell.header_back, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_x(s_shell.header_back,
                     page == SHELL_PAGE_INVENTORY ? 472 : 640);
    }
    if (s_shell.header_search) {
        if (home || page == SHELL_PAGE_INVENTORY) {
            lv_obj_set_x(s_shell.header_search,
                         page == SHELL_PAGE_INVENTORY ? 528 : 584);
            lv_obj_remove_flag(s_shell.header_search, LV_OBJ_FLAG_HIDDEN);
        }
        else lv_obj_add_flag(s_shell.header_search, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shell.header_plus) {
        if (home || page == SHELL_PAGE_INVENTORY) {
            lv_obj_set_x(s_shell.header_plus,
                         page == SHELL_PAGE_INVENTORY ? 584 : 640);
            lv_obj_remove_flag(s_shell.header_plus, LV_OBJ_FLAG_HIDDEN);
        }
        else lv_obj_add_flag(s_shell.header_plus, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shell.header_grid) {
        if (page == SHELL_PAGE_INVENTORY) {
            lv_obj_remove_flag(s_shell.header_grid, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_shell.header_grid, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_shell.header_new) {
        if (page == SHELL_PAGE_ALARM || page == SHELL_PAGE_TIMER) {
            lv_obj_remove_flag(s_shell.header_new, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_shell.header_new, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_shell.header_chat_clear) {
        if (page == FRIDGE_UI_P4_PAGE_CHAT) {
            lv_obj_remove_flag(s_shell.header_chat_clear, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_shell.header_chat_clear, LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_navigation();
    refresh_header_sub();
    refresh_tool_page();
    refresh_ctx_camera();
    refresh_camera_live_status();
    refresh_cabin_context_visibility();
    // HOME 与 MORE 共用唯一滚动区；页面切换后必须重建 PPA 快照，避免复用旧页面缓存。
    fridge_ui_scroll_ppa_invalidate_cache();
    log_internal_dma_heap("page_switch_after", page);
}

// 已移植页面在竖屏内直接重建主正文，保留隐私状态舱和方向，避免先闪回横屏。
static esp_err_t rebuild_portrait_page(shell_page_t page)
{
    if (!portrait_page_supported(page)) return ESP_ERR_NOT_SUPPORTED;
    if (page == s_shell.page) return ESP_OK;

    log_internal_dma_heap("portrait_page_before", page);
    // 删除旧竖屏滚动对象前必须让 PPA 完成当前手势，避免硬件继续读取已释放对象的滚动位置。
    fridge_ui_scroll_ppa_request_finish();
    fridge_ui_scroll_ppa_process();
    fridge_ui_scroll_ppa_set_enabled(false);
    clear_portrait_shell();

    s_shell.page = page;
    create_portrait_shell(s_shell.root);
    lv_obj_add_flag(s_shell.main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.nav, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.cabin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_shell.portrait_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_shell.root, UI_PORTRAIT_W, UI_PORTRAIT_H);
    lv_obj_scroll_to_y(s_shell.portrait_scroll, 0, LV_ANIM_OFF);
    if (s_shell.toast) lv_obj_set_pos(s_shell.toast, 68, 16);
    update_navigation();
    refresh_header_sub();
    if (s_shell.device_initialized) fridge_ui_shell_update_device(&s_shell.device);
    if (page == SHELL_PAGE_HOME) fridge_ui_shell_update_home(&s_shell.home);
    if (page == SHELL_PAGE_PLAN) refresh_plan_page();
    if (page == SHELL_PAGE_MORE || page == SHELL_PAGE_TIMER ||
        page == SHELL_PAGE_ALARM || page == SHELL_PAGE_STOPWATCH ||
        page == SHELL_PAGE_SAFETY || page == SHELL_PAGE_SETTINGS ||
        page == SHELL_PAGE_MAINTENANCE) {
        refresh_tool_page();
    }
    if (page == FRIDGE_UI_P4_PAGE_CHAT ||
        page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
        page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
        page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS) {
        refresh_portrait_chat_page();
    }
    fridge_ui_scroll_ppa_invalidate_cache();
    log_internal_dma_heap("portrait_page_after", page);
    return ESP_OK;
}

esp_err_t fridge_ui_shell_set_page(fridge_ui_p4_page_t page)
{
    // 此函数只在 ui_p4 的 LVGL 任务中调用，避免多个任务并发访问 LVGL 对象树。
    if (!s_shell.created) return ESP_ERR_INVALID_STATE;
    if (page < FRIDGE_UI_P4_PAGE_HOME || page >= FRIDGE_UI_P4_PAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    // 横屏保留旧的 CHAT-01 兼容行为；竖屏必须保留真实 CHAT-02/03/04 页面键，
    // 这样 USB 直达和页面返回都能展示对应的 ver2 布局。
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_LANDSCAPE &&
        (page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
         page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
         page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS)) {
        page = FRIDGE_UI_P4_PAGE_CHAT;
    }
    // USB Tool/外部页面请求与固定导航语义一致：inventory 永远进入可复现的 INV-01。
    if (page == FRIDGE_UI_P4_PAGE_INVENTORY) {
        s_shell.inventory_scope_level = 0;
        s_shell.inventory_list_view = false;
        s_shell.inventory_pick_location = false;
    }
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        // 未移植页面由 ui_p4 编排层先安全恢复横屏；竖屏 Shell 只接受已移植的
        // HOME/PLAN/CHAT/MORE 与 MORE-04～06 页面。
        return rebuild_portrait_page((shell_page_t)page);
    }
    switch_page((shell_page_t)page);
    return ESP_OK;
}

esp_err_t fridge_ui_shell_set_orientation(fridge_ui_p4_orientation_t orientation)
{
    if (!s_shell.created || !s_shell.main || !s_shell.nav || !s_shell.cabin) {
        return ESP_ERR_INVALID_STATE;
    }
    if (orientation < FRIDGE_UI_P4_ORIENTATION_LANDSCAPE ||
        orientation >= FRIDGE_UI_P4_ORIENTATION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        !portrait_page_supported(s_shell.page)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (orientation == s_shell.orientation) return ESP_OK;

    s_shell.orientation = orientation;
    if (orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        // 进入竖屏前释放横屏 HOME-02 的动态正文；竖屏正文使用同量级对象数，
        // 避免两套长页同时占用内部堆并挤压 SDMMC DMA bounce buffer。
        if (s_shell.page_content) {
            lv_obj_delete(s_shell.page_content);
            s_shell.page_content = NULL;
            s_shell.icon_slot_count = s_shell.page_icon_slot_start;
            clear_page_references();
        }
        if (s_shell.chat_page) {
            // 横屏 CHAT-01 的输入框/IME 与竖屏对象树不能同时存在；先销毁横屏
            // 固定层，再由 create_portrait_chat_page 重新绑定同一份模型。
            close_chat_clear_confirm();
            close_chat_ime();
            lv_obj_delete(s_shell.chat_page);
            s_shell.chat_page = NULL;
            clear_page_references();
        }
        if (!s_shell.portrait_root) {
            s_shell.portrait_icon_slot_start = s_shell.icon_slot_count;
            create_portrait_shell(s_shell.root);
        }
        lv_obj_add_flag(s_shell.main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.nav, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.cabin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_shell.portrait_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_shell.root, 600, 1024);
        lv_obj_scroll_to_y(s_shell.portrait_scroll, 0, LV_ANIM_OFF);
        if (s_shell.toast) lv_obj_set_pos(s_shell.toast, 68, 16);
        fridge_ui_scroll_ppa_set_enabled(false);
        refresh_header_sub();
        if (s_shell.device_initialized) {
            fridge_ui_shell_update_device(&s_shell.device);
        }
        if (s_shell.page == SHELL_PAGE_HOME) {
            fridge_ui_shell_update_home(&s_shell.home);
        }
        if (s_shell.page == SHELL_PAGE_PLAN) {
            refresh_plan_page();
        }
        if (s_shell.page == SHELL_PAGE_MORE || s_shell.page == SHELL_PAGE_TIMER ||
            s_shell.page == SHELL_PAGE_ALARM || s_shell.page == SHELL_PAGE_STOPWATCH ||
            s_shell.page == SHELL_PAGE_SAFETY || s_shell.page == SHELL_PAGE_SETTINGS ||
            s_shell.page == SHELL_PAGE_MAINTENANCE) {
            refresh_tool_page();
        }
        if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT) {
            refresh_portrait_chat_page();
        }
    } else {
        // 返回横屏时销毁竖屏对象树并回收其图标槽，随后按原页面恢复横屏正文。
        clear_portrait_shell();
        // 横屏只保留 CHAT-01 兼容页；从 CHAT-02/03/04 返回横屏时折叠为主对话页，
        // 但在竖屏内部页面切换不会走到这里。
        if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
            s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
            s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS) {
            s_shell.page = FRIDGE_UI_P4_PAGE_CHAT;
        }
        lv_obj_remove_flag(s_shell.main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_shell.nav, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_shell.cabin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_shell.root, 1024, 600);
        if (s_shell.page == SHELL_PAGE_HOME_DETAIL && !s_shell.page_content) {
            s_shell.page_content = lv_obj_create(s_shell.main_scroll);
            clear_container_style(s_shell.page_content);
            lv_obj_set_pos(s_shell.page_content, 16, 12);
            lv_obj_set_size(s_shell.page_content, 672, 1656);
            s_shell.page_icon_slot_start = s_shell.icon_slot_count;
            create_home_detail_page(s_shell.page_content);
            lv_obj_add_flag(s_shell.home_content, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_shell.main_scroll, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to_y(s_shell.main_scroll, 0, LV_ANIM_OFF);
        }
        if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT && !s_shell.chat_page) {
            lv_obj_add_flag(s_shell.main_scroll, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_shell.home_content, LV_OBJ_FLAG_HIDDEN);
            create_chat_page(s_shell.main);
        }
        if (s_shell.page != SHELL_PAGE_HOME &&
            s_shell.page != SHELL_PAGE_HOME_DETAIL &&
            s_shell.page != FRIDGE_UI_P4_PAGE_CHAT &&
            !s_shell.page_content) {
            // 计划、更多、定时器、闹钟和秒表在竖屏中使用独立对象树；恢复横屏
            // 时必须重新创建原有正文，保证方向切换后不是空白页。
            switch_page(s_shell.page);
        }
        if (s_shell.toast) lv_obj_set_pos(s_shell.toast, 280, 16);
        fridge_ui_scroll_ppa_set_viewport_height(0);
        fridge_ui_scroll_ppa_set_enabled(
            s_shell.page == SHELL_PAGE_HOME ||
            s_shell.page == SHELL_PAGE_HOME_DETAIL ||
            s_shell.page == SHELL_PAGE_MORE ||
            s_shell.page == SHELL_PAGE_SETTINGS ||
            s_shell.page == SHELL_PAGE_SAFETY ||
            s_shell.page == SHELL_PAGE_MAINTENANCE ||
            s_shell.page == SHELL_PAGE_ALARM ||
            s_shell.page == SHELL_PAGE_ALARM_EDITOR ||
            s_shell.page == SHELL_PAGE_STOPWATCH ||
            s_shell.page == SHELL_PAGE_PLAN);
        if (s_shell.page == SHELL_PAGE_HOME) {
            // 竖屏期间只更新了可见对象；恢复横屏后再一次性回填原 HOME 对象树。
            fridge_ui_shell_update_home(&s_shell.home);
        }
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    lv_obj_invalidate(s_shell.root);
    return ESP_OK;
}

// 秒表后端快照已经按单调时钟折算到当前毫秒值；UI 只读取这个值，不能再次加运行区间。
static uint64_t stopwatch_elapsed_for_ui(int64_t now_us)
{
    (void)now_us;
    fridge_kitchen_tools_snapshot_t snapshot = {0};
    if (fridge_kitchen_tools_get_snapshot(&snapshot) == ESP_OK) {
        // 只更新秒表子快照，避免高频显示刷新覆盖其它页面业务状态。
        s_shell.kitchen.stopwatch = snapshot.stopwatch;
    }
    return s_shell.kitchen.stopwatch.elapsed_ms;
}

static void format_stopwatch_elapsed(uint64_t elapsed_ms, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    uint64_t total_seconds = elapsed_ms / 1000U;
    uint64_t minutes = total_seconds / 60U;
    uint64_t seconds = total_seconds % 60U;
    uint64_t milliseconds = elapsed_ms % 1000U;
    // 页面参考采用 MM:SS.mmm；分钟超过两位时自然扩展，不截断计时结果。
    snprintf(out, out_size, "%02llu:%02llu.%03llu",
             (unsigned long long)minutes,
             (unsigned long long)seconds,
             (unsigned long long)milliseconds);
}

// 秒表图标有 TF A8 图片和文本回退两套对象；同一状态只能显示一套，
// 否则资源绑定后刷新会把回退字符再次显示在图片上，形成重叠图标。
static void set_stopwatch_icon_visibility(lv_obj_t *image, lv_obj_t *fallback,
                                          bool visible, bool asset_available)
{
    if (!image || !fallback) return;
    if (!visible) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    } else if (asset_available) {
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_stopwatch_page(int64_t now_us)
{
    if (s_shell.page != SHELL_PAGE_STOPWATCH || !s_shell.stopwatch_face) return;

    fridge_kitchen_stopwatch_state_t stopwatch_state = s_shell.kitchen.stopwatch.state;
    bool running = stopwatch_state == FRIDGE_KITCHEN_STOPWATCH_RUNNING;
    bool state_changed = stopwatch_state != s_shell.stopwatch_last_ui_state;
    bool lap_count_changed = s_shell.kitchen.stopwatch.lap_count !=
                              s_shell.stopwatch_last_ui_lap_count;
    // 计时后端仍按毫秒单调时钟计算；显示只按屏幕可见刷新率更新，避免每 1ms
    // 修改一次 LVGL 文本，造成整张主卡反复失效并拖慢滚动/合成。
    if (!state_changed && !lap_count_changed &&
        (!running || now_us - s_shell.stopwatch_last_ui_refresh_us < UI_STOPWATCH_REFRESH_US)) {
        return;
    }

    uint64_t elapsed_ms = stopwatch_elapsed_for_ui(now_us);
    char text[32];
    format_stopwatch_elapsed(elapsed_ms, text, sizeof(text));
    set_label_if_changed(s_shell.stopwatch_face, text);

    // 开始/暂停/复位会改变主卡上的状态文字和按钮图标。数字标签仍可走小块
    // 动态 Blend，但这些静态控件必须先让 PPA 丢弃旧快照并在空闲期重建。
    if (state_changed) {
        fridge_ui_scroll_ppa_invalidate_cache();
    }

    const char *state = running ? "计时中" :
                        stopwatch_state == FRIDGE_KITCHEN_STOPWATCH_PAUSED ?
                            "已暂停" : "就绪";
    set_label_if_changed(s_shell.stopwatch_state, state);
    set_label_if_changed(s_shell.stopwatch_toggle_label, running ? "暂停" : "开始");
    if (s_shell.stopwatch_toggle_play_image && s_shell.stopwatch_toggle_play_fallback &&
        s_shell.stopwatch_toggle_pause_image && s_shell.stopwatch_toggle_pause_fallback) {
        bool assets_ready = s_shell.assets && s_shell.assets->loaded;
        bool play_asset_available = assets_ready &&
            fridge_ui_assets_get_icon(s_shell.assets, FRIDGE_UI_ICON_PLAY) != NULL;
        bool pause_asset_available = assets_ready &&
            fridge_ui_assets_get_icon(s_shell.assets, FRIDGE_UI_ICON_PAUSE) != NULL;
        set_stopwatch_icon_visibility(s_shell.stopwatch_toggle_play_image,
                                      s_shell.stopwatch_toggle_play_fallback,
                                      !running, play_asset_available);
        set_stopwatch_icon_visibility(s_shell.stopwatch_toggle_pause_image,
                                      s_shell.stopwatch_toggle_pause_fallback,
                                      running, pause_asset_available);
    }

    bool has_time = elapsed_ms > 0;
    bool can_lap = running || has_time;
    if (s_shell.stopwatch_reset) {
        if (has_time || s_shell.kitchen.stopwatch.lap_count > 0) {
            lv_obj_clear_state(s_shell.stopwatch_reset, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_shell.stopwatch_reset, LV_STATE_DISABLED);
        }
    }
    if (s_shell.stopwatch_lap) {
        if (can_lap) lv_obj_clear_state(s_shell.stopwatch_lap, LV_STATE_DISABLED);
        else lv_obj_add_state(s_shell.stopwatch_lap, LV_STATE_DISABLED);
    }

    char count_text[32];
    snprintf(count_text, sizeof(count_text), "计次（%u）",
             (unsigned)s_shell.kitchen.stopwatch.lap_count);
    set_label_if_changed(s_shell.stopwatch_laps_count, count_text);
    bool layout_changed = false;
    if (s_shell.kitchen.stopwatch.lap_count == 0) {
        lv_obj_remove_flag(s_shell.stopwatch_laps_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shell.stopwatch_laps_list, LV_OBJ_FLAG_HIDDEN);
        if (s_shell.page_content && lv_obj_get_height(s_shell.page_content) != 360) {
            lv_obj_set_height(s_shell.page_content, 360);
            layout_changed = true;
        }
    } else {
        lv_obj_add_flag(s_shell.stopwatch_laps_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_shell.stopwatch_laps_list, LV_OBJ_FLAG_HIDDEN);
        int list_height = (int)s_shell.kitchen.stopwatch.lap_count * 48;
        if (lv_obj_get_height(s_shell.stopwatch_laps_list) != list_height) {
            lv_obj_set_height(s_shell.stopwatch_laps_list, list_height);
            layout_changed = true;
        }
        if (s_shell.page_content && lv_obj_get_height(s_shell.page_content) != 302 + list_height) {
            lv_obj_set_height(s_shell.page_content, 302 + list_height);
            layout_changed = true;
        }
    }
    if (layout_changed) {
        // 计次数量改变会改变主滚动边界；即使工具页当前不使用 PPA，
        // 也要丢弃可能来自切页前的旧快照，回到 HOME 时再按新内容重建。
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    for (size_t i = 0; i < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS; i++) {
        bool visible = i < s_shell.kitchen.stopwatch.lap_count;
        if (s_shell.stopwatch_lap_rows[i]) {
            if (visible) lv_obj_remove_flag(s_shell.stopwatch_lap_rows[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_shell.stopwatch_lap_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (!visible) continue;
        char lap_label[24];
        snprintf(lap_label, sizeof(lap_label), "计次 %u",
                 (unsigned)(s_shell.kitchen.stopwatch.lap_count - i));
        set_label_if_changed(s_shell.stopwatch_lap_labels[i], lap_label);
        format_stopwatch_elapsed(s_shell.kitchen.stopwatch.laps_ms[i], text, sizeof(text));
        set_label_if_changed(s_shell.stopwatch_lap_times[i], text);
    }
    s_shell.stopwatch_last_ui_refresh_us = now_us;
    s_shell.stopwatch_last_ui_state = stopwatch_state;
    s_shell.stopwatch_last_ui_lap_count = s_shell.kitchen.stopwatch.lap_count;
}

static void refresh_portrait_more_page(void)
{
    portrait_more_view_t *view = &s_shell.portrait_more;
    if (!view->summary) return;

    const uint8_t running_timers = more_running_timer_count(&s_shell.kitchen);
    const uint8_t enabled_alarms = more_enabled_alarm_count(&s_shell.kitchen);
    const bool stopwatch_running =
        s_shell.kitchen.stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING;

    for (size_t i = 0; i < 3; i++) {
        if (view->titles[i]) {
            const char *title = s_shell.more_initialized && s_shell.more.cards[i].title[0] ?
                                s_shell.more.cards[i].title :
                                (i == 0 ? "定时器" : i == 1 ? "闹钟" : "秒表");
            set_label_if_changed(view->titles[i], title);
        }
    }
    if (view->descriptions[0]) {
        char text[64];
        if (running_timers > 0) {
            snprintf(text, sizeof(text), "运行中 %u 个倒计时", (unsigned)running_timers);
        } else {
            strlcpy(text, "厨房倒计时 · 快捷预设", sizeof(text));
        }
        set_label_if_changed(view->descriptions[0], text);
    }
    if (view->descriptions[1]) {
        char text[64];
        if (enabled_alarms > 0) {
            snprintf(text, sizeof(text), "%u 个待触发 · 含周期提醒",
                     (unsigned)enabled_alarms);
        } else {
            strlcpy(text, "固定闹钟 · 周期提醒", sizeof(text));
        }
        set_label_if_changed(view->descriptions[1], text);
    }
    if (view->descriptions[2]) {
        set_label_if_changed(view->descriptions[2], stopwatch_running ?
                             "计时中 · 大数字面" : "计次 · 复位 · 大数字面");
    }
    const uint8_t counts[2] = {running_timers, enabled_alarms};
    for (size_t i = 0; i < 2; i++) {
        if (!view->badges[i] || !view->badge_labels[i]) continue;
        if (counts[i] == 0) {
            lv_obj_add_flag(view->badges[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            char badge[4];
            snprintf(badge, sizeof(badge), "%u", (unsigned)counts[i]);
            set_label_if_changed(view->badge_labels[i], badge);
            lv_obj_remove_flag(view->badges[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    char text[48];
    snprintf(text, sizeof(text), "Wi-Fi：%s",
             s_shell.device.wifi == FRIDGE_UI_HEALTH_OK ? "已连接" : "未连接");
    set_label_if_changed(view->summary_wifi, text);
    snprintf(text, sizeof(text), "TF：%s",
             s_shell.device.tfcard == FRIDGE_UI_HEALTH_OK ? "资源已就绪" : "资源降级");
    set_label_if_changed(view->summary_tf, text);
    snprintf(text, sizeof(text), "时间：%s",
             s_shell.kitchen.time_ready ? "已同步" : "未同步");
    set_label_if_changed(view->summary_time, text);
}

static void refresh_portrait_timer_page(void)
{
    portrait_timer_view_t *view = &s_shell.portrait_timer;
    if (s_shell.page != SHELL_PAGE_TIMER || !view->face) return;

    const fridge_kitchen_timer_snapshot_t *timer = &s_shell.kitchen.timer;
    const bool has_primary = timer->id != 0 &&
        (timer->state == FRIDGE_KITCHEN_TIMER_RUNNING ||
         timer->state == FRIDGE_KITCHEN_TIMER_PAUSED ||
         timer->state == FRIDGE_KITCHEN_TIMER_RINGING);
    const bool primary_changed = view->initialized &&
        (view->last_primary_id != (has_primary ? timer->id : 0) ||
         view->last_primary_state != (has_primary ? timer->state : FRIDGE_KITCHEN_TIMER_IDLE));
    const bool first = !view->initialized;

    char text[32];
    if (has_primary) {
        format_clock_duration(timer->remaining_seconds, text, sizeof(text));
        set_label_if_changed(view->face, text);
        lv_obj_set_style_text_color(view->face, color_hex(FRIDGE_UI_COLOR_INK), 0);
        set_label_if_changed(view->label,
                             timer->label[0] ? timer->label : timer_default_name());
        set_label_if_changed(view->chip,
                             timer->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "已暂停" :
                             timer->state == FRIDGE_KITCHEN_TIMER_RINGING ? "时间到" : "运行中");
        char total[16];
        format_clock_duration(timer->duration_seconds, total, sizeof(total));
        char detail[96];
        snprintf(detail, sizeof(detail), "总时长 %s · 来源 %s", total,
                 timer->source[0] ? timer->source : "手动");
        set_label_if_changed(view->state, detail);
        style_box(view->hero, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8, 1);
        lv_obj_remove_flag(view->ops, LV_OBJ_FLAG_HIDDEN);
        if (view->pause_label) {
            set_label_if_changed(view->pause_label,
                                 timer->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "继续" : "暂停");
        }
        if (timer->state == FRIDGE_KITCHEN_TIMER_RINGING) {
            lv_obj_add_flag(view->pause, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(view->cancel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(view->dismiss, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(view->pause, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(view->cancel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(view->dismiss, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        set_label_if_changed(view->face, "00:00");
        lv_obj_set_style_text_color(view->face, color_hex(FRIDGE_UI_COLOR_MUTED), 0);
        set_label_if_changed(view->label, "厨房倒计时");
        set_label_if_changed(view->chip, "");
        set_label_if_changed(view->state, "点下方预设立即开始，或新建自定义时长");
        style_box(view->hero, FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_LINE, 8, 1);
        lv_obj_add_flag(view->ops, LV_OBJ_FLAG_HIDDEN);
    }

    const fridge_kitchen_timer_snapshot_t *others[UI_TIMER_OTHER_MAX] = {0};
    const fridge_kitchen_timer_snapshot_t *dones[UI_TIMER_DONE_MAX] = {0};
    size_t other_count = 0;
    size_t done_count = 0;
    for (size_t i = 0; i < s_shell.kitchen.timer_count; i++) {
        const fridge_kitchen_timer_snapshot_t *candidate = &s_shell.kitchen.timers[i];
        if (has_primary && candidate->id == timer->id) continue;
        if ((candidate->state == FRIDGE_KITCHEN_TIMER_RUNNING ||
             candidate->state == FRIDGE_KITCHEN_TIMER_PAUSED ||
             candidate->state == FRIDGE_KITCHEN_TIMER_RINGING) &&
            other_count < UI_TIMER_OTHER_MAX) {
            others[other_count++] = candidate;
        } else if (candidate->state == FRIDGE_KITCHEN_TIMER_DONE &&
                   done_count < UI_TIMER_DONE_MAX) {
            dones[done_count++] = candidate;
        }
    }
    if (other_count > 0) {
        char title[32];
        snprintf(title, sizeof(title), "其他运行中（%u）", (unsigned)other_count);
        set_label_if_changed(view->other_title, title);
        lv_obj_remove_flag(view->other_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(view->other_title, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < UI_TIMER_OTHER_MAX; i++) {
        if (!view->other_rows[i]) continue;
        if (i < other_count) {
            const fridge_kitchen_timer_snapshot_t *other = others[i];
            view->other_ids[i] = other->id;
            lv_obj_remove_flag(view->other_rows[i], LV_OBJ_FLAG_HIDDEN);
            set_label_if_changed(view->other_names[i],
                                 other->label[0] ? other->label : timer_default_name());
            format_clock_duration(other->remaining_seconds, text, sizeof(text));
            set_label_if_changed(view->other_remains[i], text);
            char total[16];
            format_clock_duration(other->duration_seconds, total, sizeof(total));
            char detail[96];
            snprintf(detail, sizeof(detail), "总时长 %s · 来源 %s · %s", total,
                     other->source[0] ? other->source : "手动",
                     other->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "已暂停" : "运行中");
            set_label_if_changed(view->other_meta[i], detail);
            set_label_if_changed(view->other_pause_labels[i],
                                 other->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "继续" : "暂停");
            lv_obj_remove_event_cb(view->other_pause[i], timer_other_pause_event);
            lv_obj_add_event_cb(view->other_pause[i], timer_other_pause_event,
                                LV_EVENT_CLICKED, (void *)(uintptr_t)other->id);
            lv_obj_remove_event_cb(view->other_plus[i], timer_other_plus_event);
            lv_obj_add_event_cb(view->other_plus[i], timer_other_plus_event,
                                LV_EVENT_CLICKED, (void *)(uintptr_t)other->id);
            lv_obj_remove_event_cb(view->other_cancel[i], timer_other_cancel_event);
            lv_obj_add_event_cb(view->other_cancel[i], timer_other_cancel_event,
                                LV_EVENT_CLICKED, (void *)(uintptr_t)other->id);
        } else {
            view->other_ids[i] = 0;
            lv_obj_add_flag(view->other_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (done_count > 0) {
        char title[32];
        snprintf(title, sizeof(title), "已完成（%u）", (unsigned)done_count);
        set_label_if_changed(view->done_title, title);
        lv_obj_remove_flag(view->done_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(view->done_title, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < UI_TIMER_DONE_MAX; i++) {
        if (!view->done_rows[i]) continue;
        if (i < done_count) {
            const fridge_kitchen_timer_snapshot_t *done = dones[i];
            lv_obj_remove_flag(view->done_rows[i], LV_OBJ_FLAG_HIDDEN);
            set_label_if_changed(view->done_names[i],
                                 done->label[0] ? done->label : timer_default_name());
            char total[16];
            format_clock_duration(done->duration_seconds, total, sizeof(total));
            char detail[96];
            snprintf(detail, sizeof(detail), "总时长 %s · 来源 %s", total,
                     done->source[0] ? done->source : "手动");
            set_label_if_changed(view->done_meta[i], detail);
        } else {
            lv_obj_add_flag(view->done_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (first || primary_changed || view->last_other_count != other_count ||
        view->last_done_count != done_count) {
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    view->last_primary_id = has_primary ? timer->id : 0;
    view->last_primary_state = has_primary ? timer->state : FRIDGE_KITCHEN_TIMER_IDLE;
    view->last_other_count = other_count;
    view->last_done_count = done_count;
    view->initialized = true;
}

static void refresh_portrait_alarm_card(size_t index, const fridge_kitchen_alarm_t *alarm)
{
    portrait_alarm_view_t *view = &s_shell.portrait_alarm;
    if (index >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS || !alarm) return;
    char time_text[12];
    char meta[96];
    snprintf(time_text, sizeof(time_text), "%02u:%02u", (unsigned)alarm->hour,
             (unsigned)alarm->minute);
    snprintf(meta, sizeof(meta), "%s · 来源 %s%s", alarm_repeat_text(alarm->repeat),
             alarm_source_text(alarm->source),
             alarm->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE ? " · 语音" : "");
    set_label_if_changed(view->card_times[index], time_text);
    set_label_if_changed(view->card_names[index],
                         alarm->label[0] ? alarm->label : alarm_default_name());
    set_label_if_changed(view->card_meta[index], meta);
    set_label_if_changed(view->card_kind[index], alarm_kind_text(alarm->kind));
    lv_obj_set_style_bg_color(view->card_toggles[index],
                              color_hex(alarm->enabled ? FRIDGE_UI_COLOR_SAGE :
                                                            FRIDGE_UI_COLOR_OFFLINE), 0);
    lv_obj_set_style_border_color(view->card_toggles[index],
                                  color_hex(alarm->enabled ? FRIDGE_UI_COLOR_SAGE :
                                                            FRIDGE_UI_COLOR_OFFLINE), 0);
    lv_obj_set_x(view->card_knobs[index], alarm->enabled ? 32 : 4);
    lv_obj_set_style_opa(view->card_main[index],
                         alarm->enabled ? LV_OPA_COVER : LV_OPA_60, 0);
}

static void refresh_portrait_alarm_page(void)
{
    portrait_alarm_view_t *view = &s_shell.portrait_alarm;
    if (s_shell.page != SHELL_PAGE_ALARM || !view->status) return;
    set_label_if_changed(view->status, s_shell.kitchen.time_ready ?
                         "时间已同步，可设置固定时刻和周期提醒" :
                         "时间未同步：请先连接 Wi-Fi/SNTP");

    size_t fixed_count = 0;
    size_t cycle_count = 0;
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        if (s_shell.kitchen.alarms[i].kind == FRIDGE_KITCHEN_ALARM_KIND_CYCLE) cycle_count++;
        else fixed_count++;
    }
    char title[32];
    snprintf(title, sizeof(title), "闹钟（%u）", (unsigned)fixed_count);
    set_label_if_changed(view->fixed_title, title);
    snprintf(title, sizeof(title), "周期提醒（%u）", (unsigned)cycle_count);
    set_label_if_changed(view->cycle_title, title);

    int y = 64;
    if (fixed_count == 0) {
        lv_obj_remove_flag(view->fixed_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(view->fixed_empty, y);
        y += 84;
    } else {
        lv_obj_add_flag(view->fixed_empty, LV_OBJ_FLAG_HIDDEN);
    }
    size_t slot = 0;
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        const fridge_kitchen_alarm_t *alarm = &s_shell.kitchen.alarms[i];
        if (alarm->kind != FRIDGE_KITCHEN_ALARM_KIND_FIXED || slot >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) {
            continue;
        }
        lv_obj_set_y(view->cards[slot], y);
        lv_obj_remove_flag(view->cards[slot], LV_OBJ_FLAG_HIDDEN);
        view->card_ids[slot] = alarm->id;
        refresh_portrait_alarm_card(slot, alarm);
        lv_obj_remove_event_cb(view->card_toggles[slot], alarm_toggle_event);
        lv_obj_add_event_cb(view->card_toggles[slot], alarm_toggle_event,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)alarm->id);
        y += UI_PORTRAIT_ALARM_ROW_STEP;
        slot++;
    }
    y += 4;
    lv_obj_set_y(view->cycle_title, y);
    y += 32;
    if (cycle_count == 0) {
        lv_obj_remove_flag(view->cycle_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(view->cycle_empty, y);
        y += 84;
    } else {
        lv_obj_add_flag(view->cycle_empty, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < s_shell.kitchen.alarm_count; i++) {
        const fridge_kitchen_alarm_t *alarm = &s_shell.kitchen.alarms[i];
        if (alarm->kind != FRIDGE_KITCHEN_ALARM_KIND_CYCLE || slot >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) {
            continue;
        }
        lv_obj_set_y(view->cards[slot], y);
        lv_obj_remove_flag(view->cards[slot], LV_OBJ_FLAG_HIDDEN);
        view->card_ids[slot] = alarm->id;
        refresh_portrait_alarm_card(slot, alarm);
        lv_obj_remove_event_cb(view->card_toggles[slot], alarm_toggle_event);
        lv_obj_add_event_cb(view->card_toggles[slot], alarm_toggle_event,
                            LV_EVENT_CLICKED, (void *)(uintptr_t)alarm->id);
        y += UI_PORTRAIT_ALARM_ROW_STEP;
        slot++;
    }
    for (; slot < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; slot++) {
        view->card_ids[slot] = 0;
        lv_obj_add_flag(view->cards[slot], LV_OBJ_FLAG_HIDDEN);
    }
    if (!view->initialized || view->last_count != s_shell.kitchen.alarm_count) {
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    view->last_count = s_shell.kitchen.alarm_count;
    view->initialized = true;
}

static void refresh_portrait_stopwatch_page(int64_t now_us)
{
    portrait_stopwatch_view_t *view = &s_shell.portrait_stopwatch;
    if (s_shell.page != SHELL_PAGE_STOPWATCH || !view->face) return;
    const fridge_kitchen_stopwatch_state_t state = s_shell.kitchen.stopwatch.state;
    const bool running = state == FRIDGE_KITCHEN_STOPWATCH_RUNNING;
    const bool state_changed = view->initialized && state != view->last_state;
    const bool lap_count_changed = view->initialized &&
                                   s_shell.kitchen.stopwatch.lap_count != view->last_lap_count;
    const uint64_t elapsed_ms = stopwatch_elapsed_for_ui(now_us);
    char text[32];
    format_stopwatch_elapsed(elapsed_ms, text, sizeof(text));
    set_label_if_changed(view->face, text);
    set_label_if_changed(view->state, running ? "计时中" :
                         state == FRIDGE_KITCHEN_STOPWATCH_PAUSED ? "已暂停" : "就绪");
    set_label_if_changed(view->toggle_label, running ? "暂停" : "开始");
    // 图标槽位在对象树异步重建期间可能尚未完成绑定；此时仍刷新文字和计次，
    // 不访问空槽位，避免定时器任务在方向切换窗口触发空指针。
    if (view->toggle_play_slot && view->toggle_pause_slot) {
        bool assets_ready = s_shell.assets && s_shell.assets->loaded;
        set_stopwatch_icon_visibility(view->toggle_play_slot->image,
                                      view->toggle_play_slot->fallback, !running,
                                      assets_ready && fridge_ui_assets_get_icon(
                                          s_shell.assets, FRIDGE_UI_ICON_PLAY) != NULL);
        set_stopwatch_icon_visibility(view->toggle_pause_slot->image,
                                      view->toggle_pause_slot->fallback, running,
                                      assets_ready && fridge_ui_assets_get_icon(
                                          s_shell.assets, FRIDGE_UI_ICON_PAUSE) != NULL);
    }

    const bool has_time = elapsed_ms > 0 || s_shell.kitchen.stopwatch.lap_count > 0;
    if (has_time) lv_obj_clear_state(view->reset, LV_STATE_DISABLED);
    else lv_obj_add_state(view->reset, LV_STATE_DISABLED);
    if (running || elapsed_ms > 0) lv_obj_clear_state(view->lap, LV_STATE_DISABLED);
    else lv_obj_add_state(view->lap, LV_STATE_DISABLED);
    snprintf(text, sizeof(text), "计次（%u）",
             (unsigned)s_shell.kitchen.stopwatch.lap_count);
    set_label_if_changed(view->laps_count, text);
    if (s_shell.kitchen.stopwatch.lap_count == 0) {
        lv_obj_remove_flag(view->laps_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(view->laps_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(view->laps_empty, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view->laps_list, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS; i++) {
        bool visible = i < s_shell.kitchen.stopwatch.lap_count;
        if (visible) {
            lv_obj_remove_flag(view->lap_rows[i], LV_OBJ_FLAG_HIDDEN);
            snprintf(text, sizeof(text), "计次 %u",
                     (unsigned)(s_shell.kitchen.stopwatch.lap_count - i));
            set_label_if_changed(view->lap_labels[i], text);
            format_stopwatch_elapsed(s_shell.kitchen.stopwatch.laps_ms[i], text, sizeof(text));
            set_label_if_changed(view->lap_times[i], text);
        } else {
            lv_obj_add_flag(view->lap_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!view->initialized || state_changed || lap_count_changed) {
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    view->last_state = state;
    view->last_lap_count = s_shell.kitchen.stopwatch.lap_count;
    view->initialized = true;
}

static void refresh_tool_page(void)
{
    if (!s_shell.created) return;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        if (s_shell.page == SHELL_PAGE_MORE) {
            refresh_portrait_more_page();
            refresh_more_badges();
            refresh_portrait_more_context();
            refresh_ctx_plan();
            return;
        }
        if (s_shell.page == SHELL_PAGE_TIMER) {
            refresh_portrait_timer_page();
            refresh_portrait_more_context();
            refresh_ctx_plan();
            return;
        }
        if (s_shell.page == SHELL_PAGE_ALARM) {
            refresh_portrait_alarm_page();
            refresh_portrait_more_context();
            refresh_ctx_plan();
            return;
        }
        if (s_shell.page == SHELL_PAGE_STOPWATCH) {
            refresh_portrait_stopwatch_page(esp_timer_get_time());
            refresh_portrait_more_context();
            refresh_ctx_plan();
            return;
        }
        if (s_shell.page == SHELL_PAGE_SAFETY) {
            refresh_portrait_safety_page();
            refresh_ctx_plan();
            return;
        }
        if (s_shell.page == SHELL_PAGE_SETTINGS) {
            refresh_settings_visible();
            refresh_ctx_plan();
            return;
        }
        if (s_shell.page == SHELL_PAGE_MAINTENANCE) {
            refresh_ctx_plan();
            return;
        }
    }
    if (s_shell.page == SHELL_PAGE_MORE && s_shell.more_device) {
        char summary[144];
        snprintf(summary, sizeof(summary), "Wi-Fi：%s  ·  TF：%s  ·  %s",
                 s_shell.device.wifi == FRIDGE_UI_HEALTH_OK ? "已连接" : "未连接",
                 s_shell.device.tfcard == FRIDGE_UI_HEALTH_OK ? "资源已就绪" : "资源降级",
                 s_shell.kitchen.time_ready ? "时间已同步" : "时间未同步");
        set_label_if_changed(s_shell.more_device, summary);
    } else if (s_shell.page == SHELL_PAGE_TIMER && s_shell.timer_face) {
        const fridge_kitchen_timer_snapshot_t *timer = &s_shell.kitchen.timer;
        bool has_primary = timer->id != 0 &&
                           (timer->state == FRIDGE_KITCHEN_TIMER_RUNNING ||
                            timer->state == FRIDGE_KITCHEN_TIMER_PAUSED ||
                            timer->state == FRIDGE_KITCHEN_TIMER_RINGING);
        char text[16];
        if (has_primary) {
            format_clock_duration(timer->remaining_seconds, text, sizeof(text));
            set_label_if_changed(s_shell.timer_face, text);
            if (s_shell.timer_face) {
                lv_obj_set_style_text_color(s_shell.timer_face, color_hex(FRIDGE_UI_COLOR_INK), 0);
            }
            set_label_if_changed(s_shell.timer_label,
                                 timer->label[0] ? timer->label : timer_default_name());
            if (s_shell.timer_chip) {
                if (timer->state == FRIDGE_KITCHEN_TIMER_PAUSED) {
                    set_label_if_changed(s_shell.timer_chip, "已暂停");
                } else if (timer->state == FRIDGE_KITCHEN_TIMER_RINGING) {
                    set_label_if_changed(s_shell.timer_chip, "时间到");
                } else {
                    set_label_if_changed(s_shell.timer_chip, "运行中");
                }
            }
            char total[16];
            format_clock_duration(timer->duration_seconds, total, sizeof(total));
            char sub[96];
            snprintf(sub, sizeof(sub), "总时长 %s · 来源 %s",
                     total, timer->source[0] ? timer->source : "手动");
            set_label_if_changed(s_shell.timer_state, sub);
            if (s_shell.timer_hero) {
                style_box(s_shell.timer_hero, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8, 1);
            }
            if (s_shell.timer_ops) lv_obj_remove_flag(s_shell.timer_ops, LV_OBJ_FLAG_HIDDEN);
            if (s_shell.timer_pause_label) {
                set_label_if_changed(s_shell.timer_pause_label,
                                     timer->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "继续" : "暂停");
            }
            if (s_shell.timer_pause) {
                if (timer->state == FRIDGE_KITCHEN_TIMER_RINGING) {
                    lv_obj_add_flag(s_shell.timer_pause, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_remove_flag(s_shell.timer_pause, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_shell.timer_plus) lv_obj_remove_flag(s_shell.timer_plus, LV_OBJ_FLAG_HIDDEN);
            if (s_shell.timer_cancel) {
                if (timer->state == FRIDGE_KITCHEN_TIMER_RINGING) {
                    lv_obj_add_flag(s_shell.timer_cancel, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_remove_flag(s_shell.timer_cancel, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_shell.timer_dismiss) {
                if (timer->state == FRIDGE_KITCHEN_TIMER_RINGING) {
                    lv_obj_remove_flag(s_shell.timer_dismiss, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_shell.timer_dismiss, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_shell.timer_start) lv_obj_add_flag(s_shell.timer_start, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_label_if_changed(s_shell.timer_face, "00:00");
            if (s_shell.timer_face) {
                lv_obj_set_style_text_color(s_shell.timer_face, color_hex(FRIDGE_UI_COLOR_MUTED), 0);
            }
            set_label_if_changed(s_shell.timer_label, "厨房倒计时");
            if (s_shell.timer_chip) set_label_if_changed(s_shell.timer_chip, "");
            set_label_if_changed(s_shell.timer_state, "点下方预设立即开始，或新建自定义时长");
            if (s_shell.timer_hero) {
                style_box(s_shell.timer_hero, FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_LINE, 8, 1);
            }
            if (s_shell.timer_ops) lv_obj_add_flag(s_shell.timer_ops, LV_OBJ_FLAG_HIDDEN);
            if (s_shell.timer_start) lv_obj_add_flag(s_shell.timer_start, LV_OBJ_FLAG_HIDDEN);
        }

        // 其他运行中 / 已完成
        const fridge_kitchen_timer_snapshot_t *others[UI_TIMER_OTHER_MAX];
        const fridge_kitchen_timer_snapshot_t *dones[UI_TIMER_DONE_MAX];
        size_t other_n = 0;
        size_t done_n = 0;
        for (size_t i = 0; i < s_shell.kitchen.timer_count; i++) {
            const fridge_kitchen_timer_snapshot_t *t = &s_shell.kitchen.timers[i];
            if (has_primary && t->id == timer->id) continue;
            if ((t->state == FRIDGE_KITCHEN_TIMER_RUNNING ||
                 t->state == FRIDGE_KITCHEN_TIMER_PAUSED ||
                 t->state == FRIDGE_KITCHEN_TIMER_RINGING) &&
                other_n < UI_TIMER_OTHER_MAX) {
                others[other_n++] = t;
            } else if (t->state == FRIDGE_KITCHEN_TIMER_DONE && done_n < UI_TIMER_DONE_MAX) {
                dones[done_n++] = t;
            }
        }
        int y = 352;
        if (other_n > 0 && s_shell.timer_other_title) {
            lv_obj_remove_flag(s_shell.timer_other_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_shell.timer_other_title, y);
            char title[32];
            snprintf(title, sizeof(title), "其他运行中（%u）", (unsigned)other_n);
            set_label_if_changed(s_shell.timer_other_title, title);
            y += 28;
        } else if (s_shell.timer_other_title) {
            lv_obj_add_flag(s_shell.timer_other_title, LV_OBJ_FLAG_HIDDEN);
        }
        for (size_t i = 0; i < UI_TIMER_OTHER_MAX; i++) {
            if (!s_shell.timer_other_rows[i]) continue;
            if (i < other_n) {
                const fridge_kitchen_timer_snapshot_t *t = others[i];
                s_shell.timer_other_ids[i] = t->id;
                lv_obj_remove_flag(s_shell.timer_other_rows[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_y(s_shell.timer_other_rows[i], y);
                set_label_if_changed(s_shell.timer_other_names[i],
                                     t->label[0] ? t->label : timer_default_name());
                format_clock_duration(t->remaining_seconds, text, sizeof(text));
                set_label_if_changed(s_shell.timer_other_remains[i], text);
                char total[16];
                format_clock_duration(t->duration_seconds, total, sizeof(total));
                char meta[96];
                snprintf(meta, sizeof(meta), "总时长 %s · 来源 %s · %s",
                         total, t->source[0] ? t->source : "手动",
                         t->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "已暂停" : "运行中");
                set_label_if_changed(s_shell.timer_other_meta[i], meta);
                if (s_shell.timer_other_pause_labels[i]) {
                    set_label_if_changed(s_shell.timer_other_pause_labels[i],
                                         t->state == FRIDGE_KITCHEN_TIMER_PAUSED ? "继续" : "暂停");
                }
                // 重绑 id 到按钮 user_data（每次 refresh 更新）。
                if (s_shell.timer_other_pause[i]) {
                    lv_obj_remove_event_cb(s_shell.timer_other_pause[i], timer_other_pause_event);
                    lv_obj_add_event_cb(s_shell.timer_other_pause[i], timer_other_pause_event,
                                        LV_EVENT_CLICKED, (void *)(uintptr_t)t->id);
                }
                if (s_shell.timer_other_plus[i]) {
                    lv_obj_remove_event_cb(s_shell.timer_other_plus[i], timer_other_plus_event);
                    lv_obj_add_event_cb(s_shell.timer_other_plus[i], timer_other_plus_event,
                                        LV_EVENT_CLICKED, (void *)(uintptr_t)t->id);
                }
                if (s_shell.timer_other_cancel[i]) {
                    lv_obj_remove_event_cb(s_shell.timer_other_cancel[i], timer_other_cancel_event);
                    lv_obj_add_event_cb(s_shell.timer_other_cancel[i], timer_other_cancel_event,
                                        LV_EVENT_CLICKED, (void *)(uintptr_t)t->id);
                }
                y += 118;
            } else {
                s_shell.timer_other_ids[i] = 0;
                lv_obj_add_flag(s_shell.timer_other_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (done_n > 0 && s_shell.timer_done_title) {
            lv_obj_remove_flag(s_shell.timer_done_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_shell.timer_done_title, y);
            char title[32];
            snprintf(title, sizeof(title), "已完成（%u）", (unsigned)done_n);
            set_label_if_changed(s_shell.timer_done_title, title);
            y += 28;
        } else if (s_shell.timer_done_title) {
            lv_obj_add_flag(s_shell.timer_done_title, LV_OBJ_FLAG_HIDDEN);
        }
        for (size_t i = 0; i < UI_TIMER_DONE_MAX; i++) {
            if (!s_shell.timer_done_rows[i]) continue;
            if (i < done_n) {
                const fridge_kitchen_timer_snapshot_t *t = dones[i];
                lv_obj_remove_flag(s_shell.timer_done_rows[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_y(s_shell.timer_done_rows[i], y);
                set_label_if_changed(s_shell.timer_done_names[i],
                                     t->label[0] ? t->label : timer_default_name());
                char total[16];
                format_clock_duration(t->duration_seconds, total, sizeof(total));
                char meta[96];
                snprintf(meta, sizeof(meta), "总时长 %s · 来源 %s",
                         total, t->source[0] ? t->source : "手动");
                set_label_if_changed(s_shell.timer_done_meta[i], meta);
                y += 76;
            } else {
                lv_obj_add_flag(s_shell.timer_done_rows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_shell.page_content) {
            int need_h = y + 24;
            if (need_h < 420) need_h = 420;
            if (lv_obj_get_height(s_shell.page_content) != need_h) {
                lv_obj_set_height(s_shell.page_content, need_h);
                fridge_ui_scroll_ppa_invalidate_cache();
            }
        }
    } else if (s_shell.page == SHELL_PAGE_ALARM && s_shell.alarm_status) {
        set_label_if_changed(s_shell.alarm_status, s_shell.kitchen.time_ready ?
                             "时间已同步，可设置固定时刻和周期提醒" :
                             "时间未同步：请先连接 Wi-Fi/SNTP");
        for (size_t i = 0; i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
            if (!s_shell.alarm_cards[i]) continue;
            const fridge_kitchen_alarm_t *alarm = alarm_snapshot_by_id(s_shell.alarm_card_ids[i]);
            if (alarm) refresh_alarm_card(i, alarm);
        }
    } else if (s_shell.page == SHELL_PAGE_STOPWATCH && s_shell.stopwatch_face) {
        refresh_stopwatch_page(esp_timer_get_time());
    } else if (s_shell.page == SHELL_PAGE_PLAN) {
        refresh_plan_page();
    }
    // 烹饪态侧舱依赖 kitchen 快照；无论当前主页面如何都随快照刷新。
    refresh_ctx_plan();
}

static lv_obj_t *make_health_box(lv_obj_t *parent, int x, fridge_ui_icon_id_t icon)
{
    lv_obj_t *box = make_box(parent, x, 0, 36, 36, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
    register_icon(box, icon, FRIDGE_UI_COLOR_SAGE_DARK, 4, 4, 20, "·");
    return box;
}

static void add_divider(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    clear_container_style(line);
    lv_obj_set_pos(line, 0, y);
    lv_obj_set_size(line, 198, 1);
    lv_obj_set_style_bg_color(line, color_hex(FRIDGE_UI_COLOR_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

static void make_kv(lv_obj_t *parent, int y, const char *key, lv_obj_t **out_value)
{
    make_label(parent, key, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, y, 74, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *value = make_label(parent, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK,
                                  76, y, 118, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    if (out_value) *out_value = value;
}

static void create_ctx_default(lv_obj_t *ctx)
{
    s_shell.ctx_default = lv_obj_create(ctx);
    clear_container_style(s_shell.ctx_default);
    lv_obj_set_pos(s_shell.ctx_default, 12, 10);
    lv_obj_set_size(s_shell.ctx_default, 198, 478);
    make_label(s_shell.ctx_default, "今日摘要", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 198, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    add_divider(s_shell.ctx_default, 29);
    make_kv(s_shell.ctx_default, 38, "库存", &s_shell.summary_values[0]);
    make_kv(s_shell.ctx_default, 72, "临期", &s_shell.summary_values[1]);
    make_kv(s_shell.ctx_default, 106, "过期", &s_shell.summary_values[2]);
    make_kv(s_shell.ctx_default, 140, "晚餐", &s_shell.summary_values[3]);
    // 晚餐内容按网页参考稿允许两行，避免窄状态舱把整句硬截成省略号。
    lv_label_set_long_mode(s_shell.summary_values[3], LV_LABEL_LONG_WRAP);
    lv_obj_set_height(s_shell.summary_values[3], 52);
    make_label(s_shell.ctx_default, "下一事项", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 202, 198, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    add_divider(s_shell.ctx_default, 231);
    lv_obj_t *next = make_box(s_shell.ctx_default, 0, 242, 198, 84,
                               FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_CREAM, 8);
    register_icon(next, FRIDGE_UI_ICON_TIMER, FRIDGE_UI_COLOR_SAGE_DARK, 8, 8, 22, "T");
    s_shell.next_title = make_label(next, "", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                     FRIDGE_UI_COLOR_INK, 38, 5, 80, LV_SIZE_CONTENT,
                                     LV_LABEL_LONG_DOT);
    s_shell.next_remaining = make_label(next, "", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED, 120, 5, 70,
                                         LV_SIZE_CONTENT,
                                         LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.next_remaining, LV_TEXT_ALIGN_RIGHT, 0);
    s_shell.next_source = make_label(next, "", FRIDGE_FONT_BODY_SMALL,
                                       FRIDGE_UI_COLOR_MUTED, 8, 46, 182,
                                       LV_SIZE_CONTENT,
                                       LV_LABEL_LONG_DOT);
    align_card_bottom_label(s_shell.next_source, 8, 182, 8);
    make_label(s_shell.ctx_default, "最近同步", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 340, 198, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    add_divider(s_shell.ctx_default, 369);
    s_shell.recent_sync = make_label(s_shell.ctx_default, "", FRIDGE_FONT_BODY_SMALL,
                                       FRIDGE_UI_COLOR_MUTED, 0, 378, 194, 92,
                                       LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.recent_sync);
}

// CTX-02：库存页的路径和统计在浏览/搜索/编辑之间保持连续，不借用首页摘要。
static void create_ctx_inventory(lv_obj_t *ctx)
{
    s_shell.ctx_inventory = lv_obj_create(ctx);
    clear_container_style(s_shell.ctx_inventory);
    lv_obj_set_pos(s_shell.ctx_inventory, 12, 10);
    lv_obj_set_size(s_shell.ctx_inventory, 198, 430);
    lv_obj_add_flag(s_shell.ctx_inventory, LV_OBJ_FLAG_HIDDEN);
    s_shell.ctx_inventory_browse = lv_obj_create(s_shell.ctx_inventory);
    clear_container_style(s_shell.ctx_inventory_browse);
    lv_obj_set_pos(s_shell.ctx_inventory_browse, 0, 0);
    lv_obj_set_size(s_shell.ctx_inventory_browse, 198, 390);
    make_label(s_shell.ctx_inventory_browse, "当前位置", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.ctx_inventory_browse, "CTX-02", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 146, 0, 52, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    add_divider(s_shell.ctx_inventory_browse, 29);
    s_shell.ctx_inventory_path = make_label(s_shell.ctx_inventory_browse, "冰箱", FRIDGE_FONT_BODY,
                                              FRIDGE_UI_COLOR_INK, 0, 42, 198, 52,
                                              LV_LABEL_LONG_WRAP);
    make_label(s_shell.ctx_inventory_browse, "范围统计", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 112, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    add_divider(s_shell.ctx_inventory_browse, 141);
    make_kv(s_shell.ctx_inventory_browse, 154, "食材", &s_shell.ctx_inventory_items);
    make_kv(s_shell.ctx_inventory_browse, 188, "临期", &s_shell.ctx_inventory_expiring);
    make_kv(s_shell.ctx_inventory_browse, 222, "已过期", &s_shell.ctx_inventory_expired);
    make_kv(s_shell.ctx_inventory_browse, 256, "未定位", &s_shell.ctx_inventory_unlocated);
    make_button(s_shell.ctx_inventory_browse, 0, 314, 198, 44, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "空间形状编辑",
                FRIDGE_FONT_BODY_SMALL, inventory_layout_event, NULL, NULL);

    s_shell.ctx_inventory_layout = lv_obj_create(s_shell.ctx_inventory);
    clear_container_style(s_shell.ctx_inventory_layout);
    lv_obj_set_pos(s_shell.ctx_inventory_layout, 0, 0);
    lv_obj_set_size(s_shell.ctx_inventory_layout, 198, 390);
    lv_obj_add_flag(s_shell.ctx_inventory_layout, LV_OBJ_FLAG_HIDDEN);
    make_label(s_shell.ctx_inventory_layout, "空间形状编辑",
               FRIDGE_FONT_BODY_SMALL_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
               0, 0, 136, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.ctx_inventory_layout, "CTX-02", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 146, 0, 52, LV_SIZE_CONTENT,
               LV_LABEL_LONG_CLIP);
    add_divider(s_shell.ctx_inventory_layout, 29);
    make_label(s_shell.ctx_inventory_layout, "当前层级", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 0, 46, 86, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    make_label(s_shell.ctx_inventory_layout, "分区 / 层 / 格位",
               FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK, 0, 76, 198,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.ctx_inventory_layout, "编辑状态", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 0, 124, 86, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    make_label(s_shell.ctx_inventory_layout, "本地草稿 · 可撤销",
               FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_SAGE_DARK, 0, 154, 198,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_t *safe = make_box(s_shell.ctx_inventory_layout, 0, 208, 198, 92,
                              0xE9F1F7, 0xE9F1F7, 8);
    register_icon(safe, FRIDGE_UI_ICON_SHIELD, 0x35647A, 12, 12, 20, "");
    make_label(safe, "布局变更不会静默移动已有食材；冲突需先处理。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK, 42, 10, 144, 68,
               LV_LABEL_LONG_WRAP);
    make_button(s_shell.ctx_inventory_layout, 0, 314, 198, 44,
                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                FRIDGE_UI_COLOR_LINE, "撤销未提交修改", FRIDGE_FONT_BODY_SMALL,
                inventory_layout_undo_event, NULL, NULL);
}

static void refresh_ctx_inventory(void)
{
    if (!s_shell.ctx_inventory) return;
    const bool layout_page = s_shell.page == SHELL_PAGE_INVENTORY_LAYOUT;
    if (s_shell.ctx_inventory_browse) {
        if (layout_page) lv_obj_add_flag(s_shell.ctx_inventory_browse, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_shell.ctx_inventory_browse, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shell.ctx_inventory_layout) {
        if (layout_page) lv_obj_remove_flag(s_shell.ctx_inventory_layout, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_shell.ctx_inventory_layout, LV_OBJ_FLAG_HIDDEN);
    }
    const fridge_ui_inventory_zone_t *zone =
        s_shell.inventory_scope_level >= 1 ? inventory_current_zone() : NULL;
    const fridge_ui_inventory_layer_t *layer =
        s_shell.inventory_scope_level >= 2 ? inventory_current_layer() : NULL;
    char path[96] = "冰箱";
    if (zone) {
        strlcat(path, " / ", sizeof(path));
        strlcat(path, zone->name, sizeof(path));
    }
    if (layer) {
        strlcat(path, " / ", sizeof(path));
        strlcat(path, layer->name, sizeof(path));
    }
    int expiring = 0, expired = 0, unlocated = 0;
    size_t count = inventory_scope_count(&expiring, &expired, &unlocated);
    char value[24];
    set_label_if_changed(s_shell.ctx_inventory_path, path);
    snprintf(value, sizeof(value), "%u 项", (unsigned)count);
    set_label_if_changed(s_shell.ctx_inventory_items, value);
    snprintf(value, sizeof(value), "%d 项", expiring);
    set_label_if_changed(s_shell.ctx_inventory_expiring, value);
    snprintf(value, sizeof(value), "%d 项", expired);
    set_label_if_changed(s_shell.ctx_inventory_expired, value);
    snprintf(value, sizeof(value), "%d 项", unlocated);
    set_label_if_changed(s_shell.ctx_inventory_unlocated, value);
    lv_obj_set_style_text_color(s_shell.ctx_inventory_expiring,
                                color_hex(expiring ? FRIDGE_UI_COLOR_YOLK : FRIDGE_UI_COLOR_INK), 0);
    lv_obj_set_style_text_color(s_shell.ctx_inventory_expired,
                                color_hex(expired ? FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_INK), 0);
}

// 定时器、闹钟和秒表共用浏览器参考稿中的 CTX-07 侧舱；主页面内容仍由各自页面单独维护。
static void create_ctx_tool(lv_obj_t *ctx)
{
    s_shell.ctx_tool = lv_obj_create(ctx);
    clear_container_style(s_shell.ctx_tool);
    lv_obj_set_pos(s_shell.ctx_tool, 12, 10);
    lv_obj_set_size(s_shell.ctx_tool, 198, 330);
    lv_obj_add_flag(s_shell.ctx_tool, LV_OBJ_FLAG_HIDDEN);

    make_label(s_shell.ctx_tool, "最近到期", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 136, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    s_shell.ctx_tool_id = make_label(s_shell.ctx_tool, "CTX-07", FRIDGE_FONT_BODY_SMALL,
                                     FRIDGE_UI_COLOR_LINE_STRONG, 146, 2, 52,
                                     LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_shell.ctx_tool_id, LV_TEXT_ALIGN_RIGHT, 0);
    add_divider(s_shell.ctx_tool, 29);

    s_shell.ctx_tool_face = make_label(s_shell.ctx_tool, "08:00", FRIDGE_FONT_LARGE,
                                       FRIDGE_UI_COLOR_SAGE_DARK, 0, 48, 198,
                                       LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_shell.ctx_tool_face, LV_TEXT_ALIGN_CENTER, 0);
    s_shell.ctx_tool_source = make_label(
        s_shell.ctx_tool, "煮蛋 · 来源：菜谱·番茄炒蛋", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_MUTED, 0, 116, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.ctx_tool_source, LV_TEXT_ALIGN_CENTER, 0);

    // 两个 40px 小按钮对应参考稿的暂停/结束快捷动作；动作仍复用真实定时器后端。
    s_shell.ctx_tool_pause = make_button(
        s_shell.ctx_tool, 34, 157, 58, 40, FRIDGE_UI_COLOR_SURFACE,
        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG, "暂停",
        FRIDGE_FONT_BODY_SMALL, timer_pause_event, NULL, NULL);
    s_shell.ctx_tool_cancel = make_button(
        s_shell.ctx_tool, 100, 157, 58, 40, FRIDGE_UI_COLOR_TOMATO,
        0xFFFFFF, FRIDGE_UI_COLOR_TOMATO, "结束", FRIDGE_FONT_BODY_SMALL,
        timer_cancel_event, NULL, NULL);

    s_shell.ctx_tool_all = make_button(
        s_shell.ctx_tool, 0, 207, 198, 48, FRIDGE_UI_COLOR_MINT,
        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "全部定时器",
        FRIDGE_FONT_BODY, nav_event, (void *)(uintptr_t)SHELL_PAGE_TIMER,
        &s_shell.ctx_tool_all_label);
    register_icon(s_shell.ctx_tool_all, FRIDGE_UI_ICON_TIMER,
                  FRIDGE_UI_COLOR_SAGE_DARK, 8, 12, 20, "T");
    if (s_shell.ctx_tool_all_label) {
        lv_obj_set_width(s_shell.ctx_tool_all_label, 162);
        lv_obj_align(s_shell.ctx_tool_all_label, LV_ALIGN_LEFT_MID, 28, 0);
    }
}

// CTX-04 只展示真实设备快照：图片识别阶段和失败摘要由 ui_p4 编排层提供。
static void create_ctx_camera(lv_obj_t *ctx)
{
    s_shell.ctx_camera = lv_obj_create(ctx);
    clear_container_style(s_shell.ctx_camera);
    lv_obj_set_pos(s_shell.ctx_camera, 12, 10);
    lv_obj_set_size(s_shell.ctx_camera, 198, 290);
    lv_obj_add_flag(s_shell.ctx_camera, LV_OBJ_FLAG_HIDDEN);

    make_label(s_shell.ctx_camera, "拍照状态", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 136, LV_SIZE_CONTENT,
               LV_LABEL_LONG_DOT);
    make_label(s_shell.ctx_camera, "CTX-04", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 146, 0, 52, LV_SIZE_CONTENT,
               LV_LABEL_LONG_CLIP);
    add_divider(s_shell.ctx_camera, 29);
    make_label(s_shell.ctx_camera, "取景模式", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 42, 74, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_camera_mode = make_label(s_shell.ctx_camera, "静态预览", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_SAGE_DARK, 76, 42, 122,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.ctx_camera_mode, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(s_shell.ctx_camera, "摄像头", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 76, 74, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_camera_sensor = make_label(s_shell.ctx_camera, "读取中", FRIDGE_FONT_BODY_SMALL,
                                           FRIDGE_UI_COLOR_MUTED, 76, 76, 122,
                                           LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.ctx_camera_sensor, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(s_shell.ctx_camera, "Wi-Fi", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 110, 74, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_camera_wifi = make_label(s_shell.ctx_camera, "读取中", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED, 76, 110, 122,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.ctx_camera_wifi, LV_TEXT_ALIGN_RIGHT, 0);
    make_label(s_shell.ctx_camera, "云端", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 144, 74, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_camera_cloud = make_label(s_shell.ctx_camera, "读取中", FRIDGE_FONT_BODY_SMALL,
                                          FRIDGE_UI_COLOR_MUTED, 76, 144, 122,
                                          LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.ctx_camera_cloud, LV_TEXT_ALIGN_RIGHT, 0);
    add_divider(s_shell.ctx_camera, 166);
    make_label(s_shell.ctx_camera, "识别任务", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               0, 178, 74, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_camera_task = make_label(s_shell.ctx_camera, "等待拍摄", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED, 76, 178, 122,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_shell.ctx_camera_task, LV_TEXT_ALIGN_RIGHT, 0);
    s_shell.ctx_camera_hint = make_label(s_shell.ctx_camera,
                                         "状态每秒同步；识别结果需人工确认",
                                         FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                         0, 216, 198, 52, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.ctx_camera_hint);
}

// 连续会话活动时只显示阶段与简短状态，不显示转写、回答或完整错误详情，
// 避免右侧动态舱变成第二个对话页并增加频繁重绘。
static void create_ctx_continuous_voice(lv_obj_t *ctx)
{
    s_shell.ctx_continuous_voice = lv_obj_create(ctx);
    clear_container_style(s_shell.ctx_continuous_voice);
    lv_obj_set_pos(s_shell.ctx_continuous_voice, 12, 10);
    lv_obj_set_size(s_shell.ctx_continuous_voice, 198, 210);
    lv_obj_add_flag(s_shell.ctx_continuous_voice, LV_OBJ_FLAG_HIDDEN);

    make_label(s_shell.ctx_continuous_voice, "连续对话", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    add_divider(s_shell.ctx_continuous_voice, 29);
    s_shell.ctx_continuous_voice_state = make_label(
        s_shell.ctx_continuous_voice, "正在准备", FRIDGE_FONT_BODY,
        FRIDGE_UI_COLOR_INK, 0, 44, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_continuous_voice_detail = make_label(
        s_shell.ctx_continuous_voice, "请稍候", FRIDGE_FONT_BODY_SMALL,
        FRIDGE_UI_COLOR_MUTED, 0, 82, 198, 92, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.ctx_continuous_voice_detail);
}

static void refresh_ctx_continuous_voice(void)
{
    if (!s_shell.ctx_continuous_voice_state || !s_shell.ctx_continuous_voice_detail) return;
    const fridge_ui_device_model_t *device = &s_shell.device;
    const char *state = device->continuous_voice_state[0] ?
                        device->continuous_voice_state : "正在准备";
    const char *detail = device->continuous_voice_error && device->continuous_voice_error_text[0] ?
                         device->continuous_voice_error_text :
                         (device->continuous_voice_detail[0] ?
                          device->continuous_voice_detail : "请稍候");
    set_label_if_changed(s_shell.ctx_continuous_voice_state, state);
    set_label_if_changed(s_shell.ctx_continuous_voice_detail, detail);
    lv_obj_set_style_text_color(s_shell.ctx_continuous_voice_state,
                                color_hex(device->continuous_voice_error ?
                                              FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_INK), 0);
    lv_obj_set_style_text_color(s_shell.ctx_continuous_voice_detail,
                                color_hex(device->continuous_voice_error ?
                                              FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_MUTED), 0);
}

static void set_camera_context_value(lv_obj_t *label, const char *text, uint32_t color)
{
    set_label_if_changed(label, text);
    if (label) lv_obj_set_style_text_color(label, color_hex(color), 0);
}

// 状态由 ui_p4 每秒采样后传入 Shell；本函数不访问相机/I2C，避免 UI 旁路硬件所有权。
static void refresh_ctx_camera(void)
{
    if (!s_shell.ctx_camera_mode || !s_shell.ctx_camera_sensor || !s_shell.ctx_camera_wifi ||
        !s_shell.ctx_camera_cloud || !s_shell.ctx_camera_task || !s_shell.ctx_camera_hint) {
        return;
    }

    const fridge_ui_device_model_t *device = &s_shell.device;
    const char *camera_text = "未启用";
    uint32_t camera_color = FRIDGE_UI_COLOR_MUTED;
    if (device->camera == FRIDGE_UI_HEALTH_OK) {
        camera_text = "已初始化";
        camera_color = FRIDGE_UI_COLOR_SAGE;
    } else if (device->camera == FRIDGE_UI_HEALTH_DANGER) {
        camera_text = "异常";
        camera_color = FRIDGE_UI_COLOR_TOMATO;
    }
    const char *wifi_text = device->wifi == FRIDGE_UI_HEALTH_OK ? "已连接" :
                            device->wifi == FRIDGE_UI_HEALTH_WARN ? "连接中" : "未连接";
    uint32_t wifi_color = health_color(device->wifi);
    const char *cloud_text = device->cloud == FRIDGE_UI_HEALTH_OK ? "可上传" :
                             device->cloud == FRIDGE_UI_HEALTH_WARN ? "等待网络" : "离线";
    uint32_t cloud_color = health_color(device->cloud);
    const bool has_photo = device->camera_has_frame;

    set_camera_context_value(s_shell.ctx_camera_mode, "静态预览", FRIDGE_UI_COLOR_SAGE_DARK);
    set_camera_context_value(s_shell.ctx_camera_sensor, camera_text, camera_color);
    set_camera_context_value(s_shell.ctx_camera_wifi, wifi_text, wifi_color);
    set_camera_context_value(s_shell.ctx_camera_cloud, cloud_text, cloud_color);
    const bool confirming = s_shell.page == SHELL_PAGE_INVENTORY_RECOGNIZE;
    const char *task_text = confirming ? "等待确认" :
                            device->camera_ai_state[0] ? device->camera_ai_state :
                            has_photo ? "已有照片" : "等待拍摄";
    const uint32_t task_color = device->camera_ai_error ? FRIDGE_UI_COLOR_TOMATO :
                                (confirming || device->camera_ai_active || has_photo) ?
                                    FRIDGE_UI_COLOR_SAGE_DARK : FRIDGE_UI_COLOR_MUTED;
    set_camera_context_value(s_shell.ctx_camera_task,
                             task_text, task_color);
    set_label_if_changed(s_shell.ctx_camera_hint,
                         confirming ?
                         "逐项确认后才写入库存；取消不会产生库存变更。" :
                         device->camera_ai_detail[0] ?
                         device->camera_ai_detail :
                         device->camera == FRIDGE_UI_HEALTH_OK ?
                         "拍照后上传识别；结果需在新增库存页确认。" :
                         "未启动摄像头，状态每秒自动同步");
    lv_obj_set_style_text_color(s_shell.ctx_camera_hint,
                                color_hex(device->camera_ai_error ?
                                              FRIDGE_UI_COLOR_TOMATO :
                                              FRIDGE_UI_COLOR_MUTED), 0);
}

static void refresh_camera_live_status(void)
{
    if (!s_shell.camera_network_dot || !s_shell.camera_network_text) return;
    const fridge_ui_device_model_t *device = &s_shell.device;
    const char *text = "Wi-Fi 未连接 · 云端离线 · 等待拍摄";
    uint32_t color = FRIDGE_UI_COLOR_OFFLINE;
    char connected[96];
    if (device->wifi == FRIDGE_UI_HEALTH_OK) {
        if (device->wifi_rssi < 0) {
            snprintf(connected, sizeof(connected), "Wi-Fi %ddBm · %s · %s",
                     (int)device->wifi_rssi,
                     device->cloud == FRIDGE_UI_HEALTH_OK ? "云端可用" : "云端等待",
                     device->camera_has_frame ? "已有照片" : "等待拍摄");
        } else {
            snprintf(connected, sizeof(connected), "Wi-Fi 已连接 · %s · %s",
                     device->cloud == FRIDGE_UI_HEALTH_OK ? "云端可用" : "云端等待",
                     device->camera_has_frame ? "已有照片" : "等待拍摄");
        }
        text = connected;
        color = FRIDGE_UI_COLOR_SAGE;
    } else if (device->wifi == FRIDGE_UI_HEALTH_WARN) {
        text = "Wi-Fi 连接中 · 云端等待 · 等待拍摄";
        color = FRIDGE_UI_COLOR_YOLK;
    }
    set_label_if_changed(s_shell.camera_network_text, text);
    lv_obj_set_style_bg_color(s_shell.camera_network_dot, color_hex(color), 0);
}

// 页面专属侧舱只改变可见层，不销毁对象，避免频繁切页造成 TF 图标槽位和 LVGL 对象抖动。
static bool page_uses_tool_context(void)
{
    return s_shell.page == SHELL_PAGE_TIMER || s_shell.page == SHELL_PAGE_TIMER_EDITOR ||
           s_shell.page == SHELL_PAGE_ALARM || s_shell.page == SHELL_PAGE_ALARM_EDITOR ||
           s_shell.page == SHELL_PAGE_STOPWATCH;
}

static bool page_uses_plan_context(void)
{
    return s_shell.page == SHELL_PAGE_PLAN || s_shell.page == SHELL_PAGE_SHOPPING;
}

static bool page_uses_inventory_context(void)
{
    return s_shell.page == SHELL_PAGE_INVENTORY ||
           s_shell.page == SHELL_PAGE_INVENTORY_DETAIL ||
           s_shell.page == SHELL_PAGE_INVENTORY_EDIT ||
           s_shell.page == SHELL_PAGE_INVENTORY_SEARCH ||
           s_shell.page == SHELL_PAGE_INVENTORY_LAYOUT;
}

static void refresh_cabin_context_visibility(void)
{
    if (!s_shell.ctx_default || !s_shell.ctx_inventory || !s_shell.ctx_tool ||
        !s_shell.ctx_camera || !s_shell.ctx_plan || !s_shell.ctx_continuous_voice) return;
    bool camera = s_shell.page == SHELL_PAGE_CAMERA ||
                  s_shell.page == SHELL_PAGE_INVENTORY_RECOGNIZE;
    bool tool = page_uses_tool_context();
    bool inventory = !camera && !tool && page_uses_inventory_context();
    bool plan = !camera && !tool && !inventory && page_uses_plan_context();
    // 右侧动态上下文仅由页面决定。语音状态改用根浮层展示，不能遮掉原有上下文。
    lv_obj_add_flag(s_shell.ctx_default, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.ctx_inventory, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.ctx_tool, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.ctx_camera, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.ctx_plan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_shell.ctx_continuous_voice, LV_OBJ_FLAG_HIDDEN);
    if (s_shell.device.continuous_voice_active) {
        refresh_ctx_continuous_voice();
        lv_obj_remove_flag(s_shell.ctx_continuous_voice, LV_OBJ_FLAG_HIDDEN);
    } else if (camera) {
        lv_obj_remove_flag(s_shell.ctx_camera, LV_OBJ_FLAG_HIDDEN);
    } else if (inventory) {
        refresh_ctx_inventory();
        lv_obj_remove_flag(s_shell.ctx_inventory, LV_OBJ_FLAG_HIDDEN);
    } else if (tool) {
        lv_obj_remove_flag(s_shell.ctx_tool, LV_OBJ_FLAG_HIDDEN);
    } else if (plan) {
        lv_obj_remove_flag(s_shell.ctx_plan, LV_OBJ_FLAG_HIDDEN);
        refresh_ctx_plan();
    } else {
        lv_obj_remove_flag(s_shell.ctx_default, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_voice_popup(void)
{
    if (!s_shell.voice_popup || !s_shell.voice_popup_state || !s_shell.voice_popup_detail) {
        return;
    }
    if (!s_shell.device.voice_active) {
        lv_obj_add_flag(s_shell.voice_popup, LV_OBJ_FLAG_HIDDEN);
        if (s_shell.voice_popup_pulse_ring) {
            lv_obj_add_flag(s_shell.voice_popup_pulse_ring, LV_OBJ_FLAG_HIDDEN);
        }
        s_shell.voice_popup_pulse_started_us = 0;
        return;
    }

    // 无实时文本流时，以大号“聆听中”作为主视觉，并在 tick 中循环 1/2/3 个点。
    if (s_shell.device.voice_recording) {
        s_shell.voice_popup_dot_count = 1;
        s_shell.voice_popup_dot_deadline_us = esp_timer_get_time() + UI_VOICE_POPUP_DOT_INTERVAL_US;
        if (s_shell.voice_popup_pulse_started_us == 0) {
            s_shell.voice_popup_pulse_started_us = esp_timer_get_time();
        }
        set_label_if_changed(s_shell.voice_popup_state, "聆听中 .");
        // 聆听态不显示副文案，标题以卡片垂直中线为基准；完成态再恢复为上标题+说明的布局。
        lv_obj_set_pos(s_shell.voice_popup_state, 164, 56);
        lv_obj_add_flag(s_shell.voice_popup_detail, LV_OBJ_FLAG_HIDDEN);
        if (s_shell.voice_popup_pulse_ring) {
            lv_obj_remove_flag(s_shell.voice_popup_pulse_ring, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        s_shell.voice_popup_pulse_started_us = 0;
        set_label_if_changed(s_shell.voice_popup_state,
                             s_shell.device.voice_state[0] ? s_shell.device.voice_state : "正在准备");
        lv_obj_set_pos(s_shell.voice_popup_state, 164, 28);
        lv_obj_set_pos(s_shell.voice_popup_detail, 166, 88);
        if (s_shell.device.voice_can_submit) {
            set_label_if_changed(s_shell.voice_popup_detail, "录音已完成，可提交转写给助手");
        } else {
            set_label_if_changed(s_shell.voice_popup_detail, "请稍候，音频与网络任务正在处理");
        }
        lv_obj_remove_flag(s_shell.voice_popup_detail, LV_OBJ_FLAG_HIDDEN);
        if (s_shell.voice_popup_pulse_ring) {
            lv_obj_add_flag(s_shell.voice_popup_pulse_ring, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_shell.voice_popup_submit) {
        if (s_shell.device.voice_can_submit) lv_obj_remove_flag(s_shell.voice_popup_submit, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_shell.voice_popup_submit, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_shell.voice_popup_cancel) {
        if (s_shell.device.voice_can_cancel) lv_obj_remove_flag(s_shell.voice_popup_cancel, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_shell.voice_popup_cancel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_shell.voice_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shell.voice_popup);
}

// CTX-07 优先显示真实运行中的定时器；没有运行实例时沿用首页“下一事项”示例，保持参考稿首屏可读。
static void refresh_ctx_tool(void)
{
    if (!s_shell.ctx_tool_face || !s_shell.ctx_tool_source) return;
    bool has_timer = s_shell.kitchen.timer.state == FRIDGE_KITCHEN_TIMER_RUNNING ||
                     s_shell.kitchen.timer.state == FRIDGE_KITCHEN_TIMER_PAUSED ||
                     s_shell.kitchen.timer.state == FRIDGE_KITCHEN_TIMER_RINGING;
    char face[16];
    if (has_timer) {
        format_clock_duration(s_shell.kitchen.timer.remaining_seconds, face, sizeof(face));
    } else if (s_shell.home.summaries.next_remaining[0]) {
        strlcpy(face, s_shell.home.summaries.next_remaining, sizeof(face));
    } else {
        strlcpy(face, "08:00", sizeof(face));
    }
    set_label_if_changed(s_shell.ctx_tool_face, face);

    const char *name = has_timer && s_shell.kitchen.timer.label[0] ?
                       s_shell.kitchen.timer.label :
                       (s_shell.home.summaries.next_title[0] ?
                        s_shell.home.summaries.next_title : "煮蛋");
    const char *source = s_shell.home.summaries.next_source[0] ?
                         s_shell.home.summaries.next_source : "菜谱·番茄炒蛋";
    char line[96];
    strlcpy(line, name, sizeof(line));
    strlcat(line, " · 来源：", sizeof(line));
    strlcat(line, source, sizeof(line));
    set_label_if_changed(s_shell.ctx_tool_source, line);
}

static void refresh_ctx_plan(void)
{
    if (!s_shell.ctx_plan_dinner) return;
    set_label_if_changed(s_shell.ctx_plan_dinner, s_shell.plan.ctx_dinner);
    set_label_if_changed(s_shell.ctx_plan_shopping, s_shell.plan.ctx_shopping_count);
    set_label_if_changed(s_shell.ctx_plan_expiring, s_shell.plan.ctx_expiring_count);
    if (!s_shell.ctx_plan_cooking_section) return;
    bool cooking = (s_shell.kitchen.timer.state == FRIDGE_KITCHEN_TIMER_RUNNING ||
                    s_shell.kitchen.timer.state == FRIDGE_KITCHEN_TIMER_PAUSED) &&
                   s_shell.plan.ctx_cooking_recipe[0];
    if (cooking) {
        lv_obj_remove_flag(s_shell.ctx_plan_cooking_section, LV_OBJ_FLAG_HIDDEN);
        char sub[64];
        snprintf(sub, sizeof(sub), "%s · 第 %d/%d 步",
                 s_shell.plan.ctx_cooking_recipe,
                 s_shell.plan.ctx_cooking_step_cur,
                 s_shell.plan.ctx_cooking_step_total);
        set_label_if_changed(s_shell.ctx_plan_cooking_sub, sub);
        set_label_if_changed(s_shell.ctx_plan_cooking_step, s_shell.plan.ctx_cooking_step);
        if (s_shell.ctx_plan_cooking_bar && s_shell.plan.ctx_cooking_step_total > 0) {
            int32_t pct = (int32_t)s_shell.plan.ctx_cooking_step_cur * 100 /
                          s_shell.plan.ctx_cooking_step_total;
            lv_bar_set_value(s_shell.ctx_plan_cooking_bar, pct, LV_ANIM_OFF);
        }
    } else {
        lv_obj_add_flag(s_shell.ctx_plan_cooking_section, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_ctx_plan(lv_obj_t *ctx)
{
    s_shell.ctx_plan = lv_obj_create(ctx);
    clear_container_style(s_shell.ctx_plan);
    lv_obj_set_pos(s_shell.ctx_plan, 12, 10);
    lv_obj_set_size(s_shell.ctx_plan, 198, 478);
    lv_obj_add_flag(s_shell.ctx_plan, LV_OBJ_FLAG_HIDDEN);

    make_label(s_shell.ctx_plan, "计划摘要", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    add_divider(s_shell.ctx_plan, 29);
    make_kv(s_shell.ctx_plan, 38, "晚餐", &s_shell.ctx_plan_dinner);
    make_kv(s_shell.ctx_plan, 72, "购物", &s_shell.ctx_plan_shopping);
    make_kv(s_shell.ctx_plan, 106, "临期", &s_shell.ctx_plan_expiring);

    // 烹饪态区块：仅在有运行中定时器且 plan 有烹饪步骤时显示。
    s_shell.ctx_plan_cooking_section = lv_obj_create(s_shell.ctx_plan);
    clear_container_style(s_shell.ctx_plan_cooking_section);
    lv_obj_set_pos(s_shell.ctx_plan_cooking_section, 0, 148);
    lv_obj_set_size(s_shell.ctx_plan_cooking_section, 198, 200);
    lv_obj_add_flag(s_shell.ctx_plan_cooking_section, LV_OBJ_FLAG_HIDDEN);

    add_divider(s_shell.ctx_plan_cooking_section, 0);
    make_label(s_shell.ctx_plan_cooking_section, "烹饪中", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 0, 10, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_plan_cooking_sub = make_label(
        s_shell.ctx_plan_cooking_section, "", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        0, 36, 198, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.ctx_plan_cooking_step = make_label(
        s_shell.ctx_plan_cooking_section, "", FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK,
        0, 60, 198, 52, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.ctx_plan_cooking_step);

    s_shell.ctx_plan_cooking_bar = lv_bar_create(s_shell.ctx_plan_cooking_section);
    lv_obj_remove_style_all(s_shell.ctx_plan_cooking_bar);
    lv_obj_set_pos(s_shell.ctx_plan_cooking_bar, 0, 122);
    lv_obj_set_size(s_shell.ctx_plan_cooking_bar, 198, 8);
    lv_obj_set_style_bg_color(s_shell.ctx_plan_cooking_bar, color_hex(FRIDGE_UI_COLOR_LINE), 0);
    lv_obj_set_style_bg_opa(s_shell.ctx_plan_cooking_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_shell.ctx_plan_cooking_bar, 4, 0);
    lv_obj_set_style_bg_color(s_shell.ctx_plan_cooking_bar,
                              color_hex(FRIDGE_UI_COLOR_SAGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_shell.ctx_plan_cooking_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_shell.ctx_plan_cooking_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(s_shell.ctx_plan_cooking_bar, 0, 100);
    lv_bar_set_value(s_shell.ctx_plan_cooking_bar, 0, LV_ANIM_OFF);

    make_button(s_shell.ctx_plan_cooking_section, 0, 142, 198, 40,
                FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT,
                "下一步 →", FRIDGE_FONT_BODY_SMALL, unsupported_event,
                (void *)"PLAN-05", NULL);
}

static void create_plan_page(lv_obj_t *page)
{
    const plan_business_model_t *m = &s_shell.plan;

    // 今日菜单卡按餐次纵向排布，避免 2×2 宫格中餐次标签和长菜名互相挤压。
    lv_obj_t *meal_card = make_box(page, 0, 0, 672, 196,
                                   FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_LINE, 8);
    make_label(meal_card, "今日菜单", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 16, 12, 200, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < FRIDGE_UI_PLAN_MEAL_SLOTS; i++) {
        lv_obj_t *cell = lv_obj_create(meal_card);
        clear_container_style(cell);
        lv_obj_set_pos(cell, 16, 44 + (int)i * 32);
        lv_obj_set_size(cell, 640, 32);
        make_label(cell, m->today_meals[i].slot, FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_MUTED, 0, 0, 84, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.plan_today_dishes[i] = make_label(
            cell, m->today_meals[i].dish, FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK,
            96, 0, 528, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    }

    // 两张摘要卡统一高度，给购物清单的两条预览和底部按钮留出独立行。
    lv_obj_t *exp_card = make_box(page, 0, 208, 326, 184,
                                  FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    make_label(exp_card, "临期利用", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 16, 12, 200, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < FRIDGE_UI_PLAN_EXPIRING_MAX; i++) {
        int ey = 44 + (int)i * 32;
        s_shell.plan_expiring_names[i] = make_label(
            exp_card, i < m->expiring_count ? m->expiring[i].name : "",
            FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK, 16, ey, 180, LV_SIZE_CONTENT,
            LV_LABEL_LONG_DOT);
        uint32_t pill_color = (i < m->expiring_count &&
                               m->expiring[i].tone == FRIDGE_UI_TONE_DANGER) ?
                              FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_YOLK;
        s_shell.plan_expiring_pills[i] = make_label(
            exp_card, i < m->expiring_count ? m->expiring[i].badge : "",
            FRIDGE_FONT_BODY_SMALL, pill_color, 200, ey, 110, LV_SIZE_CONTENT,
            LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_shell.plan_expiring_pills[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    // 购物摘要卡
    lv_obj_t *shop_card = make_box(page, 342, 208, 330, 184,
                                   FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 8);
    make_label(shop_card, "购物清单", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 16, 12, 200, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char shop_count_text[32];
    snprintf(shop_count_text, sizeof(shop_count_text), "待购买 %u 项",
             (unsigned)m->shopping_count);
    s_shell.plan_shopping_count = make_label(
        shop_card, shop_count_text, FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK,
        16, 44, 290, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < 2; i++) {
        s_shell.plan_shopping_items[i] = make_label(
            shop_card, i < m->shopping_count ? m->shopping[i].name : "",
            FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 16, 76 + (int)i * 24,
            290, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    }
    // 按钮放到预览列表下方；原先与第二条食材都从 y=100 开始，导致控件重叠。
    make_button(shop_card, 16, 132, 290, 36, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG,
                "去购物清单 →", FRIDGE_FONT_BODY_SMALL,
                nav_event, (void *)(uintptr_t)SHELL_PAGE_SHOPPING, NULL);

    // 本周菜单标题行
    make_label(page, "本周菜单", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 2, 404, 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.plan_week_range = make_label(
        page, m->week_range, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
        124, 404, 540, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    // 7天条带
    for (size_t i = 0; i < FRIDGE_UI_PLAN_WEEK_DAYS; i++) {
        bool today = i < FRIDGE_UI_PLAN_WEEK_DAYS && m->week[i].today;
        bool conflict = i < FRIDGE_UI_PLAN_WEEK_DAYS && m->week[i].conflict;
        uint32_t bg = today ? FRIDGE_UI_COLOR_MINT : FRIDGE_UI_COLOR_SURFACE;
        uint32_t border = today ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE;
        lv_obj_t *day = make_box(page, (int)i * 96, 444, 88, 96, bg, border, 8);
        s_shell.plan_week_boxes[i] = day;
        if (conflict) {
            make_box(day, 72, 4, 8, 8, FRIDGE_UI_COLOR_TOMATO, FRIDGE_UI_COLOR_TOMATO, 4);
        }
        make_label(day, i < FRIDGE_UI_PLAN_WEEK_DAYS ? m->week[i].date : "",
                   FRIDGE_FONT_BODY_SMALL_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
                   4, 6, 80, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        s_shell.plan_week_labels[i] = make_label(
            day, i < FRIDGE_UI_PLAN_WEEK_DAYS ? m->week[i].summary : "",
            FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 4, 30, 80, 56,
            LV_LABEL_LONG_WRAP);
        apply_compact_body_lines(s_shell.plan_week_labels[i]);
    }

    // Agent 建议卡
    lv_obj_t *tip_card = make_box(page, 0, 556, 672, 104,
                                  FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 8);
    register_icon(tip_card, FRIDGE_UI_ICON_ZAP, FRIDGE_UI_COLOR_SAGE_DARK, 16, 40, 24, "⚡");
    s_shell.plan_agent_tip = make_label(
        tip_card, m->agent_tip.text, FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK,
        52, 12, 520, 80, LV_LABEL_LONG_WRAP);
    apply_compact_body_lines(s_shell.plan_agent_tip);
    lv_obj_t *tip_btn = make_button(tip_card, 580, 32, 80, 40,
                                    FRIDGE_UI_COLOR_SAGE, 0xFFFFFF, FRIDGE_UI_COLOR_SAGE,
                                    m->agent_tip.action[0] ? m->agent_tip.action : "去确认",
                                    FRIDGE_FONT_BODY_SMALL,
                                    unsupported_event, (void *)"CHAT-04", NULL);
    (void)tip_btn;
}

static void refresh_plan_page(void)
{
    if (!s_shell.plan_initialized) return;
    const plan_business_model_t *m = &s_shell.plan;
    for (size_t i = 0; i < FRIDGE_UI_PLAN_MEAL_SLOTS; i++) {
        if (s_shell.plan_today_dishes[i]) {
            set_label_if_changed(s_shell.plan_today_dishes[i], m->today_meals[i].dish);
        }
        if (s_shell.portrait_plan.today_dishes[i]) {
            set_label_if_changed(s_shell.portrait_plan.today_dishes[i],
                                 m->today_meals[i].dish);
        }
    }
    for (size_t i = 0; i < FRIDGE_UI_PLAN_EXPIRING_MAX; i++) {
        if (s_shell.plan_expiring_names[i]) {
            set_label_if_changed(s_shell.plan_expiring_names[i],
                                 i < m->expiring_count ? m->expiring[i].name : "");
        }
        if (s_shell.plan_expiring_pills[i]) {
            set_label_if_changed(s_shell.plan_expiring_pills[i],
                                 i < m->expiring_count ? m->expiring[i].badge : "");
            uint32_t pill_color = (i < m->expiring_count &&
                                   m->expiring[i].tone == FRIDGE_UI_TONE_DANGER) ?
                                  FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_YOLK;
            lv_obj_set_style_text_color(s_shell.plan_expiring_pills[i],
                                        color_hex(pill_color), 0);
        }
        if (s_shell.portrait_plan.expiring_names[i]) {
            set_label_if_changed(s_shell.portrait_plan.expiring_names[i],
                                 i < m->expiring_count ? m->expiring[i].name : "");
        }
        if (s_shell.portrait_plan.expiring_pills[i]) {
            set_label_if_changed(s_shell.portrait_plan.expiring_pills[i],
                                 i < m->expiring_count ? m->expiring[i].badge : "");
            uint32_t pill_color = (i < m->expiring_count &&
                                   m->expiring[i].tone == FRIDGE_UI_TONE_DANGER) ?
                                  FRIDGE_UI_COLOR_TOMATO : FRIDGE_UI_COLOR_YOLK;
            lv_obj_set_style_text_color(s_shell.portrait_plan.expiring_pills[i],
                                        color_hex(pill_color), 0);
        }
    }
    if (s_shell.plan_shopping_count) {
        char buf[32];
        snprintf(buf, sizeof(buf), "待购买 %u 项", (unsigned)m->shopping_count);
        set_label_if_changed(s_shell.plan_shopping_count, buf);
    }
    for (size_t i = 0; i < 2; i++) {
        if (s_shell.plan_shopping_items[i]) {
            set_label_if_changed(s_shell.plan_shopping_items[i],
                                 i < m->shopping_count ? m->shopping[i].name : "");
        }
    }
    if (s_shell.portrait_plan.shopping_values[0]) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u",
                 (unsigned)count_plan_shopping_group(m, "待购买"));
        set_label_if_changed(s_shell.portrait_plan.shopping_values[0], buf);
    }
    if (s_shell.portrait_plan.shopping_values[1]) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u",
                 (unsigned)count_plan_shopping_group(m, "建议购买"));
        set_label_if_changed(s_shell.portrait_plan.shopping_values[1], buf);
    }
    if (s_shell.plan_week_range) {
        set_label_if_changed(s_shell.plan_week_range, m->week_range);
    }
    for (size_t i = 0; i < FRIDGE_UI_PLAN_WEEK_DAYS; i++) {
        if (s_shell.plan_week_labels[i]) {
            set_label_if_changed(s_shell.plan_week_labels[i], m->week[i].summary);
        }
        if (s_shell.plan_week_boxes[i]) {
            bool today = m->week[i].today;
            lv_obj_set_style_bg_color(s_shell.plan_week_boxes[i],
                                      color_hex(today ? FRIDGE_UI_COLOR_MINT :
                                                FRIDGE_UI_COLOR_SURFACE), 0);
            lv_obj_set_style_border_color(s_shell.plan_week_boxes[i],
                                          color_hex(today ? FRIDGE_UI_COLOR_SAGE :
                                                    FRIDGE_UI_COLOR_LINE), 0);
        }
        if (s_shell.portrait_plan.week_dates[i]) {
            set_label_if_changed(s_shell.portrait_plan.week_dates[i], m->week[i].date);
            lv_obj_set_style_text_color(s_shell.portrait_plan.week_dates[i],
                                        color_hex(m->week[i].today ?
                                                  FRIDGE_UI_COLOR_SAGE_DARK :
                                                  FRIDGE_UI_COLOR_MUTED), 0);
        }
        if (s_shell.portrait_plan.week_labels[i]) {
            set_label_if_changed(s_shell.portrait_plan.week_labels[i], m->week[i].summary);
        }
        if (s_shell.portrait_plan.week_boxes[i]) {
            lv_obj_set_style_bg_color(s_shell.portrait_plan.week_boxes[i],
                                      color_hex(m->week[i].today ?
                                                FRIDGE_UI_COLOR_MINT :
                                                FRIDGE_UI_COLOR_SURFACE), 0);
            lv_obj_set_style_border_color(s_shell.portrait_plan.week_boxes[i],
                                          color_hex(m->week[i].today ?
                                                    FRIDGE_UI_COLOR_SAGE :
                                                    FRIDGE_UI_COLOR_LINE), 0);
        }
        if (s_shell.portrait_plan.week_conflict_dots[i]) {
            if (m->week[i].conflict) {
                lv_obj_remove_flag(s_shell.portrait_plan.week_conflict_dots[i],
                                   LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_shell.portrait_plan.week_conflict_dots[i],
                                LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_shell.plan_agent_tip) {
        set_label_if_changed(s_shell.plan_agent_tip, m->agent_tip.text);
    }
    if (s_shell.portrait_plan.agent_tip) {
        set_label_if_changed(s_shell.portrait_plan.agent_tip, m->agent_tip.text);
    }
    refresh_header_sub();
    refresh_ctx_plan();
}

// PLAN-06 的业务动作全部回交 ui_p4；保存未成功前 Shell 不改变本地模型或页面分组。
static bool shopping_dispatch(fridge_ui_shell_shopping_action_t action,
                              const fridge_ui_shell_shopping_request_t *request)
{
    if (!s_shell.actions.shopping_action) {
        fridge_ui_shell_show_toast("购物清单尚未就绪");
        return false;
    }
    esp_err_t ret = s_shell.actions.shopping_action(s_shell.actions.user_ctx, action, request);
    if (ret == ESP_OK) return true;
    if (ret == ESP_ERR_NO_MEM) fridge_ui_shell_show_toast("购物清单最多 8 项");
    else if (ret == ESP_ERR_NOT_FOUND &&
             (action == FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PENDING ||
              action == FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PURCHASED)) {
        fridge_ui_shell_show_toast("选中物品已在目标分组");
    }
    else if (ret == ESP_ERR_NOT_FOUND) fridge_ui_shell_show_toast("目标物品已不存在");
    else fridge_ui_shell_show_toast("保存失败，已保留原清单");
    return false;
}

static void shopping_toggle_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const fridge_ui_shopping_item_t *item = lv_event_get_user_data(event);
    if (item >= s_shell.shopping_model.items &&
        item < s_shell.shopping_model.items + s_shell.shopping_model.item_count) {
        size_t index = (size_t)(item - s_shell.shopping_model.items);
        s_shell.shopping_selected[index] = !s_shell.shopping_selected[index];
        const bool selected = s_shell.shopping_selected[index];
        lv_obj_t *check = s_shell.shopping_checks[index];
        if (check) {
            lv_obj_set_style_bg_color(check, color_hex(selected ? FRIDGE_UI_COLOR_SAGE :
                                                        FRIDGE_UI_COLOR_SURFACE), 0);
            lv_obj_set_style_border_color(check, color_hex(selected ? FRIDGE_UI_COLOR_SAGE :
                                                            FRIDGE_UI_COLOR_LINE), 0);
        }
        set_icon_slot_visible(s_shell.shopping_check_icons[index], selected);
        // 仅让后续空闲期重建 PPA 快照；当前滚动位置和页面对象保持不变。
        fridge_ui_scroll_ppa_invalidate_cache();
    }
}

static void shopping_move_selected_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const bool purchased = (bool)(uintptr_t)lv_event_get_user_data(event);
    char item_ids[FRIDGE_UI_SHOPPING_ITEMS_MAX][24] = {{0}};
    size_t count = 0;
    for (size_t i = 0; i < s_shell.shopping_model.item_count; i++) {
        if (!s_shell.shopping_selected[i]) continue;
        strlcpy(item_ids[count++], s_shell.shopping_model.items[i].id, sizeof(item_ids[0]));
    }
    if (count == 0) {
        fridge_ui_shell_show_toast("请先勾选物品");
        return;
    }
    fridge_ui_shell_shopping_request_t request = {
        .item_ids = item_ids,
        .item_id_count = count,
    };
    if (shopping_dispatch(purchased ? FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PURCHASED :
                                     FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PENDING,
                          &request)) {
        fridge_ui_shell_show_toast(purchased ? "已加入已购买" : "已加入待购买");
    }
}

static void shopping_to_inventory_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    fridge_ui_shell_show_toast("转为入库接口尚未接通");
}

static void shopping_sort_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    s_shell.shopping_sort_mode = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    // 当前模型没有可落地的分类排序字段；仅保留参考稿的按分类演示选中态。
    refresh_shopping_page();
}

static void close_shopping_dialogs(void)
{
    fridge_ime_clear_preedit();
    if (s_shell.shopping_clear_scrim) lv_obj_delete_async(s_shell.shopping_clear_scrim);
    if (s_shell.shopping_clear_dialog) lv_obj_delete_async(s_shell.shopping_clear_dialog);
    if (s_shell.shopping_add_scrim) lv_obj_delete_async(s_shell.shopping_add_scrim);
    if (s_shell.shopping_add_dialog) lv_obj_delete_async(s_shell.shopping_add_dialog);
    s_shell.shopping_clear_scrim = NULL;
    s_shell.shopping_clear_dialog = NULL;
    s_shell.shopping_add_scrim = NULL;
    s_shell.shopping_add_dialog = NULL;
    s_shell.shopping_textarea = NULL;
    s_shell.shopping_ime_preedit = NULL;
    memset(s_shell.shopping_ime_candidates, 0, sizeof(s_shell.shopping_ime_candidates));
}

static void shopping_clear_cancel_event(lv_event_t *event)
{
    (void)event;
    close_shopping_dialogs();
}

static void shopping_clear_confirm_event(lv_event_t *event)
{
    (void)event;
    if (shopping_dispatch(FRIDGE_UI_SHELL_SHOPPING_CLEAR_DONE, NULL)) {
        fridge_ui_shell_show_toast("已清空已完成");
    }
    close_shopping_dialogs();
}

static void shopping_clear_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event) || s_shell.shopping_clear_dialog) return;
    s_shell.shopping_clear_scrim = make_box(s_shell.root, 0, 0, 1024, UI_SCREEN_H,
                                            FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.shopping_clear_scrim, LV_OPA_50, 0);
    s_shell.shopping_clear_dialog = make_box(s_shell.root, 180, 190, 344, 192,
                                             FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.shopping_clear_dialog, "清空已完成？", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_INK, 20, 20, 304, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.shopping_clear_dialog, "此操作只会移除已购买项目。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 20, 62, 304,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(s_shell.shopping_clear_dialog, 22, 126, 136, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消", FRIDGE_FONT_BODY,
                shopping_clear_cancel_event, NULL, NULL);
    make_button(s_shell.shopping_clear_dialog, 184, 126, 138, 46, FRIDGE_UI_COLOR_TOMATO,
                0xFFFFFF, FRIDGE_UI_COLOR_TOMATO, "确认清空", FRIDGE_FONT_BODY,
                shopping_clear_confirm_event, NULL, NULL);
    lv_obj_move_foreground(s_shell.shopping_clear_scrim);
    lv_obj_move_foreground(s_shell.shopping_clear_dialog);
}

static void refresh_shopping_ime(void)
{
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    set_label_if_changed(s_shell.shopping_ime_preedit,
                         snapshot.preedit[0] ? snapshot.preedit : "输入拼音后选择候选词");
    for (size_t i = 0; i < FRIDGE_IME_MAX_CANDIDATES; i++) {
        set_label_if_changed(s_shell.shopping_ime_candidates[i],
                             i < snapshot.candidate_count ? snapshot.candidates[i] : "");
    }
}

static void shopping_text_key_event(lv_event_t *event)
{
    fridge_ime_feed_letter((char)(uintptr_t)lv_event_get_user_data(event));
    refresh_shopping_ime();
}

static void shopping_text_backspace_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_backspace();
    refresh_shopping_ime();
}

static void shopping_text_candidate_event(lv_event_t *event)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (fridge_ime_select_candidate(index) == ESP_OK) refresh_shopping_ime();
}

static void shopping_add_cancel_event(lv_event_t *event)
{
    (void)event;
    close_shopping_dialogs();
}

static void shopping_add_confirm_event(lv_event_t *event)
{
    (void)event;
    const char *name = s_shell.shopping_textarea ? lv_textarea_get_text(s_shell.shopping_textarea) : NULL;
    if (!name || !name[0]) {
        fridge_ui_shell_show_toast("请输入物品名称");
        return;
    }
    fridge_ui_shell_shopping_request_t request = {.name = name};
    if (shopping_dispatch(FRIDGE_UI_SHELL_SHOPPING_ADD_MANUAL, &request)) {
        fridge_ui_shell_show_toast("已添加到待购买");
        close_shopping_dialogs();
    }
}

static void shopping_add_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event) || s_shell.shopping_add_dialog) return;
    s_shell.shopping_add_scrim = make_box(s_shell.root, 0, 0, 1024, UI_SCREEN_H,
                                          FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.shopping_add_scrim, LV_OPA_50, 0);
    s_shell.shopping_add_dialog = make_box(s_shell.root, 48, 62, 608, 470,
                                           FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.shopping_add_dialog, "添加物品", FRIDGE_FONT_TITLE, FRIDGE_UI_COLOR_INK,
               20, 16, 568, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.shopping_textarea = lv_textarea_create(s_shell.shopping_add_dialog);
    lv_obj_set_pos(s_shell.shopping_textarea, 20, 52);
    lv_obj_set_size(s_shell.shopping_textarea, 568, 52);
    lv_textarea_set_one_line(s_shell.shopping_textarea, true);
    lv_textarea_set_max_length(s_shell.shopping_textarea, 47);
    lv_obj_add_style(s_shell.shopping_textarea, fridge_ui_theme_font_style(FRIDGE_FONT_BODY), 0);
    (void)fridge_ime_set_candidate_page_size(FRIDGE_IME_MAX_CANDIDATES);
    (void)fridge_ime_attach_textarea(s_shell.shopping_textarea);
    fridge_ime_clear_preedit();
    s_shell.shopping_ime_preedit = make_label(s_shell.shopping_add_dialog, "",
                                               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                               20, 112, 568, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    for (size_t i = 0; i < FRIDGE_IME_MAX_CANDIDATES; i++) {
        int row = (int)(i / 6U), column = (int)(i % 6U);
        make_button(s_shell.shopping_add_dialog, 20 + column * 94, 142 + row * 34, 88, 30,
                    FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_CREAM,
                    "", FRIDGE_FONT_BODY_SMALL, shopping_text_candidate_event,
                    (void *)(uintptr_t)i, &s_shell.shopping_ime_candidates[i]);
    }
    static const char *rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (size_t row = 0; row < 3; row++) {
        int x = row == 0 ? 20 : row == 1 ? 48 : 84;
        for (size_t i = 0; rows[row][i]; i++) {
            char letter[2] = {rows[row][i], '\0'};
            make_button(s_shell.shopping_add_dialog, x + (int)i * 56, 226 + (int)row * 42,
                        52, 34, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                        FRIDGE_UI_COLOR_LINE, letter, FRIDGE_FONT_BODY_SMALL,
                        shopping_text_key_event, (void *)(uintptr_t)rows[row][i], NULL);
        }
    }
    make_button(s_shell.shopping_add_dialog, 486, 310, 102, 34, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "退格", FRIDGE_FONT_BODY_SMALL,
                shopping_text_backspace_event, NULL, NULL);
    make_button(s_shell.shopping_add_dialog, 330, 392, 116, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消", FRIDGE_FONT_BODY,
                shopping_add_cancel_event, NULL, NULL);
    make_button(s_shell.shopping_add_dialog, 458, 392, 130, 46, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "确定", FRIDGE_FONT_BODY,
                shopping_add_confirm_event, NULL, NULL);
    refresh_shopping_ime();
    lv_obj_move_foreground(s_shell.shopping_add_scrim);
    lv_obj_move_foreground(s_shell.shopping_add_dialog);
}

static void create_shopping_footer(void)
{
    s_shell.shopping_footer = make_box(s_shell.main, 0, 527, UI_MAIN_W, 73,
                                       FRIDGE_UI_COLOR_PAPER, FRIDGE_UI_COLOR_LINE, 0);
    lv_obj_set_style_border_side(s_shell.shopping_footer, LV_BORDER_SIDE_TOP, 0);
    // 添加入口是创建新数据，和右侧已选项目的批量状态操作明显分组。
    lv_obj_t *add = make_button(s_shell.shopping_footer, 16, 12, 132, 48,
                                FRIDGE_UI_COLOR_SAGE, 0xFFFFFF, FRIDGE_UI_COLOR_SAGE,
                                "添加物品", FRIDGE_FONT_BODY, shopping_add_event, NULL, NULL);
    register_icon(add, FRIDGE_UI_ICON_PLUS, 0xFFFFFF, 12, 9, 18, "+");
    // 批量动作统一放入右侧浅奶油容器，避免被误认为是“添加物品”的附属动作。
    make_box(s_shell.shopping_footer, 252, 8, 436, 57, FRIDGE_UI_COLOR_CREAM,
             FRIDGE_UI_COLOR_CREAM, 12);
    make_button(s_shell.shopping_footer, 260, 13, 132, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "加入待购买",
                FRIDGE_FONT_BODY_SMALL, shopping_move_selected_event, (void *)(uintptr_t)false, NULL);
    make_button(s_shell.shopping_footer, 404, 13, 132, 46, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "加入已购买",
                FRIDGE_FONT_BODY_SMALL, shopping_move_selected_event, (void *)(uintptr_t)true, NULL);
    make_button(s_shell.shopping_footer, 548, 13, 132, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_TOMATO, FRIDGE_UI_COLOR_TOMATO, "清空已完成",
                FRIDGE_FONT_BODY_SMALL, shopping_clear_event, NULL, NULL);
}

// 业务 source 可能来自旧 JSON 或 AI 输出，这里统一成用户可理解的四种来源标签。
static void shopping_source_pill(const fridge_ui_shopping_item_t *item, const char **label,
                                 uint32_t *background, uint32_t *foreground)
{
    const char *source = item ? item->source : "";
    if (strcmp(source, "菜谱缺失") == 0 || strcmp(source, "菜单计划") == 0 ||
        strcmp(source, "来自菜谱") == 0) {
        *label = "来自菜谱";
        *background = FRIDGE_UI_COLOR_MINT;
        *foreground = FRIDGE_UI_COLOR_SAGE_DARK;
    } else if (strcmp(source, "低库存") == 0) {
        *label = "低库存";
        *background = FRIDGE_UI_COLOR_YOLK;
        *foreground = FRIDGE_UI_COLOR_INK;
    } else if (strcmp(source, "device_ai") == 0 || strcmp(source, "AI建议") == 0 ||
               strcmp(source, "Agent建议") == 0 || strcmp(source, "Agent 建议") == 0 ||
               strcmp(source, "小精灵建议") == 0 || strcmp(source, "小精灵 建议") == 0) {
        *label = "小精灵建议";
        *background = FRIDGE_UI_COLOR_SAGE;
        *foreground = 0xFFFFFF;
    } else {
        *label = "手动添加";
        *background = FRIDGE_UI_COLOR_CREAM;
        *foreground = FRIDGE_UI_COLOR_TOMATO;
    }
}

static void create_shopping_page(lv_obj_t *page)
{
    const shopping_business_model_t *m = &s_shell.shopping_model;
    make_label(page, "本地清单已保存 · 暂未接入小程序同步", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 8, 0, 640, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_box(page, 0, 28, 672, 1, FRIDGE_UI_COLOR_LINE, FRIDGE_UI_COLOR_LINE, 0);
    make_label(page, "排序", FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               8, 42, 44, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    static const char *sorts[] = {"默认", "按分类"};
    for (size_t i = 0; i < 2; i++) {
        bool selected = s_shell.shopping_sort_mode == i;
        make_button(page, 60 + (int)i * 84, 34, 80,
                    34, selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_SURFACE,
                    selected ? 0xFFFFFF : FRIDGE_UI_COLOR_SAGE_DARK,
                    selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE,
                    sorts[i], FRIDGE_FONT_BODY_SMALL, shopping_sort_event,
                    (void *)(uintptr_t)i, NULL);
    }
    int y = 88;
    const char *groups[] = {"待购买", "建议购买", "已购买"};
    for (size_t group = 0; group < 3; group++) {
        size_t count = 0;
        for (size_t i = 0; i < m->item_count; i++) {
            if (strcmp(m->items[i].group, groups[group]) == 0) count++;
        }
        char heading[48];
        snprintf(heading, sizeof(heading), "%s（%u）", groups[group], (unsigned)count);
        make_label(page, heading, FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_SAGE_DARK,
                   8, y, 220, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        if (group == 1) make_label(page, "小精灵 / 低库存建议", FRIDGE_FONT_BODY_SMALL,
                                   FRIDGE_UI_COLOR_MUTED, 158, y, 220, LV_SIZE_CONTENT,
                                   LV_LABEL_LONG_DOT);
        y += 30;
        for (size_t i = 0; i < m->item_count; i++) {
            const fridge_ui_shopping_item_t *item = &m->items[i];
            if (strcmp(item->group, groups[group]) != 0) continue;
            lv_obj_t *row = make_box(page, 8, y, 656, 72, FRIDGE_UI_COLOR_SURFACE,
                                     FRIDGE_UI_COLOR_LINE, 8);
            s_shell.shopping_rows[i] = row;
            bool selected = s_shell.shopping_selected[i];
            lv_obj_t *check = make_button(row, 16, 20, 32, 32,
                                          selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_SURFACE,
                                          0xFFFFFF, selected ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_LINE,
                                          "", FRIDGE_FONT_BODY_SMALL, shopping_toggle_event,
                                          (void *)item, NULL);
            s_shell.shopping_checks[i] = check;
            size_t icon_slot_before = s_shell.icon_slot_count;
            // A8 图标实际边框为 28px，置于 32px 按钮的 (2,2) 才是严格居中。
            register_icon(check, FRIDGE_UI_ICON_CHECK, 0xFFFFFF, 2, 2, 28, "√");
            if (s_shell.icon_slot_count > icon_slot_before) {
                s_shell.shopping_check_icons[i] = &s_shell.icon_slots[icon_slot_before];
                set_icon_slot_visible(s_shell.shopping_check_icons[i], selected);
            }
            s_shell.shopping_names[i] = make_label(row, item->name, FRIDGE_FONT_BODY_EMPHASIS,
                                                    FRIDGE_UI_COLOR_INK, 62, 10, 228,
                                                    LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            char meta[128];
            snprintf(meta, sizeof(meta), "%s · 关联：%s", item->quantity,
                     item->relation[0] ? item->relation : "—");
            s_shell.shopping_meta[i] = make_label(row, meta, FRIDGE_FONT_BODY_SMALL,
                                                   FRIDGE_UI_COLOR_MUTED, 62, 40, 260,
                                                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            const char *source_label = NULL;
            uint32_t source_background = 0;
            uint32_t source_foreground = 0;
            shopping_source_pill(item, &source_label, &source_background, &source_foreground);
            lv_obj_t *source = make_box(row, 332, 22, 96, 28, source_background,
                                         source_background, 14);
            s_shell.shopping_groups[i] = make_label(source, source_label, FRIDGE_FONT_BODY_SMALL,
                                                     source_foreground, 4, 0, 88,
                                                     LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            center_single_line_label(s_shell.shopping_groups[i], 88);
            if (group == 2) {
                make_button(row, 532, 18, 108, 36, FRIDGE_UI_COLOR_SURFACE,
                            FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "转为入库",
                            FRIDGE_FONT_BODY_SMALL, shopping_to_inventory_event, NULL, NULL);
            }
            y += 80;
        }
        if (count == 0) {
            make_label(page, group == 0 ? "没有待买物品" :
                       group == 1 ? "暂无建议" : "还没有已完成的物品",
                       FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 8, y, 300,
                       LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
            y += 36;
        }
        y += 12;
    }
    create_shopping_footer();
}

static void refresh_shopping_page(void)
{
    if (s_shell.page == SHELL_PAGE_SHOPPING) {
        // 勾选会改变分组和行数量，安全重建当前滚动页，避免残留旧行的点击绑定。
        switch_page(SHELL_PAGE_SHOPPING);
    }
}

static void create_cabin(lv_obj_t *root)
{
    lv_obj_t *cabin = make_box(root, UI_MAIN_W + UI_NAV_W, 0, UI_CABIN_W, UI_SCREEN_H,
                               FRIDGE_UI_COLOR_CREAM, FRIDGE_UI_COLOR_LINE, 0);
    s_shell.cabin = cabin;
    lv_obj_set_style_border_side(cabin, LV_BORDER_SIDE_LEFT, 0);
    s_shell.time = make_label(cabin, "--:--", FRIDGE_FONT_LARGE_STRONG, FRIDGE_UI_COLOR_INK,
                                12, 1, 130, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    s_shell.date = make_label(cabin, "未校时", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                               FRIDGE_UI_COLOR_INK, 146, 12, 90, LV_SIZE_CONTENT,
                               LV_LABEL_LONG_DOT);
    s_shell.weekday = make_label(cabin, "", FRIDGE_FONT_BODY_SMALL,
                                   FRIDGE_UI_COLOR_MUTED, 146, 35, 90, LV_SIZE_CONTENT,
                                   LV_LABEL_LONG_DOT);
    register_icon(cabin, FRIDGE_UI_ICON_TEMP, FRIDGE_UI_COLOR_SAGE_DARK, 12, 69, 18, "°");
    s_shell.temperature = make_label(cabin, "--.-°C", FRIDGE_FONT_BODY_SMALL,
                                      FRIDGE_UI_COLOR_SAGE_DARK, 36, 68, 72,
                                      LV_SIZE_CONTENT,
                                      LV_LABEL_LONG_DOT);
    register_icon(cabin, FRIDGE_UI_ICON_DROP, FRIDGE_UI_COLOR_SAGE_DARK, 110, 69, 18, "%");
    s_shell.humidity = make_label(cabin, "--%", FRIDGE_FONT_BODY_SMALL,
                                   FRIDGE_UI_COLOR_SAGE_DARK, 134, 68, 70,
                                   LV_SIZE_CONTENT,
                                   LV_LABEL_LONG_DOT);
    const fridge_ui_icon_id_t health_icons[] = {
        FRIDGE_UI_ICON_WIFI, FRIDGE_UI_ICON_CLOUD, FRIDGE_UI_ICON_TF,
        FRIDGE_UI_ICON_CAMERA, FRIDGE_UI_ICON_MIC,
    };
    lv_obj_t *health_row = lv_obj_create(cabin);
    clear_container_style(health_row);
    lv_obj_set_pos(health_row, 12, 100);
    lv_obj_set_size(health_row, 224, 40);
    for (size_t i = 0; i < 5; i++) {
        s_shell.health_boxes[i] = make_health_box(health_row, (int)i * 43, health_icons[i]);
        // 状态点归属各自图标并收进边界内，避免在状态舱顶部形成一排“悬空黑点”。
        s_shell.health_dots[i] = make_box(s_shell.health_boxes[i], 27, 1, 8, 8,
                                           FRIDGE_UI_COLOR_OFFLINE,
                                           FRIDGE_UI_COLOR_PAPER, 4);
    }

    lv_obj_t *ctx = make_box(cabin, 12, 148, 224, 376, FRIDGE_UI_COLOR_SURFACE,
                             FRIDGE_UI_COLOR_LINE, 8);
    s_shell.context_scroll = ctx;
    lv_obj_add_flag(ctx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ctx, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ctx, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(ctx, main_scroll_activity_event, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(ctx, main_scroll_activity_event, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(ctx, main_scroll_activity_event, LV_EVENT_SCROLL_END, NULL);
    create_ctx_default(ctx);
    create_ctx_inventory(ctx);
    create_ctx_tool(ctx);
    create_ctx_camera(ctx);
    create_ctx_plan(ctx);
    create_ctx_continuous_voice(ctx);

    lv_obj_t *voice = make_button(cabin, 12, 528, 106, 64, FRIDGE_UI_COLOR_SAGE,
                                  0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "语音",
                                  FRIDGE_FONT_BODY_SMALL_EMPHASIS, voice_event, NULL,
                                  &s_shell.voice_button_label);
    register_icon(voice, FRIDGE_UI_ICON_MIC, 0xFFFFFF, 39, 4, 24, "M");
    lv_obj_set_width(s_shell.voice_button_label, 98);
    set_single_line_content_height(s_shell.voice_button_label);
    lv_obj_align(s_shell.voice_button_label, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_t *camera_text = NULL;
    lv_obj_t *camera = make_button(cabin, 130, 528, 106, 64, FRIDGE_UI_COLOR_SURFACE,
                                   FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE_STRONG,
                                    "拍照", FRIDGE_FONT_BODY_SMALL_EMPHASIS,
                                    camera_event, NULL,
                                    &camera_text);
    register_icon(camera, FRIDGE_UI_ICON_CAMERA, FRIDGE_UI_COLOR_SAGE_DARK,
                   39, 4, 24, "C");
    if (camera_text) {
        lv_obj_set_width(camera_text, 98);
        set_single_line_content_height(camera_text);
        lv_obj_align(camera_text, LV_ALIGN_BOTTOM_MID, 0, -3);
    }
}

// 参考稿语音卡以 root 为父级，不依赖右侧状态舱。这样浮层出现时，CTX-01/CTX-04/CTX-06
// 仍保留原有状态与滚动位置；结束后只隐藏浮层，不需要重建上下文。
static void create_voice_popup(lv_obj_t *root)
{
    // 固定在主内容区底部 10px，既贴近参考稿的位置，也不覆盖右侧动态上下文舱。
    s_shell.voice_popup = make_box(root, 36, UI_SCREEN_H - 170, 636, 160,
                                   FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    lv_obj_add_flag(s_shell.voice_popup, LV_OBJ_FLAG_HIDDEN);

    make_box(s_shell.voice_popup, 12, 16, 128, 128,
             FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 64);

    // 层级必须位于白色底圈之上、固定绿色圆之下：扩张部分和中心圆连成实心圆，
    // 避免白色底圈遮住填充区域后，画面只剩外侧的一圈线。
    s_shell.voice_popup_pulse_ring = make_box(s_shell.voice_popup, 24, 28, 104, 104,
                                              FRIDGE_UI_COLOR_SAGE,
                                              FRIDGE_UI_COLOR_SAGE, 52);
    lv_obj_set_style_border_width(s_shell.voice_popup_pulse_ring, 0, 0);
    lv_obj_set_style_outline_width(s_shell.voice_popup_pulse_ring, 0, 0);
    lv_obj_set_style_outline_opa(s_shell.voice_popup_pulse_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_shell.voice_popup_pulse_ring, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_shell.voice_popup_pulse_ring, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *mic_inner = make_box(s_shell.voice_popup, 24, 28, 104, 104,
                                   FRIDGE_UI_COLOR_SAGE, FRIDGE_UI_COLOR_SAGE, 52);
    // register_icon 的布局框固定为 28px，缩放围绕该框中心进行；按 28px 框居中，
    // 才能让放大后的 44px 麦克风真正落在 104px 圆心。
    register_icon(mic_inner, FRIDGE_UI_ICON_MIC, 0xFFFFFF, 38, 38, 44, "M");
    center_header_icon(mic_inner);

    s_shell.voice_popup_state = make_label(s_shell.voice_popup, "聆听中 .",
                                            FRIDGE_FONT_SECTION_STRONG,
                                            FRIDGE_UI_COLOR_SAGE_DARK, 164, 56, 200,
                                            LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.voice_popup_detail = make_label(s_shell.voice_popup, "",
                                             FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                             166, 88, 198, LV_SIZE_CONTENT,
                                             LV_LABEL_LONG_DOT);
    s_shell.voice_popup_cancel = make_button(s_shell.voice_popup, 374, 54, 104, 54,
                                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                              FRIDGE_UI_COLOR_LINE_STRONG, "取消",
                                              FRIDGE_FONT_BODY_EMPHASIS,
                                              voice_popup_cancel_event, NULL, NULL);
    s_shell.voice_popup_submit = make_button(s_shell.voice_popup, 488, 54, 132, 54,
                                              FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                                              FRIDGE_UI_COLOR_SAGE, "提交",
                                              FRIDGE_FONT_BODY_EMPHASIS,
                                              voice_popup_submit_event, NULL,
                                              &s_shell.voice_popup_submit_label);
    // 让“对钩 + 提交”作为一个视觉组居中：原来文字独自居中而图标偏左，视觉上会显得歪。
    if (s_shell.voice_popup_submit_label) {
        lv_obj_set_width(s_shell.voice_popup_submit_label, 62);
        center_single_line_label(s_shell.voice_popup_submit_label, 62);
        lv_obj_align(s_shell.voice_popup_submit_label, LV_ALIGN_CENTER, 6, 0);
    }
    register_icon(s_shell.voice_popup_submit, FRIDGE_UI_ICON_CHECK, 0xFFFFFF, 25, 13, 20, "√");
}

static void refresh_header_sub(void)
{
    char text[128];
    const char *date = s_shell.device.time_valid ? s_shell.device.date_text : "未校时";
    if (s_shell.page == SHELL_PAGE_HOME_DETAIL) {
        strlcpy(text, "临期 · 推荐 · 缺料 · 动态一览", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_STOPWATCH) {
        strlcpy(text, "计次 · 复位", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_TIMER) {
        strlcpy(text, "倒计时 · 厨房计时", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_TIMER_EDITOR) {
        strlcpy(text, "新建或修改倒计时", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_ALARM) {
        strlcpy(text, "固定时刻 · 周期提醒", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_ALARM_EDITOR) {
        strlcpy(text, "固定时刻或周期提醒", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_CAMERA) {
        strlcpy(text, "将食材摆在画面中央", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_INVENTORY) {
        const fridge_ui_inventory_zone_t *zone =
            s_shell.inventory_scope_level >= 1 ? inventory_current_zone() : NULL;
        const fridge_ui_inventory_layer_t *layer =
            s_shell.inventory_scope_level >= 2 ? inventory_current_layer() : NULL;
        if (layer) snprintf(text, sizeof(text), "%s · %s", zone->name, layer->name);
        else if (zone) strlcpy(text, zone->name, sizeof(text));
        else strlcpy(text, "全冰箱", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_INVENTORY_DETAIL) {
        strlcpy(text, "核对数量、到期日期和存放位置", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_INVENTORY_EDIT) {
        strlcpy(text, "逐项填写登记信息", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_INVENTORY_RECOGNIZE) {
        strlcpy(text, "核对候选并确认入库", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_INVENTORY_SEARCH) {
        strlcpy(text, "按名称、临期和位置筛选", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_INVENTORY_LAYOUT) {
        strlcpy(text, "草稿先保存，确认后再修改空间", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_SHOPPING) {
        size_t pending = 0;
        for (size_t i = 0; i < s_shell.shopping_model.item_count; i++) {
            if (strcmp(s_shell.shopping_model.items[i].group, "待购买") == 0) pending++;
        }
        snprintf(text, sizeof(text), "%u 项待买 · 已保存到本机", (unsigned)pending);
    } else if (s_shell.page == SHELL_PAGE_PLAN) {
        snprintf(text, sizeof(text), "%s · 计划总览",
                 s_shell.plan.week_range[0] ? s_shell.plan.week_range : date);
    } else if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT) {
        strlcpy(text, "与小精灵讨论库存、菜谱和计划", sizeof(text));
    } else if (s_shell.page == SHELL_PAGE_MORE) {
        snprintf(text, sizeof(text), "%s %s · 时间工具", date,
                 s_shell.device.weekday_text);
    } else if (s_shell.resource_ready) {
        snprintf(text, sizeof(text), "%s %s · 冰箱一切正常", date,
                 s_shell.device.weekday_text);
    } else if (strncmp(s_shell.resource_detail, "等待", strlen("等待")) == 0) {
        snprintf(text, sizeof(text), "%s %s · TF 资源加载中", date,
                 s_shell.device.weekday_text);
    } else {
        snprintf(text, sizeof(text), "%s %s · TF 资源降级", date,
                 s_shell.device.weekday_text);
    }
    set_label_if_changed(s_shell.header_sub, text);
    // 竖屏对象树独立于横屏页头；方向切换或设备日期更新时同步首页副标题。
    set_label_if_changed(s_shell.portrait_header_sub, text);
}

esp_err_t fridge_ui_shell_create(lv_obj_t *root, const home_business_model_t *home,
                                 const fridge_ui_shell_actions_t *actions)
{
    if (!root || !home || !actions || s_shell.created) return ESP_ERR_INVALID_ARG;
    memset(&s_shell, 0, sizeof(s_shell));
    s_shell.root = root;
    s_shell.actions = *actions;
    s_shell.home = *home;
    s_shell.page = SHELL_PAGE_HOME;
    s_shell.orientation = FRIDGE_UI_P4_ORIENTATION_LANDSCAPE;
    s_shell.timer_selected_minutes = 1;
    s_shell.alarm_hour = 7;
    s_shell.alarm_minute = 0;
    s_shell.resource_ready = false;
    strlcpy(s_shell.resource_detail, "等待 TF 资源", sizeof(s_shell.resource_detail));
    // 库存模型从已有同步快照读取；首次无数据时由模型写入可编辑的参考库存。
    if (fridge_ui_inventory_model_load(&s_shell.inventory) == ESP_OK) {
        s_shell.inventory_initialized = true;
    }
    s_shell.inventory_recognize_keep[0] = true;
    s_shell.inventory_recognize_keep[1] = true;
    s_shell.inventory_recognize_keep[2] = false;
    // CHAT-01 只有一个可恢复 foreground 会话；初始化时绝不注入参考页演示消息。
    fridge_ui_chat_model_init_foreground(&s_shell.chat);
    s_shell.chat_initialized = false;
    s_shell.chat_ime_open = false;
    clear_container_style(root);
    lv_obj_set_style_bg_color(root, color_hex(FRIDGE_UI_COLOR_PAPER), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, color_hex(FRIDGE_UI_COLOR_INK), 0);
    lv_obj_set_size(root, 1024, 600);

    s_shell.main = make_box(root, 0, 0, UI_MAIN_W, UI_SCREEN_H,
                            FRIDGE_UI_COLOR_PAPER, FRIDGE_UI_COLOR_PAPER, 0);
    create_header(s_shell.main);
    create_home(s_shell.main);
    create_nav(root);
    create_cabin(root);
    create_voice_popup(root);

    s_shell.toast = make_box(root, 280, 16, 464, 52, FRIDGE_UI_COLOR_INK,
                             FRIDGE_UI_COLOR_INK, 8);
    lv_obj_set_style_bg_opa(s_shell.toast, LV_OPA_90, 0);
    s_shell.toast_label = make_label(s_shell.toast, "", FRIDGE_FONT_BODY_SMALL,
                                      0xFFFFFF, 14, 0, 436, LV_SIZE_CONTENT,
                                      LV_LABEL_LONG_DOT);
    center_single_line_label(s_shell.toast_label, 436);
    lv_obj_add_flag(s_shell.toast, LV_OBJ_FLAG_HIDDEN);
    // OV-09 到时浮层挂 root，跨页可弹，不依赖定时器页是否打开。
    create_timer_ring_overlay(root);
    s_shell.created = true;
    fridge_ui_shell_update_home(home);
    refresh_header_sub();
    return ESP_OK;
}

void fridge_ui_shell_bind_assets(const fridge_ui_assets_t *assets)
{
    if (!s_shell.created || !assets || !assets->loaded) return;
    s_shell.assets = assets;
    for (size_t i = 0; i < s_shell.icon_slot_count; i++) {
        icon_slot_t *slot = &s_shell.icon_slots[i];
        const lv_image_dsc_t *image = fridge_ui_assets_get_icon(assets, slot->id);
        if (!image) continue;
        lv_image_set_src(slot->image, image);
        lv_image_set_scale(slot->image, slot->scale);
        lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(slot->fallback, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < s_shell.shopping_model.item_count; i++) {
        set_icon_slot_visible(s_shell.shopping_check_icons[i], s_shell.shopping_selected[i]);
    }
    fridge_ui_shell_force_redraw();
}

void fridge_ui_shell_set_camera_review(const uint8_t *pixels, size_t len,
                                       int width, int height)
{
    if (!s_shell.created || s_shell.page != SHELL_PAGE_CAMERA ||
            !s_shell.camera_photo || !s_shell.camera_capture_button ||
            !s_shell.camera_capture_label) {
        return;
    }

    lv_image_set_src(s_shell.camera_photo, NULL);
    lv_image_cache_drop(&s_shell.camera_photo_dsc);
    memset(&s_shell.camera_photo_dsc, 0, sizeof(s_shell.camera_photo_dsc));
    const bool show = pixels && len > 0 && width > 0 && height > 0;
    if (show) {
        s_shell.camera_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_shell.camera_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_shell.camera_photo_dsc.header.w = (uint32_t)width;
        s_shell.camera_photo_dsc.header.h = (uint32_t)height;
        s_shell.camera_photo_dsc.header.stride = (uint32_t)(width * sizeof(uint16_t));
        s_shell.camera_photo_dsc.data_size = (uint32_t)len;
        s_shell.camera_photo_dsc.data = pixels;
        lv_image_set_src(s_shell.camera_photo, &s_shell.camera_photo_dsc);
        lv_obj_remove_flag(s_shell.camera_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_shell.camera_placeholder) lv_obj_add_flag(s_shell.camera_placeholder, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_shell.camera_capture_button,
                                  color_hex(FRIDGE_UI_COLOR_TOMATO), 0);
        lv_obj_set_style_border_color(s_shell.camera_capture_button,
                                      color_hex(FRIDGE_UI_COLOR_TOMATO), 0);
        set_label_if_changed(s_shell.camera_capture_label, "上传");
    } else {
        lv_obj_add_flag(s_shell.camera_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_shell.camera_placeholder) lv_obj_remove_flag(s_shell.camera_placeholder, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_shell.camera_capture_button,
                                  color_hex(FRIDGE_UI_COLOR_SAGE), 0);
        lv_obj_set_style_border_color(s_shell.camera_capture_button,
                                      color_hex(FRIDGE_UI_COLOR_SAGE), 0);
        set_label_if_changed(s_shell.camera_capture_label, "拍摄");
    }
    s_shell.camera_review_active = show;
    lv_obj_invalidate(s_shell.camera_page);
}

// HOME-01 竖屏更新只写当前竖屏对象引用；横屏对象仍由原更新路径维护，避免两套布局互相覆盖。
static void update_portrait_home_view(const home_business_model_t *home)
{
    if (!home || !s_shell.portrait_home.stats[0].number) return;
    char text[192];
    char number[32];
    for (size_t i = 0; i < FRIDGE_UI_HOME_STATS_MAX; i++) {
        snprintf(number, sizeof(number), "%d", home->stats[i].value);
        set_label_if_changed(s_shell.portrait_home.stats[i].number, number);
        set_label_if_changed(s_shell.portrait_home.stats[i].label, home->stats[i].label);
    }

    snprintf(number, sizeof(number), "全部 %d 项 >", home->stats[1].value);
    set_label_if_changed(s_shell.portrait_home.expiring_side, number);
    for (size_t i = 0; i < FRIDGE_UI_HOME_EXPIRING_MAX; i++) {
        expiring_view_t *view = &s_shell.portrait_home.expiring[i];
        if (i >= home->expiring_count) {
            lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(view->row, LV_OBJ_FLAG_HIDDEN);
        const fridge_ui_home_expiring_t *entry = &home->expiring[i];
        set_label_if_changed(view->name, entry->name);
        snprintf(text, sizeof(text), "%s · %s", entry->location, entry->quantity);
        set_label_if_changed(view->meta, text);
        set_label_if_changed(view->badge, entry->badge);
        if (entry->badge[0]) lv_obj_remove_flag(view->badge_box, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->badge_box, LV_OBJ_FLAG_HIDDEN);
        set_label_if_changed(view->expiry, entry->expiry);
        uint32_t bg = tone_background(entry->tone);
        uint32_t fg = entry->tone == FRIDGE_UI_TONE_DANGER ? FRIDGE_UI_COLOR_TOMATO :
                      FRIDGE_UI_COLOR_WARN_INK;
        lv_obj_set_style_bg_color(view->expiry_box, color_hex(bg), 0);
        lv_obj_set_style_border_color(view->expiry_box, color_hex(bg), 0);
        lv_obj_set_style_text_color(view->expiry, color_hex(fg), 0);
    }

    set_label_if_changed(s_shell.portrait_home.rec_title, home->recommendation.title);
    snprintf(text, sizeof(text), "%s · %s · 库存覆盖 %d%%",
             home->recommendation.time, home->recommendation.difficulty,
             home->recommendation.coverage);
    set_label_if_changed(s_shell.portrait_home.rec_meta, text);
    set_label_if_changed(s_shell.portrait_home.rec_ready, home->recommendation.ready);
    set_label_if_changed(s_shell.portrait_home.rec_reason, home->recommendation.reason);
    set_label_if_changed(s_shell.portrait_home.rec_primary,
                         home->recommendation.primary_action);
    set_label_if_changed(s_shell.portrait_home.rec_secondary,
                         home->recommendation.secondary_action);
    for (size_t i = 0; i < FRIDGE_UI_HOME_TIPS_MAX; i++) {
        if (i < home->agent_tip_count) {
            set_label_if_changed(s_shell.portrait_home.tip_text[i], home->agent_tips[i].text);
            set_label_if_changed(s_shell.portrait_home.tip_action[i], home->agent_tips[i].action);
        } else {
            set_label_if_changed(s_shell.portrait_home.tip_text[i], "暂无更多建议");
            set_label_if_changed(s_shell.portrait_home.tip_action[i], "知道了");
        }
    }
    set_label_if_changed(s_shell.portrait_home.dinner, home->summaries.dinner);
    set_label_if_changed(s_shell.portrait_home.shopping, home->summaries.shopping);
    set_label_if_changed(s_shell.portrait_home.sync_line, home->last_sync);

    snprintf(text, sizeof(text), "%s · 还剩 %s", home->summaries.next_title,
             home->summaries.next_remaining);
    set_label_if_changed(s_shell.portrait_home.ctx_next_title, text);
    const char *dinner = home->summaries.dinner;
    if (strncmp(dinner, "晚：", strlen("晚：")) == 0) dinner += strlen("晚：");
    char sync_time[16] = "最近";
    if (sscanf(home->last_sync, "%15s", sync_time) != 1) strlcpy(sync_time, "最近", sizeof(sync_time));
    snprintf(text, sizeof(text), "晚餐：%s · %s 已同步", dinner, sync_time);
    set_label_if_changed(s_shell.portrait_home.ctx_detail, text);
    snprintf(number, sizeof(number), "库存 %d", home->stats[0].value);
    set_label_if_changed(s_shell.portrait_home.ctx_summary_values[0], number);
    snprintf(number, sizeof(number), "临期 %d", home->stats[1].value);
    set_label_if_changed(s_shell.portrait_home.ctx_summary_values[1], number);
    snprintf(number, sizeof(number), "过期 %d", home->stats[2].value);
    set_label_if_changed(s_shell.portrait_home.ctx_summary_values[2], number);
}

void fridge_ui_shell_update_home(const home_business_model_t *home)
{
    if (!s_shell.created || !home) return;
    s_shell.home = *home;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        // 竖屏时横屏 HOME 对象树仍被隐藏；不能更新其 704px/1024px 坐标，
        // 否则 LVGL 会把超出 600px 逻辑宽度的同步区域交给 DIRECT 缓冲。
        update_portrait_home_view(home);
        return;
    }
    char number[32];
    for (size_t i = 0; i < FRIDGE_UI_HOME_STATS_MAX; i++) {
        snprintf(number, sizeof(number), "%d", home->stats[i].value);
        set_label_if_changed(s_shell.stats[i].number, number);
        set_label_if_changed(s_shell.stats[i].label, home->stats[i].label);
    }
    snprintf(number, sizeof(number), "全部 %d 项 >", home->stats[1].value);
    set_label_if_changed(s_shell.expiring_side, number);
    for (size_t i = 0; i < FRIDGE_UI_HOME_EXPIRING_MAX; i++) {
        expiring_view_t *view = &s_shell.expiring[i];
        if (i >= home->expiring_count) {
            lv_obj_add_flag(view->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(view->row, LV_OBJ_FLAG_HIDDEN);
        const fridge_ui_home_expiring_t *entry = &home->expiring[i];
        set_label_if_changed(view->name, entry->name);
        char meta[96];
        snprintf(meta, sizeof(meta), "%s · %s", entry->location, entry->quantity);
        set_label_if_changed(view->meta, meta);
        set_label_if_changed(view->badge, entry->badge);
        if (entry->badge[0]) lv_obj_remove_flag(view->badge_box, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(view->badge_box, LV_OBJ_FLAG_HIDDEN);
        set_label_if_changed(view->expiry, entry->expiry);
        uint32_t bg = tone_background(entry->tone);
        uint32_t fg = entry->tone == FRIDGE_UI_TONE_DANGER ? FRIDGE_UI_COLOR_TOMATO :
                      FRIDGE_UI_COLOR_WARN_INK;
        lv_obj_set_style_bg_color(view->expiry_box, color_hex(bg), 0);
        lv_obj_set_style_border_color(view->expiry_box, color_hex(bg), 0);
        lv_obj_set_style_text_color(view->expiry, color_hex(fg), 0);
    }
    set_label_if_changed(s_shell.rec_title, home->recommendation.title);
    char meta[64];
    snprintf(meta, sizeof(meta), "%s · %s", home->recommendation.time,
             home->recommendation.difficulty);
    set_label_if_changed(s_shell.rec_meta, meta);
    snprintf(meta, sizeof(meta), "库存覆盖 %d%%", home->recommendation.coverage);
    set_label_if_changed(s_shell.rec_coverage, meta);
    set_label_if_changed(s_shell.rec_ready, home->recommendation.ready);
    set_label_if_changed(s_shell.rec_reason, home->recommendation.reason);
    set_label_if_changed(s_shell.rec_primary, home->recommendation.primary_action);
    set_label_if_changed(s_shell.rec_secondary, home->recommendation.secondary_action);
    for (size_t i = 0; i < FRIDGE_UI_HOME_TIPS_MAX; i++) {
        if (i < home->agent_tip_count) {
            set_label_if_changed(s_shell.tip_text[i], home->agent_tips[i].text);
            set_label_if_changed(s_shell.tip_action[i], home->agent_tips[i].action);
        } else {
            set_label_if_changed(s_shell.tip_text[i], "暂无更多建议");
            set_label_if_changed(s_shell.tip_action[i], "知道了");
        }
    }
    set_label_if_changed(s_shell.dinner, home->summaries.dinner);
    set_label_if_changed(s_shell.shopping, home->summaries.shopping);
    set_label_if_changed(s_shell.sync_line, home->last_sync);
    update_portrait_home_view(home);
    snprintf(number, sizeof(number), "%d 项", home->stats[0].value);
    set_label_if_changed(s_shell.summary_values[0], number);
    snprintf(number, sizeof(number), "%d 项", home->stats[1].value);
    set_label_if_changed(s_shell.summary_values[1], number);
    snprintf(number, sizeof(number), "%d 项", home->stats[2].value);
    set_label_if_changed(s_shell.summary_values[2], number);
    set_label_if_changed(s_shell.summary_values[3], home->summaries.dinner);
    set_label_if_changed(s_shell.next_title, home->summaries.next_title);
    char remain[32];
    snprintf(remain, sizeof(remain), "%s", home->summaries.next_remaining);
    set_label_if_changed(s_shell.next_remaining, remain);
    char source[80];
    snprintf(source, sizeof(source), "来源：%s", home->summaries.next_source);
    set_label_if_changed(s_shell.next_source, source);
    set_label_if_changed(s_shell.recent_sync, home->last_sync);
    refresh_ctx_tool();
}

static uint32_t health_color(fridge_ui_health_t health)
{
    switch (health) {
        case FRIDGE_UI_HEALTH_OK: return FRIDGE_UI_COLOR_SAGE;
        case FRIDGE_UI_HEALTH_WARN: return FRIDGE_UI_COLOR_YOLK;
        case FRIDGE_UI_HEALTH_DANGER: return FRIDGE_UI_COLOR_TOMATO;
        default: return FRIDGE_UI_COLOR_OFFLINE;
    }
}

void fridge_ui_shell_update_device(const fridge_ui_device_model_t *device)
{
    if (!s_shell.created || !device) return;
    fridge_ui_device_model_t previous = s_shell.device;
    bool first_update = !s_shell.device_initialized;
    bool voice_error_changed = device->voice_error &&
                               strcmp(device->voice_error_text,
                                      previous.voice_error_text) != 0;
    bool continuous_voice_error_changed = device->continuous_voice_error &&
                                          strcmp(device->continuous_voice_error_text,
                                                 previous.continuous_voice_error_text) != 0;
    set_label_if_changed(s_shell.time, device->time_text);
    set_label_if_changed(s_shell.date, device->date_text);
    set_label_if_changed(s_shell.weekday, device->weekday_text);
    set_label_if_changed(s_shell.temperature, device->temperature);
    set_label_if_changed(s_shell.humidity, device->humidity);
    set_label_if_changed(s_shell.portrait_time, device->time_text);
    // 竖屏日期按参考稿使用 16px 灰色加重字，并与隐私胶囊底部对齐。
    char portrait_date[48];
    if (device->weekday_text[0]) {
        snprintf(portrait_date, sizeof(portrait_date), "%s · %s",
                 device->date_text, device->weekday_text);
    } else {
        strlcpy(portrait_date, device->date_text, sizeof(portrait_date));
    }
    set_label_if_changed(s_shell.portrait_date, portrait_date);
    set_label_if_changed(s_shell.portrait_temperature, device->temperature);
    set_label_if_changed(s_shell.portrait_humidity, device->humidity);
    const fridge_ui_health_t values[] = {
        device->wifi, device->cloud, device->tfcard, device->camera, device->microphone,
    };
    const fridge_ui_health_t previous_values[] = {
        previous.wifi, previous.cloud, previous.tfcard, previous.camera, previous.microphone,
    };
    for (size_t i = 0; i < 5; i++) {
        if (first_update || values[i] != previous_values[i]) {
            uint32_t color = health_color(values[i]);
            lv_obj_set_style_bg_color(s_shell.health_dots[i], color_hex(color), 0);
            if (values[i] == FRIDGE_UI_HEALTH_OFFLINE) {
                lv_obj_set_style_bg_color(s_shell.health_boxes[i],
                                          color_hex(FRIDGE_UI_COLOR_OFFLINE_BG), 0);
            } else {
                lv_obj_set_style_bg_color(s_shell.health_boxes[i],
                                          color_hex(FRIDGE_UI_COLOR_SURFACE), 0);
            }
        }
        // 竖屏绑定隐私模式：摄像头状态始终显示灰色关闭，不暴露后台硬件就绪态。
        fridge_ui_health_t portrait_health =
            i == 3U ? FRIDGE_UI_HEALTH_OFFLINE : values[i];
        uint32_t portrait_color = health_color(portrait_health);
        if (s_shell.portrait_health_dots[i]) {
            lv_obj_set_style_bg_color(s_shell.portrait_health_dots[i],
                                      color_hex(portrait_color), 0);
        }
        if (s_shell.portrait_health_boxes[i]) {
            lv_obj_set_style_bg_color(
                s_shell.portrait_health_boxes[i],
                color_hex(portrait_health == FRIDGE_UI_HEALTH_OFFLINE ?
                              FRIDGE_UI_COLOR_OFFLINE_BG :
                              FRIDGE_UI_COLOR_SURFACE),
                0);
        }
        set_icon_slot_color(
            s_shell.portrait_health_icons[i],
            portrait_health == FRIDGE_UI_HEALTH_OFFLINE ?
                FRIDGE_UI_COLOR_MUTED : FRIDGE_UI_COLOR_SAGE_DARK);
    }
    if (first_update || device->continuous_voice_active != previous.continuous_voice_active) {
        set_label_if_changed(s_shell.voice_button_label,
                             device->continuous_voice_active ? "结束" : "语音");
    }
    s_shell.device = *device;
    s_shell.device_initialized = true;
    refresh_voice_popup();
    if (voice_error_changed) {
        fridge_ui_shell_show_toast(device->voice_error_text[0] ?
                                   device->voice_error_text : "语音会话失败");
    }
    if (continuous_voice_error_changed) {
        fridge_ui_shell_show_toast(device->continuous_voice_error_text[0] ?
                                   device->continuous_voice_error_text : "连续语音会话失败");
    }
    refresh_cabin_context_visibility();
    refresh_header_sub();
    refresh_tool_page();
    refresh_ctx_camera();
    refresh_camera_live_status();
}

void fridge_ui_shell_update_safety(const fridge_ui_safety_model_t *state)
{
    if (!s_shell.created || !state) return;
    const bool changed = memcmp(&s_shell.safety, state, sizeof(*state)) != 0;
    s_shell.safety = *state;
    if (!changed) return;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        s_shell.page == SHELL_PAGE_SAFETY) {
        refresh_portrait_safety_page();
        fridge_ui_scroll_ppa_invalidate_cache();
    }
}

void fridge_ui_shell_update_settings(const fridge_ui_shell_settings_state_t *state)
{
    if (!s_shell.created || !state) return;
    const size_t next_count = state->wifi_ap_count > FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS ?
                              FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS : state->wifi_ap_count;
    const bool changed = !s_shell.settings_initialized ||
                         s_shell.settings.brightness_percent != state->brightness_percent ||
                         s_shell.settings.wake_word_user_enabled != state->wake_word_user_enabled ||
                         s_shell.settings.wake_word_listening != state->wake_word_listening ||
                         s_shell.settings.wifi_phase != state->wifi_phase ||
                         s_shell.settings.wifi_ap_count != next_count ||
                         strcmp(s_shell.settings.wake_word_detail, state->wake_word_detail) != 0 ||
                         strcmp(s_shell.settings.wifi_status, state->wifi_status) != 0 ||
                         strcmp(s_shell.settings.wifi_ssid, state->wifi_ssid) != 0 ||
                         strcmp(s_shell.settings.wifi_ip, state->wifi_ip) != 0 ||
                         memcmp(s_shell.settings.wifi_aps, state->wifi_aps,
                                next_count * sizeof(state->wifi_aps[0])) != 0;
    s_shell.settings = *state;
    s_shell.settings.wifi_ap_count = next_count;
    s_shell.settings_initialized = true;
    refresh_settings_visible();
    settings_wifi_modal_refresh_list();
    // 设置页属于静态 PPA 白名单；只有真实状态变化且没有动态弹窗时才延迟重建快照。
    if (changed && s_shell.page == SHELL_PAGE_SETTINGS && !s_shell.settings_wifi_scrim) {
        fridge_ui_scroll_ppa_invalidate_cache();
    }
}

void fridge_ui_shell_update_kitchen_tools(const fridge_kitchen_tools_snapshot_t *snapshot)
{
    if (!s_shell.created || !snapshot) return;
    uint8_t previous_timer_badge = more_running_timer_count(&s_shell.kitchen);
    uint8_t previous_alarm_badge = more_enabled_alarm_count(&s_shell.kitchen);
    bool first = !s_shell.kitchen_initialized;
    bool alert_changed = !first &&
                         strcmp(s_shell.kitchen.last_alert, snapshot->last_alert) != 0;
    bool error_changed = !first &&
                         strcmp(s_shell.kitchen.last_error, snapshot->last_error) != 0;
    bool alarms_changed = first ||
                          s_shell.kitchen.alarm_count != snapshot->alarm_count ||
                          memcmp(s_shell.kitchen.alarms, snapshot->alarms,
                                 snapshot->alarm_count * sizeof(snapshot->alarms[0])) != 0;
    s_shell.kitchen = *snapshot;
    s_shell.kitchen_initialized = true;
    uint8_t current_timer_badge = more_running_timer_count(snapshot);
    uint8_t current_alarm_badge = more_enabled_alarm_count(snapshot);
    bool more_badges_changed = previous_timer_badge != current_timer_badge ||
                               previous_alarm_badge != current_alarm_badge;
    refresh_ctx_tool();
    refresh_tool_page();
    refresh_timer_ring_overlay();
    if (s_shell.page == SHELL_PAGE_MORE && more_badges_changed) {
        refresh_more_badges();
        // 数量变化时才重建静态 PPA 快照；每秒剩余时间刷新不会反复栅格化更多页。
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    // 管理页的开关、禁用透明度和到点状态位于滚动快照中；状态变化后延迟重抓快照，
    // 让下一次滚动读取新画面，而不是冻结在切页时的旧开关位置。
    if (alarms_changed && s_shell.page == SHELL_PAGE_ALARM) {
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    // 到时和后端失败由业务快照驱动一次 Toast；重复的每秒快照不会反复打扰用户。
    // RINGING 已有 OV-09 浮层时不再重复 toast，避免叠层打扰。
    if (alert_changed && snapshot->last_alert[0] && s_shell.timer_ring_id == 0) {
        fridge_ui_shell_show_toast(snapshot->last_alert);
    } else if (error_changed && snapshot->last_error[0]) {
        fridge_ui_shell_show_toast(snapshot->last_error);
    }
}

void fridge_ui_shell_update_more(const more_business_model_t *more)
{
    if (!s_shell.created || !more) return;
    s_shell.more = *more;
    s_shell.more_initialized = true;
    if (!s_shell.timer_preset_user_selected) {
        s_shell.timer_selected_minutes = timer_preset_minutes(0);
    }
    // 静态文案只在更多入口创建时读取；资源于用户停留该页时完成加载，则局部重建该页。
    if (s_shell.page == SHELL_PAGE_MORE && s_shell.page_content) {
        lv_obj_add_flag(s_shell.page_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_shell.page_content);
        s_shell.icon_slot_count = s_shell.page_icon_slot_start;
        clear_page_references();
        s_shell.page_content = lv_obj_create(s_shell.main_scroll);
        clear_container_style(s_shell.page_content);
        lv_obj_set_pos(s_shell.page_content, 16, 12);
        // 与首次进入更多页保持同一内容高度，避免异步刷新后底部卡片溢出滚动容器，
        // 也避免 PPA 重建时得到不同的滚动几何。
        lv_obj_set_size(s_shell.page_content, 672, 790);
        s_shell.page_icon_slot_start = s_shell.icon_slot_count;
        create_more_page(s_shell.page_content);
        fridge_ui_scroll_ppa_invalidate_cache();
    }
    refresh_tool_page();
}

void fridge_ui_shell_update_plan(const plan_business_model_t *plan)
{
    if (!s_shell.created || !plan) return;
    s_shell.plan = *plan;
    s_shell.plan_initialized = true;
    refresh_plan_page();
}

void fridge_ui_shell_update_shopping(const shopping_business_model_t *shopping)
{
    if (!s_shell.created || !shopping) return;
    s_shell.shopping_model = *shopping;
    // 业务模型已写入新副本，旧行的瞬时勾选不能迁移到新分组或新索引。
    memset(s_shell.shopping_selected, 0, sizeof(s_shell.shopping_selected));
    s_shell.shopping_initialized = true;
    refresh_shopping_page();
}

void fridge_ui_shell_set_resource_state(bool ready, const char *detail)
{
    if (!s_shell.created) return;
    bool changed = ready != s_shell.resource_ready ||
                   strcmp(s_shell.resource_detail, detail ? detail : "") != 0;
    s_shell.resource_ready = ready;
    strlcpy(s_shell.resource_detail, detail ? detail : "", sizeof(s_shell.resource_detail));
    refresh_header_sub();
    if (changed && ready) fridge_ui_shell_show_toast("ver2 TF 页面资源已加载");
}

void fridge_ui_shell_show_toast(const char *text)
{
    if (!s_shell.created || !s_shell.toast || !text || !text[0]) return;
    set_label_if_changed(s_shell.toast_label, text);
    lv_obj_remove_flag(s_shell.toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shell.toast);
    s_shell.toast_deadline_us = esp_timer_get_time() + UI_TOAST_DURATION_US;
}

void fridge_ui_shell_tick(int64_t now_us)
{
    if (!s_shell.created) return;
    if (s_shell.voice_popup && !lv_obj_has_flag(s_shell.voice_popup, LV_OBJ_FLAG_HIDDEN) &&
        s_shell.device.voice_recording && now_us >= s_shell.voice_popup_dot_deadline_us) {
        char listening[24] = "聆听中 ";
        s_shell.voice_popup_dot_count = (uint8_t)(s_shell.voice_popup_dot_count % 3U + 1U);
        for (uint8_t i = 0; i < s_shell.voice_popup_dot_count; i++) {
            strlcat(listening, ".", sizeof(listening));
        }
        set_label_if_changed(s_shell.voice_popup_state, listening);
        s_shell.voice_popup_dot_deadline_us = now_us + UI_VOICE_POPUP_DOT_INTERVAL_US;
    }
    if (s_shell.voice_popup_pulse_ring && s_shell.device.voice_recording &&
        !lv_obj_has_flag(s_shell.voice_popup_pulse_ring, LV_OBJ_FLAG_HIDDEN)) {
        if (s_shell.voice_popup_pulse_started_us == 0) {
            s_shell.voice_popup_pulse_started_us = now_us;
        }
        int64_t elapsed = (now_us - s_shell.voice_popup_pulse_started_us) %
                          UI_VOICE_POPUP_PULSE_PERIOD_US;
        int pulse_size = 104 + (int)(elapsed * 48 / UI_VOICE_POPUP_PULSE_PERIOD_US);
        int pulse_offset = (pulse_size - 104) / 2;
        lv_obj_set_pos(s_shell.voice_popup_pulse_ring, 24 - pulse_offset, 28 - pulse_offset);
        lv_obj_set_size(s_shell.voice_popup_pulse_ring, pulse_size, pulse_size);
        lv_obj_set_style_radius(s_shell.voice_popup_pulse_ring, pulse_size / 2, 0);
        lv_opa_t opacity = (lv_opa_t)(LV_OPA_COVER -
                                       elapsed * (LV_OPA_COVER - LV_OPA_10) /
                                           UI_VOICE_POPUP_PULSE_PERIOD_US);
        lv_obj_set_style_bg_opa(s_shell.voice_popup_pulse_ring, opacity, 0);
    }
    // 计时值来自单调时钟；这里按 LVGL 任务节奏刷新显示，不让 1 秒业务快照限制毫秒表盘。
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        if (s_shell.page == SHELL_PAGE_STOPWATCH) {
            refresh_portrait_stopwatch_page(now_us);
        } else if (s_shell.page == SHELL_PAGE_TIMER) {
            refresh_portrait_timer_page();
            refresh_portrait_more_context();
        }
    } else {
        refresh_stopwatch_page(now_us);
    }
    if (s_shell.alarm_pending && now_us >= s_shell.alarm_pending_confirm_us) {
        bool pointer_down = s_shell.scroll_press_indev &&
                            lv_indev_get_state(s_shell.scroll_press_indev) ==
                                LV_INDEV_STATE_PRESSED;
        if (pointer_down) {
            // 触摸控制器可能短暂误报 RELEASED；确认按键抬起后再处理业务动作。
            s_shell.alarm_pending_confirm_us = now_us + 50000LL;
        } else {
            alarm_pending_action_t action = s_shell.alarm_pending_action;
            uint8_t id = s_shell.alarm_pending_id;
            intptr_t value = s_shell.alarm_pending_value;
            bool editor_action = action >= ALARM_PENDING_EDITOR_ADJUST;
            bool confirmed = !s_shell.main_scroll_dragging && !s_shell.scroll_press_moved &&
                             s_shell.page == (editor_action ? SHELL_PAGE_ALARM_EDITOR :
                                                              SHELL_PAGE_ALARM);
            cancel_pending_alarm_action();
            if (confirmed && action == ALARM_PENDING_NEW) {
                memset(&s_shell.alarm_draft, 0, sizeof(s_shell.alarm_draft));
                s_shell.alarm_draft.kind = FRIDGE_KITCHEN_ALARM_KIND_FIXED;
                s_shell.alarm_draft.repeat = FRIDGE_KITCHEN_ALARM_REPEAT_DAILY;
                s_shell.alarm_draft.weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
                s_shell.alarm_draft.sound = FRIDGE_KITCHEN_ALARM_SOUND_CHIME;
                s_shell.alarm_draft.source = FRIDGE_KITCHEN_ALARM_SOURCE_MANUAL;
                s_shell.alarm_draft.hour = 7;
                s_shell.alarm_draft.minute = 30;
                strlcpy(s_shell.alarm_draft.label, alarm_default_name(),
                        sizeof(s_shell.alarm_draft.label));
                strlcpy(s_shell.alarm_draft.speech_text, "闹钟时间到了",
                        sizeof(s_shell.alarm_draft.speech_text));
                s_shell.alarm_editing_id = 0;
                switch_page(SHELL_PAGE_ALARM_EDITOR);
            } else if (confirmed && action == ALARM_PENDING_EDIT) {
                const fridge_kitchen_alarm_t *alarm = alarm_snapshot_by_id(id);
                if (alarm) {
                    alarm_draft_from_alarm(alarm);
                    s_shell.alarm_editing_id = id;
                    switch_page(SHELL_PAGE_ALARM_EDITOR);
                }
            } else if (confirmed && action == ALARM_PENDING_TOGGLE) {
                const fridge_kitchen_alarm_t *alarm = alarm_snapshot_by_id(id);
                if (alarm) {
                    fridge_ui_shell_kitchen_request_t request = {
                        .alarm_id = id,
                        .alarm_enabled = !alarm->enabled,
                    };
                    (void)dispatch_kitchen_action(FRIDGE_UI_SHELL_KITCHEN_ALARM_SET_ENABLED,
                                                  &request,
                                                  alarm->enabled ? "闹钟已关闭" : "闹钟已启用",
                                                  "闹钟开关更新失败");
                }
            } else if (confirmed && action == ALARM_PENDING_EDITOR_ADJUST) {
                if (value == -60) s_shell.alarm_draft.hour =
                    (uint8_t)((s_shell.alarm_draft.hour + 23U) % 24U);
                else if (value == 60) s_shell.alarm_draft.hour =
                    (uint8_t)((s_shell.alarm_draft.hour + 1U) % 24U);
                else if (value == -1) s_shell.alarm_draft.minute =
                    (uint8_t)((s_shell.alarm_draft.minute + 55U) % 60U);
                else if (value == 1) s_shell.alarm_draft.minute =
                    (uint8_t)((s_shell.alarm_draft.minute + 5U) % 60U);
                switch_page(SHELL_PAGE_ALARM_EDITOR);
            } else if (confirmed && action == ALARM_PENDING_EDITOR_KIND) {
                s_shell.alarm_draft.kind = (fridge_kitchen_alarm_kind_t)value;
                switch_page(SHELL_PAGE_ALARM_EDITOR);
            } else if (confirmed && action == ALARM_PENDING_EDITOR_REPEAT) {
                s_shell.alarm_draft.repeat = (fridge_kitchen_alarm_repeat_t)value;
                if (s_shell.alarm_draft.repeat == FRIDGE_KITCHEN_ALARM_REPEAT_DAILY) {
                    s_shell.alarm_draft.weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
                } else if (s_shell.alarm_draft.repeat == FRIDGE_KITCHEN_ALARM_REPEAT_WEEKDAYS) {
                    s_shell.alarm_draft.weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_WORKDAYS;
                } else if (s_shell.alarm_draft.repeat == FRIDGE_KITCHEN_ALARM_REPEAT_SUNDAY) {
                    s_shell.alarm_draft.weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_SUNDAY;
                } else if (s_shell.alarm_draft.weekday_mask == 0) {
                    s_shell.alarm_draft.weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
                }
                switch_page(SHELL_PAGE_ALARM_EDITOR);
            } else if (confirmed && action == ALARM_PENDING_EDITOR_WEEKDAY) {
                uint8_t next = s_shell.alarm_draft.weekday_mask ^ (uint8_t)value;
                if (next != 0) {
                    s_shell.alarm_draft.weekday_mask = next;
                    switch_page(SHELL_PAGE_ALARM_EDITOR);
                } else {
                    fridge_ui_shell_show_toast("至少选择一天");
                }
            } else if (confirmed && action == ALARM_PENDING_EDITOR_SOUND) {
                s_shell.alarm_draft.sound = (fridge_kitchen_alarm_sound_t)value;
                switch_page(SHELL_PAGE_ALARM_EDITOR);
            } else if (confirmed && action == ALARM_PENDING_EDITOR_NAME) {
                alarm_open_text_editor(false);
            } else if (confirmed && action == ALARM_PENDING_EDITOR_SPEECH) {
                alarm_open_text_editor(true);
            }
        }
    }
    if (s_shell.toast && s_shell.toast_deadline_us != 0 &&
        now_us >= s_shell.toast_deadline_us) {
        lv_obj_add_flag(s_shell.toast, LV_OBJ_FLAG_HIDDEN);
        s_shell.toast_deadline_us = 0;
    }
}

void fridge_ui_shell_force_redraw(void)
{
    if (!s_shell.created || !s_shell.root) return;
    // 字体从 Flash 回退字切换到 TF FreeType 后，先重算 LV_SIZE_CONTENT 与对齐坐标。
    lv_obj_update_layout(s_shell.root);
    lv_obj_invalidate(s_shell.root);
}

lv_obj_t *fridge_ui_shell_get_main_scroll(void)
{
    return s_shell.created ? s_shell.main_scroll : NULL;
}

lv_obj_t *fridge_ui_shell_get_portrait_scroll(void)
{
    return s_shell.created ? s_shell.portrait_scroll : NULL;
}

// PPA 绑定完成后由 ui_p4 重新注册当前页的动态覆盖层；对象生命周期始终归 Shell 管理。
lv_obj_t *fridge_ui_shell_get_ppa_dynamic_overlay(void)
{
    if (!s_shell.created) return NULL;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        if (s_shell.page == SHELL_PAGE_TIMER) return s_shell.portrait_timer.face_region;
        if (s_shell.page == SHELL_PAGE_STOPWATCH) {
            return s_shell.portrait_stopwatch.face_region;
        }
        return NULL;
    }
    if (s_shell.page == SHELL_PAGE_STOPWATCH) return s_shell.stopwatch_face_region;
    if (s_shell.page == SHELL_PAGE_ALARM_EDITOR) return s_shell.alarm_editor_footer;
    return NULL;
}

// ============ CHAT-01 唯一 Agent 对话页 ============

// 对话页从外层 main_scroll 独立出来：消息区自身滚动，输入区与 IME 固定在左侧 704px
// 主区。这样滚动不会带走输入框，也不会和 PPA 的静态缓存相互覆盖。
#define CHAT_IME_VISIBLE_CANDIDATES 6

static void refresh_chat_ime(void)
{
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    for (size_t i = 0; i < CHAT_IME_VISIBLE_CANDIDATES; i++) {
        set_label_if_changed(s_shell.chat_ime_candidates[i],
                             i < snapshot.candidate_count ? snapshot.candidates[i] : "");
    }

    // CHAT-01 把正在组合的拼音直接放在输入框末尾，并在其下方画短横线。
    // 候选栏只显示候选词，不再占用空间重复显示拼音或页码。
    if (!s_shell.chat_ime_underline || !s_shell.chat_textarea || !snapshot.preedit[0]) {
        if (s_shell.chat_ime_underline) {
            lv_obj_add_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    const char *text = lv_textarea_get_text(s_shell.chat_textarea);
    lv_textarea_set_cursor_pos(s_shell.chat_textarea, LV_TEXTAREA_CURSOR_LAST);
    uint32_t text_chars = lv_textarea_get_cursor_pos(s_shell.chat_textarea);
    uint32_t preedit_chars = (uint32_t)strlen(snapshot.preedit);
    if (!text || text_chars < preedit_chars) {
        lv_obj_add_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_update_layout(s_shell.chat_textarea);
    lv_obj_t *label = lv_textarea_get_label(s_shell.chat_textarea);
    lv_point_t start = {0};
    lv_point_t end = {0};
    lv_label_get_letter_pos(label, text_chars - preedit_chars, &start);
    lv_label_get_letter_pos(label, text_chars, &end);
    int width = end.x - start.x;
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int line_height = font ? lv_font_get_line_height(font) : 22;
    lv_obj_set_pos(s_shell.chat_ime_underline,
                   lv_obj_get_x(label) + start.x,
                   lv_obj_get_y(label) + start.y + line_height - 2);
    lv_obj_set_size(s_shell.chat_ime_underline, width > 1 ? width : 1, 2);
    lv_obj_remove_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_HIDDEN);
}

static void layout_chat_page(void)
{
    if (!s_shell.chat_initialized) return;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        // 竖屏 CHAT-01 的输入区固定在主区底部；消息视口始终从 header 下方开始，
        // 键盘打开时关闭 PPA，避免输入法覆盖层被静态滚动缓存复制。
        lv_obj_set_pos(s_shell.chat_scroll, 0, UI_HEADER_H);
        lv_obj_set_width(s_shell.chat_scroll, UI_PORTRAIT_W);
        if (s_shell.chat_ime_open) {
            lv_obj_set_height(s_shell.chat_scroll, UI_PORTRAIT_CHAT_OPEN_SCROLL_H);
            lv_obj_set_pos(s_shell.chat_input_area, UI_PORTRAIT_CONTENT_X,
                           UI_PORTRAIT_CHAT_IME_Y - UI_PORTRAIT_CHAT_INPUT_H -
                               UI_PORTRAIT_CHAT_GAP);
            lv_obj_set_size(s_shell.chat_input_area, UI_PORTRAIT_CONTENT_W,
                            UI_PORTRAIT_CHAT_INPUT_H);
            lv_obj_set_pos(s_shell.chat_ime_panel, UI_PORTRAIT_CONTENT_X,
                           UI_PORTRAIT_CHAT_IME_Y);
            lv_obj_set_size(s_shell.chat_ime_panel, UI_PORTRAIT_CONTENT_W,
                            UI_PORTRAIT_CHAT_IME_H);
            lv_obj_remove_flag(s_shell.chat_ime_panel, LV_OBJ_FLAG_HIDDEN);
            fridge_ui_scroll_ppa_set_enabled(false);
        } else {
            lv_obj_set_height(s_shell.chat_scroll, UI_PORTRAIT_CHAT_SCROLL_H);
            lv_obj_set_pos(s_shell.chat_input_area, UI_PORTRAIT_CONTENT_X,
                           UI_PORTRAIT_CHAT_INPUT_Y);
            lv_obj_set_size(s_shell.chat_input_area, UI_PORTRAIT_CONTENT_W,
                            UI_PORTRAIT_CHAT_INPUT_H);
            lv_obj_add_flag(s_shell.chat_ime_panel, LV_OBJ_FLAG_HIDDEN);
            fridge_ui_scroll_ppa_set_viewport_height(UI_PORTRAIT_CHAT_SCROLL_H);
            fridge_ui_scroll_ppa_invalidate_cache();
        }
        return;
    }
    if (s_shell.chat_ime_open) {
        // 实屏校准后键盘外框距 600px 屏幕底边约 3px；键盘打开时只缩短消息区，
        // 不销毁历史滚动位置。
        lv_obj_set_pos(s_shell.chat_scroll, 16, 0);
        lv_obj_set_size(s_shell.chat_scroll, 672, 111);
        lv_obj_set_pos(s_shell.chat_input_area, 16, 119);
        lv_obj_set_size(s_shell.chat_input_area, 672, 100);
        lv_obj_set_pos(s_shell.chat_ime_panel, 16, 227);
        lv_obj_set_size(s_shell.chat_ime_panel, 672, 282);
        lv_obj_remove_flag(s_shell.chat_ime_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(s_shell.chat_scroll, 16, 0);
        lv_obj_set_size(s_shell.chat_scroll, 672, 395);
        lv_obj_set_pos(s_shell.chat_input_area, 16, 403);
        lv_obj_set_size(s_shell.chat_input_area, 672, 104);
        lv_obj_add_flag(s_shell.chat_ime_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_chat_ime(void)
{
    if (!s_shell.chat_initialized) return;
    if (s_shell.page != FRIDGE_UI_P4_PAGE_CHAT || !s_shell.chat_ime_panel ||
        !s_shell.chat_textarea) {
        s_shell.chat_ime_open = false;
        fridge_ime_clear_preedit();
        return;
    }
    s_shell.chat_ime_open = false;
    fridge_ime_clear_preedit();
    if (s_shell.chat_ime_underline) {
        lv_obj_add_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_HIDDEN);
    }
    layout_chat_page();
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        fridge_ui_scroll_ppa_set_viewport_height(UI_PORTRAIT_CHAT_SCROLL_H);
        fridge_ui_scroll_ppa_set_enabled(true);
        fridge_ui_scroll_ppa_invalidate_cache();
    }
}

static void open_chat_ime(void)
{
    if (!s_shell.chat_initialized || !s_shell.chat_textarea) return;
    (void)fridge_ime_attach_textarea(s_shell.chat_textarea);
    (void)fridge_ime_set_candidate_page_size(CHAT_IME_VISIBLE_CANDIDATES);
    s_shell.chat_ime_open = true;
    layout_chat_page();
    refresh_chat_ime();
    lv_obj_move_foreground(s_shell.chat_input_area);
    lv_obj_move_foreground(s_shell.chat_ime_panel);
}

static void chat_textarea_focus_event(lv_event_t *event)
{
    (void)event;
    open_chat_ime();
}

static void chat_ime_key_event(lv_event_t *event)
{
    char key = (char)(uintptr_t)lv_event_get_user_data(event);
    if (!s_shell.chat_textarea) return;
    lv_textarea_set_cursor_pos(s_shell.chat_textarea, LV_TEXTAREA_CURSOR_LAST);
    if (lv_textarea_get_cursor_pos(s_shell.chat_textarea) >=
        FRIDGE_UI_P4_AGENT_TEXT_MAX - 1U) {
        return;
    }
    // 组合串同时是真实 textarea 内容，候选词确认时再原位替换成汉字。
    lv_textarea_add_char(s_shell.chat_textarea, (uint32_t)key);
    fridge_ime_feed_letter(key);
    refresh_chat_ime();
}

static void chat_ime_remove_displayed_preedit(const fridge_ime_snapshot_t *snapshot)
{
    if (!snapshot || !s_shell.chat_textarea) return;
    lv_textarea_set_cursor_pos(s_shell.chat_textarea, LV_TEXTAREA_CURSOR_LAST);
    for (size_t i = 0; snapshot->preedit[i]; i++) {
        lv_textarea_delete_char(s_shell.chat_textarea);
    }
}

static void chat_ime_commit_preedit_or_raw(bool prefer_candidate)
{
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (!snapshot.preedit[0]) {
        // 直接输入数字、空格或标点时退出上一词的联想状态。
        fridge_ime_clear_preedit();
        return;
    }
    if (prefer_candidate && snapshot.candidate_count > 0) {
        chat_ime_remove_displayed_preedit(&snapshot);
        (void)fridge_ime_select_candidate(0);
    } else {
        // 拼音已经显示在 textarea 中；这里只结束组合，不重复插入文字。
        fridge_ime_clear_preedit();
    }
}

static void chat_ime_digit_event(lv_event_t *event)
{
    char digit = (char)(uintptr_t)lv_event_get_user_data(event);
    // 数字和标点属于直接输入；已有拼音时先提交首选，没有候选则保留英文。
    chat_ime_commit_preedit_or_raw(true);
    lv_textarea_add_char(s_shell.chat_textarea, (uint32_t)digit);
    refresh_chat_ime();
}

static void chat_ime_backspace_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (snapshot.preedit[0] && s_shell.chat_textarea) {
        lv_textarea_set_cursor_pos(s_shell.chat_textarea, LV_TEXTAREA_CURSOR_LAST);
        lv_textarea_delete_char(s_shell.chat_textarea);
    }
    fridge_ime_backspace();
    refresh_chat_ime();
}

static void chat_ime_candidate_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (index >= snapshot.candidate_count) return;
    if (snapshot.preedit[0]) {
        chat_ime_remove_displayed_preedit(&snapshot);
    }
    if (fridge_ime_select_candidate(index) == ESP_OK) {
        // 选词后保留键盘，继续显示词语联想，行为与手机输入法一致。
        refresh_chat_ime();
    }
}

static void chat_ime_space_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (!s_shell.chat_textarea) return;
    if (snapshot.preedit[0]) {
        chat_ime_commit_preedit_or_raw(true);
    } else {
        fridge_ime_clear_preedit();
    }
    if (!snapshot.preedit[0] || snapshot.candidate_count == 0) {
        lv_textarea_add_text(s_shell.chat_textarea, " ");
    }
    refresh_chat_ime();
}

static void chat_ime_punctuation_event(lv_event_t *event)
{
    const char *punctuation = (const char *)lv_event_get_user_data(event);
    if (!punctuation || !s_shell.chat_textarea) return;
    chat_ime_commit_preedit_or_raw(true);
    lv_textarea_add_text(s_shell.chat_textarea, punctuation);
    refresh_chat_ime();
}

static void chat_ime_done_event(lv_event_t *event)
{
    (void)event;
    fridge_ime_snapshot_t snapshot = {0};
    fridge_ime_get_snapshot(&snapshot);
    if (snapshot.preedit[0]) {
        // 有组合串时“完成”只结束拼音组合，原英文保留，键盘继续展开。
        fridge_ime_clear_preedit();
        refresh_chat_ime();
        return;
    }
    close_chat_ime();
}

static void chat_ime_close_event(lv_event_t *event)
{
    (void)event;
    close_chat_ime();
}

static void chat_ime_previous_page_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (fridge_ime_previous_candidate_page() == ESP_OK) refresh_chat_ime();
}

static void chat_ime_next_page_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (fridge_ime_next_candidate_page() == ESP_OK) refresh_chat_ime();
}

static bool chat_submit_text(const char *text)
{
    if (!text || !text[0]) {
        fridge_ui_shell_show_toast("请输入问题后再发送");
        return false;
    }
    if (!s_shell.actions.chat_submit) {
        fridge_ui_shell_show_toast("小精灵正在初始化");
        return false;
    }
    esp_err_t ret = s_shell.actions.chat_submit(s_shell.actions.user_ctx, text);
    if (ret != ESP_OK) {
        fridge_ui_shell_show_toast(ret == ESP_ERR_INVALID_STATE ? "小精灵尚未就绪" :
                                                               "发送失败，请稍后重试");
        return false;
    }
    if (s_shell.chat_textarea) lv_textarea_set_text(s_shell.chat_textarea, "");
    close_chat_ime();
    return true;
}

// 对话清空弹窗是独立根覆盖层；先清空引用再异步删除，避免同一帧重复点击使用旧对象。
static void close_chat_clear_confirm(void)
{
    lv_obj_t *dialog = s_shell.chat_clear_dialog;
    lv_obj_t *scrim = s_shell.chat_clear_scrim;
    s_shell.chat_clear_dialog = NULL;
    s_shell.chat_clear_scrim = NULL;
    if (dialog) lv_obj_delete_async(dialog);
    if (scrim) lv_obj_delete_async(scrim);
}

static void chat_clear_cancel_event(lv_event_t *event)
{
    (void)event;
    close_chat_clear_confirm();
}

static void chat_clear_confirm_event(lv_event_t *event)
{
    (void)event;
    esp_err_t ret = s_shell.actions.chat_clear_session ?
                        s_shell.actions.chat_clear_session(s_shell.actions.user_ctx) :
                        ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) {
        // Runtime 已同步清除 TF 会话快照并会补发 SESSION_RESET；这里先刷新前台，
        // 让确认后的视觉反馈无需等待下一轮事件轮询。
        if (s_shell.chat_textarea) lv_textarea_set_text(s_shell.chat_textarea, "");
        close_chat_ime();
        fridge_ui_chat_model_reset_foreground_session(&s_shell.chat);
        if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
            refresh_portrait_chat_page();
        } else {
            refresh_chat_page(true);
        }
        fridge_ui_shell_show_toast("对话记录已清空");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        fridge_ui_shell_show_toast("小精灵正在处理，暂不能清空对话");
    } else {
        fridge_ui_shell_show_toast("清空失败，请稍后重试");
    }
    close_chat_clear_confirm();
}

static void chat_show_clear_confirm(void)
{
    if (!s_shell.root || s_shell.chat_clear_dialog) return;
    if (s_shell.chat.agent_working) {
        fridge_ui_shell_show_toast("小精灵正在处理，暂不能清空对话");
        return;
    }
    const bool portrait = s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY;
    const int root_width = portrait ? UI_PORTRAIT_W : 1024;
    const int root_height = portrait ? UI_PORTRAIT_H : UI_SCREEN_H;
    const int dialog_width = portrait ? 520 : 352;
    const int dialog_x = (root_width - dialog_width) / 2;
    const int dialog_y = portrait ? 360 : 190;
    s_shell.chat_clear_scrim = make_box(s_shell.root, 0, 0, root_width, root_height,
                                        FRIDGE_UI_COLOR_INK, FRIDGE_UI_COLOR_INK, 0);
    lv_obj_set_style_bg_opa(s_shell.chat_clear_scrim, LV_OPA_50, 0);
    lv_obj_add_flag(s_shell.chat_clear_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_shell.chat_clear_scrim, chat_clear_cancel_event, LV_EVENT_CLICKED, NULL);
    s_shell.chat_clear_dialog = make_box(s_shell.root, dialog_x, dialog_y, dialog_width, 202,
                                         FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 10);
    make_label(s_shell.chat_clear_dialog, "清空全部对话？", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_INK, 20, 20, dialog_width - 40,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(s_shell.chat_clear_dialog, "这会删除本机保存的对话记录，且无法恢复。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 20, 64,
               dialog_width - 40, 44,
               LV_LABEL_LONG_WRAP);
    const int confirm_x = dialog_width - 168;
    make_button(s_shell.chat_clear_dialog, portrait ? 22 : 22, 140,
                portrait ? 210 : 136, 46, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "取消", FRIDGE_FONT_BODY,
                chat_clear_cancel_event, NULL, NULL);
    make_button(s_shell.chat_clear_dialog, confirm_x, 140, portrait ? 146 : 138, 46,
                FRIDGE_UI_COLOR_TOMATO,
                0xFFFFFF, FRIDGE_UI_COLOR_TOMATO, "确认清空", FRIDGE_FONT_BODY,
                chat_clear_confirm_event, NULL, NULL);
    lv_obj_move_foreground(s_shell.chat_clear_scrim);
    lv_obj_move_foreground(s_shell.chat_clear_dialog);
}

static void chat_send_stop_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.chat.agent_working) {
        esp_err_t ret = s_shell.actions.chat_cancel ?
                        s_shell.actions.chat_cancel(s_shell.actions.user_ctx) :
                        ESP_ERR_INVALID_STATE;
        fridge_ui_shell_show_toast(ret == ESP_OK ? "已请求停止当前回答和播报" : "停止请求失败");
        return;
    }
    chat_submit_text(s_shell.chat_textarea ? lv_textarea_get_text(s_shell.chat_textarea) : NULL);
}

static void chat_quick_command_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const char *command = (const char *)lv_event_get_user_data(event);
    if (!command) return;
    if (s_shell.chat_textarea) lv_textarea_set_text(s_shell.chat_textarea, command);
    (void)chat_submit_text(command);
}

static void chat_voice_event(lv_event_t *event)
{
    (void)event;
    // 对话页保留原有“录音后手动提交”语义，不能误启动全局连续对话。
    if (s_shell.actions.manual_voice_pressed) {
        s_shell.actions.manual_voice_pressed(s_shell.actions.user_ctx);
    } else {
        fridge_ui_shell_show_toast("语音输入暂不可用");
    }
}

static void chat_confirmation_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const fridge_ui_chat_message_t *message =
        (const fridge_ui_chat_message_t *)lv_event_get_user_data(event);
    if (!message || !message->run_id[0] || !message->call_id[0]) return;
    esp_err_t ret = s_shell.actions.chat_resolve_confirmation ?
                    s_shell.actions.chat_resolve_confirmation(s_shell.actions.user_ctx,
                                                               message->run_id, message->call_id,
                                                               true) : ESP_ERR_INVALID_STATE;
    fridge_ui_shell_show_toast(ret == ESP_OK ? "已确认，正在执行" : "确认请求失败");
}

static void chat_reject_event(lv_event_t *event)
{
    // 用独立回调表达拒绝，避免从 LVGL 子对象反推业务状态。
    if (scroll_click_is_suppressed(event)) return;
    const fridge_ui_chat_message_t *message =
        (const fridge_ui_chat_message_t *)lv_event_get_user_data(event);
    if (!message || !message->run_id[0] || !message->call_id[0]) return;
    esp_err_t ret = s_shell.actions.chat_resolve_confirmation ?
                    s_shell.actions.chat_resolve_confirmation(s_shell.actions.user_ctx,
                                                               message->run_id, message->call_id,
                                                               false) : ESP_ERR_INVALID_STATE;
    fridge_ui_shell_show_toast(ret == ESP_OK ? "已拒绝此操作" : "拒绝请求失败");
}

// CHAT-01 的卡片宽度跟随当前逻辑方向；横屏保持 672px，竖屏严格收进 568px 正文栏。
static int chat_content_width(void)
{
    return s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY ?
               UI_PORTRAIT_CONTENT_W : 672;
}

// 创建对话气泡。高度在字体布局后按实际文字内容确定，避免长回复被固定卡片裁切。
static lv_obj_t *create_chat_bubble(lv_obj_t *parent, const fridge_ui_chat_message_t *msg, int y)
{
    bool user = msg->type == FRIDGE_UI_CHAT_MSG_USER;
    int content_width = chat_content_width();
    int width = user ? content_width - 158 : content_width - 68;
    int x = user ? content_width - width : 0;
    uint32_t background = user ? FRIDGE_UI_COLOR_YOLK : FRIDGE_UI_COLOR_CREAM;
    uint32_t border = user ? FRIDGE_UI_COLOR_YOLK : FRIDGE_UI_COLOR_LINE;
    lv_obj_t *bubble = make_box(parent, x, y, width, 52, background, border, 8);
    const char *text = msg->text[0] ? msg->text : "…";
    lv_obj_t *body = make_label(bubble, text, FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_INK,
                                12, 10, width - 24, LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(body);
    int body_h = lv_obj_get_height(body);
    int height = body_h + 20;
    if (msg->time[0]) {
        make_label(bubble, msg->time, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                   12, body_h + 12, width - 24, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
        height += 20;
    }
    lv_obj_set_height(bubble, height < 52 ? 52 : height);
    return bubble;
}

static const char *chat_tool_status_text(fridge_ui_chat_tool_status_t status)
{
    switch (status) {
        case FRIDGE_UI_CHAT_TOOL_STATUS_RUNNING: return "正在执行";
        case FRIDGE_UI_CHAT_TOOL_STATUS_OK: return "已完成";
        case FRIDGE_UI_CHAT_TOOL_STATUS_ERROR: return "执行失败";
        default: return "等待确认";
    }
}

static lv_obj_t *create_tool_card(lv_obj_t *parent, const fridge_ui_chat_message_t *msg, int y)
{
    bool pending = msg->requires_confirmation &&
                   msg->tool_status == FRIDGE_UI_CHAT_TOOL_STATUS_PENDING;
    uint32_t accent = msg->risk == FRIDGE_UI_CHAT_RISK_HIGH ? FRIDGE_UI_COLOR_TOMATO :
                      msg->risk == FRIDGE_UI_CHAT_RISK_MEDIUM ? FRIDGE_UI_COLOR_YOLK :
                                                               FRIDGE_UI_COLOR_SAGE;
    int height = pending ? 154 : 110;
    int width = chat_content_width();
    lv_obj_t *card = make_box(parent, 0, y, width, height, FRIDGE_UI_COLOR_SURFACE,
                              accent, 8);
    const char *title = msg->tool_label[0] ? msg->tool_label : msg->tool_name;
    make_label(card, title[0] ? title : "工具调用", FRIDGE_FONT_BODY_EMPHASIS,
               FRIDGE_UI_COLOR_INK, 12, 10, width - 168, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    const char *risk = msg->risk == FRIDGE_UI_CHAT_RISK_HIGH ? "高风险" :
                       msg->risk == FRIDGE_UI_CHAT_RISK_MEDIUM ? "中风险" : "低风险";
    lv_obj_t *risk_chip = make_box(card, width - 96, 8, 84, 24, accent, accent, 12);
    lv_obj_t *risk_label = make_label(risk_chip, risk, FRIDGE_FONT_BODY_SMALL, 0xFFFFFF,
                                      0, 0, 84, LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(risk_label, 84);
    make_label(card, msg->tool_detail[0] ? msg->tool_detail : "正在准备工具请求",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 12, 40, width - 24, 38,
               LV_LABEL_LONG_WRAP);
    char status[72];
    snprintf(status, sizeof(status), "%s%s%s", chat_tool_status_text(msg->tool_status),
             msg->tool_latency[0] ? " · " : "", msg->tool_latency);
    uint32_t status_color = msg->tool_status == FRIDGE_UI_CHAT_TOOL_STATUS_ERROR ?
                            FRIDGE_UI_COLOR_TOMATO : accent;
    make_label(card, status, FRIDGE_FONT_BODY_SMALL_EMPHASIS, status_color,
               12, 80, width - 238, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (pending) {
        make_label(card, "该操作会修改本地数据，需要你确认。", FRIDGE_FONT_BODY_SMALL,
                   FRIDGE_UI_COLOR_WARN_INK, 12, 104, width - 238,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
        make_button(card, width - 256, 102, 108, 40, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "拒绝",
                    FRIDGE_FONT_BODY_SMALL, chat_reject_event, (void *)msg, NULL);
        make_button(card, width - 140, 102, 128, 40, FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                    FRIDGE_UI_COLOR_SAGE, "确认执行", FRIDGE_FONT_BODY_SMALL,
                    chat_confirmation_event, (void *)msg, NULL);
    }
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) {
        // 竖屏工具卡可进入 CHAT-03；既有 8px 拖动保护会吞掉滚动期间的误触。
        add_click_event(card, portrait_chat_tool_event, (void *)msg);
    }
    return card;
}

static lv_obj_t *create_result_card(lv_obj_t *parent, const fridge_ui_chat_message_t *msg, int y)
{
    int width = chat_content_width();
    lv_obj_t *card = make_box(parent, 0, y, width, 88, FRIDGE_UI_COLOR_MINT,
                              FRIDGE_UI_COLOR_SAGE, 8);
    make_label(card, msg->result_title[0] ? msg->result_title : "已更新", FRIDGE_FONT_BODY_EMPHASIS,
               FRIDGE_UI_COLOR_SAGE_DARK, 12, 10, width - 24, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(card, msg->result_desc, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_INK,
               12, 40, width - 24, 36, LV_LABEL_LONG_WRAP);
    return card;
}

// 竖屏二级对话页只从当前 foreground 模型派生内容；没有 Runtime 数据时明确展示空态。
static const fridge_ui_chat_message_t *portrait_chat_latest_tool(bool pending_only)
{
    for (size_t i = s_shell.chat.message_count; i > 0; i--) {
        const fridge_ui_chat_message_t *message = &s_shell.chat.messages[i - 1];
        if (message->type != FRIDGE_UI_CHAT_MSG_TOOL) continue;
        if (pending_only && (!message->requires_confirmation ||
                             message->tool_status != FRIDGE_UI_CHAT_TOOL_STATUS_PENDING)) {
            continue;
        }
        return message;
    }
    return NULL;
}

static void portrait_chat_empty_state(lv_obj_t *parent, int y, const char *title,
                                      const char *detail)
{
    lv_obj_t *card = make_box(parent, 0, y, UI_PORTRAIT_CONTENT_W, 146,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    make_label(card, title, FRIDGE_FONT_TITLE, FRIDGE_UI_COLOR_INK,
               20, 22, UI_PORTRAIT_CONTENT_W - 40, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(card, detail, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               20, 62, UI_PORTRAIT_CONTENT_W - 40, 48, LV_LABEL_LONG_WRAP);
}

static void portrait_chat_render_history(void)
{
    lv_obj_t *flow = s_shell.chat_message_flow;
    if (!flow) return;
    lv_obj_clean(flow);
    lv_obj_set_pos(flow, UI_PORTRAIT_CONTENT_X, 12);
    lv_obj_set_size(flow, UI_PORTRAIT_CONTENT_W, 360);
    lv_obj_t *search = make_box(flow, 0, 0, 414, 48, FRIDGE_UI_COLOR_SURFACE,
                                FRIDGE_UI_COLOR_LINE, 10);
    make_label(search, "搜索会话（当前仅保留前台会话）", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 16, 0, 390, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    lv_obj_align(lv_obj_get_child(search, 0), LV_ALIGN_LEFT_MID, 16, 0);
    make_button(flow, 430, 0, 138, 48, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "仅看同步",
                FRIDGE_FONT_BODY_SMALL, portrait_chat_new_event, NULL, NULL);
    if (s_shell.chat.message_count == 0) {
        portrait_chat_empty_state(flow, 72, "暂无历史会话",
                                  "当前 Runtime 只提供前台会话；完成一次对话后，它会出现在这里。\n不会注入参考页的模拟会话。");
        return;
    }
    lv_obj_t *row = make_button(flow, 0, 72, UI_PORTRAIT_CONTENT_W, 112,
                                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE,
                                FRIDGE_UI_COLOR_LINE, "", FRIDGE_FONT_BODY,
                                portrait_chat_continue_event, NULL, NULL);
    make_label(row, "当前会话", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, 14, 280, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char meta[96];
    snprintf(meta, sizeof(meta), "前台会话 · %u 条消息 · %s",
             (unsigned)s_shell.chat.message_count,
             s_shell.chat.online ? "已同步" : "本地暂存");
    make_label(row, meta, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               16, 48, 350, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_button(row, 404, 14, 136, 38, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "继续",
                FRIDGE_FONT_BODY_SMALL, portrait_chat_continue_event, NULL, NULL);
    make_button(row, 404, 60, 64, 32, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE, "重命名",
                FRIDGE_FONT_BODY_SMALL, portrait_chat_new_event, NULL, NULL);
    make_button(row, 476, 60, 64, 32, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE, "归档",
                FRIDGE_FONT_BODY_SMALL, portrait_chat_new_event, NULL, NULL);
}

static void portrait_chat_render_tool_detail(void)
{
    lv_obj_t *flow = s_shell.chat_message_flow;
    if (!flow) return;
    lv_obj_clean(flow);
    lv_obj_set_pos(flow, UI_PORTRAIT_CONTENT_X, 12);
    lv_obj_set_size(flow, UI_PORTRAIT_CONTENT_W, 980);
    const fridge_ui_chat_message_t *tool = portrait_chat_latest_tool(false);
    if (!tool) {
        portrait_chat_empty_state(flow, 0, "暂无工具执行记录",
                                  "当前 foreground 会话还没有可展示的工具调用。参数、业务对象和日志不会被虚构填充。");
        return;
    }
    const char *title = tool->tool_label[0] ? tool->tool_label :
                        (tool->tool_name[0] ? tool->tool_name : "工具调用");
    lv_obj_t *info = make_box(flow, 0, 0, UI_PORTRAIT_CONTENT_W, 154,
                              FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    make_label(info, title, FRIDGE_FONT_TITLE, FRIDGE_UI_COLOR_INK,
               16, 14, 350, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(info, tool->tool_name[0] ? tool->tool_name : "Runtime 未提供工具标识",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 16, 52, 360,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    char call[64];
    snprintf(call, sizeof(call), "调用 ID：%s", tool->call_id[0] ? tool->call_id : "未提供");
    make_label(info, call, FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
               16, 82, 360, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    const char *risk = tool->risk == FRIDGE_UI_CHAT_RISK_HIGH ? "高风险" :
                       tool->risk == FRIDGE_UI_CHAT_RISK_MEDIUM ? "中风险" : "低风险";
    make_label(info, risk, FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               tool->risk == FRIDGE_UI_CHAT_RISK_HIGH ? FRIDGE_UI_COLOR_TOMATO :
               tool->risk == FRIDGE_UI_CHAT_RISK_MEDIUM ? FRIDGE_UI_COLOR_WARN_INK :
                                                           FRIDGE_UI_COLOR_SAGE_DARK,
               424, 18, 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(info, chat_tool_status_text(tool->tool_status), FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_SAGE_DARK, 424, 52, 120, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(info, tool->tool_latency[0] ? tool->tool_latency : "耗时未提供",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 424, 84, 120,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(info, tool->requires_confirmation ? "权限：等待用户确认" : "权限：按当前策略执行",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 16, 118, 520,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);

    lv_obj_t *detail = make_box(flow, 0, 166, UI_PORTRAIT_CONTENT_W, 184,
                                FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    make_label(detail, "调用内容", FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK,
               16, 14, 220, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(detail, tool->tool_detail[0] ? tool->tool_detail : "Runtime 未提供可展示的参数摘要",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 16, 48,
               UI_PORTRAIT_CONTENT_W - 32, 42, LV_LABEL_LONG_WRAP);
    make_label(detail, "业务对象：暂无 Runtime 数据", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 16, 102, UI_PORTRAIT_CONTENT_W - 32,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(detail, "执行日志：暂无 Runtime 数据", FRIDGE_FONT_BODY_SMALL,
               FRIDGE_UI_COLOR_MUTED, 16, 134, UI_PORTRAIT_CONTENT_W - 32,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    if (tool->requires_confirmation &&
        tool->tool_status == FRIDGE_UI_CHAT_TOOL_STATUS_PENDING) {
        make_button(flow, 16, 368, 172, 48, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "拒绝",
                    FRIDGE_FONT_BODY, chat_reject_event, (void *)tool, NULL);
        make_button(flow, 204, 368, 172, 48, FRIDGE_UI_COLOR_SAGE,
                    0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "确认执行",
                    FRIDGE_FONT_BODY, chat_confirmation_event, (void *)tool, NULL);
    } else if (tool->tool_status == FRIDGE_UI_CHAT_TOOL_STATUS_RUNNING) {
        make_button(flow, 16, 368, 172, 48, FRIDGE_UI_COLOR_DANGER_BG,
                    FRIDGE_UI_COLOR_TOMATO, FRIDGE_UI_COLOR_DANGER_BG, "停止执行",
                    FRIDGE_FONT_BODY, chat_send_stop_event, NULL, NULL);
    }
}

static void portrait_chat_render_pending_actions(void)
{
    lv_obj_t *flow = s_shell.chat_message_flow;
    if (!flow) return;
    lv_obj_clean(flow);
    lv_obj_set_pos(flow, UI_PORTRAIT_CONTENT_X, 12);
    lv_obj_set_size(flow, UI_PORTRAIT_CONTENT_W, 760);
    const fridge_ui_chat_message_t *tool = portrait_chat_latest_tool(true);
    if (!tool) {
        portrait_chat_empty_state(flow, 0, "没有待确认行动",
                                  "当前 foreground 会话没有等待确认的工具调用。完成中的 Run 会继续沿用现有状态回调。");
        return;
    }
    lv_obj_t *summary = make_box(flow, 0, 0, UI_PORTRAIT_CONTENT_W, 178,
                                 FRIDGE_UI_COLOR_WARN_BG, FRIDGE_UI_COLOR_YOLK, 12);
    make_label(summary, "Agent 等待你的决定", FRIDGE_FONT_TITLE,
               FRIDGE_UI_COLOR_WARN_INK, 16, 16, 420, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(summary, tool->tool_label[0] ? tool->tool_label : "工具调用",
               FRIDGE_FONT_BODY_EMPHASIS, FRIDGE_UI_COLOR_INK, 16, 58, 380,
               LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(summary, tool->tool_detail[0] ? tool->tool_detail : "影响范围未由 Runtime 提供",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 16, 90,
               UI_PORTRAIT_CONTENT_W - 32, 38, LV_LABEL_LONG_WRAP);
    make_label(summary, tool->risk == FRIDGE_UI_CHAT_RISK_HIGH ? "风险：高" :
               tool->risk == FRIDGE_UI_CHAT_RISK_MEDIUM ? "风险：中" : "风险：低",
               FRIDGE_FONT_BODY_SMALL_EMPHASIS,
               tool->risk == FRIDGE_UI_CHAT_RISK_HIGH ? FRIDGE_UI_COLOR_TOMATO :
               tool->risk == FRIDGE_UI_CHAT_RISK_MEDIUM ? FRIDGE_UI_COLOR_WARN_INK :
                                                           FRIDGE_UI_COLOR_SAGE_DARK,
               16, 140, 160, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    make_label(flow, "确认后将复用现有 chat_resolve_confirmation，不新增 active_run.json 字段。",
               FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED, 0, 198,
               UI_PORTRAIT_CONTENT_W, LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
    make_button(flow, 16, 248, 172, 50, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "拒绝",
                FRIDGE_FONT_BODY, chat_reject_event, (void *)tool, NULL);
    make_button(flow, 204, 248, 172, 50, FRIDGE_UI_COLOR_SAGE,
                0xFFFFFF, FRIDGE_UI_COLOR_SAGE, "确认执行",
                FRIDGE_FONT_BODY, chat_confirmation_event, (void *)tool, NULL);
    make_button(flow, 392, 248, 160, 50, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_WARN_INK, FRIDGE_UI_COLOR_YOLK, "修改",
                FRIDGE_FONT_BODY, portrait_chat_new_event, NULL, NULL);
}

static void portrait_chat_render_subpage(void)
{
    if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY) {
        portrait_chat_render_history();
    } else if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL) {
        portrait_chat_render_tool_detail();
    } else if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS) {
        portrait_chat_render_pending_actions();
    }
    fridge_ui_scroll_ppa_invalidate_cache();
}

// 创建竖屏 CHAT-01～04。横屏 CHAT-01 仍使用下方原有 builder；两套对象树共享
// chat_business_model_t 和 IME 回调，避免为竖屏复制一份 Runtime 状态。
static void create_portrait_chat_page(void)
{
    s_shell.chat_page = lv_obj_create(s_shell.portrait_root);
    clear_container_style(s_shell.chat_page);
    // CHAT 固定层覆盖在 portrait_scroll 上方，只承载输入框和 IME。
    // lv_obj_create 默认带 CLICKABLE；若不清除，消息区的空白触摸会被该透明层
    // 截获，LVGL 就无法沿父链找到真正的 portrait_scroll，表现为无法上下滑动。
    lv_obj_clear_flag(s_shell.chat_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_shell.chat_page, 0, 0);
    lv_obj_set_size(s_shell.chat_page, UI_PORTRAIT_W, UI_PORTRAIT_MAIN_H);
    s_shell.chat_scroll = s_shell.portrait_scroll;
    if (s_shell.page != FRIDGE_UI_P4_PAGE_CHAT) {
        s_shell.chat_message_flow = lv_obj_create(s_shell.chat_scroll);
        clear_container_style(s_shell.chat_message_flow);
        s_shell.chat_initialized = true;
        lv_obj_set_height(s_shell.chat_scroll, UI_PORTRAIT_CHAT_SUBPAGE_SCROLL_H);
        fridge_ui_scroll_ppa_set_viewport_height(UI_PORTRAIT_CHAT_SUBPAGE_SCROLL_H);
        lv_obj_t *footer = make_box(s_shell.chat_page, UI_PORTRAIT_CONTENT_X,
                                    UI_PORTRAIT_MAIN_H - UI_PORTRAIT_CHAT_SUBPAGE_FOOTER_H,
                                    UI_PORTRAIT_CONTENT_W, UI_PORTRAIT_CHAT_SUBPAGE_FOOTER_H,
                                    FRIDGE_UI_COLOR_PAPER, FRIDGE_UI_COLOR_PAPER, 0);
        make_button(footer, 0, 8, 132, 44, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "返回对话",
                    FRIDGE_FONT_BODY_SMALL, portrait_chat_back_event, NULL, NULL);
        if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY) {
            make_button(footer, 420, 8, 148, 44, FRIDGE_UI_COLOR_MINT,
                        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "新建会话",
                        FRIDGE_FONT_BODY_SMALL, portrait_chat_new_event, NULL, NULL);
        } else if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS) {
            make_button(footer, 420, 8, 148, 44, FRIDGE_UI_COLOR_SURFACE,
                        FRIDGE_UI_COLOR_MUTED, FRIDGE_UI_COLOR_LINE, "查看对话",
                        FRIDGE_FONT_BODY_SMALL, portrait_chat_continue_event, NULL, NULL);
        }
        portrait_chat_render_subpage();
        return;
    }

    s_shell.chat_status_row = lv_obj_create(s_shell.chat_scroll);
    clear_container_style(s_shell.chat_status_row);
    lv_obj_set_pos(s_shell.chat_status_row, UI_PORTRAIT_CONTENT_X, 0);
    lv_obj_set_size(s_shell.chat_status_row, UI_PORTRAIT_CONTENT_W, 42);
    s_shell.chat_permission_chip = make_box(s_shell.chat_status_row, 0, 8, 92, 24,
                                            FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 12);
    s_shell.chat_permission_label = make_label(s_shell.chat_permission_chip, "确认模式",
                                               FRIDGE_FONT_BODY_SMALL,
                                               FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 92,
                                               LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(s_shell.chat_permission_label, 92);
    s_shell.chat_sync_dot = make_box(s_shell.chat_status_row, 108, 16, 10, 10,
                                     FRIDGE_UI_COLOR_OFFLINE, FRIDGE_UI_COLOR_OFFLINE, 5);
    s_shell.chat_sync_label = make_label(s_shell.chat_status_row, "本地会话",
                                         FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                         126, 10, 86, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.chat_agent_label = make_label(s_shell.chat_status_row, "等待小精灵就绪",
                                          FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_SAGE_DARK,
                                          224, 10, 196, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.chat_tts_label = make_label(s_shell.chat_status_row, "播报待命",
                                        FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_MUTED,
                                        432, 10, 136, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.chat_message_flow = lv_obj_create(s_shell.chat_scroll);
    clear_container_style(s_shell.chat_message_flow);
    lv_obj_set_pos(s_shell.chat_message_flow, UI_PORTRAIT_CONTENT_X, 46);
    lv_obj_set_size(s_shell.chat_message_flow, UI_PORTRAIT_CONTENT_W, 1);

    s_shell.chat_input_area = make_box(s_shell.chat_page, UI_PORTRAIT_CONTENT_X,
                                       UI_PORTRAIT_CHAT_INPUT_Y, UI_PORTRAIT_CONTENT_W,
                                       UI_PORTRAIT_CHAT_INPUT_H,
                                       FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    static const char *const quick_commands[] = {"今晚吃什么", "登记食材", "看看临期"};
    int quick_x = 12;
    for (size_t i = 0; i < sizeof(quick_commands) / sizeof(quick_commands[0]); i++) {
        int width = i == 1 ? 104 : 112;
        make_button(s_shell.chat_input_area, quick_x, 6, width, 28, FRIDGE_UI_COLOR_MINT,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, quick_commands[i],
                    FRIDGE_FONT_BODY_SMALL, chat_quick_command_event,
                    (void *)quick_commands[i], NULL);
        quick_x += width + 8;
    }
    s_shell.chat_textarea = lv_textarea_create(s_shell.chat_input_area);
    lv_obj_set_pos(s_shell.chat_textarea, 12, 44);
    lv_textarea_set_one_line(s_shell.chat_textarea, true);
    lv_textarea_set_max_length(s_shell.chat_textarea, FRIDGE_UI_P4_AGENT_TEXT_MAX - 1);
    lv_textarea_set_placeholder_text(s_shell.chat_textarea, "点此输入文字…");
    lv_obj_add_style(s_shell.chat_textarea, fridge_ui_theme_font_style(FRIDGE_FONT_BODY), 0);
    lv_obj_set_style_bg_color(s_shell.chat_textarea, color_hex(FRIDGE_UI_COLOR_CREAM), 0);
    lv_obj_set_style_border_color(s_shell.chat_textarea, color_hex(FRIDGE_UI_COLOR_LINE), 0);
    lv_obj_set_style_radius(s_shell.chat_textarea, 8, 0);
    lv_obj_set_style_pad_top(s_shell.chat_textarea, 0, 0);
    lv_obj_set_style_pad_bottom(s_shell.chat_textarea, 0, 0);
    lv_obj_set_style_pad_left(s_shell.chat_textarea, 12, 0);
    lv_obj_set_style_pad_right(s_shell.chat_textarea, 12, 0);
    lv_obj_set_style_text_line_space(s_shell.chat_textarea, 0, 0);
    lv_obj_set_size(s_shell.chat_textarea, 350, 48);
    lv_obj_add_event_cb(s_shell.chat_textarea, chat_textarea_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_shell.chat_textarea, chat_textarea_focus_event, LV_EVENT_CLICKED, NULL);
    s_shell.chat_voice_btn = make_button(s_shell.chat_input_area, 370, 44, 70, 48,
                                         FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                         FRIDGE_UI_COLOR_LINE, "语音", FRIDGE_FONT_BODY_SMALL,
                                         chat_voice_event, NULL, NULL);
    s_shell.chat_send_stop_btn = make_button(s_shell.chat_input_area, 448, 44, 108, 48,
                                             FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                                             FRIDGE_UI_COLOR_SAGE, "发送", FRIDGE_FONT_BODY_SMALL,
                                             chat_send_stop_event, NULL,
                                             &s_shell.chat_send_stop_label);

    s_shell.chat_ime_panel = make_box(s_shell.chat_page, UI_PORTRAIT_CONTENT_X,
                                      UI_PORTRAIT_CHAT_IME_Y, UI_PORTRAIT_CONTENT_W,
                                      UI_PORTRAIT_CHAT_IME_H,
                                      FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    for (size_t i = 0; i < CHAT_IME_VISIBLE_CANDIDATES; i++) {
        lv_obj_t *candidate = make_button(s_shell.chat_ime_panel, 8 + (int)i * 84, 4, 80, 32,
                                          FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                          FRIDGE_UI_COLOR_SURFACE, "", FRIDGE_FONT_BODY_SMALL,
                                          chat_ime_candidate_event, (void *)(uintptr_t)i,
                                          &s_shell.chat_ime_candidates[i]);
        lv_obj_set_style_bg_opa(candidate, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(candidate, 0, 0);
        lv_obj_set_style_shadow_width(candidate, 0, 0);
        lv_obj_set_style_bg_opa(candidate, LV_OPA_30, LV_STATE_PRESSED);
    }
    make_button(s_shell.chat_ime_panel, 516, 4, 44, 32, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "⌄", FRIDGE_FONT_BODY,
                chat_ime_close_event, NULL, NULL);
    static const char *const key_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (size_t row = 0; row < 4; row++) {
        const char *letters = key_rows[row];
        size_t count = strlen(letters);
        int width = row <= 1 ? 50 : row == 2 ? 54 : 58;
        int gap = row <= 1 ? 3 : 4;
        int total = (int)count * width + ((int)count - 1) * gap;
        int x = (568 - total) / 2;
        for (size_t i = 0; i < count; i++) {
            char label[2] = {letters[i], '\0'};
            make_button(s_shell.chat_ime_panel, x + (int)i * (width + gap),
                        44 + (int)row * 42, width, 38, FRIDGE_UI_COLOR_SURFACE,
                        FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, label,
                        FRIDGE_FONT_BODY_SMALL,
                        row == 0 ? chat_ime_digit_event : chat_ime_key_event,
                        (void *)(uintptr_t)letters[i], NULL);
        }
    }
    make_button(s_shell.chat_ime_panel, 486, 170, 70, 38, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "退格",
                FRIDGE_FONT_BODY_SMALL, chat_ime_backspace_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 12, 220, 56, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "，", FRIDGE_FONT_BODY,
                chat_ime_punctuation_event, (void *)"，", NULL);
    make_button(s_shell.chat_ime_panel, 72, 220, 56, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "。", FRIDGE_FONT_BODY,
                chat_ime_punctuation_event, (void *)"。", NULL);
    make_button(s_shell.chat_ime_panel, 132, 220, 186, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "空格/首选",
                FRIDGE_FONT_BODY_SMALL, chat_ime_space_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 322, 220, 76, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "上一页",
                FRIDGE_FONT_BODY_SMALL, chat_ime_previous_page_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 402, 220, 76, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "下一页",
                FRIDGE_FONT_BODY_SMALL, chat_ime_next_page_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 482, 220, 74, 48, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "完成",
                FRIDGE_FONT_BODY_SMALL, chat_ime_done_event, NULL, NULL);
    s_shell.chat_ime_underline = lv_obj_create(s_shell.chat_textarea);
    lv_obj_remove_style_all(s_shell.chat_ime_underline);
    lv_obj_set_style_bg_color(s_shell.chat_ime_underline,
                              color_hex(FRIDGE_UI_COLOR_SAGE_DARK), 0);
    lv_obj_set_style_bg_opa(s_shell.chat_ime_underline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_shell.chat_ime_underline, 1, 0);
    lv_obj_clear_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_HIDDEN);
    s_shell.chat_initialized = true;
    s_shell.chat_ime_open = false;
    (void)fridge_ime_attach_textarea(s_shell.chat_textarea);
    (void)fridge_ime_set_candidate_page_size(CHAT_IME_VISIBLE_CANDIDATES);
    fridge_ime_clear_preedit();
    layout_chat_page();
    refresh_chat_ime();
    refresh_chat_page(true);
}

static void portrait_chat_history_event(lv_event_t *event)
{
    (void)event;
    if (s_shell.actions.navigate_requested) {
        s_shell.actions.navigate_requested(s_shell.actions.user_ctx,
                                           FRIDGE_UI_P4_PAGE_CHAT_HISTORY);
    }
}

static void portrait_chat_new_event(lv_event_t *event)
{
    (void)event;
    // 当前 Runtime 没有多会话、新建、重命名、归档或修改参数接口，不能伪造成功。
    fridge_ui_shell_show_toast("当前 Runtime 尚未提供该会话操作");
}

static void portrait_chat_continue_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    if (s_shell.actions.navigate_requested) {
        s_shell.actions.navigate_requested(s_shell.actions.user_ctx,
                                           FRIDGE_UI_P4_PAGE_CHAT);
    }
}

static void portrait_chat_tool_event(lv_event_t *event)
{
    if (scroll_click_is_suppressed(event)) return;
    const fridge_ui_chat_message_t *tool =
        (const fridge_ui_chat_message_t *)lv_event_get_user_data(event);
    if (!tool || !tool->call_id[0]) {
        fridge_ui_shell_show_toast("当前工具记录缺少调用 ID");
        return;
    }
    if (s_shell.actions.navigate_requested) {
        s_shell.actions.navigate_requested(s_shell.actions.user_ctx,
                                           FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL);
    }
}

static void portrait_chat_back_event(lv_event_t *event)
{
    (void)event;
    page_back_event(NULL);
}

static void render_chat_messages(bool scroll_to_latest)
{
    if (!s_shell.chat_message_flow || !s_shell.chat_scroll) return;
    lv_obj_clean(s_shell.chat_message_flow);
    int y = 0;
    if (s_shell.chat.message_count == 0) {
        make_label(s_shell.chat_message_flow, "你好，我是冰箱小精灵。可以问我库存、菜谱或提醒。",
                   FRIDGE_FONT_BODY, FRIDGE_UI_COLOR_MUTED, 0, 8,
                   chat_content_width() - 8,
                   LV_SIZE_CONTENT, LV_LABEL_LONG_WRAP);
        y = 54;
    }
    for (size_t i = 0; i < s_shell.chat.message_count; i++) {
        const fridge_ui_chat_message_t *message = &s_shell.chat.messages[i];
        lv_obj_t *item = NULL;
        if (message->type == FRIDGE_UI_CHAT_MSG_USER || message->type == FRIDGE_UI_CHAT_MSG_AGENT) {
            item = create_chat_bubble(s_shell.chat_message_flow, message, y);
        } else if (message->type == FRIDGE_UI_CHAT_MSG_TOOL) {
            item = create_tool_card(s_shell.chat_message_flow, message, y);
        } else if (message->type == FRIDGE_UI_CHAT_MSG_RESULT) {
            item = create_result_card(s_shell.chat_message_flow, message, y);
        }
        if (item) {
            lv_obj_update_layout(item);
            y += lv_obj_get_height(item) + 10;
        }
    }
    lv_obj_set_size(s_shell.chat_message_flow, chat_content_width(), y > 0 ? y : 1);
    if (scroll_to_latest) lv_obj_scroll_to_y(s_shell.chat_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void refresh_chat_page(bool scroll_to_latest)
{
    if (!s_shell.chat_initialized || s_shell.page != FRIDGE_UI_P4_PAGE_CHAT) return;
    const char *permission_mode = s_shell.settings_initialized ?
                                      settings_agent_permission_name(s_shell.settings.agent_permission_mode) :
                                      (s_shell.chat.permission_mode[0] ?
                                           s_shell.chat.permission_mode : "确认模式");
    set_label_if_changed(s_shell.chat_permission_label,
                         permission_mode);
    bool online = s_shell.chat.online;
    lv_obj_set_style_bg_color(s_shell.chat_sync_dot,
                              color_hex(online ? FRIDGE_UI_COLOR_SAGE : FRIDGE_UI_COLOR_OFFLINE), 0);
    set_label_if_changed(s_shell.chat_sync_label, online ? "云端可用" : "本地会话");
    set_label_if_changed(s_shell.chat_agent_label,
                         s_shell.chat.agent_state[0] ? s_shell.chat.agent_state : "等待小精灵就绪");
    set_label_if_changed(s_shell.chat_tts_label,
                         s_shell.chat.tts_state[0] ? s_shell.chat.tts_state : "播报待命");
    set_label_if_changed(s_shell.chat_send_stop_label,
                         s_shell.chat.agent_working ? "停止" : "发送");
    lv_obj_set_style_bg_color(s_shell.chat_send_stop_btn,
                              color_hex(s_shell.chat.agent_working ? FRIDGE_UI_COLOR_DANGER_BG :
                                                                   FRIDGE_UI_COLOR_SAGE), 0);
    lv_obj_set_style_text_color(s_shell.chat_send_stop_label,
                                color_hex(s_shell.chat.agent_working ? FRIDGE_UI_COLOR_TOMATO :
                                                                     0xFFFFFF), 0);
    render_chat_messages(scroll_to_latest);
}

static void refresh_portrait_chat_page(void)
{
    if (!s_shell.chat_initialized ||
        s_shell.orientation != FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY) return;
    if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT) {
        refresh_chat_page(false);
    } else {
        portrait_chat_render_subpage();
    }
}

static void create_chat_page(lv_obj_t *page)
{
    if (!page || s_shell.chat_initialized) return;
    s_shell.chat_page = make_box(page, 0, UI_HEADER_H, UI_MAIN_W, UI_PAGE_H,
                                 FRIDGE_UI_COLOR_PAPER, FRIDGE_UI_COLOR_PAPER, 0);
    s_shell.chat_scroll = lv_obj_create(s_shell.chat_page);
    lv_obj_remove_style_all(s_shell.chat_scroll);
    lv_obj_set_pos(s_shell.chat_scroll, 16, 0);
    lv_obj_set_size(s_shell.chat_scroll, 672, 389);
    lv_obj_set_scroll_dir(s_shell.chat_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_shell.chat_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_all(s_shell.chat_scroll, 0, 0);
    lv_obj_add_event_cb(s_shell.chat_scroll, main_scroll_activity_event, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_shell.chat_scroll, main_scroll_activity_event, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_shell.chat_scroll, main_scroll_activity_event, LV_EVENT_SCROLL_END, NULL);

    s_shell.chat_status_row = lv_obj_create(s_shell.chat_scroll);
    clear_container_style(s_shell.chat_status_row);
    lv_obj_set_pos(s_shell.chat_status_row, 0, 0);
    lv_obj_set_size(s_shell.chat_status_row, 672, 42);
    s_shell.chat_permission_chip = make_box(s_shell.chat_status_row, 0, 8, 88, 24,
                                           FRIDGE_UI_COLOR_MINT, FRIDGE_UI_COLOR_MINT, 12);
    s_shell.chat_permission_label = make_label(s_shell.chat_permission_chip, "标准模式",
                                                FRIDGE_FONT_BODY_SMALL,
                                                FRIDGE_UI_COLOR_SAGE_DARK, 0, 0, 88,
                                                LV_SIZE_CONTENT, LV_LABEL_LONG_CLIP);
    center_single_line_label(s_shell.chat_permission_label, 88);
    s_shell.chat_sync_dot = make_box(s_shell.chat_status_row, 104, 16, 10, 10,
                                     FRIDGE_UI_COLOR_OFFLINE, FRIDGE_UI_COLOR_OFFLINE, 5);
    s_shell.chat_sync_label = make_label(s_shell.chat_status_row, "本地会话", FRIDGE_FONT_BODY_SMALL,
                                          FRIDGE_UI_COLOR_MUTED, 122, 10, 90,
                                          LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.chat_agent_label = make_label(s_shell.chat_status_row, "等待小精灵就绪",
                                           FRIDGE_FONT_BODY_SMALL, FRIDGE_UI_COLOR_SAGE_DARK,
                                           224, 10, 224, LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.chat_tts_label = make_label(s_shell.chat_status_row, "播报待命", FRIDGE_FONT_BODY_SMALL,
                                         FRIDGE_UI_COLOR_MUTED, 458, 10, 204,
                                         LV_SIZE_CONTENT, LV_LABEL_LONG_DOT);
    s_shell.chat_message_flow = lv_obj_create(s_shell.chat_scroll);
    clear_container_style(s_shell.chat_message_flow);
    lv_obj_set_pos(s_shell.chat_message_flow, 0, 46);
    lv_obj_set_size(s_shell.chat_message_flow, 672, 1);

    s_shell.chat_input_area = make_box(s_shell.chat_page, 16, 403, 672, 104,
                                       FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    static const char *const quick_commands[] = {"今晚吃什么", "登记食材", "看看临期"};
    int quick_x = 12;
    for (size_t i = 0; i < sizeof(quick_commands) / sizeof(quick_commands[0]); i++) {
        int width = i == 1 ? 104 : 112;
        make_button(s_shell.chat_input_area, quick_x, 6, width, 28, FRIDGE_UI_COLOR_MINT,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, quick_commands[i],
                    FRIDGE_FONT_BODY_SMALL, chat_quick_command_event,
                    (void *)quick_commands[i], NULL);
        quick_x += width + 8;
    }
    s_shell.chat_textarea = lv_textarea_create(s_shell.chat_input_area);
    lv_obj_set_pos(s_shell.chat_textarea, 12, 44);
    lv_textarea_set_one_line(s_shell.chat_textarea, true);
    lv_textarea_set_max_length(s_shell.chat_textarea, FRIDGE_UI_P4_AGENT_TEXT_MAX - 1);
    lv_textarea_set_placeholder_text(s_shell.chat_textarea, "点此输入文字…");
    lv_obj_add_style(s_shell.chat_textarea, fridge_ui_theme_font_style(FRIDGE_FONT_BODY), 0);
    lv_obj_set_style_bg_color(s_shell.chat_textarea, color_hex(FRIDGE_UI_COLOR_CREAM), 0);
    lv_obj_set_style_border_color(s_shell.chat_textarea, color_hex(FRIDGE_UI_COLOR_LINE), 0);
    lv_obj_set_style_radius(s_shell.chat_textarea, 8, 0);
    // 当前 22px CJK FreeType 字体的字面位于行盒下半部；textarea 默认再加 8px 顶边距
    // 会让文字和光标整体下沉。取消上下 padding，让字面中心与 48px 输入框中心重合。
    lv_obj_set_style_pad_top(s_shell.chat_textarea, 0, 0);
    lv_obj_set_style_pad_bottom(s_shell.chat_textarea, 0, 0);
    lv_obj_set_style_pad_left(s_shell.chat_textarea, 12, 0);
    lv_obj_set_style_pad_right(s_shell.chat_textarea, 12, 0);
    lv_obj_set_style_text_line_space(s_shell.chat_textarea, 0, 0);
    // 光标仅比 22px 字面略高：对称收紧 textarea 的 32px 行盒，保持垂直中心不漂移。
    lv_obj_set_style_pad_top(s_shell.chat_textarea, -6,
                             LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_bottom(s_shell.chat_textarea, -6,
                                LV_PART_CURSOR | LV_STATE_FOCUSED);
    // one_line 会按字体和 padding 重算高度，必须在其后重新锁定尺寸，避免实际高度越过输入面板。
    lv_obj_set_size(s_shell.chat_textarea, 500, 48);
    lv_obj_add_event_cb(s_shell.chat_textarea, chat_textarea_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_shell.chat_textarea, chat_textarea_focus_event, LV_EVENT_CLICKED, NULL);
    s_shell.chat_voice_btn = make_button(s_shell.chat_input_area, 520, 44, 66, 48,
                                         FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                                         FRIDGE_UI_COLOR_LINE, "语音", FRIDGE_FONT_BODY_SMALL,
                                         chat_voice_event, NULL, NULL);
    s_shell.chat_send_stop_btn = make_button(s_shell.chat_input_area, 594, 44, 66, 48,
                                             FRIDGE_UI_COLOR_SAGE, 0xFFFFFF,
                                             FRIDGE_UI_COLOR_SAGE, "发送", FRIDGE_FONT_BODY_SMALL,
                                             chat_send_stop_event, NULL,
                                             &s_shell.chat_send_stop_label);

    // 完整拼音 IME，仅在 CHAT-01 里显示；复用启动时已经初始化的全局词库，禁止重复 init。
    s_shell.chat_ime_panel = make_box(s_shell.chat_page, 16, 227, 672, 282,
                                      FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_LINE, 12);
    for (size_t i = 0; i < CHAT_IME_VISIBLE_CANDIDATES; i++) {
        lv_obj_t *candidate = make_button(
            s_shell.chat_ime_panel, 12 + (int)i * 98, 4, 92, 32,
            FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_SURFACE,
            "", FRIDGE_FONT_BODY_SMALL, chat_ime_candidate_event,
            (void *)(uintptr_t)i, &s_shell.chat_ime_candidates[i]);
        // 手机输入法式候选条：文字可点击，但常态不显示按钮框。
        lv_obj_set_style_bg_opa(candidate, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(candidate, 0, 0);
        lv_obj_set_style_shadow_width(candidate, 0, 0);
        lv_obj_set_style_bg_opa(candidate, LV_OPA_30, LV_STATE_PRESSED);
    }
    lv_obj_t *ime_collapse_btn =
        make_button(s_shell.chat_ime_panel, 616, 4, 44, 32, FRIDGE_UI_COLOR_SURFACE,
                    FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "", FRIDGE_FONT_BODY,
                    chat_ime_close_event, NULL, NULL);
    // 收起图标使用两段线组成向下折线，避免字体把它渲染成封闭或实心三角。
    static const lv_point_precise_t ime_collapse_points[] = {
        {12, 10},
        {22, 20},
        {32, 10},
    };
    lv_obj_t *ime_collapse_icon = lv_line_create(ime_collapse_btn);
    lv_line_set_points(ime_collapse_icon, ime_collapse_points,
                       sizeof(ime_collapse_points) / sizeof(ime_collapse_points[0]));
    lv_obj_set_style_line_color(ime_collapse_icon, color_hex(FRIDGE_UI_COLOR_SAGE_DARK), 0);
    lv_obj_set_style_line_width(ime_collapse_icon, 2, 0);
    lv_obj_set_style_line_rounded(ime_collapse_icon, true, 0);
    lv_obj_set_pos(ime_collapse_icon, 0, 0);
    static const char *const key_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    for (size_t row = 0; row < 4; row++) {
        const char *letters = key_rows[row];
        size_t count = strlen(letters);
        int width = row <= 1 ? 60 : row == 2 ? 62 : 64;
        int gap = row <= 1 ? 4 : row == 2 ? 5 : 6;
        int x = row <= 1 ? 18 : row == 2 ? 37 : 59;
        for (size_t i = 0; i < count; i++) {
            char label[2] = {letters[i], '\0'};
            make_button(s_shell.chat_ime_panel, x + (int)i * (width + gap), 44 + (int)row * 42,
                         width, 38, FRIDGE_UI_COLOR_SURFACE, FRIDGE_UI_COLOR_SAGE_DARK,
                         FRIDGE_UI_COLOR_LINE, label, FRIDGE_FONT_BODY_SMALL,
                         row == 0 ? chat_ime_digit_event : chat_ime_key_event,
                        (void *)(uintptr_t)letters[i], NULL);
        }
    }
    // 退格紧跟在 M 键右侧；底栏腾出两个全角中文标点，完成键缩窄。
    make_button(s_shell.chat_ime_panel, 549, 170, 64, 38, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "退格",
                FRIDGE_FONT_BODY_SMALL, chat_ime_backspace_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 24, 220, 64, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "，",
                FRIDGE_FONT_BODY, chat_ime_punctuation_event, (void *)"，", NULL);
    make_button(s_shell.chat_ime_panel, 96, 220, 64, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "。",
                FRIDGE_FONT_BODY, chat_ime_punctuation_event, (void *)"。", NULL);
    make_button(s_shell.chat_ime_panel, 168, 220, 196, 48, FRIDGE_UI_COLOR_SURFACE,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "空格/首选",
                FRIDGE_FONT_BODY_SMALL, chat_ime_space_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 372, 220, 72, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "上一页",
                FRIDGE_FONT_BODY_SMALL, chat_ime_previous_page_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 452, 220, 72, 48, FRIDGE_UI_COLOR_CREAM,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_LINE, "下一页",
                FRIDGE_FONT_BODY_SMALL, chat_ime_next_page_event, NULL, NULL);
    make_button(s_shell.chat_ime_panel, 532, 220, 112, 48, FRIDGE_UI_COLOR_MINT,
                FRIDGE_UI_COLOR_SAGE_DARK, FRIDGE_UI_COLOR_MINT, "完成",
                FRIDGE_FONT_BODY_SMALL, chat_ime_done_event, NULL, NULL);

    // 组合串横线属于 textarea 的视觉层，跟随一行文本水平滚动，不占候选栏空间。
    s_shell.chat_ime_underline = lv_obj_create(s_shell.chat_textarea);
    lv_obj_remove_style_all(s_shell.chat_ime_underline);
    lv_obj_set_style_bg_color(s_shell.chat_ime_underline,
                              color_hex(FRIDGE_UI_COLOR_SAGE_DARK), 0);
    lv_obj_set_style_bg_opa(s_shell.chat_ime_underline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_shell.chat_ime_underline, 1, 0);
    lv_obj_clear_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_shell.chat_ime_underline, LV_OBJ_FLAG_HIDDEN);

    s_shell.chat_initialized = true;
    s_shell.chat_ime_open = false;
    (void)fridge_ime_attach_textarea(s_shell.chat_textarea);
    (void)fridge_ime_set_candidate_page_size(CHAT_IME_VISIBLE_CANDIDATES);
    fridge_ime_clear_preedit();
    layout_chat_page();
    refresh_chat_ime();
    refresh_chat_page(true);
}

static void chat_append_message(fridge_ui_chat_message_type_t type,
                                const fridge_ui_p4_agent_event_t *event)
{
    if (s_shell.chat.message_count >= FRIDGE_UI_CHAT_MESSAGES_MAX) {
        memmove(&s_shell.chat.messages[0], &s_shell.chat.messages[1],
                (FRIDGE_UI_CHAT_MESSAGES_MAX - 1) * sizeof(s_shell.chat.messages[0]));
        s_shell.chat.message_count = FRIDGE_UI_CHAT_MESSAGES_MAX - 1;
    }
    fridge_ui_chat_message_t *message = &s_shell.chat.messages[s_shell.chat.message_count++];
    memset(message, 0, sizeof(*message));
    message->type = type;
    strlcpy(message->run_id, event->run_id, sizeof(message->run_id));
    strlcpy(message->call_id, event->call_id, sizeof(message->call_id));
    strlcpy(message->time, event->time, sizeof(message->time));
    strlcpy(message->text, event->text, sizeof(message->text));
}

static fridge_ui_chat_message_t *find_chat_tool(const fridge_ui_p4_agent_event_t *event)
{
    for (size_t i = s_shell.chat.message_count; i > 0; i--) {
        fridge_ui_chat_message_t *message = &s_shell.chat.messages[i - 1];
        if (message->type == FRIDGE_UI_CHAT_MSG_TOOL &&
            strcmp(message->run_id, event->run_id) == 0 &&
            strcmp(message->call_id, event->call_id) == 0) {
            return message;
        }
    }
    return NULL;
}

void fridge_ui_shell_apply_agent_event(const fridge_ui_p4_agent_event_t *event)
{
    if (!event || !s_shell.created) return;
    bool redraw_messages = false;
    switch (event->kind) {
        case FRIDGE_UI_P4_AGENT_EVENT_SESSION_RESET:
            // Runtime 紧接着回放 TF 中的单会话记录。先只清空旧会话，保留页面级
            // 网络与权限状态，避免恢复历史时混入参考页/多会话占位数据。
            fridge_ui_chat_model_reset_foreground_session(&s_shell.chat);
            if (event->state[0]) strlcpy(s_shell.chat.agent_state, event->state,
                                         sizeof(s_shell.chat.agent_state));
            redraw_messages = true;
            break;
        case FRIDGE_UI_P4_AGENT_EVENT_USER_MESSAGE:
            chat_append_message(FRIDGE_UI_CHAT_MSG_USER, event);
            s_shell.chat.agent_working = event->working;
            redraw_messages = true;
            break;
        case FRIDGE_UI_P4_AGENT_EVENT_ASSISTANT_MESSAGE:
            chat_append_message(FRIDGE_UI_CHAT_MSG_AGENT, event);
            s_shell.chat.agent_working = event->working;
            redraw_messages = true;
            break;
        case FRIDGE_UI_P4_AGENT_EVENT_TOOL: {
            fridge_ui_chat_message_t *message = find_chat_tool(event);
            if (!message) {
                chat_append_message(FRIDGE_UI_CHAT_MSG_TOOL, event);
                message = &s_shell.chat.messages[s_shell.chat.message_count - 1];
            }
            strlcpy(message->tool_name, event->tool_name, sizeof(message->tool_name));
            strlcpy(message->tool_label, event->tool_label, sizeof(message->tool_label));
            strlcpy(message->tool_detail, event->detail, sizeof(message->tool_detail));
            strlcpy(message->tool_latency, event->latency, sizeof(message->tool_latency));
            message->risk = event->risk <= FRIDGE_UI_CHAT_RISK_HIGH ?
                            (fridge_ui_chat_risk_t)event->risk : FRIDGE_UI_CHAT_RISK_HIGH;
            message->tool_status = event->tool_status <= FRIDGE_UI_CHAT_TOOL_STATUS_ERROR ?
                                   (fridge_ui_chat_tool_status_t)event->tool_status :
                                   FRIDGE_UI_CHAT_TOOL_STATUS_ERROR;
            message->requires_confirmation = event->requires_confirmation;
            s_shell.chat.agent_working = event->working;
            redraw_messages = true;
            break;
        }
        case FRIDGE_UI_P4_AGENT_EVENT_TTS:
            strlcpy(s_shell.chat.tts_state, event->text[0] ? event->text : event->state,
                    sizeof(s_shell.chat.tts_state));
            // Agent 文本已经落盘后，TTS 仍在独立任务中运行。发送/停止槽位在合成和播放
            // 阶段必须保持“停止”，使用户能立即关闭 PA_CTRL；完成、失败或停止后才回到发送。
            if (strcmp(event->state, "synthesizing") == 0 || strcmp(event->state, "playing") == 0) {
                s_shell.chat.agent_working = true;
            } else if (strcmp(event->state, "done") == 0 || strcmp(event->state, "stopped") == 0 ||
                       strcmp(event->state, "error") == 0) {
                s_shell.chat.agent_working = false;
            }
            break;
        case FRIDGE_UI_P4_AGENT_EVENT_STATUS:
            strlcpy(s_shell.chat.agent_state, event->state[0] ? event->state : event->text,
                    sizeof(s_shell.chat.agent_state));
            s_shell.chat.agent_working = event->working;
            break;
        default:
            return;
    }
    if (s_shell.chat_initialized &&
        s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
         s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
         s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS)) {
        refresh_portrait_chat_page();
    } else if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT && s_shell.chat_initialized) {
        refresh_chat_page(redraw_messages);
    }
}

void fridge_ui_shell_update_chat(const chat_business_model_t *chat)
{
    if (!chat || !s_shell.created) return;
    s_shell.chat = *chat;
    if (s_shell.orientation == FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY &&
        s_shell.chat_initialized &&
        (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT ||
         s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_HISTORY ||
         s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL ||
         s_shell.page == FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS)) {
        refresh_portrait_chat_page();
    } else if (s_shell.page == FRIDGE_UI_P4_PAGE_CHAT && s_shell.chat_initialized) {
        refresh_chat_page(true);
    }
}

void fridge_ui_shell_update_binding(const fridge_ui_p4_binding_model_t *model)
{
    if (!model) return;
    s_shell.binding = *model;
    s_shell.binding.qr_payload[sizeof(s_shell.binding.qr_payload) - 1U] = '\0';
    s_shell.binding.device_sn[sizeof(s_shell.binding.device_sn) - 1U] = '\0';
    s_shell.binding.status[sizeof(s_shell.binding.status) - 1U] = '\0';
    s_shell.binding.detail[sizeof(s_shell.binding.detail) - 1U] = '\0';
    if (s_shell.page == SHELL_PAGE_BINDING) refresh_binding_page();
}

void fridge_ui_shell_refresh_ime(void)
{
    if (!s_shell.created) return;
    // TF 词库延迟加载完成后只刷新当前存在的输入界面；所有调用均来自 LVGL/UI 任务。
    if (s_shell.alarm_text_dialog) refresh_alarm_ime();
    if (s_shell.chat_initialized && s_shell.page == FRIDGE_UI_P4_PAGE_CHAT) {
        refresh_chat_ime();
    }
}
