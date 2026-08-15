/*
 * 冰箱小精灵 P4 Agent MQTT v2 协议组件。
 *
 * 本组件只负责公网 MQTT 连接、v2 envelope 编解码和 Agent Tool Broker 桥接。
 * 它不依赖任何 S3 时代的传感器、storage、ASR 或 speaker 数据模型；
 * 仅在 ai_config 域落盘后复用 P4 当前 ai_client NVS 接口更新运行配置。
 *
 * 并发约束：
 * - ESP-MQTT 事件回调只做状态更新、64KiB 长度检查、PSRAM 分片拷贝和队列投递；
 * - JSON 解析、Tool Broker 执行、订阅和发布都在低优先级 worker 中进行；
 * - MQTT/TLS 任务栈留在内部 SRAM，大消息和完整分片优先放 32MB PSRAM。
 */

#include "fridge_mqtt_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fridge_agent_storage.h"
#include "fridge_ai_client.h"
#include "fridge_network.h"
#include "fridge_task_scheduler.h"
#include "fridge_tool_broker.h"
#include "nvs.h"

static const char *TAG = "fridge_mqtt_v2";

// 使用全新的 P4 v2 namespace，启动时不会误读设备上残留的 S3 MQTT 凭据或绑定关系。
#define MQTT_NVS_NAMESPACE "p4_mqtt_v2"
#define MQTT_NVS_KEY_URI "uri"
#define MQTT_NVS_KEY_HOME "home"
#define MQTT_NVS_KEY_DEVICE "device"
#define MQTT_NVS_KEY_USER "user"
#define MQTT_NVS_KEY_PASS "pass"
#define MQTT_NVS_KEY_KEEPALIVE "keepalive"
#define MQTT_NVS_KEY_ENABLED "enabled"
#define MQTT_NVS_KEY_TLS "tls"
#define MQTT_NVS_KEY_EPOCH "bind_epoch"
#define MQTT_NVS_KEY_DEDUPE "dedupe"

#define MQTT_WORKER_QUEUE_DEPTH 10
#define MQTT_WORKER_STACK_BYTES 8192
#define MQTT_WORKER_PRIORITY 4
#define MQTT_RX_TOPIC_BYTES (FRIDGE_MQTT_MAX_TOPIC_LEN + 1U)
#define MQTT_MESSAGE_ID_BYTES 40
#define MQTT_TYPE_BYTES 48
#define MQTT_DOMAIN_BYTES 48
#define MQTT_OPERATION_BYTES 16
#define MQTT_RECONNECT_TIMEOUT_MS 2000
#define MQTT_OUTBOX_LIMIT_BYTES (256U * 1024U)
#define MQTT_AGENT_FILE_MAX_BYTES (192U * 1024U)
#define MQTT_HTTP_SNAPSHOT_MAX_BYTES (2U * 1024U * 1024U)
#define MQTT_DEDUPE_MAGIC 0x4D513244U
#define MQTT_DEDUPE_CAPACITY 8

typedef enum {
    MQTT_DEDUPE_EMPTY = 0,
    MQTT_DEDUPE_EXECUTING = 1,
    MQTT_DEDUPE_COMPLETED = 2,
} mqtt_dedupe_state_t;

typedef struct {
    uint8_t state;
    bool ok;
    char request_id[FRIDGE_MQTT_MAX_ID_LEN + 1];
    char command[MQTT_TYPE_BYTES];
    char error_code[FRIDGE_TOOL_MAX_CODE_LEN + 1];
    char message[FRIDGE_TOOL_MAX_SUMMARY_LEN + 1];
} mqtt_dedupe_entry_t;

typedef struct {
    uint32_t magic;
    uint8_t next;
    mqtt_dedupe_entry_t entries[MQTT_DEDUPE_CAPACITY];
} mqtt_dedupe_store_t;

typedef struct {
    const char *path;
    const char *array_name;
    const char *task_kind;
} mqtt_sync_target_t;

typedef struct {
    uint32_t local_revision;
    uint32_t server_revision;
    bool applied;
    bool duplicate;
} mqtt_sync_apply_result_t;

typedef enum {
    MQTT_JOB_CONNECTED = 0,
    MQTT_JOB_RX_MESSAGE,
    MQTT_JOB_TX_MESSAGE,
} mqtt_job_type_t;

typedef struct {
    mqtt_job_type_t type;
    char topic[MQTT_RX_TOPIC_BYTES];
    char *payload; // RX/TX 完整负载，所有权随队列转移给 worker。
    size_t payload_len;
    uint8_t qos;
    bool retain;
} mqtt_job_t;

typedef struct {
    char *payload; // 分片组装缓冲，优先位于 PSRAM。
    size_t total_len;
    size_t received_len;
    int msg_id;
    char topic[MQTT_RX_TOPIC_BYTES];
} mqtt_rx_assembly_t;

