// HOME-01 业务示例数据与状态舱只读快照。
// TF JSON 只能填充这些有界字段，不能描述 LVGL 对象、事件回调或硬件权限。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fridge_sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_UI_HOME_SCHEMA_VERSION 1
#define FRIDGE_UI_HOME_STATS_MAX 4
#define FRIDGE_UI_HOME_EXPIRING_MAX 3
#define FRIDGE_UI_HOME_TIPS_MAX 2
#define FRIDGE_UI_HOME_JSON_MAX (24U * 1024U)
#define FRIDGE_UI_MORE_SCHEMA_VERSION 1
#define FRIDGE_UI_MORE_TOOL_COUNT 3
#define FRIDGE_UI_MORE_TIMER_PRESETS_MAX 6
#define FRIDGE_UI_MORE_JSON_MAX (8U * 1024U)
#define FRIDGE_UI_PLAN_SCHEMA_VERSION 1
#define FRIDGE_UI_PLAN_MEAL_SLOTS 4
#define FRIDGE_UI_PLAN_EXPIRING_MAX 3
#define FRIDGE_UI_PLAN_SHOPPING_MAX 4
#define FRIDGE_UI_PLAN_WEEK_DAYS 7
#define FRIDGE_UI_PLAN_JSON_MAX (16U * 1024U)
#define FRIDGE_UI_SHOPPING_SCHEMA_VERSION 1
#define FRIDGE_UI_SHOPPING_ITEMS_MAX 8
#define FRIDGE_UI_SHOPPING_JSON_MAX (8U * 1024U)
#define FRIDGE_UI_CHAT_SCHEMA_VERSION 1
#define FRIDGE_UI_CHAT_MESSAGES_MAX 24
#define FRIDGE_UI_CHAT_JSON_MAX (32U * 1024U)

typedef enum {
    FRIDGE_UI_TONE_NORMAL = 0,
    FRIDGE_UI_TONE_OK,
    FRIDGE_UI_TONE_WARN,
    FRIDGE_UI_TONE_DANGER,
} fridge_ui_tone_t;

typedef struct {
    char key[20];
    int value;
    char label[24];
    char icon[20];
    fridge_ui_tone_t tone;
} fridge_ui_home_stat_t;

typedef struct {
    char name[40];
    char location[56];
    char quantity[20];
    char badge[20];
    char expiry[24];
    fridge_ui_tone_t tone;
} fridge_ui_home_expiring_t;

typedef struct {
    char title[48];
    char time[24];
    char difficulty[20];
    int coverage;
    char ready[32];
    char reason[144];
    char primary_action[32];
    char secondary_action[32];
} fridge_ui_home_recommendation_t;

typedef struct {
    char text[192];
    char action[40];
} fridge_ui_home_tip_t;

typedef struct {
    char dinner[80];
    char shopping[64];
    char next_title[40];
    char next_remaining[20];
    char next_source[64];
} fridge_ui_home_summaries_t;

typedef struct {
    uint32_t schema_version;
    fridge_ui_home_stat_t stats[FRIDGE_UI_HOME_STATS_MAX];
    size_t stat_count;
    fridge_ui_home_expiring_t expiring[FRIDGE_UI_HOME_EXPIRING_MAX];
    size_t expiring_count;
    fridge_ui_home_recommendation_t recommendation;
    fridge_ui_home_tip_t agent_tips[FRIDGE_UI_HOME_TIPS_MAX];
    size_t agent_tip_count;
    fridge_ui_home_summaries_t summaries;
    char last_sync[192];
    bool from_tf;
} home_business_model_t;

typedef enum {
    FRIDGE_UI_MORE_TOOL_TIMER = 0,
    FRIDGE_UI_MORE_TOOL_ALARM,
    FRIDGE_UI_MORE_TOOL_STOPWATCH,
} fridge_ui_more_tool_id_t;

typedef struct {
    char id[16];
    char title[24];
    char description[64];
    char icon[20];
} fridge_ui_more_tool_t;

