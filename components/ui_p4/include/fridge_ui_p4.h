// 冰箱小精灵 P4 原生 MIPI-LVGL UI 公共接口。
// 该组件只依赖 P4 MIPI/GT911/Agent 基础设施，不复用 S3 旧 UI 的硬件依赖。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// P4 当前可由 Agent/USB 请求的页面。页面切换只改变显示状态，不写入本地业务数据。
typedef enum {
    FRIDGE_UI_P4_PAGE_HOME = 0,
    FRIDGE_UI_P4_PAGE_MORE,
    FRIDGE_UI_P4_PAGE_TIMER,
    // MORE-03 倒计时编辑：与闹钟编辑分离，避免表单字段互相污染。
    FRIDGE_UI_P4_PAGE_TIMER_EDITOR,
    FRIDGE_UI_P4_PAGE_ALARM,
    // MORE-03：闹钟新建和详情编辑共用同一个任务页模板。
    FRIDGE_UI_P4_PAGE_ALARM_EDITOR,
    FRIDGE_UI_P4_PAGE_STOPWATCH,
    // INV-05 静态拍照取景页；当前不启动摄像头或创建拍照任务。
    FRIDGE_UI_P4_PAGE_CAMERA,
    // PLAN-01 计划总览：与 HOME/MORE 同为顶级导航页，不写入本地业务数据。
    FRIDGE_UI_P4_PAGE_PLAN,
    // CHAT-01 对话主页面：消息流（user/agent气泡 + tool工具卡 + result结果卡）
    FRIDGE_UI_P4_PAGE_CHAT,
    // CHAT-02 会话历史：搜索条 + 同步筛选 + 会话列表 + 行操作
    FRIDGE_UI_P4_PAGE_CHAT_HISTORY,
    // CHAT-03 工具执行详情：工具信息、权限决策、参数摘要、执行时间线
    FRIDGE_UI_P4_PAGE_CHAT_TOOL_DETAIL,
    // CHAT-04 待确认行动：计划摘要 + 逐条待确认动作
    FRIDGE_UI_P4_PAGE_CHAT_PENDING_ACTIONS,
    // HOME-02 今日摘要详情：复用 HOME 主滚动区与 CTX-01，不写入库存数据。
    FRIDGE_UI_P4_PAGE_HOME_DETAIL,
    // 设备绑定：显示后端签发的短期二维码、有效期和当前 MQTT/家庭状态。
    FRIDGE_UI_P4_PAGE_BINDING,
    // PLAN-06 购物清单：追加枚举值，避免改变既有页面数值；Shell 不直接访问 LittleFS。
    FRIDGE_UI_P4_PAGE_SHOPPING,
    // MORE-05 设置中心：左侧分区栏 + 右侧内容的分区模板，只读展示设备真实状态，
    // 未落地的配置项标注为后续开放，不写入 NVS，也不直接操作硬件。
    FRIDGE_UI_P4_PAGE_SETTINGS,
    // MORE-06 维护中心：与设置中心共用分区模板；存储/关于分区展示真实容量与版本，
    // 插件、日志、OTA 分区当前只做只读占位，不触发 Flash 写入或 OTA 流程。
    FRIDGE_UI_P4_PAGE_MAINTENANCE,
    // INV-01 库存浏览器：分区/层/固定九宫格空间视图与列表视图共用同一份本地库存快照。
    FRIDGE_UI_P4_PAGE_INVENTORY,
    // INV-03 食材详情；所有写操作仍需经过库存模型的原子持久化接口。
    FRIDGE_UI_P4_PAGE_INVENTORY_DETAIL,
    // INV-04 新增/编辑食材；名称、备注等文本字段复用聊天页的中文拼音输入法。
    FRIDGE_UI_P4_PAGE_INVENTORY_EDIT,
    // INV-06 拍照识别候选确认；未确认的候选绝不写入库存。
    FRIDGE_UI_P4_PAGE_INVENTORY_RECOGNIZE,
    // INV-07 搜索与筛选结果；保留进入前的库存空间范围。
    FRIDGE_UI_P4_PAGE_INVENTORY_SEARCH,
    // INV-08 空间形状编辑；布局变更先形成草稿，确认后再持久化。
    FRIDGE_UI_P4_PAGE_INVENTORY_LAYOUT,
    // MORE-04 设备与厨房安全：只读展示设备、连接、传感器和安全事件状态。
    // 追加到枚举末尾，避免改变既有页面的数值编号。
    FRIDGE_UI_P4_PAGE_SAFETY,
    FRIDGE_UI_P4_PAGE_COUNT,
} fridge_ui_p4_page_t;