typedef struct {
    bool initialized;
    fridge_mqtt_config_t config;
    fridge_mqtt_status_t status;
    esp_mqtt_client_handle_t client;
    QueueHandle_t queue;
    TaskHandle_t worker_task;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t sync_lock;
    mqtt_rx_assembly_t rx;
    mqtt_dedupe_store_t dedupe;
    uint32_t consecutive_disconnects;
    int64_t reconnect_due_us;
    bool reconnect_pending;
    uint32_t message_counter;

    // ESP-MQTT 不复制这些配置字符串，必须在 client 整个生命周期内保持有效。
    char topic_down_command[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    char topic_up_event[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    char topic_up_ack[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    char topic_up_state[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    char topic_up_error[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    char lwt_payload[512];
} mqtt_context_t;

static mqtt_context_t s_mqtt;
static uint32_t s_agent_session_entity_revision;

static void *mqtt_large_alloc(size_t size);
static uint32_t mqtt_json_u32(const cJSON *object, const char *name,
                              uint32_t fallback);

static void mqtt_load_agent_session_revision(void)
{
    char *text = mqtt_large_alloc(MQTT_AGENT_FILE_MAX_BYTES + 1U);
    if (!text) return;
    size_t text_size = 0;
    if (fridge_agent_storage_json_read(
            "agent/data/sessions.json", text,
            MQTT_AGENT_FILE_MAX_BYTES + 1U, &text_size, NULL) == ESP_OK) {
        cJSON *root = cJSON_ParseWithLength(text, text_size);
        if (root) {
            s_agent_session_entity_revision =
                mqtt_json_u32(root, "sync_entity_revision", 0);
            cJSON_Delete(root);
        }
    }
    free(text);
}

static void *mqtt_large_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        // PSRAM 尚未就绪时允许小概率回退，避免组件初始化直接崩溃；诊断状态会记录内存错误。
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

static void mqtt_set_status_text_locked(const char *text, int last_error)
{
    strlcpy(s_mqtt.status.status_text, text ? text : "", sizeof(s_mqtt.status.status_text));
    s_mqtt.status.last_error = last_error;
}

static void mqtt_set_status_text(const char *text, int last_error)
{
    if (s_mqtt.lock && xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        mqtt_set_status_text_locked(text, last_error);
        xSemaphoreGive(s_mqtt.lock);
    }
}

static bool mqtt_is_tls_uri(const char *uri)
{
    return uri && (strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0);
}

static bool mqtt_is_plain_uri(const char *uri)
{
    return uri && (strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "ws://", 5) == 0);
}

static esp_err_t mqtt_validate_config(const fridge_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->broker_uri[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "broker URI is empty");
    ESP_RETURN_ON_FALSE(config->home_id[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "home ID is empty");
    ESP_RETURN_ON_FALSE(config->device_id[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "device ID is empty");
    ESP_RETURN_ON_FALSE(mqtt_is_tls_uri(config->broker_uri) || mqtt_is_plain_uri(config->broker_uri),
                        ESP_ERR_INVALID_ARG, TAG, "unsupported broker URI scheme");
    ESP_RETURN_ON_FALSE(config->use_tls == mqtt_is_tls_uri(config->broker_uri),
                        ESP_ERR_INVALID_ARG, TAG, "TLS flag does not match broker URI");
    return ESP_OK;
}

static esp_err_t mqtt_nvs_read_string(nvs_handle_t handle, const char *key,
                                      char *out, size_t out_size)
{
    size_t required = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t mqtt_load_config(fridge_mqtt_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open MQTT NVS failed");

    err = mqtt_nvs_read_string(handle, MQTT_NVS_KEY_URI, out->broker_uri, sizeof(out->broker_uri));
    if (err == ESP_OK) err = mqtt_nvs_read_string(handle, MQTT_NVS_KEY_HOME, out->home_id, sizeof(out->home_id));
    if (err == ESP_OK) err = mqtt_nvs_read_string(handle, MQTT_NVS_KEY_DEVICE, out->device_id, sizeof(out->device_id));
    if (err == ESP_OK) err = mqtt_nvs_read_string(handle, MQTT_NVS_KEY_USER, out->username, sizeof(out->username));
    if (err == ESP_OK) err = mqtt_nvs_read_string(handle, MQTT_NVS_KEY_PASS, out->password, sizeof(out->password));
    if (err == ESP_OK) {
        uint16_t keepalive = 0;
        if (nvs_get_u16(handle, MQTT_NVS_KEY_KEEPALIVE, &keepalive) == ESP_OK && keepalive > 0) {
            out->keepalive_seconds = keepalive;
        }
        uint8_t value = 0;
        if (nvs_get_u8(handle, MQTT_NVS_KEY_ENABLED, &value) == ESP_OK) out->enabled = value != 0;
        if (nvs_get_u8(handle, MQTT_NVS_KEY_TLS, &value) == ESP_OK) out->use_tls = value != 0;
        (void)nvs_get_u32(handle, MQTT_NVS_KEY_EPOCH, &out->binding_epoch);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t mqtt_save_config(const fridge_mqtt_config_t *config, bool update_password)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open MQTT NVS failed");

    esp_err_t err = nvs_set_str(handle, MQTT_NVS_KEY_URI, config->broker_uri);
    if (err == ESP_OK) err = nvs_set_str(handle, MQTT_NVS_KEY_HOME, config->home_id);
    if (err == ESP_OK) err = nvs_set_str(handle, MQTT_NVS_KEY_DEVICE, config->device_id);
    if (err == ESP_OK) err = nvs_set_str(handle, MQTT_NVS_KEY_USER, config->username);
    if (err == ESP_OK && update_password) err = nvs_set_str(handle, MQTT_NVS_KEY_PASS, config->password);
    if (err == ESP_OK) {
        const uint16_t keepalive = config->keepalive_seconds > 0
                                       ? config->keepalive_seconds
                                       : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
        err = nvs_set_u16(handle, MQTT_NVS_KEY_KEEPALIVE, keepalive);
    }
    if (err == ESP_OK) err = nvs_set_u8(handle, MQTT_NVS_KEY_ENABLED, config->enabled ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_u8(handle, MQTT_NVS_KEY_TLS, config->use_tls ? 1U : 0U);
    if (err == ESP_OK) err = nvs_set_u32(handle, MQTT_NVS_KEY_EPOCH, config->binding_epoch);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t mqtt_save_dedupe(void);

static esp_err_t mqtt_load_dedupe(void)
{
    memset(&s_mqtt.dedupe, 0, sizeof(s_mqtt.dedupe));
    s_mqtt.dedupe.magic = MQTT_DEDUPE_MAGIC;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "open MQTT dedupe NVS failed");
    size_t size = sizeof(s_mqtt.dedupe);
    err = nvs_get_blob(handle, MQTT_NVS_KEY_DEDUPE, &s_mqtt.dedupe, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_mqtt.dedupe, 0, sizeof(s_mqtt.dedupe));
        s_mqtt.dedupe.magic = MQTT_DEDUPE_MAGIC;
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH || size != sizeof(s_mqtt.dedupe) ||
        s_mqtt.dedupe.magic != MQTT_DEDUPE_MAGIC ||
        s_mqtt.dedupe.next >= MQTT_DEDUPE_CAPACITY) {
        ESP_LOGW(TAG, "Reset incompatible MQTT request dedupe store");
        memset(&s_mqtt.dedupe, 0, sizeof(s_mqtt.dedupe));
        s_mqtt.dedupe.magic = MQTT_DEDUPE_MAGIC;
        return mqtt_save_dedupe();
    }
    return err;
}

static esp_err_t mqtt_save_dedupe(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open MQTT dedupe NVS failed");
    esp_err_t err = nvs_set_blob(handle, MQTT_NVS_KEY_DEDUPE,
                                 &s_mqtt.dedupe, sizeof(s_mqtt.dedupe));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static mqtt_dedupe_entry_t *mqtt_find_dedupe(const char *request_id)
{
    for (size_t i = 0; i < MQTT_DEDUPE_CAPACITY; ++i) {
        mqtt_dedupe_entry_t *entry = &s_mqtt.dedupe.entries[i];
        if (entry->state != MQTT_DEDUPE_EMPTY &&
            strcmp(entry->request_id, request_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

static esp_err_t mqtt_begin_dedupe(const char *request_id, const char *command,
                                   mqtt_dedupe_entry_t **out)
{
    mqtt_dedupe_entry_t *entry = &s_mqtt.dedupe.entries[s_mqtt.dedupe.next];
    s_mqtt.dedupe.next = (uint8_t)((s_mqtt.dedupe.next + 1U) % MQTT_DEDUPE_CAPACITY);
    memset(entry, 0, sizeof(*entry));
    entry->state = MQTT_DEDUPE_EXECUTING;
    strlcpy(entry->request_id, request_id, sizeof(entry->request_id));
    strlcpy(entry->command, command, sizeof(entry->command));
    esp_err_t err = mqtt_save_dedupe();
    if (err != ESP_OK) {
        memset(entry, 0, sizeof(*entry));
        return err;
    }
    *out = entry;
    return ESP_OK;
}

static bool mqtt_build_topics_locked(void)
{
    const char *prefix = "fridge/v2";
    int n1 = snprintf(s_mqtt.topic_down_command, sizeof(s_mqtt.topic_down_command),
                      "%s/%s/%s/down/command", prefix,
                      s_mqtt.config.home_id, s_mqtt.config.device_id);
    int n2 = snprintf(s_mqtt.topic_up_event, sizeof(s_mqtt.topic_up_event),
                      "%s/%s/%s/up/event", prefix,
                      s_mqtt.config.home_id, s_mqtt.config.device_id);
    int n3 = snprintf(s_mqtt.topic_up_ack, sizeof(s_mqtt.topic_up_ack),
                      "%s/%s/%s/up/ack", prefix,
                      s_mqtt.config.home_id, s_mqtt.config.device_id);
    int n4 = snprintf(s_mqtt.topic_up_state, sizeof(s_mqtt.topic_up_state),
                      "%s/%s/%s/up/state", prefix,
                      s_mqtt.config.home_id, s_mqtt.config.device_id);
    int n5 = snprintf(s_mqtt.topic_up_error, sizeof(s_mqtt.topic_up_error),
                      "%s/%s/%s/up/error", prefix,
                      s_mqtt.config.home_id, s_mqtt.config.device_id);
    return n1 > 0 && (size_t)n1 < sizeof(s_mqtt.topic_down_command) &&
           n2 > 0 && (size_t)n2 < sizeof(s_mqtt.topic_up_event) &&
           n3 > 0 && (size_t)n3 < sizeof(s_mqtt.topic_up_ack) &&
           n4 > 0 && (size_t)n4 < sizeof(s_mqtt.topic_up_state) &&
           n5 > 0 && (size_t)n5 < sizeof(s_mqtt.topic_up_error);
}

static void mqtt_make_message_id(char out[MQTT_MESSAGE_ID_BYTES])
{
    // 该计数器可能同时由 Agent 调用任务和 MQTT worker 使用；原子递增避免为生成 ID 持有状态锁。
    const uint32_t counter = __atomic_add_fetch(&s_mqtt.message_counter, 1U, __ATOMIC_RELAXED);
    snprintf(out, MQTT_MESSAGE_ID_BYTES, "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
             esp_random(), (uint32_t)esp_timer_get_time(), counter);
}

static int64_t mqtt_now_ms(void)
{
    time_t now = time(NULL);
    if (now >= 1700000000) {
        return (int64_t)now * 1000LL;
    }
    return esp_timer_get_time() / 1000LL;
}

static cJSON *mqtt_create_envelope_for_config(const fridge_mqtt_config_t *config,
                                               const char *type,
                                               const char *request_id)
{
    char message_id[MQTT_MESSAGE_ID_BYTES];
    mqtt_make_message_id(message_id);

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "schemaVersion", FRIDGE_MQTT_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "messageId", message_id);
    if (request_id && request_id[0]) {
        cJSON_AddStringToObject(root, "requestId", request_id);
    } else {
        cJSON_AddNullToObject(root, "requestId");
    }
    cJSON_AddStringToObject(root, "homeId", config->home_id);
    cJSON_AddStringToObject(root, "deviceId", config->device_id);
    cJSON_AddNumberToObject(root, "bindingEpoch", config->binding_epoch);
    cJSON_AddNumberToObject(root, "sentAt", (double)mqtt_now_ms());
    cJSON_AddStringToObject(root, "type", type);
    return root;
}

static cJSON *mqtt_create_envelope(const char *type, const char *request_id)
{
    fridge_mqtt_config_t config = {0};
    if (fridge_mqtt_get_config(&config) != ESP_OK) {
        return NULL;
    }
    return mqtt_create_envelope_for_config(&config, type, request_id);
}

static esp_err_t mqtt_queue_owned_message(const char *topic, char *payload,
                                          size_t payload_len, int qos, bool retain)
{
    if (!topic || !payload || payload_len == 0 || payload_len > FRIDGE_MQTT_MAX_PAYLOAD_BYTES) {
        free(payload);
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(payload);
        return ESP_ERR_TIMEOUT;
    }
    const bool connected = s_mqtt.status.connected && s_mqtt.client != NULL;
    xSemaphoreGive(s_mqtt.lock);
    if (!connected) {
        free(payload);
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_job_t job = {
        .type = MQTT_JOB_TX_MESSAGE,
        .payload = payload,
        .payload_len = payload_len,
        .qos = (uint8_t)qos,
        .retain = retain,
    };
    strlcpy(job.topic, topic, sizeof(job.topic));
    if (xQueueSend(s_mqtt.queue, &job, 0) != pdTRUE) {
        free(payload);
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.queue_dropped_count++;
            mqtt_set_status_text_locked("MQTT worker queue full", ESP_ERR_TIMEOUT);
            xSemaphoreGive(s_mqtt.lock);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t mqtt_queue_json(const char *topic, cJSON *root, int qos, bool retain)
{
    if (!root) return ESP_ERR_NO_MEM;
    // cJSON 默认分配器使用内部堆；用预分配 PSRAM 缓冲避免大型 envelope 与 TLS/DMA 抢内部 SRAM。
    char *payload = mqtt_large_alloc(FRIDGE_MQTT_MAX_PAYLOAD_BYTES + 1U);
    if (!payload) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    const bool printed = cJSON_PrintPreallocated(root, payload,
                                                FRIDGE_MQTT_MAX_PAYLOAD_BYTES + 1U,
                                                false);
    cJSON_Delete(root);
    if (!printed) {
        free(payload);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t length = strlen(payload);
    return mqtt_queue_owned_message(topic, payload, length, qos, retain);
}

static esp_err_t mqtt_publish_error(const char *request_id, const char *code, const char *message)
{
    cJSON *root = mqtt_create_envelope("device.error", request_id);
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    if (!payload) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "code", code ? code : "UNKNOWN");
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    return mqtt_queue_json(s_mqtt.topic_up_error, root, 1, false);
}

static bool mqtt_json_string(cJSON *object, const char *name, const char **out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static bool mqtt_sync_domain_valid(const char *domain)
{
    static const char *const domains[] = {
        "inventory", "reminders", "timers", "alarms", "stopwatch", "shopping",
        "recipes", "nutrition", "agent_sessions", "agent_memory",
        "device_settings", "ai_config",
    };
    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i) {
        if (strcmp(domain, domains[i]) == 0) return true;
    }
    return false;
}

static bool mqtt_sync_target_for_domain(const char *domain, mqtt_sync_target_t *out)
{
    if (strcmp(domain, "inventory") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/inventory.json", "items", NULL};
    } else if (strcmp(domain, "reminders") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/tasks.json", "tasks", "reminder"};
    } else if (strcmp(domain, "timers") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/tasks.json", "tasks", "timer"};
    } else if (strcmp(domain, "alarms") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/tasks.json", "tasks", "alarm"};
    } else if (strcmp(domain, "stopwatch") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/stopwatch.json", "items", NULL};
    } else if (strcmp(domain, "shopping") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/shopping.json", "items", NULL};
    } else if (strcmp(domain, "recipes") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/recipes.json", "items", NULL};
    } else if (strcmp(domain, "nutrition") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/nutrition.json", "items", NULL};
    } else if (strcmp(domain, "agent_sessions") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/sessions.json", "sessions", NULL};
    } else if (strcmp(domain, "agent_memory") == 0) {
        *out = (mqtt_sync_target_t){"agent/data/memory.json", "items", NULL};
    } else if (strcmp(domain, "device_settings") == 0) {
        *out = (mqtt_sync_target_t){"agent/config/device_settings.json", "items", NULL};
    } else if (strcmp(domain, "ai_config") == 0) {
        *out = (mqtt_sync_target_t){"agent/config/ai_config.json", "items", NULL};
    } else {
        return false;
    }
    return true;
}

static uint32_t mqtt_json_u32(const cJSON *object, const char *name, uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)item->valuedouble;
}

static int mqtt_find_entity_index(const cJSON *array, const char *entity_id)
{
    int index = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(id) && strcmp(id->valuestring, entity_id) == 0) {
            return index;
        }
        ++index;
    }
    return -1;
}

static cJSON *mqtt_ensure_array(cJSON *root, const char *name)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsArray(array)) return array;
    if (array) cJSON_DeleteItemFromObjectCaseSensitive(root, name);
    return cJSON_AddArrayToObject(root, name);
}

static void mqtt_json_replace_string(cJSON *object, const char *name, const char *value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(object, name);
    cJSON_AddStringToObject(object, name, value);
}

static void mqtt_json_replace_number(cJSON *object, const char *name, uint32_t value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(object, name);
    cJSON_AddNumberToObject(object, name, value);
}

static esp_err_t mqtt_apply_ai_config(const cJSON *data)
{
    const cJSON *base_url = cJSON_GetObjectItemCaseSensitive(data, "apiBaseUrl");
    const cJSON *model = cJSON_GetObjectItemCaseSensitive(data, "model");
    const cJSON *api_key = cJSON_GetObjectItemCaseSensitive(data, "apiKey");
    if (!base_url && !model && !api_key) return ESP_OK;
    if ((base_url && !cJSON_IsString(base_url)) ||
        (model && !cJSON_IsString(model)) ||
        (api_key && !cJSON_IsString(api_key))) {
        return ESP_ERR_INVALID_ARG;
    }

    fridge_ai_config_update_t *config = mqtt_large_alloc(sizeof(*config));
    if (!config) return ESP_ERR_NO_MEM;
    memset(config, 0, sizeof(*config));
    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }
    if (base_url) {
        if (strlen(base_url->valuestring) > FRIDGE_AI_MAX_BASE_URL_LEN) err = ESP_ERR_INVALID_SIZE;
        else strlcpy(config->api_base_url, base_url->valuestring, sizeof(config->api_base_url));
    }
    if (err == ESP_OK && model) {
        if (strlen(model->valuestring) > FRIDGE_AI_MAX_MODEL_LEN) err = ESP_ERR_INVALID_SIZE;
        else strlcpy(config->model, model->valuestring, sizeof(config->model));
    }
    if (err == ESP_OK && api_key) {
        if (strlen(api_key->valuestring) > FRIDGE_AI_MAX_API_KEY_LEN) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            strlcpy(config->api_key, api_key->valuestring, sizeof(config->api_key));
            config->update_api_key = true;
        }
    }
    if (err == ESP_OK) {
        // 日志只由 ai_client 输出 key_updated 标志，当前层绝不记录 apiKey 明文。
        err = fridge_ai_client_set_config(config);
    }
    memset(config->api_key, 0, sizeof(config->api_key));
    free(config);
    return err;
}

static esp_err_t mqtt_write_sync_root(const char *path, cJSON *root)
{
    char *printed = mqtt_large_alloc(MQTT_AGENT_FILE_MAX_BYTES + 1U);
    if (!printed) return ESP_ERR_NO_MEM;
    const bool print_ok = cJSON_PrintPreallocated(root, printed,
                                                 MQTT_AGENT_FILE_MAX_BYTES + 1U,
                                                 false);
    if (!print_ok) {
        free(printed);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = fridge_agent_storage_json_write(path, printed, strlen(printed));
    free(printed);
    return err;
}

static esp_err_t mqtt_apply_sync_event_locked(const char *domain,
                                              const cJSON *event,
                                              mqtt_sync_apply_result_t *out)
{
    memset(out, 0, sizeof(*out));
    mqtt_sync_target_t target;
    ESP_RETURN_ON_FALSE(mqtt_sync_target_for_domain(domain, &target),
                        ESP_ERR_INVALID_ARG, TAG, "unsupported sync domain");

    const char *entity_id = NULL;
    const char *operation = "upsert";
    ESP_RETURN_ON_FALSE(mqtt_json_string((cJSON *)event, "entityId", &entity_id) &&
                            strlen(entity_id) <= FRIDGE_MQTT_MAX_ID_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sync entityId");
    const cJSON *operation_item = cJSON_GetObjectItemCaseSensitive(event, "operation");
    if (cJSON_IsString(operation_item)) operation = operation_item->valuestring;
    ESP_RETURN_ON_FALSE(strcmp(operation, "upsert") == 0 ||
                            strcmp(operation, "append") == 0 ||
                            strcmp(operation, "delete") == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sync operation");

    const uint32_t incoming_revision = mqtt_json_u32(event, "entityRevision", 0);
    const uint32_t server_revision = mqtt_json_u32(event, "serverRevision", 0);
    ESP_RETURN_ON_FALSE(incoming_revision > 0 && server_revision > 0,
                        ESP_ERR_INVALID_ARG, TAG, "sync revisions are required");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(event, "data");
    if (strcmp(operation, "delete") != 0) {
        ESP_RETURN_ON_FALSE(cJSON_IsObject(data), ESP_ERR_INVALID_ARG,
                            TAG, "sync upsert/append data must be object");
    }

    if (strcmp(domain, "agent_sessions") == 0) {
        // Agent Runtime 的真实文件 schema 是 {foreground:{messages:...}}，不能套用
        // 通用 {sessions:[]} 容器，否则下一次会话恢复会丢失。
        out->local_revision = s_agent_session_entity_revision;
        out->server_revision = server_revision;
        if (s_agent_session_entity_revision > incoming_revision) {
            return ESP_ERR_INVALID_STATE;
        }
        if (s_agent_session_entity_revision == incoming_revision &&
            incoming_revision != 0) {
            out->duplicate = true;
            return ESP_OK;
        }
        cJSON *session_root = cJSON_CreateObject();
        cJSON *foreground =
            strcmp(operation, "delete") == 0
                ? cJSON_CreateObject()
                : cJSON_Duplicate(data, true);
        if (!session_root || !foreground) {
            cJSON_Delete(session_root);
            cJSON_Delete(foreground);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(session_root, "schema_version", 1);
        if (strcmp(operation, "delete") == 0) {
            cJSON_AddNumberToObject(foreground, "next_sequence", 0);
            cJSON_AddArrayToObject(foreground, "messages");
        }
        cJSON_AddItemToObject(session_root, "foreground", foreground);
        cJSON_AddNumberToObject(session_root, "sync_entity_revision",
                                incoming_revision);
        cJSON_AddNumberToObject(session_root, "sync_server_revision",
                                server_revision);
        esp_err_t session_err = mqtt_write_sync_root(
            "agent/data/sessions.json", session_root);
        cJSON_Delete(session_root);
        if (session_err == ESP_OK) {
            s_agent_session_entity_revision = incoming_revision;
            out->local_revision = incoming_revision;
            out->applied = true;
        }
        return session_err;
    }

    char fallback[96];
    int fallback_len = snprintf(fallback, sizeof(fallback),
                                "{\"schema_version\":1,\"%s\":[]}", target.array_name);
    ESP_RETURN_ON_FALSE(fallback_len > 0 && (size_t)fallback_len < sizeof(fallback),
                        ESP_ERR_INVALID_SIZE, TAG, "sync fallback is too large");

    char *text = mqtt_large_alloc(MQTT_AGENT_FILE_MAX_BYTES + 1U);
    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "allocate Agent sync read buffer failed");
    size_t text_size = 0;
    esp_err_t err = fridge_agent_storage_json_read(target.path, text,
                                                    MQTT_AGENT_FILE_MAX_BYTES + 1U,
                                                    &text_size, fallback);
    if (err != ESP_OK) {
        free(text);
        return err;
    }
    cJSON *root = cJSON_ParseWithLength(text, text_size);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Agent sync file is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *items = mqtt_ensure_array(root, target.array_name);
    cJSON *tombstones = mqtt_ensure_array(root, "sync_tombstones");
    if (!items || !tombstones) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int item_index = mqtt_find_entity_index(items, entity_id);
    int tombstone_index = mqtt_find_entity_index(tombstones, entity_id);
    cJSON *existing = item_index >= 0 ? cJSON_GetArrayItem(items, item_index)
                                     : (tombstone_index >= 0
                                            ? cJSON_GetArrayItem(tombstones, tombstone_index)
                                            : NULL);
    const uint32_t local_revision = existing
                                        ? mqtt_json_u32(existing, "entityRevision", 0)
                                        : 0;
    out->local_revision = local_revision;
    out->server_revision = server_revision;
    if (local_revision > incoming_revision) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }
    if (local_revision == incoming_revision && local_revision != 0) {
        out->duplicate = true;
        cJSON_Delete(root);
        return strcmp(domain, "ai_config") == 0 && cJSON_IsObject(data)
                   ? mqtt_apply_ai_config(data)
                   : ESP_OK;
    }

    if (strcmp(operation, "delete") == 0) {
        if (item_index >= 0) cJSON_DeleteItemFromArray(items, item_index);
        cJSON *tombstone = cJSON_CreateObject();
        if (!tombstone) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(tombstone, "id", entity_id);
        cJSON_AddStringToObject(tombstone, "domain", domain);
        cJSON_AddBoolToObject(tombstone, "deleted", true);
        cJSON_AddNumberToObject(tombstone, "entityRevision", incoming_revision);
        cJSON_AddNumberToObject(tombstone, "serverRevision", server_revision);
        if (tombstone_index >= 0) cJSON_ReplaceItemInArray(tombstones, tombstone_index, tombstone);
        else cJSON_AddItemToArray(tombstones, tombstone);
    } else {
        cJSON *item = cJSON_Duplicate(data, true);
        if (!item) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        if (strcmp(domain, "ai_config") == 0) {
            // AI Key 允许在比赛联调阶段经鉴权 MQTT 明文同步，但设备长期存储只写
            // ai_client 的 NVS；TF 上的 Agent 镜像仅记录是否已配置，避免密钥进入
            // 可直接读取的 JSON、截图导出或后续诊断包。
            const cJSON *api_key =
                cJSON_GetObjectItemCaseSensitive(item, "apiKey");
            const bool has_api_key =
                cJSON_IsString(api_key) && api_key->valuestring[0] != '\0';
            cJSON_DeleteItemFromObjectCaseSensitive(item, "apiKey");
            cJSON_DeleteItemFromObjectCaseSensitive(item, "hasApiKey");
            cJSON_AddBoolToObject(item, "hasApiKey", has_api_key);
        }
        mqtt_json_replace_string(item, "id", entity_id);
        mqtt_json_replace_number(item, "entityRevision", incoming_revision);
        mqtt_json_replace_number(item, "serverRevision", server_revision);
        if (target.task_kind) mqtt_json_replace_string(item, "kind", target.task_kind);
        if (item_index >= 0) cJSON_ReplaceItemInArray(items, item_index, item);
        else cJSON_AddItemToArray(items, item);
        if (tombstone_index >= 0) cJSON_DeleteItemFromArray(tombstones, tombstone_index);
    }
    const uint32_t file_server_revision = mqtt_json_u32(root, "server_revision", 0);
    if (server_revision > file_server_revision) {
        mqtt_json_replace_number(root, "server_revision", server_revision);
    }

    err = mqtt_write_sync_root(target.path, root);
    cJSON_Delete(root);
    if (err != ESP_OK) return err;
    if (target.task_kind) fridge_task_scheduler_notify_reload();

    out->local_revision = incoming_revision;
    out->applied = true;
    if (strcmp(domain, "ai_config") == 0 && cJSON_IsObject(data)) {
        err = mqtt_apply_ai_config(data);
    }
    if (strcmp(domain, "stopwatch") == 0) {
        // kitchen_tools 暂无导入接口；当前只更新 Agent canonical store，运行态在后续专用接口补齐后接入。
        ESP_LOGD(TAG, "stopwatch sync stored in Agent canonical file");
    }
    return err;
}

static esp_err_t mqtt_reset_snapshot_domain_locked(const char *domain,
                                                   uint32_t server_revision)
{
    if (strcmp(domain, "agent_sessions") == 0) {
        // 快照里的 foreground 实体随后走专用 apply；空域时保留设备本地历史，
        // 避免一个尚未产生云端聊天记录的家庭把当前会话清空。
        s_agent_session_entity_revision = 0;
        return ESP_OK;
    }

    mqtt_sync_target_t target;
    ESP_RETURN_ON_FALSE(mqtt_sync_target_for_domain(domain, &target),
                        ESP_ERR_INVALID_ARG, TAG, "unsupported snapshot domain");

    char fallback[96];
    int fallback_len = snprintf(fallback, sizeof(fallback),
                                "{\"schema_version\":1,\"%s\":[]}", target.array_name);
    ESP_RETURN_ON_FALSE(fallback_len > 0 && (size_t)fallback_len < sizeof(fallback),
                        ESP_ERR_INVALID_SIZE, TAG, "snapshot fallback is too large");

    char *text = mqtt_large_alloc(MQTT_AGENT_FILE_MAX_BYTES + 1U);
    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "allocate snapshot read buffer failed");
    size_t text_size = 0;
    esp_err_t err = fridge_agent_storage_json_read(target.path, text,
                                                    MQTT_AGENT_FILE_MAX_BYTES + 1U,
                                                    &text_size, fallback);
    if (err != ESP_OK) {
        free(text);
        return err;
    }
    cJSON *root = cJSON_ParseWithLength(text, text_size);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *items = mqtt_ensure_array(root, target.array_name);
    cJSON *tombstones = mqtt_ensure_array(root, "sync_tombstones");
    if (!items || !tombstones) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    if (!target.task_kind) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, target.array_name);
        items = cJSON_AddArrayToObject(root, target.array_name);
        cJSON_DeleteItemFromObjectCaseSensitive(root, "sync_tombstones");
        tombstones = cJSON_AddArrayToObject(root, "sync_tombstones");
        if (!items || !tombstones) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    } else {
        // reminders/timers/alarms 共用 tasks.json；完整快照只替换当前 kind，不能清空另外两域。
        for (int i = cJSON_GetArraySize(items) - 1; i >= 0; --i) {
            const cJSON *item = cJSON_GetArrayItem(items, i);
            const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
            if (cJSON_IsString(kind) && strcmp(kind->valuestring, target.task_kind) == 0) {
                cJSON_DeleteItemFromArray(items, i);
            }
        }
        for (int i = cJSON_GetArraySize(tombstones) - 1; i >= 0; --i) {
            const cJSON *item = cJSON_GetArrayItem(tombstones, i);
            const cJSON *item_domain = cJSON_GetObjectItemCaseSensitive(item, "domain");
            if (!cJSON_IsString(item_domain) || strcmp(item_domain->valuestring, domain) == 0) {
                cJSON_DeleteItemFromArray(tombstones, i);
            }
        }
    }
    mqtt_json_replace_number(root, "server_revision", server_revision);
    err = mqtt_write_sync_root(target.path, root);
    cJSON_Delete(root);
    if (err == ESP_OK && target.task_kind) fridge_task_scheduler_notify_reload();
    return err;
}

static esp_err_t mqtt_execute_tool_call(const cJSON *root, const cJSON *payload)
{
    const char *request_id = NULL;
    const char *tool_name = NULL;
    cJSON *request_item = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (cJSON_IsString(request_item)) request_id = request_item->valuestring;
    if (!request_id || strlen(request_id) > FRIDGE_MQTT_MAX_ID_LEN ||
        !mqtt_json_string((cJSON *)payload, "toolName", &tool_name) ||
        strlen(tool_name) > FRIDGE_TOOL_MAX_NAME_LEN) {
        (void)mqtt_publish_error(request_id, "INVALID_TOOL_CALL", "requestId/toolName is required");
        return ESP_ERR_INVALID_ARG;
    }

    mqtt_dedupe_entry_t *dedupe = mqtt_find_dedupe(request_id);
    if (dedupe) {
        // QoS1 重投或设备重启后的重复命令只重发 ACK，绝不再次调用有副作用的 Tool。
        const bool completed = dedupe->state == MQTT_DEDUPE_COMPLETED;
        return fridge_mqtt_publish_command_ack_ex(
            request_id, dedupe->command, "completed",
            completed && dedupe->ok,
            completed ? dedupe->error_code : "DUPLICATE_EXECUTION_UNKNOWN",
            0, 0, completed,
            completed ? dedupe->message : "命令已开始执行，但设备未记录到最终结果");
    }

    fridge_tool_call_t *call = mqtt_large_alloc(sizeof(*call));
    fridge_tool_result_t *result = mqtt_large_alloc(sizeof(*result));
    if (!call || !result) {
        free(call);
        free(result);
        (void)mqtt_publish_error(request_id, "NO_MEMORY", "cannot allocate tool workspace");
        return ESP_ERR_NO_MEM;
    }
    memset(call, 0, sizeof(*call));
    memset(result, 0, sizeof(*result));
    strlcpy(call->tool_name, tool_name, sizeof(call->tool_name));

    const char *text = NULL;
    if (mqtt_json_string((cJSON *)payload, "callId", &text)) {
        if (strlen(text) >= sizeof(call->call_id)) {
            free(call);
            free(result);
            (void)mqtt_publish_error(request_id, "INVALID_TOOL_CALL", "callId is too long");
            return ESP_ERR_INVALID_SIZE;
        }
        strlcpy(call->call_id, text, sizeof(call->call_id));
    }
    if (mqtt_json_string((cJSON *)payload, "runId", &text)) {
        if (strlen(text) >= sizeof(call->run_id)) {
            free(call);
            free(result);
            (void)mqtt_publish_error(request_id, "INVALID_TOOL_CALL", "runId is too long");
            return ESP_ERR_INVALID_SIZE;
        }
        strlcpy(call->run_id, text, sizeof(call->run_id));
    }
    if (mqtt_json_string((cJSON *)payload, "idempotencyKey", &text)) {
        if (strlen(text) >= sizeof(call->idempotency_key)) {
            free(call);
            free(result);
            (void)mqtt_publish_error(request_id, "INVALID_TOOL_CALL",
                                     "idempotencyKey is too long");
            return ESP_ERR_INVALID_SIZE;
        }
        strlcpy(call->idempotency_key, text, sizeof(call->idempotency_key));
    } else {
        strlcpy(call->idempotency_key, request_id, sizeof(call->idempotency_key));
    }
    cJSON *confirmed = cJSON_GetObjectItemCaseSensitive(payload, "userConfirmed");
    call->user_confirmed = cJSON_IsTrue(confirmed);

    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(payload, "arguments");
    char *arguments_text = arguments ? cJSON_PrintUnformatted(arguments) : NULL;
    if (arguments_text && strlen(arguments_text) > FRIDGE_TOOL_MAX_ARGS_LEN) {
        cJSON_free(arguments_text);
        free(call);
        free(result);
        (void)mqtt_publish_error(request_id, "TOOL_ARGUMENTS_TOO_LARGE",
                                 "tool arguments exceed 1024 bytes");
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(call->arguments_json, arguments_text ? arguments_text : "{}", sizeof(call->arguments_json));
    if (arguments_text) cJSON_free(arguments_text);

    esp_err_t dedupe_err = mqtt_begin_dedupe(request_id, "agent.tool_call", &dedupe);
    if (dedupe_err != ESP_OK) {
        free(call);
        free(result);
        (void)mqtt_publish_error(request_id, "DEDUPE_PERSIST_FAILED",
                                 "cannot persist request ID before tool execution");
        return dedupe_err;
    }

    esp_err_t err = fridge_tool_broker_call(call, result);

    dedupe->state = MQTT_DEDUPE_COMPLETED;
    dedupe->ok = err == ESP_OK && result->status == FRIDGE_TOOL_RESULT_OK;
    strlcpy(dedupe->error_code, result->code, sizeof(dedupe->error_code));
    strlcpy(dedupe->message, result->summary, sizeof(dedupe->message));
    esp_err_t dedupe_save_err = mqtt_save_dedupe();
    if (dedupe_save_err != ESP_OK) {
        ESP_LOGE(TAG, "Persist completed request ID failed: %s",
                 esp_err_to_name(dedupe_save_err));
    }

    cJSON *ack = mqtt_create_envelope("command.ack", request_id);
    cJSON *ack_payload = ack ? cJSON_AddObjectToObject(ack, "payload") : NULL;
    if (!ack_payload) {
        cJSON_Delete(ack);
        free(call);
        free(result);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(ack_payload, "command", "agent.tool_call");
    cJSON_AddStringToObject(ack_payload, "stage", "completed");
    cJSON_AddBoolToObject(ack_payload, "ok", err == ESP_OK && result->status == FRIDGE_TOOL_RESULT_OK);
    cJSON_AddStringToObject(ack_payload, "toolName", call->tool_name);
    cJSON_AddStringToObject(ack_payload, "status", fridge_tool_result_status_name(result->status));
    cJSON_AddStringToObject(ack_payload, "code", result->code);
    cJSON_AddStringToObject(ack_payload, "message", result->summary);
    if (result->data_json[0]) {
        cJSON *data = cJSON_Parse(result->data_json);
        if (data) cJSON_AddItemToObject(ack_payload, "data", data);
    }
    esp_err_t publish_err = mqtt_queue_json(s_mqtt.topic_up_ack, ack, 1, false);
    free(call);
    free(result);
    return err == ESP_OK ? publish_err : err;
}

static esp_err_t mqtt_apply_sync_command(const cJSON *root,
                                         const cJSON *payload,
                                         const char *command_type)
{
    const cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "messageId");
    const char *request_id = cJSON_IsString(request) ? request->valuestring
                             : cJSON_IsString(message) ? message->valuestring
                                                      : NULL;
    ESP_RETURN_ON_FALSE(request_id, ESP_ERR_INVALID_ARG, TAG, "sync command has no correlation ID");

    const cJSON *events = cJSON_GetObjectItemCaseSensitive(payload, "events");
    const bool wrapped_events = cJSON_IsArray(events);
    const int event_count = wrapped_events ? cJSON_GetArraySize(events) : 1;
    ESP_RETURN_ON_FALSE(event_count > 0 && event_count <= 50,
                        ESP_ERR_INVALID_SIZE, TAG, "sync command event count is invalid");

    (void)fridge_mqtt_publish_command_ack_ex(request_id, command_type, "accepted",
                                             true, "", 0, 0, false,
                                             "sync command accepted by P4 worker");

    if (xSemaphoreTake(s_mqtt.sync_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        (void)fridge_mqtt_publish_command_ack_ex(request_id, command_type, "rejected",
                                                 false, "SYNC_BUSY", 0, 0, false,
                                                 "waiting for Agent sync lock timed out");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t storage_lock_err =
        fridge_agent_storage_transaction_begin(30000U);
    if (storage_lock_err != ESP_OK) {
        xSemaphoreGive(s_mqtt.sync_lock);
        (void)fridge_mqtt_publish_command_ack_ex(
            request_id, command_type, "rejected", false, "SYNC_STORAGE_BUSY",
            0, 0, false, "waiting for Agent Storage transaction timed out");
        return storage_lock_err;
    }

    esp_err_t final_err = ESP_OK;
    mqtt_sync_apply_result_t last = {0};
    for (int i = 0; i < event_count; ++i) {
        const cJSON *event = wrapped_events ? cJSON_GetArrayItem(events, i) : payload;
        const char *domain = NULL;
        if (!cJSON_IsObject(event) ||
            !mqtt_json_string((cJSON *)event, "domain", &domain)) {
            final_err = ESP_ERR_INVALID_ARG;
            break;
        }
        final_err = mqtt_apply_sync_event_locked(domain, event, &last);
        if (final_err != ESP_OK) break;
    }
    fridge_agent_storage_transaction_end();
    xSemaphoreGive(s_mqtt.sync_lock);

    if (final_err == ESP_OK) {
        (void)fridge_mqtt_publish_command_ack_ex(
            request_id, command_type, "completed", true, "",
            last.local_revision, last.server_revision, last.applied,
            last.duplicate ? "sync event already applied" : "sync event applied");
    } else {
        const bool conflict = final_err == ESP_ERR_INVALID_STATE;
        (void)fridge_mqtt_publish_command_ack_ex(
            request_id, command_type, "rejected", false,
            conflict ? "REVISION_CONFLICT" : esp_err_to_name(final_err),
            last.local_revision, last.server_revision, last.applied,
            conflict ? "local entity revision is newer than cloud event"
                     : "P4 Agent sync apply failed");
    }
    return final_err;
}

static esp_err_t mqtt_validate_envelope(cJSON *root, const char **out_type)
{
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    cJSON *epoch = cJSON_GetObjectItemCaseSensitive(root, "bindingEpoch");
    cJSON *sent_at = cJSON_GetObjectItemCaseSensitive(root, "sentAt");
    const char *home_id = NULL;
    const char *device_id = NULL;
    const char *message_id = NULL;
    const char *type = NULL;
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) ||
        version->valueint != FRIDGE_MQTT_PROTOCOL_VERSION ||
        !mqtt_json_string(root, "messageId", &message_id) ||
        !mqtt_json_string(root, "homeId", &home_id) ||
        !mqtt_json_string(root, "deviceId", &device_id) ||
        !mqtt_json_string(root, "type", &type) ||
        !cJSON_IsNumber(epoch) || epoch->valuedouble < 0 ||
        epoch->valuedouble > UINT32_MAX ||
        epoch->valuedouble != (double)(uint32_t)epoch->valuedouble ||
        !cJSON_IsNumber(sent_at)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(message_id) > FRIDGE_MQTT_MAX_ID_LEN ||
        strlen(home_id) > FRIDGE_MQTT_MAX_ID_LEN ||
        strlen(device_id) > FRIDGE_MQTT_MAX_ID_LEN ||
        strlen(type) >= MQTT_TYPE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(home_id, s_mqtt.config.home_id) != 0 ||
        strcmp(device_id, s_mqtt.config.device_id) != 0 ||
        (uint32_t)epoch->valuedouble != s_mqtt.config.binding_epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_type = type;
    return ESP_OK;
}

static void mqtt_process_rx_message(mqtt_job_t *job)
{
    if (strcmp(job->topic, s_mqtt.topic_down_command) != 0) {
        mqtt_set_status_text("rejected unexpected MQTT topic", ESP_ERR_INVALID_ARG);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(job->payload, job->payload_len);
    const char *type = NULL;
    esp_err_t err = root ? mqtt_validate_envelope(root, &type) : ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) {
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.rejected_count++;
            mqtt_set_status_text_locked("rejected invalid v2 envelope", err);
            xSemaphoreGive(s_mqtt.lock);
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        (void)mqtt_publish_error(NULL, "INVALID_PAYLOAD", "payload must be an object");
    } else if (strcmp(type, "agent.tool_call") == 0) {
        (void)mqtt_execute_tool_call(root, payload);
    } else if (strcmp(type, "sync.request_snapshot") == 0) {
        const char *domain = NULL;
        if (!mqtt_json_string(payload, "domain", &domain)) domain = "all";
        cJSON *data = cJSON_CreateObject();
        if (data) {
            cJSON_AddStringToObject(data, "requestedDomain", domain);
            cJSON_AddBoolToObject(data, "httpSnapshotRequired", true);
            char *data_text = cJSON_PrintUnformatted(data);
            cJSON_Delete(data);
            if (data_text) {
                (void)fridge_mqtt_publish_sync_event("device_settings", "sync_control",
                                                     "upsert", 0, data_text);
                cJSON_free(data_text);
            }
        }
    } else if (strcmp(type, "device.ping") == 0) {
        (void)fridge_mqtt_publish_state(true);
    } else if (strcmp(type, "sync.ack") == 0) {
        mqtt_set_status_text("last sync event acknowledged", ESP_OK);
    } else if (strcmp(type, "sync.events") == 0 || strcmp(type, "sync.apply") == 0) {
        (void)mqtt_apply_sync_command(root, payload, type);
    } else {
        cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "requestId");
        (void)mqtt_publish_error(cJSON_IsString(request) ? request->valuestring : NULL,
                                 "UNSUPPORTED_COMMAND", type);
    }
    cJSON_Delete(root);
}

static void mqtt_worker(void *arg)
{
    (void)arg;
    mqtt_job_t job;
    for (;;) {
        // 重连状态独立于业务队列，防止队列已满时丢失唯一的重连触发。
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_mqtt.reconnect_pending && s_mqtt.client &&
                !s_mqtt.status.connected &&
                esp_timer_get_time() >= s_mqtt.reconnect_due_us) {
                s_mqtt.reconnect_pending = false;
                const esp_err_t err = esp_mqtt_client_reconnect(s_mqtt.client);
                if (err != ESP_OK) {
                    mqtt_set_status_text_locked("MQTT reconnect request failed", err);
                }
            }
            xSemaphoreGive(s_mqtt.lock);
        }

        if (xQueueReceive(s_mqtt.queue, &job, pdMS_TO_TICKS(250)) != pdTRUE) continue;

        if (job.type == MQTT_JOB_CONNECTED) {
            if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                esp_mqtt_client_handle_t client = s_mqtt.client;
                int id = client
                             ? esp_mqtt_client_subscribe(client, s_mqtt.topic_down_command, 1)
                             : -1;
                xSemaphoreGive(s_mqtt.lock);
                if (id < 0) {
                    mqtt_set_status_text("subscribe command topic failed", ESP_FAIL);
                } else {
                    ESP_LOGI(TAG, "Subscribed P4 v2 command topic (QoS1)");
                    (void)fridge_mqtt_publish_state(true);
                }
            }
        } else if (job.type == MQTT_JOB_RX_MESSAGE) {
            mqtt_process_rx_message(&job);
        } else if (job.type == MQTT_JOB_TX_MESSAGE) {
            if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                esp_mqtt_client_handle_t client = s_mqtt.client;
                const bool connected = s_mqtt.status.connected;
                // enqueue 不在调用者/LVGL/Agent 任务里做网络 I/O；QoS1 数据进入 ESP-MQTT outbox。
                int id = (client && connected)
                             ? esp_mqtt_client_enqueue(client, job.topic, job.payload,
                                                       (int)job.payload_len, job.qos,
                                                       job.retain ? 1 : 0, true)
                             : -1;
                if (id >= 0) {
                    s_mqtt.status.published_count++;
                } else {
                    mqtt_set_status_text_locked(id == -2 ? "MQTT outbox full" : "MQTT enqueue failed",
                                                ESP_FAIL);
                }
                xSemaphoreGive(s_mqtt.lock);
            } else {
                ESP_LOGW(TAG, "MQTT state lock timeout while publishing");
            }
        }
        free(job.payload);
    }
}

static void mqtt_reset_rx_assembly(void)
{
    free(s_mqtt.rx.payload);
    memset(&s_mqtt.rx, 0, sizeof(s_mqtt.rx));
}

static void mqtt_reject_rx(const char *reason, int error)
{
    mqtt_reset_rx_assembly();
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_mqtt.status.rejected_count++;
        mqtt_set_status_text_locked(reason, error);
        xSemaphoreGive(s_mqtt.lock);
    }
}

static void mqtt_receive_fragment(esp_mqtt_event_handle_t event)
{
    if (event->total_data_len <= 0 ||
        (size_t)event->total_data_len > FRIDGE_MQTT_MAX_PAYLOAD_BYTES ||
        event->current_data_offset < 0 || event->data_len < 0 ||
        event->current_data_offset + event->data_len > event->total_data_len) {
        mqtt_reject_rx("MQTT payload exceeds 64KiB or has invalid offsets", ESP_ERR_INVALID_SIZE);
        return;
    }

    if (event->current_data_offset == 0) {
        mqtt_reset_rx_assembly();
        if (event->topic_len <= 0 || (size_t)event->topic_len > FRIDGE_MQTT_MAX_TOPIC_LEN) {
            mqtt_reject_rx("MQTT topic length is invalid", ESP_ERR_INVALID_SIZE);
            return;
        }
        s_mqtt.rx.payload = mqtt_large_alloc((size_t)event->total_data_len + 1U);
        if (!s_mqtt.rx.payload) {
            mqtt_reject_rx("cannot allocate MQTT RX buffer", ESP_ERR_NO_MEM);
            return;
        }
        s_mqtt.rx.total_len = (size_t)event->total_data_len;
        s_mqtt.rx.msg_id = event->msg_id;
        memcpy(s_mqtt.rx.topic, event->topic, (size_t)event->topic_len);
        s_mqtt.rx.topic[event->topic_len] = '\0';
    }

    if (!s_mqtt.rx.payload || s_mqtt.rx.msg_id != event->msg_id ||
        s_mqtt.rx.total_len != (size_t)event->total_data_len ||
        s_mqtt.rx.received_len != (size_t)event->current_data_offset) {
        mqtt_reject_rx("MQTT fragments are missing or out of order", ESP_ERR_INVALID_STATE);
        return;
    }

    memcpy(s_mqtt.rx.payload + event->current_data_offset, event->data, (size_t)event->data_len);
    s_mqtt.rx.received_len += (size_t)event->data_len;
    if (s_mqtt.rx.received_len != s_mqtt.rx.total_len) return;

    s_mqtt.rx.payload[s_mqtt.rx.total_len] = '\0';
    mqtt_job_t job = {
        .type = MQTT_JOB_RX_MESSAGE,
        .payload = s_mqtt.rx.payload,
        .payload_len = s_mqtt.rx.total_len,
        .qos = 1,
    };
    strlcpy(job.topic, s_mqtt.rx.topic, sizeof(job.topic));
    s_mqtt.rx.payload = NULL;
    memset(&s_mqtt.rx, 0, sizeof(s_mqtt.rx));

    if (xQueueSend(s_mqtt.queue, &job, 0) != pdTRUE) {
        free(job.payload);
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.queue_dropped_count++;
            mqtt_set_status_text_locked("MQTT worker queue full", ESP_ERR_TIMEOUT);
            xSemaphoreGive(s_mqtt.lock);
        }
        return;
    }
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_mqtt.status.received_count++;
        xSemaphoreGive(s_mqtt.lock);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.connected = true;
            s_mqtt.consecutive_disconnects = 0;
            s_mqtt.reconnect_pending = false;
            mqtt_set_status_text_locked("connected", ESP_OK);
            xSemaphoreGive(s_mqtt.lock);
        }
        mqtt_job_t job = {.type = MQTT_JOB_CONNECTED};
        if (xQueueSend(s_mqtt.queue, &job, 0) != pdTRUE &&
            xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.queue_dropped_count++;
            xSemaphoreGive(s_mqtt.lock);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        mqtt_reset_rx_assembly();
        static const uint32_t backoff_ms[] = {1000, 2000, 5000, 10000, 30000};
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.connected = false;
            s_mqtt.status.reconnect_count++;
            const uint32_t retry_index = s_mqtt.consecutive_disconnects++;
            const size_t index = retry_index < (sizeof(backoff_ms) / sizeof(backoff_ms[0]))
                                     ? retry_index
                                     : (sizeof(backoff_ms) / sizeof(backoff_ms[0]) - 1U);
            s_mqtt.reconnect_due_us =
                esp_timer_get_time() + (int64_t)backoff_ms[index] * 1000LL;
            s_mqtt.reconnect_pending = true;
            mqtt_set_status_text_locked("disconnected; ESP-MQTT will reconnect", ESP_ERR_INVALID_STATE);
            xSemaphoreGive(s_mqtt.lock);
        }
        break;
    case MQTT_EVENT_DATA:
        mqtt_receive_fragment(event);
        break;
    case MQTT_EVENT_ERROR:
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_mqtt.status.last_error = event->error_handle
                                           ? event->error_handle->esp_tls_last_esp_err
                                           : ESP_FAIL;
            strlcpy(s_mqtt.status.status_text, "MQTT transport error",
                    sizeof(s_mqtt.status.status_text));
            xSemaphoreGive(s_mqtt.lock);
        }
        break;
    default:
        break;
    }
}