// MORE-01 只允许通过 TF 覆盖这组静态文案和倒计时预设；实时状态由固件维护。
typedef struct {
    uint32_t schema_version;
    fridge_ui_more_tool_t cards[FRIDGE_UI_MORE_TOOL_COUNT];
    char timer_default_name[32];
    char alarm_default_name[32];
    uint16_t timer_presets_minutes[FRIDGE_UI_MORE_TIMER_PRESETS_MAX];
    bool from_tf;
} more_business_model_t;

typedef enum {
    FRIDGE_UI_HEALTH_OFFLINE = 0,
    FRIDGE_UI_HEALTH_OK,
    FRIDGE_UI_HEALTH_WARN,
    FRIDGE_UI_HEALTH_DANGER,
} fridge_ui_health_t;

typedef struct {
    bool time_valid;
    char time_text[8];
    char date_text[20];
    char weekday_text[12];
    char temperature[16];
    char humidity[12];
    fridge_ui_health_t wifi;
    fridge_ui_health_t cloud;
    fridge_ui_health_t tfcard;
    fridge_ui_health_t camera;
    fridge_ui_health_t microphone;
    // 仅供页面展示的实时连接/照片状态；不承载密码、图像数据或硬件控制权。
    int8_t wifi_rssi;
    bool camera_has_frame;
    // 图片识别状态由 ui_p4 编排层维护。Shell 只展示阶段和有界错误摘要，
    // 不接触 JPEG、模型原始回复或 API Key；失败状态会保留到重拍或再次上传。
    bool camera_ai_active;
    bool camera_ai_error;
    char camera_ai_state[24];
    char camera_ai_detail[112];
    // 手动语音弹窗的展示状态。录音、识别等耗时操作由 ui_p4 后台任务执行，
    // Shell 只读取该快照绘制浮层，不直接控制 ES8311、WakeNet 或网络请求。
    bool voice_active;
    bool voice_recording;
    bool voice_can_submit;
    bool voice_can_cancel;
    bool voice_error;
    char voice_state[24];
    char voice_error_text[96];
    // 连续语音对话只在右侧状态舱展示阶段状态，不承载识别文本或会话历史。
    // 所有字段均为编排层写入的有界快照，Shell 不直接触碰音频、WakeNet 或网络任务。
    bool continuous_voice_active;
    bool continuous_voice_error;
    char continuous_voice_state[24];
    char continuous_voice_detail[96];
    char continuous_voice_error_text[96];
} fridge_ui_device_model_t;

// MORE-04 设备与厨房安全的有界只读快照。数据由 ui_p4 从 P4 已接入的状态源复制，
// Shell 只负责显示；这里不新增 USB/MQTT 字段，也不承载安全事件队列或硬件控制权。
typedef struct {
    bool ready;
    bool light_ready;
    bool imu_ready;
    uint8_t light_percent;
    float angle_delta;
    float vibration_peak;
    bool radar_ready;
    bool radar_presence;
    uint16_t radar_distance_raw;
    uint8_t radar_confidence;
    // MQ-5 可燃气体/烟雾（GPIO20 = ADC1_CH4，AO 分压输入）只读展示字段。
    // 全部来自 sensors 最近一次快照；raw/value/percent 均为相对 ADC 读数，不是 ppm，
    // 当前不触发正式燃气报警，也不能替代经认证的燃气/烟雾报警器。
    fridge_mq5_state_t mq5_state;
    bool mq5_ready;
    bool mq5_error;               // 最近一次采样/初始化失败（非 ESP_OK）
    uint16_t mq5_raw_12bit;
    uint16_t mq5_value_10bit;
    uint8_t mq5_adc_percent;
    uint32_t mq5_updated_at_ms;
    uint32_t mq5_warmup_elapsed_sec;
    bool mq5_daily_warmup_complete;
    bool mq5_burn_in_confirmed;
    bool mq5_baseline_ready;
    uint16_t mq5_baseline_raw_12bit;
    int32_t mq5_delta_from_baseline;
} fridge_ui_safety_model_t;

