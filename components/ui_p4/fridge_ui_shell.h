// ver2 横屏 Shell（HOME-01 + 五项导航 + CTX-01）。
// 本模块只在 LVGL 任务中创建和更新对象，硬件动作通过回调交还 ui_p4 编排层。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fridge_kitchen_tools.h"
#include "fridge_ui_assets.h"
#include "fridge_ui_model.h"
#include "fridge_ui_p4.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 计时工具操作由 UI 编排层执行，Shell 不直接访问 kitchen_tools，确保所有 LVGL 更新
// 都留在当前 UI 任务中，业务状态仍由编排层集中维护。
typedef enum {
    FRIDGE_UI_SHELL_KITCHEN_TIMER_START = 0,
    FRIDGE_UI_SHELL_KITCHEN_TIMER_PAUSE,
    FRIDGE_UI_SHELL_KITCHEN_TIMER_RESUME,
    FRIDGE_UI_SHELL_KITCHEN_TIMER_CANCEL,
    FRIDGE_UI_SHELL_KITCHEN_TIMER_DISMISS,
    FRIDGE_UI_SHELL_KITCHEN_TIMER_ADD_SECONDS,
    FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_START,
    FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_PAUSE,
    FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_LAP,
    FRIDGE_UI_SHELL_KITCHEN_STOPWATCH_RESET,
    FRIDGE_UI_SHELL_KITCHEN_ALARM_CREATE,
    FRIDGE_UI_SHELL_KITCHEN_ALARM_UPDATE,
    FRIDGE_UI_SHELL_KITCHEN_ALARM_SET_ENABLED,
    FRIDGE_UI_SHELL_KITCHEN_ALARM_PREVIEW,
    FRIDGE_UI_SHELL_KITCHEN_ALARM_CANCEL,
    FRIDGE_UI_SHELL_KITCHEN_ALARM_DISMISS,
} fridge_ui_shell_kitchen_action_t;

// 倒计时：seconds/label/source/sound/timer_id；闹钟：alarm_config 或 alarm_id。
// 指针均指向 Shell 内部或静态数据，回调必须在函数返回前完成读取，不能异步保存该指针。
typedef struct {
    uint32_t seconds;
    uint8_t alarm_id;
    uint8_t timer_id;
    bool alarm_enabled;
    const char *label;
    const char *source;
    fridge_kitchen_timer_sound_t timer_sound;
    const fridge_kitchen_alarm_config_t *alarm_config;
} fridge_ui_shell_kitchen_request_t;

typedef esp_err_t (*fridge_ui_shell_kitchen_action_cb_t)(
    void *user_ctx,
    fridge_ui_shell_kitchen_action_t action,
    const fridge_ui_shell_kitchen_request_t *request);

typedef enum {
    FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PENDING = 0,
    FRIDGE_UI_SHELL_SHOPPING_MOVE_SELECTED_TO_PURCHASED,
    FRIDGE_UI_SHELL_SHOPPING_ADD_MANUAL,
    FRIDGE_UI_SHELL_SHOPPING_CLEAR_DONE,
} fridge_ui_shell_shopping_action_t;

// 单项添加/清空使用 item_id 或 name；批量移动使用固定上限的 ID 列表，回调同步消费。
typedef struct {
    const char *item_id;
    const char *name;
    const char (*item_ids)[24];
    size_t item_id_count;
} fridge_ui_shell_shopping_request_t;

typedef esp_err_t (*fridge_ui_shell_shopping_action_cb_t)(
    void *user_ctx, fridge_ui_shell_shopping_action_t action,
    const fridge_ui_shell_shopping_request_t *request);

// 设置中心与 ui_p4 编排层之间只传递固定长度的纯数据快照，避免 UI 层依赖 network、
// display 或 wake_word 组件的内部类型。Wi-Fi 密码只存在于一次操作请求中，状态快照
// 绝不能回填或记录明文密码。
#define FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS 12
#define FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN 32
#define FRIDGE_UI_SHELL_SETTINGS_WIFI_PASSWORD_LEN 64
#define FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN 96
#define FRIDGE_UI_SHELL_SETTINGS_WAKE_DETAIL_LEN 64

typedef enum {
    FRIDGE_UI_SHELL_SETTINGS_SET_BRIGHTNESS = 0,
    FRIDGE_UI_SHELL_SETTINGS_SET_WAKE_WORD_ENABLED,
    // Agent 权限模式仅由设置后台任务持久化，避免在 LVGL 回调里擦写 Flash。
    FRIDGE_UI_SHELL_SETTINGS_SET_AGENT_PERMISSION_MODE,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_SCAN,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT,
    // 已保存热点复用 NVS 中已验证凭据；请求只携带 SSID，不经过密码输入框。
    FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECT_SAVED,
} fridge_ui_shell_settings_action_t;