esp_err_t fridge_mqtt_protocol_init(void)
{
    if (s_mqtt.initialized) return ESP_OK;
    memset(&s_mqtt, 0, sizeof(s_mqtt));

    s_mqtt.lock = xSemaphoreCreateMutex();
    s_mqtt.sync_lock = xSemaphoreCreateMutex();
    s_mqtt.queue = xQueueCreate(MQTT_WORKER_QUEUE_DEPTH, sizeof(mqtt_job_t));
    if (!s_mqtt.lock || !s_mqtt.sync_lock || !s_mqtt.queue) {
        if (s_mqtt.queue) vQueueDelete(s_mqtt.queue);
        if (s_mqtt.lock) vSemaphoreDelete(s_mqtt.lock);
        if (s_mqtt.sync_lock) vSemaphoreDelete(s_mqtt.sync_lock);
        memset(&s_mqtt, 0, sizeof(s_mqtt));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t load_err = mqtt_load_config(&s_mqtt.config);
    if (load_err == ESP_OK) load_err = mqtt_load_dedupe();
    if (load_err != ESP_OK) {
        vQueueDelete(s_mqtt.queue);
        vSemaphoreDelete(s_mqtt.lock);
        vSemaphoreDelete(s_mqtt.sync_lock);
        memset(&s_mqtt, 0, sizeof(s_mqtt));
        return load_err;
    }
    mqtt_load_agent_session_revision();
    s_mqtt.status.initialized = true;
    s_mqtt.status.enabled = s_mqtt.config.enabled;
    s_mqtt.status.configured =
        s_mqtt.config.broker_uri[0] && s_mqtt.config.home_id[0] && s_mqtt.config.device_id[0];
    s_mqtt.status.has_password = s_mqtt.config.password[0] != '\0';
    s_mqtt.status.binding_epoch = s_mqtt.config.binding_epoch;
    s_mqtt.status.protocol_version = FRIDGE_MQTT_PROTOCOL_VERSION;
    strlcpy(s_mqtt.status.broker_uri, s_mqtt.config.broker_uri, sizeof(s_mqtt.status.broker_uri));
    strlcpy(s_mqtt.status.home_id, s_mqtt.config.home_id, sizeof(s_mqtt.status.home_id));
    strlcpy(s_mqtt.status.device_id, s_mqtt.config.device_id, sizeof(s_mqtt.status.device_id));
    strlcpy(s_mqtt.status.username, s_mqtt.config.username, sizeof(s_mqtt.status.username));
    mqtt_set_status_text_locked(s_mqtt.status.configured ? "configured; stopped" : "not configured",
                                ESP_OK);

    BaseType_t created = xTaskCreate(mqtt_worker, "p4_mqtt_worker",
                                     MQTT_WORKER_STACK_BYTES, NULL,
                                     MQTT_WORKER_PRIORITY, &s_mqtt.worker_task);
    if (created != pdPASS) {
        vQueueDelete(s_mqtt.queue);
        vSemaphoreDelete(s_mqtt.lock);
        vSemaphoreDelete(s_mqtt.sync_lock);
        memset(&s_mqtt, 0, sizeof(s_mqtt));
        return ESP_ERR_NO_MEM;
    }
    s_mqtt.initialized = true;
#ifndef CONFIG_MQTT_OUTBOX_DATA_ON_EXTERNAL_MEMORY
    ESP_LOGW(TAG, "CONFIG_MQTT_OUTBOX_DATA_ON_EXTERNAL_MEMORY is disabled; "
                  "enable it to keep QoS1 outbox payloads in PSRAM");
#endif
    ESP_LOGI(TAG, "P4 Agent MQTT v2 initialized, payload limit=%u bytes",
             (unsigned)FRIDGE_MQTT_MAX_PAYLOAD_BYTES);
    return ESP_OK;
}

esp_err_t fridge_mqtt_get_config(fridge_mqtt_config_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "config output is NULL");
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_mqtt.config;
    xSemaphoreGive(s_mqtt.lock);
    return ESP_OK;
}