typedef struct {
    char slot[12];
    char dish[48];
} fridge_ui_plan_meal_t;

typedef struct {
    char name[40];
    char badge[20];
    fridge_ui_tone_t tone;
} fridge_ui_plan_expiring_t;

typedef struct {
    char name[40];
    char group[20];
} fridge_ui_plan_shopping_t;

typedef struct {
    char date[12];
    char summary[64];
    bool today;
    bool conflict;
} fridge_ui_plan_week_day_t;

typedef struct {
    char text[192];
    char action[40];
} fridge_ui_plan_tip_t;

typedef struct {
    uint32_t schema_version;
    char week_range[32];
    fridge_ui_plan_meal_t today_meals[FRIDGE_UI_PLAN_MEAL_SLOTS];
    fridge_ui_plan_expiring_t expiring[FRIDGE_UI_PLAN_EXPIRING_MAX];
    size_t expiring_count;
    fridge_ui_plan_shopping_t shopping[FRIDGE_UI_PLAN_SHOPPING_MAX];
    size_t shopping_count;
    fridge_ui_plan_week_day_t week[FRIDGE_UI_PLAN_WEEK_DAYS];
    fridge_ui_plan_tip_t agent_tip;
    char ctx_dinner[64];
    char ctx_shopping_count[16];
    char ctx_expiring_count[16];
    char ctx_cooking_recipe[48];
    char ctx_cooking_step[64];
    int  ctx_cooking_step_cur;
    int  ctx_cooking_step_total;
    bool from_tf;
} plan_business_model_t;

void fridge_ui_home_model_set_fallback(home_business_model_t *out);
void fridge_ui_more_model_set_fallback(more_business_model_t *out);
void fridge_ui_plan_model_set_fallback(plan_business_model_t *out);
esp_err_t fridge_ui_plan_model_parse_json(const char *json, size_t json_len,
                                          plan_business_model_t *out,
                                          char *error, size_t error_size);

// PLAN-06 独立购物模型。schema v1 的旧项目允许缺少新增字段，载入时会补齐默认值。
typedef struct {
    char id[24];
    char name[48];
    char quantity[32];
    char reason[120];
    char priority[16];
    char group[20];
    char source[32];
    char relation[64];
    bool checked;
} fridge_ui_shopping_item_t;

typedef struct {
    uint32_t schema_version;
    char source[32];
    fridge_ui_shopping_item_t items[FRIDGE_UI_SHOPPING_ITEMS_MAX];
    size_t item_count;
    bool from_local;
} shopping_business_model_t;

typedef enum {
    FRIDGE_UI_SHOPPING_MUTATE_ADD_MANUAL = 0,
    FRIDGE_UI_SHOPPING_MUTATE_CLEAR_DONE,
} fridge_ui_shopping_mutation_t;

void fridge_ui_shopping_model_set_fallback(shopping_business_model_t *out);
// 从 storage 的 /cache/shopping_list.json 读取、兼容旧 schema v1，并在首次空 local_seed 时写入六项参考示例。
esp_err_t fridge_ui_shopping_model_load(shopping_business_model_t *out);
// 在副本中变更并持久化；仅在原子写入成功后才提交 out，失败时 out 保持不变。
esp_err_t fridge_ui_shopping_model_mutate(shopping_business_model_t *out,
                                          fridge_ui_shopping_mutation_t mutation,
                                          const char *item_id,
                                          const char *name);
// 将一组已选项目一次性移动到待购买或已购买；选择态由 Shell 管理，不写入 JSON。
esp_err_t fridge_ui_shopping_model_move_selected(shopping_business_model_t *out,
                                                 const char item_ids[][24],
                                                 size_t item_id_count,
                                                 bool purchased);

// 解析完成后立即释放 cJSON DOM；任一未知类型、超长字段或越界数组都会拒绝整个文件。
esp_err_t fridge_ui_home_model_parse_json(const char *json, size_t json_len,
                                          home_business_model_t *out,
                                          char *error, size_t error_size);