// P4 屏幕物理扫描时序始终保持 1024x600；竖屏隐私模式只在 LVGL/PPA 层旋转内容，
// 不修改 MIPI-DSI 时钟、分辨率或面板寄存器。
typedef enum {
    FRIDGE_UI_P4_ORIENTATION_LANDSCAPE = 0,
    FRIDGE_UI_P4_ORIENTATION_PORTRAIT_PRIVACY,
    FRIDGE_UI_P4_ORIENTATION_COUNT,
} fridge_ui_p4_orientation_t;

// Agent Runtime 与 UI 的轻量桥接协议。该类型故意不引用 Agent 内部对象，避免 UI、
// Tool Broker 与 Runtime 形成静态链接循环；提交/取消/确认由 UI 任务非阻塞调用，
// 事件只能由 UI 任务通过 poll_event 拉取并应用到 LVGL。
#define FRIDGE_UI_P4_AGENT_TEXT_MAX 512
#define FRIDGE_UI_P4_AGENT_DETAIL_MAX 192
#define FRIDGE_UI_P4_AGENT_ID_MAX 32

typedef enum {
    FRIDGE_UI_P4_AGENT_EVENT_SESSION_RESET = 0,
    FRIDGE_UI_P4_AGENT_EVENT_USER_MESSAGE,
    FRIDGE_UI_P4_AGENT_EVENT_ASSISTANT_MESSAGE,
    FRIDGE_UI_P4_AGENT_EVENT_TOOL,
    FRIDGE_UI_P4_AGENT_EVENT_STATUS,
    FRIDGE_UI_P4_AGENT_EVENT_TTS,
} fridge_ui_p4_agent_event_kind_t;

typedef struct {
    fridge_ui_p4_agent_event_kind_t kind;
    char run_id[FRIDGE_UI_P4_AGENT_ID_MAX];
    char call_id[FRIDGE_UI_P4_AGENT_ID_MAX];
    char time[8];
    char state[32];
    char text[FRIDGE_UI_P4_AGENT_TEXT_MAX];
    char tool_name[48];
    char tool_label[64];
    char detail[FRIDGE_UI_P4_AGENT_DETAIL_MAX];
    char latency[16];
    uint8_t risk;
    uint8_t tool_status;
    bool requires_confirmation;
    bool working;
    // Agent 仅在完成且无需后续追问时标记，连续语音协调器据此决定是否自动收尾。
    bool dialog_complete;
} fridge_ui_p4_agent_event_t;

typedef struct {
    esp_err_t (*submit_text)(const char *text, void *user_ctx);
    esp_err_t (*cancel_active)(void *user_ctx);
    // 清空当前前台会话：仅本地删除已持久化聊天记录，运行中的 Agent 会拒绝此请求。
    esp_err_t (*clear_session)(void *user_ctx);
    esp_err_t (*resolve_confirmation)(const char *run_id, const char *call_id,
                                      bool confirmed, void *user_ctx);
    bool (*poll_event)(fridge_ui_p4_agent_event_t *out, void *user_ctx);
    void *user_ctx;
} fridge_ui_p4_agent_actions_t;

esp_err_t fridge_ui_p4_init(void);
bool fridge_ui_p4_is_ready(void);