esp_err_t fridge_mqtt_set_config(const fridge_mqtt_config_t *config, bool update_password)
{
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");
    ESP_RETURN_ON_ERROR(mqtt_validate_config(config), TAG, "invalid MQTT config");

    fridge_mqtt_config_t saved = *config;
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_mqtt.client) {
        xSemaphoreGive(s_mqtt.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!update_password) {
        strlcpy(saved.password, s_mqtt.config.password, sizeof(saved.password));
    }
    xSemaphoreGive(s_mqtt.lock);
    if (saved.keepalive_seconds == 0) {
        saved.keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    }
    ESP_RETURN_ON_ERROR(mqtt_save_config(&saved, true), TAG, "save MQTT config failed");

    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_mqtt.config = saved;
    s_mqtt.status.enabled = saved.enabled;
    s_mqtt.status.configured = true;
    s_mqtt.status.has_password = saved.password[0] != '\0';
    s_mqtt.status.binding_epoch = saved.binding_epoch;
    strlcpy(s_mqtt.status.broker_uri, saved.broker_uri, sizeof(s_mqtt.status.broker_uri));
    strlcpy(s_mqtt.status.home_id, saved.home_id, sizeof(s_mqtt.status.home_id));
    strlcpy(s_mqtt.status.device_id, saved.device_id, sizeof(s_mqtt.status.device_id));
    strlcpy(s_mqtt.status.username, saved.username, sizeof(s_mqtt.status.username));
    mqtt_set_status_text_locked("configured; stopped", ESP_OK);
    xSemaphoreGive(s_mqtt.lock);
    return ESP_OK;
}