// Wi-Fi 连接在编排层先以 save=false 测试；仅 DHCP 与 SNTP 验证成功后才按
// save_after_verified 保存 NVS。回调必须在返回前复制 password，之后 Shell 会清零请求。
typedef struct {
    uint8_t brightness_percent;      // 10～100；拖动期间 persist=false，抬手时才持久化。
    bool persist;
    bool wake_word_enabled;
    uint8_t agent_permission_mode;  // 0=确认，1=标准，2=自动（全访问）。
    bool save_after_verified;
    char ssid[FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN + 1];
    char password[FRIDGE_UI_SHELL_SETTINGS_WIFI_PASSWORD_LEN + 1];
} fridge_ui_shell_settings_request_t;

typedef esp_err_t (*fridge_ui_shell_settings_action_cb_t)(
    void *user_ctx, fridge_ui_shell_settings_action_t action,
    const fridge_ui_shell_settings_request_t *request);

typedef enum {
    FRIDGE_UI_SHELL_SETTINGS_WIFI_IDLE = 0,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_SCANNING,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTING,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_VERIFYING,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_CONNECTED,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_RESTORING,
    FRIDGE_UI_SHELL_SETTINGS_WIFI_FAILED,
} fridge_ui_shell_settings_wifi_phase_t;

typedef struct {
    char ssid[FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    bool secured;
    bool saved;
    char authmode[24];
} fridge_ui_shell_settings_wifi_ap_t;

// UI 编排层在 LVGL 任务内写入的设置状态。wifi_status 可展示 DHCP、SNTP 验证或
// 恢复旧网络进度，但不得包含密码或其它长期凭据。
typedef struct {
    uint8_t brightness_percent;
    bool wake_word_user_enabled;
    bool wake_word_listening;
    uint8_t agent_permission_mode;
    char wake_word_detail[FRIDGE_UI_SHELL_SETTINGS_WAKE_DETAIL_LEN];
    fridge_ui_shell_settings_wifi_phase_t wifi_phase;
    char wifi_status[FRIDGE_UI_SHELL_SETTINGS_WIFI_STATUS_LEN];
    char wifi_ssid[FRIDGE_UI_SHELL_SETTINGS_WIFI_SSID_LEN + 1];
    char wifi_ip[16];
    size_t wifi_ap_count;
    fridge_ui_shell_settings_wifi_ap_t wifi_aps[FRIDGE_UI_SHELL_SETTINGS_WIFI_MAX_APS];
} fridge_ui_shell_settings_state_t;

typedef struct {
    void (*camera_pressed)(void *user_ctx);
    void (*camera_reshoot)(void *user_ctx);
    void (*camera_upload)(void *user_ctx);
    void (*camera_exit)(void *user_ctx);
    void (*camera_release)(void *user_ctx);
    // 右侧固定语音按钮用于启动/结束连续语音会话；不会复用对话页的单次录音入口。
    void (*continuous_voice_pressed)(void *user_ctx);
    // 对话页语音按钮保持单次录音；浮层内的取消和提交也只属于这条手动链路。
    void (*manual_voice_pressed)(void *user_ctx);
    void (*voice_submitted)(void *user_ctx);
    void (*voice_cancelled)(void *user_ctx);
    // 竖屏隐私页的返回、导航和主按钮通过编排层排队；HOME、PLAN、CHAT-01～04
    // 与 MORE-01/02/04/05/06/07/08 可保持竖屏直接切换，其它目标页由编排层先安全恢复横屏。
    void (*navigate_requested)(void *user_ctx, fridge_ui_p4_page_t page);
    esp_err_t (*chat_submit)(void *user_ctx, const char *text);
    esp_err_t (*chat_cancel)(void *user_ctx);
    esp_err_t (*chat_clear_session)(void *user_ctx);
    esp_err_t (*chat_resolve_confirmation)(void *user_ctx, const char *run_id,
                                            const char *call_id, bool confirmed);
    fridge_ui_shell_kitchen_action_cb_t kitchen_action;
    fridge_ui_shell_shopping_action_cb_t shopping_action;
    // 设置操作回调只负责快速投递后台工作；不得在 LVGL 回调中扫描、连网、写 NVS 或
    // 加载/停止 WakeNet。回调返回前必须复制 request 中的固定长度字段。
    fridge_ui_shell_settings_action_cb_t settings_action;
    void *user_ctx;
} fridge_ui_shell_actions_t;

esp_err_t fridge_ui_shell_create(lv_obj_t *root, const home_business_model_t *home,
                                 const fridge_ui_shell_actions_t *actions);
// 仅允许 ui_p4 的 LVGL 任务调用；跨任务请求必须先进入 fridge_ui_p4 的异步队列。
esp_err_t fridge_ui_shell_set_page(fridge_ui_p4_page_t page);
// 仅允许 ui_p4 的 LVGL 任务调用；竖屏布局覆盖 HOME、PLAN、CHAT-01～04 与
// MORE-01/02/04/05/06/07/08，编辑页及其它未移植页继续使用横屏对象树。
esp_err_t fridge_ui_shell_set_orientation(fridge_ui_p4_orientation_t orientation);
void fridge_ui_shell_bind_assets(const fridge_ui_assets_t *assets);
void fridge_ui_shell_update_home(const home_business_model_t *home);
void fridge_ui_shell_update_device(const fridge_ui_device_model_t *device);
// 更新 MORE-04 的只读设备/传感器快照；仅由 ui_p4 的 LVGL 任务调用，不改变公共协议。
void fridge_ui_shell_update_safety(const fridge_ui_safety_model_t *state);
// 后台设置工作完成后由 ui_p4 的 LVGL 任务调用，Shell 只刷新当前可见对象。
void fridge_ui_shell_update_settings(const fridge_ui_shell_settings_state_t *state);
// 快照由编排层每秒传入；Shell 仅更新当前可见页面中发生变化的文本。
void fridge_ui_shell_update_kitchen_tools(const fridge_kitchen_tools_snapshot_t *snapshot);
// 静态更多页文案和预设可来自经校验的 TF more.json；无 TF 时保持固件内置默认值。
void fridge_ui_shell_update_more(const more_business_model_t *more);
void fridge_ui_shell_update_plan(const plan_business_model_t *plan);
void fridge_ui_shell_update_shopping(const shopping_business_model_t *shopping);
// 更新对话页数据模型；集成本地 agent_runtime 的消息流和工具调用状态。
void fridge_ui_shell_update_chat(const chat_business_model_t *chat);
// 更新设备二维码绑定页；仅允许 UI 任务调用，二维码内容不得包含长期凭据。
void fridge_ui_shell_update_binding(const fridge_ui_p4_binding_model_t *model);
// TF 完整词库延迟加载完成后刷新当前可见输入法；仅允许 ui_p4 的 LVGL 任务调用。
void fridge_ui_shell_refresh_ime(void);
// UI 编排层在 LVGL 任务中逐条应用 Agent 事件；Agent Runtime 本身不得调用本函数。
void fridge_ui_shell_apply_agent_event(const fridge_ui_p4_agent_event_t *event);
void fridge_ui_shell_set_resource_state(bool ready, const char *detail);
// 在拍照页取景区显示/清除 RGB565 回看图，并同步切换“拍摄/上传”按钮状态。
// pixels 由 ui_p4 编排层持有；Shell 只在当前 LVGL 任务中引用，不负责释放。
void fridge_ui_shell_set_camera_review(const uint8_t *pixels, size_t len,
                                       int width, int height);
// 将图片识别出的首选候选填入 INV-04 草稿。这里只打开确认页，不写库存；
// name/quantity/note 必须是已校验长度的 UTF-8 文本，调用期间同步复制。
esp_err_t fridge_ui_shell_open_inventory_prefill(const char *name,
                                                 const char *quantity,
                                                 const char *note);
void fridge_ui_shell_show_toast(const char *text);
void fridge_ui_shell_tick(int64_t now_us);
void fridge_ui_shell_force_redraw(void);

// 返回 HOME-01 唯一的主滚动容器；仅供 UI 任务内的 PPA 滚动加速器绑定事件。
lv_obj_t *fridge_ui_shell_get_main_scroll(void);
// 返回竖屏主滚动容器；对象只在 portrait_privacy 模式存在，所有 MORE 工具页共用它。
lv_obj_t *fridge_ui_shell_get_portrait_scroll(void);
// 返回当前页面的 PPA 动态覆盖区域；定时器/秒表只覆盖实时数字，避免滚动时冻结。
lv_obj_t *fridge_ui_shell_get_ppa_dynamic_overlay(void);

#ifdef __cplusplus
}
#endif