// MORE-01 的字段、工具集合和预设数量固定；未知字段、字符串越界或预设乱序都会拒绝。
esp_err_t fridge_ui_more_model_parse_json(const char *json, size_t json_len,
                                          more_business_model_t *out,
                                          char *error, size_t error_size);

// ============ CHAT 对话页数据模型 ============

typedef enum {
    FRIDGE_UI_CHAT_MSG_USER = 0,    // 用户气泡
    FRIDGE_UI_CHAT_MSG_AGENT,       // Agent气泡
    FRIDGE_UI_CHAT_MSG_TOOL,        // 工具调用卡
    FRIDGE_UI_CHAT_MSG_RESULT,      // 结果卡
} fridge_ui_chat_message_type_t;

typedef enum {
    FRIDGE_UI_CHAT_TOOL_STATUS_PENDING = 0,  // 待确认
    FRIDGE_UI_CHAT_TOOL_STATUS_RUNNING,      // 执行中
    FRIDGE_UI_CHAT_TOOL_STATUS_OK,           // 已完成
    FRIDGE_UI_CHAT_TOOL_STATUS_ERROR,        // 失败
} fridge_ui_chat_tool_status_t;

typedef enum {
    FRIDGE_UI_CHAT_RISK_LOW = 0,     // 低风险
    FRIDGE_UI_CHAT_RISK_MEDIUM,      // 中风险
    FRIDGE_UI_CHAT_RISK_HIGH,        // 高风险
} fridge_ui_chat_risk_t;

typedef enum {
    FRIDGE_UI_CHAT_RESULT_RECIPE = 0,     // 菜谱结果
    FRIDGE_UI_CHAT_RESULT_SHOPPING,       // 购物清单结果
    FRIDGE_UI_CHAT_RESULT_INVENTORY,      // 库存结果
} fridge_ui_chat_result_kind_t;

// 对话消息（user/agent气泡、tool工具卡、result结果卡）
typedef struct {
    fridge_ui_chat_message_type_t type;
    char run_id[32];         // 对应 Agent Run，用于确认/取消与状态更新
    char call_id[32];        // 对应 Tool Call；仅工具卡使用
    char text[512];           // user/agent气泡文本
    char time[8];             // 消息时间 "14:20"
    // tool字段
    char tool_name[32];       // 工具标识名 "inventory_query"
    char tool_label[40];      // 工具显示名 "查询库存"
    char tool_detail[128];    // 工具执行详情
    char tool_latency[16];    // 执行耗时 "0.4s"
    fridge_ui_chat_risk_t risk;
    fridge_ui_chat_tool_status_t tool_status;
    bool requires_confirmation;
    // result字段
    fridge_ui_chat_result_kind_t result_kind;
    char result_title[64];    // 结果标题
    char result_desc[160];    // 结果描述
    char result_ref_id[24];   // 结果关联ID
} fridge_ui_chat_message_t;

// CHAT-01 对话主页面数据模型
typedef struct {
    uint32_t schema_version;
    char permission_mode[20]; // 权限模式 "标准模式"
    bool online;              // 网络在线
    fridge_ui_chat_message_t messages[FRIDGE_UI_CHAT_MESSAGES_MAX];
    size_t message_count;
    bool agent_working;
    char agent_state[32];
    char tts_state[32];
    bool from_tf;
} chat_business_model_t;

// 初始化唯一 foreground 会话的空模型。此入口不写入演示消息或多会话数据。
void fridge_ui_chat_model_init_foreground(chat_business_model_t *out);
// 清空 foreground 的消息与瞬态运行态，保留已有网络与权限状态供恢复流程继续刷新。
void fridge_ui_chat_model_reset_foreground_session(chat_business_model_t *out);
// 兼容旧的配置加载调用；语义与空 foreground 初始化一致。
void fridge_ui_chat_model_set_fallback(chat_business_model_t *out);
esp_err_t fridge_ui_chat_model_parse_json(const char *json, size_t json_len,
                                          chat_business_model_t *out,
                                          char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