// UI 截屏稳定性快照。跨任务读取只使用原子状态，不触碰 LVGL 对象树。
// resource_ready 表示 TF 图标/字体已完成绑定，framebuffers_synced 表示资源整页
// 已经真正提交并同步到另一块显示缓冲，page_frame_ready 表示最近一次页面切换已有
// 至少一帧成功提交；三项都满足后才允许读取当前可见帧。
typedef struct {
    bool ready;
    bool resource_ready;
    bool framebuffers_synced;
    bool page_frame_ready;
    bool privacy_mode;
    fridge_ui_p4_page_t page;
    fridge_ui_p4_orientation_t orientation;
} fridge_ui_p4_status_t;

#define FRIDGE_UI_P4_BINDING_QR_MAX 256
#define FRIDGE_UI_P4_BINDING_TEXT_MAX 96

// 设备绑定页只消费这一份轻量快照。网络任务负责生成二维码内容，UI 任务只负责显示，
// 禁止把 bootstrap secret、MQTT 密码或 AI Key 放进 qr_payload。
typedef struct {
    bool provisioned;
    bool bound;
    bool qr_ready;
    uint32_t expires_in_seconds;
    char qr_payload[FRIDGE_UI_P4_BINDING_QR_MAX];
    char device_sn[40];
    char status[FRIDGE_UI_P4_BINDING_TEXT_MAX];
    char detail[FRIDGE_UI_P4_BINDING_TEXT_MAX];
} fridge_ui_p4_binding_model_t;

// 获取给 USB/Web/Codex 使用的只读 UI 状态；调用不会访问 LVGL。
esp_err_t fridge_ui_p4_get_status(fridge_ui_p4_status_t *out);

// 将页面键名转换为页面枚举；键名来自 USB Tool/Agent 的 JSON 参数。
esp_err_t fridge_ui_p4_page_from_key(const char *key, fridge_ui_p4_page_t *out);

// 返回页面的稳定键名；无效枚举值返回 NULL。
const char *fridge_ui_p4_page_key(fridge_ui_p4_page_t page);

// 横竖屏使用稳定键名 landscape / portrait_privacy，供 USB Tool 与截屏脚本复用。
esp_err_t fridge_ui_p4_orientation_from_key(const char *key,
                                             fridge_ui_p4_orientation_t *out);
const char *fridge_ui_p4_orientation_key(fridge_ui_p4_orientation_t orientation);

// 线程安全、非阻塞地提交页面切换请求。成功只表示请求已进入队列，
// LVGL 对象会在 ui_p4 自己的任务中更新，调用方不得直接触碰 LVGL。
esp_err_t fridge_ui_p4_request_page_async(fridge_ui_p4_page_t page);

// 线程安全、非阻塞地提交横竖屏切换。portrait_privacy 支持 HOME、PLAN、CHAT-01～04、
// MORE-01/02/04/05/06/07/08；其它未移植页面保持横屏。
// 成功只表示请求已进入 UI 队列。
esp_err_t fridge_ui_p4_request_orientation_async(fridge_ui_p4_orientation_t orientation);

// 线程安全地更新绑定页快照；函数只复制数据并置脏标记，不直接访问 LVGL。
esp_err_t fridge_ui_p4_update_binding(const fridge_ui_p4_binding_model_t *model);

// 在 Tool Broker 初始化成功后注册 ui.switch_page 与 ui.set_orientation。两个 Tool
// 都只向 UI 请求队列投递，不允许 Broker 或 Agent 任务直接访问 LVGL。
esp_err_t fridge_ui_p4_register_tools(void);

// 由 Agent Runtime 初始化后注册一次；UI 未注册时仍可正常显示静态页面。
// 调用不会触碰 LVGL，允许 Runtime 在非 UI 任务调用。
esp_err_t fridge_ui_p4_register_agent_actions(const fridge_ui_p4_agent_actions_t *actions);

#ifdef __cplusplus
}
#endif
