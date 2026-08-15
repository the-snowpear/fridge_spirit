#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_MQTT_MAX_URI_LEN 160
#define FRIDGE_MQTT_MAX_ID_LEN 64
#define FRIDGE_MQTT_MAX_TOKEN_LEN 160
#define FRIDGE_MQTT_MAX_STATUS_LEN 160
#define FRIDGE_MQTT_MAX_TOPIC_LEN 192
#define FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS 60
#define FRIDGE_MQTT_PROTOCOL_VERSION 2
#define FRIDGE_MQTT_MAX_PAYLOAD_BYTES (64U * 1024U)

// P4 Agent 公网绑定配置：由 USB 管理通道或二维码绑定流程写入 NVS。
// 注意：password/token 只用于连接 MQTT Broker，绝不能输出到串口响应或日志。
typedef struct {
    char broker_uri[FRIDGE_MQTT_MAX_URI_LEN + 1];
    char home_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char device_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char username[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char password[FRIDGE_MQTT_MAX_TOKEN_LEN + 1];
    uint32_t binding_epoch;
    uint16_t keepalive_seconds;
    bool use_tls;
    bool enabled;
} fridge_mqtt_config_t;

// 云端同步状态：只返回脱敏后的连接情况，供 UI、USB 和诊断组件展示。
typedef struct {
    bool initialized;
    bool enabled;
    bool configured;
    bool connected;
    bool has_password;
    uint32_t reconnect_count;
    uint32_t published_count;
    uint32_t received_count;
    uint32_t rejected_count;
    uint32_t queue_dropped_count;
    uint32_t binding_epoch;
    uint16_t protocol_version;
    int last_error;
    char broker_uri[FRIDGE_MQTT_MAX_URI_LEN + 1];
    char home_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char device_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char username[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char status_text[FRIDGE_MQTT_MAX_STATUS_LEN + 1];
} fridge_mqtt_status_t;

// 初始化 MQTT 协议组件；只创建内部状态，不会在未配置 broker 时发起连接。
esp_err_t fridge_mqtt_protocol_init(void);

// 读取云端绑定配置；password 会返回给调用方用于内部连接，Web 输出时必须自行脱敏。
esp_err_t fridge_mqtt_get_config(fridge_mqtt_config_t *out);

// 保存云端绑定配置。update_password=false 时保留旧密码，避免前端刷新误清凭证。
esp_err_t fridge_mqtt_set_config(const fridge_mqtt_config_t *config, bool update_password);

// 清除 MQTT 密码/token，保留 broker、home_id 和 device_id。
esp_err_t fridge_mqtt_clear_secret(void);

// 获取脱敏状态。
esp_err_t fridge_mqtt_get_status(fridge_mqtt_status_t *out);

// Wi-Fi 联网后启动 MQTT。未启用或配置不完整时返回 ESP_ERR_INVALID_STATE。
esp_err_t fridge_mqtt_start(void);

// 主动停止 MQTT 连接；用于切换配置或调试。
esp_err_t fridge_mqtt_stop(void);

// 发布设备状态快照。online=false 时可用于离线前遗嘱语义的手动上报。
esp_err_t fridge_mqtt_publish_state(bool online);

// 发布设备诊断聚合事件；P4 v2 不依赖旧 S3 传感器模型，也不发布原始高频流。
esp_err_t fridge_mqtt_publish_sensor_snapshot(void);

// 兼容旧调用点：发布 inventory 域“快照可拉取”事件，不再通过 MQTT 发送大型完整快照。
esp_err_t fridge_mqtt_publish_inventory_snapshot(bool force_import);

// 请求后台发布 inventory 域“快照可拉取”事件。
esp_err_t fridge_mqtt_request_inventory_publish(bool force_import);

// 兼容旧调用点：发布 agent_sessions 域“增量可拉取”事件，不直接上传完整会话。
esp_err_t fridge_mqtt_publish_chat_history_delta(void);

// 发布 P4 Agent v2 业务同步事件。data_json 必须是 JSON object/array/null，最大 64KiB。
// 函数只把消息投递给 MQTT worker，不在调用方任务里执行网络 I/O。
esp_err_t fridge_mqtt_publish_sync_event(const char *domain,
                                         const char *entity_id,
                                         const char *operation,
                                         uint32_t base_entity_revision,
                                         const char *data_json);

// 应用后端 HTTP 快照 data（也接受外层 {"data": ...} ApiResponse）：
// {"serverRevision":N,"domains":{"inventory":[{"entityId":"...","entityRevision":N,
// "serverRevision":N,"data":{...}}], ...}}。
// 复用 MQTT sync.apply 的版本/幂等规则并通过 Agent Storage 可恢复写入。
esp_err_t fridge_mqtt_apply_snapshot_json(const char *json);

// 发布命令回执；request_id 必须来自云端命令，便于后端幂等处理。
esp_err_t fridge_mqtt_publish_command_ack(const char *request_id, const char *command, bool ok, const char *message);

// 发布带执行阶段语义的命令回执。
// stage=received 表示设备已收到命令；stage=completed 表示本地执行已完成并携带版本结果。
esp_err_t fridge_mqtt_publish_command_ack_ex(const char *request_id,
                                             const char *command,
                                             const char *stage,
                                             bool ok,
                                             const char *error_code,
                                             uint32_t local_revision,
                                             uint32_t server_revision,
                                             bool applied,
                                             const char *message);

#ifdef __cplusplus
}
#endif