esp_err_t fridge_mqtt_clear_secret(void)
{
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    const bool running = s_mqtt.client != NULL;
    xSemaphoreGive(s_mqtt.lock);
    ESP_RETURN_ON_FALSE(!running, ESP_ERR_INVALID_STATE, TAG, "stop MQTT before clearing secret");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open MQTT NVS failed");
    esp_err_t err = nvs_erase_key(handle, MQTT_NVS_KEY_PASS);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) return err;

    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    memset(s_mqtt.config.password, 0, sizeof(s_mqtt.config.password));
    s_mqtt.status.has_password = false;
    xSemaphoreGive(s_mqtt.lock);
    return ESP_OK;
}

esp_err_t fridge_mqtt_get_status(fridge_mqtt_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "status output is NULL");
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = s_mqtt.status;
    xSemaphoreGive(s_mqtt.lock);
    return ESP_OK;
}

esp_err_t fridge_mqtt_start(void)
{
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");

    fridge_network_status_t network = {0};
    ESP_RETURN_ON_ERROR(fridge_network_get_status(&network), TAG, "read network status failed");
    ESP_RETURN_ON_FALSE(network.connected && network.hosted_ready, ESP_ERR_INVALID_STATE,
                        TAG, "P4/C6 network is not ready");

    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_mqtt.client) {
        xSemaphoreGive(s_mqtt.lock);
        return ESP_OK;
    }
    esp_err_t validation = mqtt_validate_config(&s_mqtt.config);
    if (!s_mqtt.config.enabled || validation != ESP_OK || !mqtt_build_topics_locked()) {
        xSemaphoreGive(s_mqtt.lock);
        // 未绑定/未配置属于当前状态尚不可启动，不是调用参数错误。
        // 保持与公共接口约定一致，让启动编排可以安静等待二维码绑定完成。
        return ESP_ERR_INVALID_STATE;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (s_mqtt.config.use_tls) {
        mqtt_set_status_text_locked("TLS requires ESP certificate bundle", ESP_ERR_NOT_SUPPORTED);
        xSemaphoreGive(s_mqtt.lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    s_mqtt.reconnect_pending = false;
    s_mqtt.consecutive_disconnects = 0;

    cJSON *lwt = mqtt_create_envelope_for_config(&s_mqtt.config, "device.state", NULL);
    cJSON *payload = lwt ? cJSON_AddObjectToObject(lwt, "payload") : NULL;
    if (!payload) {
        cJSON_Delete(lwt);
        xSemaphoreGive(s_mqtt.lock);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(payload, "online", false);
    cJSON_AddStringToObject(payload, "reason", "connection_lost");
    char *lwt_text = cJSON_PrintUnformatted(lwt);
    cJSON_Delete(lwt);
    if (!lwt_text || strlen(lwt_text) >= sizeof(s_mqtt.lwt_payload)) {
        if (lwt_text) cJSON_free(lwt_text);
        xSemaphoreGive(s_mqtt.lock);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_mqtt.lwt_payload, lwt_text, sizeof(s_mqtt.lwt_payload));
    cJSON_free(lwt_text);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt.config.broker_uri,
        .credentials.client_id = s_mqtt.config.device_id,
        .credentials.username = s_mqtt.config.username[0] ? s_mqtt.config.username : NULL,
        .credentials.authentication.password =
            s_mqtt.config.password[0] ? s_mqtt.config.password : NULL,
        .session.keepalive = s_mqtt.config.keepalive_seconds,
        .session.disable_clean_session = true,
        .session.last_will.topic = s_mqtt.topic_up_state,
        .session.last_will.msg = s_mqtt.lwt_payload,
        .session.last_will.msg_len = (int)strlen(s_mqtt.lwt_payload),
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
        .network.disable_auto_reconnect = true,
        .task.priority = 5,
        .task.stack_size = 6144,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
        .outbox.limit = MQTT_OUTBOX_LIMIT_BYTES,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (s_mqtt.config.use_tls) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif
    s_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt.client) {
        mqtt_set_status_text_locked("esp_mqtt_client_init failed", ESP_ERR_NO_MEM);
        xSemaphoreGive(s_mqtt.lock);
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_handle_t client = s_mqtt.client;
    xSemaphoreGive(s_mqtt.lock);

    esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_mqtt.client = NULL;
            s_mqtt.status.connected = false;
            mqtt_set_status_text_locked("MQTT start failed", err);
            xSemaphoreGive(s_mqtt.lock);
        }
        esp_mqtt_client_destroy(client);
        return err;
    }
    mqtt_set_status_text("connecting", ESP_OK);
    return ESP_OK;
}

esp_err_t fridge_mqtt_stop(void)
{
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");
    esp_mqtt_client_handle_t client = NULL;
    if (xSemaphoreTake(s_mqtt.lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    client = s_mqtt.client;
    s_mqtt.client = NULL;
    s_mqtt.status.connected = false;
    s_mqtt.reconnect_pending = false;
    mqtt_set_status_text_locked("stopped", ESP_OK);
    xSemaphoreGive(s_mqtt.lock);
    if (!client) return ESP_OK;

    esp_err_t stop_err = esp_mqtt_client_stop(client);
    esp_err_t destroy_err = esp_mqtt_client_destroy(client);
    mqtt_reset_rx_assembly();
    return stop_err != ESP_OK ? stop_err : destroy_err;
}

esp_err_t fridge_mqtt_publish_state(bool online)
{
    cJSON *root = mqtt_create_envelope("device.state", NULL);
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    if (!payload) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    fridge_network_status_t network = {0};
    fridge_agent_storage_status_t storage = {0};
    (void)fridge_network_get_status(&network);
    fridge_agent_storage_get_status(&storage);
    cJSON_AddBoolToObject(payload, "online", online);
    cJSON_AddStringToObject(payload, "firmwareTarget", "esp32p4");
    cJSON_AddNumberToObject(payload, "protocolVersion", FRIDGE_MQTT_PROTOCOL_VERSION);
    cJSON_AddBoolToObject(payload, "hostedReady", network.hosted_ready);
    cJSON_AddBoolToObject(payload, "internetReady", network.internet_ready);
    cJSON_AddBoolToObject(payload, "agentStorageReady", storage.ready);
    cJSON_AddNumberToObject(payload, "internalFree",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(payload, "internalLargest",
                            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(payload, "psramFree",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return mqtt_queue_json(s_mqtt.topic_up_state, root, 1, true);
}

esp_err_t fridge_mqtt_publish_sensor_snapshot(void)
{
    // 兼容旧调用点：P4 不再把诊断伪装成业务域事件，统一刷新 retained state。
    return fridge_mqtt_publish_state(true);
}

static esp_err_t mqtt_publish_snapshot_available(const char *domain, bool force_import)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(data, "httpSnapshotRequired", true);
    cJSON_AddBoolToObject(data, "forceImportRequested", force_import);
    char *text = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = fridge_mqtt_publish_sync_event(domain, "snapshot",
                                                   "upsert", 0, text);
    cJSON_free(text);
    return err;
}

esp_err_t fridge_mqtt_publish_inventory_snapshot(bool force_import)
{
    return mqtt_publish_snapshot_available("inventory", force_import);
}

esp_err_t fridge_mqtt_request_inventory_publish(bool force_import)
{
    return fridge_mqtt_publish_inventory_snapshot(force_import);
}

esp_err_t fridge_mqtt_publish_chat_history_delta(void)
{
    return mqtt_publish_snapshot_available("agent_sessions", false);
}

esp_err_t fridge_mqtt_publish_sync_event(const char *domain,
                                         const char *entity_id,
                                         const char *operation,
                                         uint32_t base_entity_revision,
                                         const char *data_json)
{
    ESP_RETURN_ON_FALSE(domain && domain[0] && strlen(domain) < MQTT_DOMAIN_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sync domain");
    ESP_RETURN_ON_FALSE(mqtt_sync_domain_valid(domain),
                        ESP_ERR_INVALID_ARG, TAG, "unsupported sync domain");
    ESP_RETURN_ON_FALSE(entity_id && entity_id[0] && strlen(entity_id) <= FRIDGE_MQTT_MAX_ID_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid entity ID");
    ESP_RETURN_ON_FALSE(operation && operation[0] && strlen(operation) < MQTT_OPERATION_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sync operation");
    ESP_RETURN_ON_FALSE(strcmp(operation, "upsert") == 0 ||
                            strcmp(operation, "delete") == 0 ||
                            strcmp(operation, "append") == 0,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported sync operation");
    ESP_RETURN_ON_FALSE(data_json, ESP_ERR_INVALID_ARG, TAG, "sync data is NULL");

    cJSON *data = cJSON_Parse(data_json);
    if (!data || !(cJSON_IsObject(data) || cJSON_IsArray(data) || cJSON_IsNull(data))) {
        cJSON_Delete(data);
        ESP_LOGE(TAG, "sync data must be JSON object/array/null");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = mqtt_create_envelope("sync.event", NULL);
    cJSON *payload = root ? cJSON_AddObjectToObject(root, "payload") : NULL;
    if (!payload) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    char event_id[MQTT_MESSAGE_ID_BYTES];
    mqtt_make_message_id(event_id);
    cJSON_AddStringToObject(payload, "eventId", event_id);
    cJSON_AddStringToObject(payload, "domain", domain);
    cJSON_AddStringToObject(payload, "entityId", entity_id);
    cJSON_AddStringToObject(payload, "operation", operation);
    cJSON_AddNumberToObject(payload, "baseEntityRevision", base_entity_revision);
    cJSON_AddItemToObject(payload, "data", data);
    return mqtt_queue_json(s_mqtt.topic_up_event, root, 1, false);
}

static cJSON *mqtt_read_local_sync_entity(const char *domain,
                                          const char *entity_id,
                                          uint32_t *out_base_revision)
{
    mqtt_sync_target_t target;
    if (!mqtt_sync_target_for_domain(domain, &target)) return NULL;

    char fallback[96];
    int fallback_len = snprintf(fallback, sizeof(fallback),
                                "{\"schema_version\":1,\"%s\":[]}",
                                target.array_name);
    if (fallback_len <= 0 || (size_t)fallback_len >= sizeof(fallback)) return NULL;

    char *text = mqtt_large_alloc(MQTT_AGENT_FILE_MAX_BYTES + 1U);
    if (!text) return NULL;
    size_t text_size = 0;
    esp_err_t err = fridge_agent_storage_json_read(
        target.path, text, MQTT_AGENT_FILE_MAX_BYTES + 1U, &text_size, fallback);
    if (err != ESP_OK) {
        free(text);
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(text, text_size);
    free(text);
    if (!root) return NULL;
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, target.array_name);
    int index = cJSON_IsArray(items) ? mqtt_find_entity_index(items, entity_id) : -1;
    cJSON *stored = index >= 0 ? cJSON_GetArrayItem(items, index) : NULL;
    cJSON *copy = stored ? cJSON_Duplicate(stored, true) : NULL;
    if (copy) {
        *out_base_revision = mqtt_json_u32(copy, "entityRevision", 0);
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "entityRevision");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "serverRevision");
    }
    cJSON_Delete(root);
    return copy;
}

static void mqtt_publish_local_entity(const char *domain,
                                      const char *entity_id,
                                      const char *operation,
                                      cJSON *data,
                                      uint32_t base_revision)
{
    char *text = cJSON_PrintUnformatted(data);
    if (!text) return;
    esp_err_t err = fridge_mqtt_publish_sync_event(
        domain, entity_id, operation, base_revision, text);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Local Tool sync enqueue failed: domain=%s err=%s",
                 domain, esp_err_to_name(err));
    }
    cJSON_free(text);
}

// Tool Broker 成功回调：把当前 P4 Agent 本地写入转成增量事件。
// 回调只读已经提交的本地状态并投递 MQTT worker，不在 Tool 任务里执行网络 I/O。
void fridge_mqtt_notify_tool_success(const fridge_tool_call_t *call,
                                     const fridge_tool_result_t *result)
{
    if (!call || !result || !s_mqtt.initialized) return;
    cJSON *args = cJSON_Parse(call->arguments_json[0] ?
                                  call->arguments_json : "{}");
    cJSON *result_data = cJSON_Parse(result->data_json[0] ?
                                         result->data_json : "{}");
    if (!cJSON_IsObject(args) || !cJSON_IsObject(result_data)) {
        cJSON_Delete(args);
        cJSON_Delete(result_data);
        return;
    }
    const cJSON *duplicate =
        cJSON_GetObjectItemCaseSensitive(result_data, "duplicate");
    if (cJSON_IsTrue(duplicate)) {
        cJSON_Delete(args);
        cJSON_Delete(result_data);
        return;
    }

    const char *tool = call->tool_name;
    const char *domain = NULL;
    const char *operation = "upsert";
    const char *entity_id = NULL;
    cJSON *owned_data = NULL;
    uint32_t base_revision = 0;

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(result_data, "id");
    if (!cJSON_IsString(id_item)) {
        id_item = cJSON_GetObjectItemCaseSensitive(args, "id");
    }
    if (cJSON_IsString(id_item)) entity_id = id_item->valuestring;

    if (strncmp(tool, "inventory.", 10) == 0 &&
        strcmp(tool, "inventory.list_expiring") != 0) {
        domain = "inventory";
        if (strcmp(tool, "inventory.delete") == 0) {
            operation = "delete";
            const cJSON *deleted =
                cJSON_GetObjectItemCaseSensitive(result_data, "deleted");
            base_revision = cJSON_IsObject(deleted)
                                ? mqtt_json_u32(deleted, "entityRevision", 0)
                                : 0;
            owned_data = cJSON_CreateObject();
        } else if (entity_id) {
            owned_data = mqtt_read_local_sync_entity(
                domain, entity_id, &base_revision);
        }
    } else if (strncmp(tool, "task.", 5) == 0 &&
               strcmp(tool, "task.list") != 0 && entity_id) {
        // reminders/timers/alarms 共用 tasks.json；创建工具可直接判定 kind，
        // 更新类工具从已落盘任务读取 kind 后选择同步域。
        if (strcmp(tool, "task.create_timer") == 0 ||
            strcmp(tool, "task.trigger") == 0) {
            domain = "timers";
        } else if (strcmp(tool, "task.create_reminder") == 0) {
            domain = "reminders";
        } else {
            uint32_t ignored_revision = 0;
            cJSON *task = mqtt_read_local_sync_entity(
                "timers", entity_id, &ignored_revision);
            const cJSON *kind = task
                                    ? cJSON_GetObjectItemCaseSensitive(task, "kind")
                                    : NULL;
            domain = cJSON_IsString(kind) &&
                             strcmp(kind->valuestring, "reminder") == 0
                         ? "reminders"
                         : cJSON_IsString(kind) &&
                                   strcmp(kind->valuestring, "alarm") == 0
                               ? "alarms"
                               : "timers";
            cJSON_Delete(task);
        }
        owned_data = mqtt_read_local_sync_entity(
            domain, entity_id, &base_revision);
    } else if (strcmp(tool, "memory.add") == 0 && entity_id) {
        domain = "agent_memory";
        owned_data = mqtt_read_local_sync_entity(
            domain, entity_id, &base_revision);
    } else if (strcmp(tool, "memory_md.append") == 0) {
        domain = "agent_memory";
        entity_id = call->idempotency_key[0] ? call->idempotency_key
                                             : call->call_id;
        owned_data = cJSON_CreateObject();
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(args, "title");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
        if (owned_data) {
            cJSON_AddStringToObject(owned_data, "type", "memory_md");
            cJSON_AddStringToObject(
                owned_data, "title",
                cJSON_IsString(title) ? title->valuestring : "Agent 记忆补充");
            cJSON_AddStringToObject(
                owned_data, "text",
                cJSON_IsString(text) ? text->valuestring : "");
        }
    } else if (strncmp(tool, "stopwatch.", 10) == 0 &&
               strcmp(tool, "stopwatch.status") != 0) {
        domain = "stopwatch";
        entity_id = "primary";
        cJSON *previous = mqtt_read_local_sync_entity(
            domain, entity_id, &base_revision);
        cJSON_Delete(previous);
        owned_data = cJSON_Duplicate(result_data, true);
        if (owned_data) {
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "duplicate");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "idempotencyKey");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "alreadyRunning");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "alreadyPaused");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "resumed");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "newestLapMs");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "droppedOldest");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "clearedElapsedMs");
            cJSON_DeleteItemFromObjectCaseSensitive(owned_data, "clearedLapCount");
        }
    }

    if (domain && entity_id && entity_id[0] && owned_data) {
        mqtt_publish_local_entity(domain, entity_id, operation,
                                  owned_data, base_revision);
    }
    cJSON_Delete(owned_data);
    cJSON_Delete(args);
    cJSON_Delete(result_data);
}

void fridge_mqtt_notify_agent_session_json(const char *session_json)
{
    if (!session_json || !s_mqtt.initialized) return;
    cJSON *root = cJSON_Parse(session_json);
    cJSON *foreground = root
                            ? cJSON_GetObjectItemCaseSensitive(root, "foreground")
                            : NULL;
    if (cJSON_IsObject(foreground)) {
        cJSON_AddNumberToObject(root, "sync_entity_revision",
                                s_agent_session_entity_revision);
        char *persisted = cJSON_PrintUnformatted(root);
        if (persisted) {
            (void)fridge_agent_storage_json_write(
                "agent/data/sessions.json", persisted, strlen(persisted));
            cJSON_free(persisted);
        }
        mqtt_publish_local_entity(
            "agent_sessions", "foreground", "upsert",
            foreground, s_agent_session_entity_revision);
    }
    cJSON_Delete(root);
}

esp_err_t fridge_mqtt_apply_snapshot_json(const char *json)
{
    ESP_RETURN_ON_FALSE(s_mqtt.initialized, ESP_ERR_INVALID_STATE, TAG, "MQTT not initialized");
    ESP_RETURN_ON_FALSE(json, ESP_ERR_INVALID_ARG, TAG, "snapshot JSON is NULL");
    const size_t json_size = strlen(json);
    ESP_RETURN_ON_FALSE(json_size > 0 && json_size <= MQTT_HTTP_SNAPSHOT_MAX_BYTES,
                        ESP_ERR_INVALID_SIZE, TAG, "snapshot JSON is too large");

    cJSON *root = cJSON_ParseWithLength(json, json_size);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "snapshot root is invalid");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(snapshot) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(snapshot, "domains"))) {
        snapshot = root;
    }
    cJSON *domains = cJSON_GetObjectItemCaseSensitive(snapshot, "domains");
    if (!cJSON_IsObject(domains)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t snapshot_revision = mqtt_json_u32(snapshot, "serverRevision", 0);
    esp_err_t err = ESP_OK;

    if (xSemaphoreTake(s_mqtt.sync_lock, pdMS_TO_TICKS(60000)) != pdTRUE) {
        cJSON_Delete(root);
        return ESP_ERR_TIMEOUT;
    }
    err = fridge_agent_storage_transaction_begin(60000U);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mqtt.sync_lock);
        cJSON_Delete(root);
        return err;
    }
    err = ESP_OK;
    cJSON *domain_items = NULL;
    cJSON_ArrayForEach(domain_items, domains) {
        const char *domain = domain_items->string;
        if (!domain || !mqtt_sync_domain_valid(domain) || !cJSON_IsArray(domain_items)) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = mqtt_reset_snapshot_domain_locked(domain, snapshot_revision);
        if (err != ESP_OK) break;

        const int count = cJSON_GetArraySize(domain_items);
        for (int i = 0; i < count; ++i) {
            cJSON *event = cJSON_GetArrayItem(domain_items, i);
            if (!cJSON_IsObject(event)) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(event, "serverRevision"))) {
                cJSON_AddNumberToObject(event, "serverRevision", snapshot_revision);
            }
            mqtt_sync_apply_result_t result = {0};
            err = mqtt_apply_sync_event_locked(domain, event, &result);
            if (err != ESP_OK) break;
        }
        if (err != ESP_OK) break;
        if (strcmp(domain, "ai_config") == 0 && count == 0) {
            // 云端权威快照无 AI 配置时至少清除设备密钥，避免旧家庭密钥跨绑定残留。
            err = fridge_ai_client_clear_key();
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            if (err != ESP_OK) break;
        }
    }
    fridge_agent_storage_transaction_end();
    xSemaphoreGive(s_mqtt.sync_lock);
    cJSON_Delete(root);
    if (err == ESP_OK) mqtt_set_status_text("HTTP snapshot applied", ESP_OK);
    return err;
}

esp_err_t fridge_mqtt_publish_command_ack(const char *request_id, const char *command,
                                          bool ok, const char *message)
{
    return fridge_mqtt_publish_command_ack_ex(request_id, command, "completed", ok,
                                              ok ? "" : "COMMAND_FAILED",
                                              0, 0, ok, message);
}

esp_err_t fridge_mqtt_publish_command_ack_ex(const char *request_id,
                                             const char *command,
                                             const char *stage,
                                             bool ok,
                                             const char *error_code,
                                             uint32_t local_revision,
                                             uint32_t server_revision,
                                             bool applied,
                                             const char *message)
{
    ESP_RETURN_ON_FALSE(request_id && request_id[0] && command && command[0],
                        ESP_ERR_INVALID_ARG, TAG, "request ID/command is required");
    cJSON *root = mqtt_create_envelope("command.ack", request_id);
    if (!root) return ESP_ERR_NO_MEM;
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    if (!payload) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(payload, "command", command);
    cJSON_AddStringToObject(payload, "stage", stage ? stage : "completed");
    cJSON_AddBoolToObject(payload, "ok", ok);
    cJSON_AddStringToObject(payload, "errorCode", error_code ? error_code : "");
    cJSON_AddNumberToObject(payload, "localRevision", local_revision);
    cJSON_AddNumberToObject(payload, "serverRevision", server_revision);
    cJSON_AddBoolToObject(payload, "applied", applied);
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    return mqtt_queue_json(s_mqtt.topic_up_ack, root, 1, false);
}
