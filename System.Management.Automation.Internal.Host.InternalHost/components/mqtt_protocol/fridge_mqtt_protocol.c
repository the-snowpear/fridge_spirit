// 冰箱小精灵 MQTT 云端同步组件。
// 负责设备状态、低频传感快照、云端命令和回执；图片、音频不走 MQTT。
// 比赛同步链路中，远程 AI 文本请求通过 ai_chat 命令转发到设备直连 AI，再用 cmd_ack.message 返回短回复。
// 硬件注意：本组件只使用 Wi-Fi/TLS 网络栈，不操作 GPIO；Wi-Fi 发射峰值仍要求稳定 5V/USB 供电。

#include "fridge_mqtt_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_ai_client.h"
#include "fridge_asr.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#define MQTT_NVS_NAMESPACE "fridge_mqtt"
#define MQTT_NVS_KEY_URI "uri"
#define MQTT_NVS_KEY_HOME "home_id"
#define MQTT_NVS_KEY_DEVICE "device_id"
#define MQTT_NVS_KEY_USER "username"
#define MQTT_NVS_KEY_PASS "password"
#define MQTT_NVS_KEY_KEEPALIVE "keepalive"
#define MQTT_NVS_KEY_ENABLED "enabled"
#define MQTT_TOPIC_STATE "fridge/%s/%s/state"
#define MQTT_TOPIC_SENSOR "fridge/%s/%s/sensor"
#define MQTT_TOPIC_CMD "fridge/%s/%s/cmd"
#define MQTT_TOPIC_ACK "fridge/%s/%s/cmd_ack"
#define MQTT_TOPIC_ERROR "fridge/%s/%s/error"
#define MQTT_TOPIC_INVENTORY "fridge/%s/%s/inventory"
#define MQTT_TOPIC_SYNC "fridge/%s/%s/sync"
#define MQTT_TOPIC_AI_CONFIG "fridge/%s/%s/ai_config"
#define MQTT_SYNC_CONFIG_JSON_LEN (FRIDGE_STORAGE_MAX_JSON_LEN * 2)
#define FRIDGE_MQTT_TASK_STACK_BYTES 4096
#define FRIDGE_MQTT_AI_TASK_STACK_BYTES 20480
#define FRIDGE_MQTT_CMD_WORKER_STACK_BYTES 8192
#define FRIDGE_MQTT_CMD_QUEUE_DEPTH 4
#define FRIDGE_MQTT_AI_QUEUE_DEPTH 1
#define FRIDGE_MQTT_BUFFER_BYTES 1024
#define FRIDGE_MQTT_OUTBOX_LIMIT_BYTES 12288
#define FRIDGE_MQTT_AI_ACK_MESSAGE_BYTES 1800

static const char *TAG = "fridge_mqtt";

static esp_mqtt_client_handle_t s_client;
static bool s_initialized;
static bool s_connected;
static bool s_started;
static uint32_t s_reconnect_count;
static uint32_t s_published_count;
static uint32_t s_received_count;
static int s_last_error;
static char s_status_text[FRIDGE_MQTT_MAX_STATUS_LEN + 1] = "not initialized";
static fridge_mqtt_config_t s_config;
static char *s_command_fragment_buffer;
static int s_command_fragment_total_len;
static int s_command_fragment_received_len;
static QueueHandle_t s_command_queue;
static TaskHandle_t s_command_worker_task;
static QueueHandle_t s_ai_worker_queue;
static TaskHandle_t s_ai_worker_task;
static volatile bool s_ai_worker_busy;
static portMUX_TYPE s_inventory_publish_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inventory_publish_pending;
static bool s_inventory_publish_force;
static portMUX_TYPE s_sync_docs_publish_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_sync_docs_publish_pending;
static bool s_sync_docs_publish_force;

typedef struct {
    char request_id[96];
    char command[32];
    fridge_ai_config_update_t config;
    fridge_ai_assistant_request_t request;
} mqtt_ai_chat_job_t;

typedef struct {
    int payload_len;
    char payload[];
} mqtt_command_job_t;

typedef struct {
    void *job;
} mqtt_ai_worker_dispatch_t;

static void command_worker_task(void *arg);
static void ai_worker_task(void *arg);
static esp_err_t publish_device_sync_documents(bool force_import);

static void set_status_text(const char *text)
{
    strlcpy(s_status_text, text ? text : "", sizeof(s_status_text));
}

static size_t utf8_sequence_len(uint8_t first)
{
    if (first < 0x80) {
        return 1;
    }
    if ((first & 0xE0) == 0xC0) {
        return 2;
    }
    if ((first & 0xF0) == 0xE0) {
        return 3;
    }
    if ((first & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;
}

static bool utf8_sequence_valid(const unsigned char *p, size_t len)
{
    if (!p || len == 0) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return false;
        }
    }
    if (len == 2 && p[0] < 0xC2) {
        return false;
    }
    if (len == 3) {
        if (p[0] == 0xE0 && p[1] < 0xA0) {
            return false;
        }
        if (p[0] == 0xED && p[1] >= 0xA0) {
            return false;
        }
    }
    if (len == 4) {
        if (p[0] == 0xF0 && p[1] < 0x90) {
            return false;
        }
        if (p[0] == 0xF4 && p[1] > 0x8F) {
            return false;
        }
    }
    return true;
}

static char *mqtt_utf8_clean_alloc(const char *text)
{
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;) {
        if (*p < 0x20) {
            len++;
            p++;
            continue;
        }
        size_t seq_len = utf8_sequence_len(*p);
        if (seq_len > 0 && utf8_sequence_valid(p, seq_len)) {
            len += seq_len;
            p += seq_len;
        } else {
            len++;
            p++;
        }
    }
    char *out = calloc(1, len + 1);
    if (!out) {
        return NULL;
    }
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;) {
        if (*p < 0x20) {
            *w++ = ' ';
            p++;
            continue;
        }
        size_t seq_len = utf8_sequence_len(*p);
        if (seq_len > 0 && utf8_sequence_valid(p, seq_len)) {
            memcpy(w, p, seq_len);
            w += seq_len;
            p += seq_len;
        } else {
            *w++ = '?';
            p++;
        }
    }
    *w = '\0';
    return out;
}

static void mqtt_sanitize_json_strings(cJSON *item)
{
    if (!item) {
        return;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        char *clean = mqtt_utf8_clean_alloc(item->valuestring);
        if (clean) {
            cJSON_SetValuestring(item, clean);
            free(clean);
        }
        return;
    }
    if (cJSON_IsArray(item) || cJSON_IsObject(item)) {
        for (cJSON *child = item->child; child; child = child->next) {
            mqtt_sanitize_json_strings(child);
        }
    }
}

static char *alloc_mqtt_payload_buffer(size_t size)
{
    char *buffer = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = calloc(1, size);
    }
    return buffer;
}

static void reset_command_fragment_buffer(void)
{
    free(s_command_fragment_buffer);
    s_command_fragment_buffer = NULL;
    s_command_fragment_total_len = 0;
    s_command_fragment_received_len = 0;
}

static BaseType_t create_internal_worker_task(TaskFunction_t task,
                                              const char *name,
                                              uint32_t stack_bytes,
                                              void *arg,
                                              UBaseType_t priority,
                                              TaskHandle_t *handle)
{
    // 调用方会执行 NVS/LittleFS 等 Flash 路径时，任务栈必须留在内部 SRAM；
    // ESP-IDF 关闭 cache 做 Flash 事务时不能从 PSRAM 栈取数据。
    BaseType_t ok = xTaskCreate(task, name, stack_bytes, arg, priority, handle);
    if (ok != pdPASS) {
        ESP_LOGW(TAG,
                 "%s internal stack create failed, stack=%lu, largest_internal=%u bytes, free_psram=%u KB",
                 name,
                 (unsigned long)stack_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));
    }
    return ok;
}

static esp_err_t ensure_command_worker_started(void)
{
    if (!s_command_queue) {
        s_command_queue = xQueueCreate(FRIDGE_MQTT_CMD_QUEUE_DEPTH, sizeof(mqtt_command_job_t *));
        ESP_RETURN_ON_FALSE(s_command_queue, ESP_ERR_NO_MEM, TAG, "create MQTT command queue failed");
    }
    if (!s_command_worker_task) {
        // 云端命令可能写 NVS/LittleFS，常驻 worker 必须使用内部 SRAM 栈；
        // 初始化早期创建可避免运行一段时间后内部 SRAM 碎片化导致临时 16KB 栈创建失败。
        BaseType_t ok = create_internal_worker_task(command_worker_task,
                                                    "mqtt_cmd",
                                                    FRIDGE_MQTT_CMD_WORKER_STACK_BYTES,
                                                    NULL,
                                                    5,
                                                    &s_command_worker_task);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "start MQTT command worker failed");
    }
    return ESP_OK;
}

static esp_err_t ensure_ai_worker_started(void)
{
    if (!s_ai_worker_queue) {
        s_ai_worker_queue = xQueueCreate(FRIDGE_MQTT_AI_QUEUE_DEPTH, sizeof(mqtt_ai_worker_dispatch_t));
        ESP_RETURN_ON_FALSE(s_ai_worker_queue, ESP_ERR_NO_MEM, TAG, "create MQTT AI queue failed");
    }
    if (!s_ai_worker_task) {
        // AI HTTPS/TLS 调用栈较深；调用前已预加载 NVS 配置并完成网络校时，
        // 因此这里允许使用 PSRAM 栈，避免和 Wi-Fi/MQTT/LVGL 争抢内部 SRAM。
        BaseType_t ok = xTaskCreateWithCaps(ai_worker_task,
                                            "mqtt_ai_worker",
                                            FRIDGE_MQTT_AI_TASK_STACK_BYTES,
                                            NULL,
                                            4,
                                            &s_ai_worker_task,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            s_ai_worker_task = NULL;
            ESP_LOGW(TAG,
                     "mqtt_ai_worker PSRAM stack create failed, stack=%lu, largest_internal=%u bytes, free_psram=%u KB",
                     (unsigned long)FRIDGE_MQTT_AI_TASK_STACK_BYTES,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static bool config_is_complete(const fridge_mqtt_config_t *config)
{
    return config && config->enabled && config->broker_uri[0] && config->home_id[0] && config->device_id[0] && config->username[0] &&
           config->password[0];
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t load_config(fridge_mqtt_config_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    memset(out, 0, sizeof(*out));
    out->keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open MQTT NVS failed");

    bool enabled = false;
    uint16_t keepalive = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    read_nvs_string(handle, MQTT_NVS_KEY_URI, out->broker_uri, sizeof(out->broker_uri));
    read_nvs_string(handle, MQTT_NVS_KEY_HOME, out->home_id, sizeof(out->home_id));
    read_nvs_string(handle, MQTT_NVS_KEY_DEVICE, out->device_id, sizeof(out->device_id));
    read_nvs_string(handle, MQTT_NVS_KEY_USER, out->username, sizeof(out->username));
    read_nvs_string(handle, MQTT_NVS_KEY_PASS, out->password, sizeof(out->password));
    if (nvs_get_u16(handle, MQTT_NVS_KEY_KEEPALIVE, &keepalive) == ESP_OK && keepalive > 0) {
        out->keepalive_seconds = keepalive;
    }
    if (nvs_get_u8(handle, MQTT_NVS_KEY_ENABLED, (uint8_t *)&enabled) != ESP_OK) {
        enabled = false;
    }
    out->enabled = enabled;
    out->use_tls = strncmp(out->broker_uri, "mqtts://", 8) == 0 || strncmp(out->broker_uri, "wss://", 6) == 0;
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t save_config(const fridge_mqtt_config_t *config, bool update_password)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open MQTT NVS failed");
    esp_err_t err = nvs_set_str(handle, MQTT_NVS_KEY_URI, config->broker_uri);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_HOME, config->home_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_DEVICE, config->device_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_USER, config->username);
    }
    if (err == ESP_OK && update_password) {
        err = nvs_set_str(handle, MQTT_NVS_KEY_PASS, config->password);
    }
    if (err == ESP_OK) {
        uint16_t keepalive = config->keepalive_seconds ? config->keepalive_seconds : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
        err = nvs_set_u16(handle, MQTT_NVS_KEY_KEEPALIVE, keepalive);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, MQTT_NVS_KEY_ENABLED, config->enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t make_topic(char *out, size_t out_size, const char *pattern)
{
    ESP_RETURN_ON_FALSE(out && pattern, ESP_ERR_INVALID_ARG, TAG, "invalid topic args");
    int written = snprintf(out, out_size, pattern, s_config.home_id, s_config.device_id);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < out_size, ESP_ERR_INVALID_SIZE, TAG, "topic too long");
    return ESP_OK;
}

static void add_common_fields(cJSON *root, const char *kind)
{
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "device_id", s_config.device_id);
    cJSON_AddStringToObject(root, "home_id", s_config.home_id);
    cJSON_AddStringToObject(root, "kind", kind ? kind : "event");
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)(esp_timer_get_time() / 1000));
}

static esp_err_t publish_json(const char *topic_pattern, cJSON *root, int qos, bool retain)
{
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "json root is NULL");
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    ESP_RETURN_ON_ERROR(make_topic(topic, sizeof(topic), topic_pattern), TAG, "make topic failed");
    char *payload = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_NO_MEM, TAG, "print MQTT JSON failed");

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain ? 1 : 0);
    free(payload);
    if (msg_id < 0) {
        set_status_text("publish failed");
        return ESP_FAIL;
    }
    s_published_count++;
    return ESP_OK;
}

static const cJSON *json_object_payload_or_root(const cJSON *root)
{
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    return cJSON_IsObject(payload) ? payload : root;
}

static const char *json_pick_string(const cJSON *root, const char *camel_key, const char *snake_key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, camel_key);
    if (!cJSON_IsString(item) && snake_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, snake_key);
    }
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static int64_t json_pick_i64(const cJSON *root, const char *camel_key, const char *snake_key, int64_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, camel_key);
    if (!cJSON_IsNumber(item) && snake_key) {
        item = cJSON_GetObjectItemCaseSensitive(root, snake_key);
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return fallback;
    }
    return (int64_t)item->valuedouble;
}

static uint32_t json_pick_u32(const cJSON *root, const char *camel_key, const char *snake_key, uint32_t fallback)
{
    int64_t value = json_pick_i64(root, camel_key, snake_key, fallback);
    if (value > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)value;
}

static void copy_json_string(char *out, size_t out_size, const cJSON *root, const char *camel_key, const char *snake_key)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *value = json_pick_string(root, camel_key, snake_key);
    if (value) {
        strlcpy(out, value, out_size);
    }
}

static void copy_json_string_any(char *out, size_t out_size, const cJSON *root, const char *key_a, const char *key_b, const char *key_c)
{
    if (!out || out_size == 0 || !root) {
        return;
    }
    const char *value = NULL;
    if (key_a) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key_a);
        if (cJSON_IsString(item) && item->valuestring) {
            value = item->valuestring;
        }
    }
    if (!value && key_b) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key_b);
        if (cJSON_IsString(item) && item->valuestring) {
            value = item->valuestring;
        }
    }
    if (!value && key_c) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key_c);
        if (cJSON_IsString(item) && item->valuestring) {
            value = item->valuestring;
        }
    }
    if (value) {
        strlcpy(out, value, out_size);
    }
}

static void truncate_ack_message(char *text, size_t text_size)
{
    if (!text || text_size == 0) {
        return;
    }
    size_t len = strlen(text);
    if (len < FRIDGE_MQTT_AI_ACK_MESSAGE_BYTES || FRIDGE_MQTT_AI_ACK_MESSAGE_BYTES + 16 >= text_size) {
        return;
    }
    size_t cut = FRIDGE_MQTT_AI_ACK_MESSAGE_BYTES;
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) {
        cut--;
    }
    text[cut] = '\0';
    strlcat(text, "\n[设备回复已截断]", text_size);
}

static void ai_chat_worker_task(void *arg)
{
    mqtt_ai_chat_job_t *job = (mqtt_ai_chat_job_t *)arg;
    if (!job) {
        return;
    }

    fridge_ai_assistant_result_t *result = heap_caps_calloc(1, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) {
        result = calloc(1, sizeof(*result));
    }
    if (!result) {
        fridge_mqtt_publish_command_ack_ex(job->request_id,
                                           job->command,
                                           "completed",
                                           false,
                                           "ai_chat_oom",
                                           0,
                                           0,
                                           false,
                                           "allocate AI result failed");
        free(job);
        return;
    }

    job->request.preloaded_config = &job->config;
    esp_err_t err = fridge_ai_client_assistant_chat_with_config(&job->config, &job->request, result);
    bool ok = err == ESP_OK && result->chat.reply[0] != '\0';
    char message[FRIDGE_AI_MAX_REPLY_LEN + 64] = {0};
    if (ok) {
        strlcpy(message, result->chat.reply, sizeof(message));
        truncate_ack_message(message, sizeof(message));
    } else {
        strlcpy(message, result->chat.error[0] ? result->chat.error : esp_err_to_name(err), sizeof(message));
    }

    ESP_LOGI(TAG,
             "MQTT ai_chat worker done ok=%s http=%d latency=%lu ms",
             ok ? "true" : "false",
             result->chat.http_status,
             (unsigned long)result->chat.latency_ms);
    fridge_mqtt_publish_command_ack_ex(job->request_id,
                                       job->command,
                                       "completed",
                                       ok,
                                       ok ? NULL : "ai_chat_failed",
                                       0,
                                       0,
                                       ok,
                                       message);
    free(result);
    free(job);
}

static void ai_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        mqtt_ai_worker_dispatch_t dispatch = {0};
        if (xQueueReceive(s_ai_worker_queue, &dispatch, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ai_chat_worker_task(dispatch.job);
        s_ai_worker_busy = false;
        ESP_LOGI(TAG,
                 "MQTT AI worker high watermark=%u words",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static esp_err_t start_ai_chat_worker(const char *request_id, const char *command, const cJSON *payload)
{
    const char *prompt = json_pick_string(payload, "prompt", "message");
    ESP_RETURN_ON_FALSE(prompt && prompt[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ai_chat missing prompt");
    ESP_RETURN_ON_FALSE(!s_ai_worker_busy &&
                            (!s_ai_worker_queue || uxQueueMessagesWaiting(s_ai_worker_queue) == 0),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "MQTT AI worker busy");

    mqtt_ai_chat_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) {
        job = calloc(1, sizeof(*job));
    }
    ESP_RETURN_ON_FALSE(job, ESP_ERR_NO_MEM, TAG, "allocate ai_chat job failed");

    esp_err_t err = fridge_ai_client_load_active_config(&job->config);
    if (err != ESP_OK) {
        free(job);
        ESP_RETURN_ON_ERROR(err, TAG, "load AI config for MQTT chat failed");
    }

    err = fridge_ai_client_ensure_network_ready(NULL, 0);
    if (err != ESP_OK) {
        free(job);
        ESP_RETURN_ON_ERROR(err, TAG, "network not ready for MQTT AI chat");
    }

    strlcpy(job->request_id, request_id ? request_id : "unknown", sizeof(job->request_id));
    strlcpy(job->command, command ? command : "ai_chat", sizeof(job->command));
    strlcpy(job->request.message, prompt, sizeof(job->request.message));
    copy_json_string_any(job->request.task_type,
                         sizeof(job->request.task_type),
                         payload,
                         "taskType",
                         "task_type",
                         "task");
    if (job->request.task_type[0] == '\0') {
        strlcpy(job->request.task_type, "remote_ai_chat", sizeof(job->request.task_type));
    }
    copy_json_string_any(job->request.context_json,
                         sizeof(job->request.context_json),
                         payload,
                         "context",
                         "contextJson",
                         "context_json");
    job->request.context_injected = job->request.context_json[0] != '\0';
    const cJSON *needs_confirmation = cJSON_GetObjectItemCaseSensitive(payload, "needsConfirmation");
    if (!cJSON_IsBool(needs_confirmation)) {
        needs_confirmation = cJSON_GetObjectItemCaseSensitive(payload, "needs_confirmation");
    }
    job->request.needs_confirmation = cJSON_IsBool(needs_confirmation) && cJSON_IsTrue(needs_confirmation);
    job->request.local_snapshot_version = json_pick_u32(payload, "localSnapshotVersion", "local_snapshot_version", 0);

    err = ensure_ai_worker_started();
    if (err != ESP_OK) {
        free(job);
        return err;
    }
    mqtt_ai_worker_dispatch_t dispatch = {
        .job = job,
    };
    s_ai_worker_busy = true;
    if (xQueueSend(s_ai_worker_queue, &dispatch, pdMS_TO_TICKS(1000)) != pdTRUE) {
        s_ai_worker_busy = false;
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t fridge_mqtt_publish_inventory_snapshot(bool force_import)
{
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    char *payload = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_NO_MEM, TAG, "allocate inventory payload failed");
    esp_err_t err = fridge_storage_get_inventory_sync_payload(payload, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (err == ESP_OK) {
        cJSON *root = cJSON_Parse(payload);
        if (!root) {
            free(payload);
            return ESP_ERR_INVALID_ARG;
        }
        if (force_import) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "forceImport");
            cJSON_AddBoolToObject(root, "forceImport", true);
        }
        char *published = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!published) {
            free(payload);
            return ESP_ERR_NO_MEM;
        }

        char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
        err = make_topic(topic, sizeof(topic), MQTT_TOPIC_INVENTORY);
        if (err == ESP_OK) {
            // 库存整快照可能接近 8KB，使用 QoS0 避免占满 MQTT outbox；失败时 dirty 会保留到下次重连。
            int msg_id = esp_mqtt_client_publish(s_client, topic, published, 0, 0, 0);
            if (msg_id < 0) {
                ESP_LOGW(TAG, "publish inventory snapshot failed len=%u force=%s", (unsigned)strlen(published), force_import ? "true" : "false");
                err = ESP_FAIL;
            } else {
                ESP_LOGI(TAG, "publish inventory snapshot queued msg_id=%d len=%u force=%s", msg_id, (unsigned)strlen(published), force_import ? "true" : "false");
                s_published_count++;
            }
        }
        cJSON_free(published);
    }
    free(payload);
    return err;
}

static void drain_publish_requests(void)
{
    while (true) {
        bool inv_force_import = false;
        portENTER_CRITICAL(&s_inventory_publish_lock);
        bool inv_pending = s_inventory_publish_pending;
        inv_force_import = s_inventory_publish_force;
        s_inventory_publish_pending = false;
        s_inventory_publish_force = false;
        portEXIT_CRITICAL(&s_inventory_publish_lock);

        if (inv_pending) {
            vTaskDelay(pdMS_TO_TICKS(300));

            // 库存快照序列化和 MQTT 入队可能占用较多栈/堆，放到 MQTT 命令 worker，避免阻塞 LVGL 触摸事件。
            esp_err_t err = fridge_mqtt_publish_inventory_snapshot(inv_force_import);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "background inventory publish failed: %s", esp_err_to_name(err));
            }
        }

        bool docs_force_import = false;
        portENTER_CRITICAL(&s_sync_docs_publish_lock);
        bool docs_pending = s_sync_docs_publish_pending;
        docs_force_import = s_sync_docs_publish_force;
        s_sync_docs_publish_pending = false;
        s_sync_docs_publish_force = false;
        portEXIT_CRITICAL(&s_sync_docs_publish_lock);

        if (docs_pending) {
            vTaskDelay(pdMS_TO_TICKS(300));

            // AI/购物清单/菜谱等同步文档会触发多次 cJSON 解析和拼包，放到 MQTT 命令 worker；
            // 避免 MQTT 事件回调栈承受大 JSON 峰值导致黑屏重启，也避免运行期临时申请 8KB 栈失败。
            esp_err_t err = publish_device_sync_documents(docs_force_import);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "background sync documents publish failed: %s", esp_err_to_name(err));
            }
        }

        portENTER_CRITICAL(&s_inventory_publish_lock);
        bool inv_more = s_inventory_publish_pending;
        portEXIT_CRITICAL(&s_inventory_publish_lock);

        portENTER_CRITICAL(&s_sync_docs_publish_lock);
        bool docs_more = s_sync_docs_publish_pending;
        portEXIT_CRITICAL(&s_sync_docs_publish_lock);
        if (!inv_more && !docs_more) {
            break;
        }
    }
}

static esp_err_t notify_command_worker(void)
{
    ESP_RETURN_ON_ERROR(ensure_command_worker_started(), TAG, "MQTT command worker not ready");
    ESP_RETURN_ON_FALSE(xTaskNotifyGive(s_command_worker_task) == pdPASS, ESP_FAIL, TAG, "notify MQTT command worker failed");
    return ESP_OK;
}

esp_err_t fridge_mqtt_request_inventory_publish(bool force_import)
{
    portENTER_CRITICAL(&s_inventory_publish_lock);
    s_inventory_publish_pending = true;
    s_inventory_publish_force = s_inventory_publish_force || force_import;
    portEXIT_CRITICAL(&s_inventory_publish_lock);

    return notify_command_worker();
}

static esp_err_t request_sync_documents_publish(bool force_import)
{
    portENTER_CRITICAL(&s_sync_docs_publish_lock);
    s_sync_docs_publish_pending = true;
    s_sync_docs_publish_force = s_sync_docs_publish_force || force_import;
    portEXIT_CRITICAL(&s_sync_docs_publish_lock);

    return notify_command_worker();
}

static esp_err_t apply_inventory_replace_payload(const cJSON *payload, uint32_t *local_revision, uint32_t *server_revision, bool *applied)
{
    const cJSON *inventory = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(payload, "inventory"))
                                 ? cJSON_GetObjectItemCaseSensitive(payload, "inventory")
                                 : payload;
    uint32_t revision = json_pick_u32(payload, "serverRevision", "server_revision", 0);
    if (revision == 0 && inventory != payload) {
        revision = json_pick_u32(inventory, "serverRevision", "server_revision", 0);
    }
    char *printed = cJSON_PrintUnformatted(inventory);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print inventory replace payload failed");
    esp_err_t err = fridge_storage_apply_inventory_cloud_snapshot(printed,
                                                                  revision,
                                                                  s_config.home_id,
                                                                  s_config.device_id,
                                                                  applied);
    cJSON_free(printed);
    fridge_storage_inventory_sync_status_t status = {0};
    if (fridge_storage_get_inventory_sync_status(&status) == ESP_OK) {
        if (local_revision) {
            *local_revision = status.snapshot_version;
        }
        if (server_revision) {
            *server_revision = status.server_revision;
        }
    }
    return err;
}

static esp_err_t apply_fridge_zones_payload(const cJSON *payload, uint32_t *local_revision, uint32_t *server_revision, bool *applied)
{
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(payload, "zones");
    ESP_RETURN_ON_FALSE(cJSON_IsArray(zones), ESP_ERR_INVALID_ARG, TAG, "fridge_zones_update missing zones");

    char *current = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    ESP_RETURN_ON_FALSE(current, ESP_ERR_NO_MEM, TAG, "allocate current inventory failed");
    esp_err_t err = fridge_storage_get_inventory_sync_payload(current, FRIDGE_STORAGE_MAX_JSON_LEN);
    cJSON *root = err == ESP_OK ? cJSON_Parse(current) : NULL;
    free(current);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_ARG, TAG, "parse current inventory failed");

    cJSON_DeleteItemFromObjectCaseSensitive(root, "zones");
    cJSON_AddItemToObject(root, "zones", cJSON_Duplicate(zones, true));
    uint32_t revision = json_pick_u32(payload, "serverRevision", "server_revision", 0);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print zones inventory failed");
    err = fridge_storage_apply_inventory_cloud_snapshot(printed,
                                                        revision,
                                                        s_config.home_id,
                                                        s_config.device_id,
                                                        applied);
    cJSON_free(printed);
    fridge_storage_inventory_sync_status_t status = {0};
    if (fridge_storage_get_inventory_sync_status(&status) == ESP_OK) {
        if (local_revision) {
            *local_revision = status.snapshot_version;
        }
        if (server_revision) {
            *server_revision = status.server_revision;
        }
    }
    return err;
}

static esp_err_t apply_ai_config_payload(const cJSON *payload)
{
    fridge_ai_config_update_t config = {0};
    fridge_ai_config_view_t current = {0};
    (void)fridge_ai_client_get_config(&current);
    strlcpy(config.api_base_url, current.api_base_url, sizeof(config.api_base_url));
    strlcpy(config.model, current.model, sizeof(config.model));
    strlcpy(config.system_prompt, current.system_prompt, sizeof(config.system_prompt));
    strlcpy(config.profile_name, current.profile_name, sizeof(config.profile_name));
    config.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    copy_json_string(config.api_base_url, sizeof(config.api_base_url), payload, "apiBaseUrl", "api_base_url");
    copy_json_string(config.model, sizeof(config.model), payload, "chatModel", "model");
    copy_json_string(config.system_prompt, sizeof(config.system_prompt), payload, "systemPrompt", "system_prompt");
    copy_json_string(config.profile_name, sizeof(config.profile_name), payload, "profileName", "profile_name");
    config.timeout_ms = json_pick_u32(payload, "timeoutMs", "timeout_ms", config.timeout_ms);
    config.config_updated_at_ms = json_pick_i64(payload, "configUpdatedAt", "config_updated_at", 0);
    const char *api_key = json_pick_string(payload, "apiKey", "api_key");
    if (api_key) {
        strlcpy(config.api_key, api_key, sizeof(config.api_key));
        config.update_api_key = true;
    }
    return fridge_ai_client_set_config(&config);
}

static esp_err_t apply_asr_config_payload(const cJSON *payload)
{
    fridge_asr_config_update_t config = {0};
    fridge_asr_config_view_t current = {0};
    (void)fridge_asr_get_config(&current);
    strlcpy(config.api_base_url, current.api_base_url, sizeof(config.api_base_url));
    strlcpy(config.model, current.model, sizeof(config.model));
    config.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_ASR_DEFAULT_TIMEOUT_MS;
    copy_json_string(config.api_base_url, sizeof(config.api_base_url), payload, "asrApiBaseUrl", "apiBaseUrl");
    copy_json_string(config.model, sizeof(config.model), payload, "asrModel", "model");
    config.timeout_ms = json_pick_u32(payload, "asrTimeoutMs", "timeoutMs", config.timeout_ms);
    config.config_updated_at_ms = json_pick_i64(payload, "configUpdatedAt", "config_updated_at", 0);
    const char *api_key = json_pick_string(payload, "asrApiKey", "apiKey");
    if (api_key) {
        strlcpy(config.api_key, api_key, sizeof(config.api_key));
        config.update_api_key = true;
    }
    return fridge_asr_set_config(&config);
}

static esp_err_t apply_tts_config_payload(const cJSON *payload)
{
    fridge_tts_config_update_t config = {0};
    fridge_tts_config_view_t current = {0};
    (void)fridge_tts_get_config(&current);
    strlcpy(config.api_base_url, current.api_base_url, sizeof(config.api_base_url));
    strlcpy(config.model, current.model, sizeof(config.model));
    strlcpy(config.voice, current.voice, sizeof(config.voice));
    config.timeout_ms = current.timeout_ms ? current.timeout_ms : FRIDGE_TTS_DEFAULT_TIMEOUT_MS;
    copy_json_string(config.api_base_url, sizeof(config.api_base_url), payload, "ttsApiBaseUrl", "apiBaseUrl");
    copy_json_string(config.model, sizeof(config.model), payload, "ttsModel", "model");
    copy_json_string(config.voice, sizeof(config.voice), payload, "ttsVoice", "voice");
    config.timeout_ms = json_pick_u32(payload, "ttsTimeoutMs", "timeoutMs", config.timeout_ms);
    config.config_updated_at_ms = json_pick_i64(payload, "configUpdatedAt", "config_updated_at", 0);
    const char *api_key = json_pick_string(payload, "ttsApiKey", "apiKey");
    if (api_key) {
        strlcpy(config.api_key, api_key, sizeof(config.api_key));
        config.update_api_key = true;
    }
    return fridge_tts_set_config(&config);
}

static esp_err_t apply_storage_document_payload(const cJSON *payload, esp_err_t (*setter)(const char *json_text))
{
    ESP_RETURN_ON_FALSE(payload && setter, ESP_ERR_INVALID_ARG, TAG, "invalid storage document args");
    char *printed = cJSON_PrintUnformatted(payload);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print storage document payload failed");
    esp_err_t err = setter(printed);
    cJSON_free(printed);
    return err;
}

static bool add_storage_doc_event_ex(cJSON *events,
                                     const char *domain,
                                     const char *op,
                                     const char *json_text,
                                     const char *client_suffix,
                                     bool force_import)
{
    cJSON *payload = json_text ? cJSON_Parse(json_text) : NULL;
    if (!payload || !cJSON_IsObject(payload)) {
        cJSON_Delete(payload);
        return false;
    }
    if (force_import) {
        cJSON_DeleteItemFromObjectCaseSensitive(payload, "forceImport");
        cJSON_AddBoolToObject(payload, "forceImport", true);
    }
    cJSON *event = cJSON_CreateObject();
    if (!event) {
        cJSON_Delete(payload);
        return false;
    }
    char event_id[160];
    snprintf(event_id,
             sizeof(event_id),
             "device:%s:%s:%" PRId64,
             s_config.device_id,
             client_suffix,
             esp_timer_get_time() / 1000);
    cJSON_AddStringToObject(event, "clientEventId", event_id);
    cJSON_AddStringToObject(event, "domain", domain);
    cJSON_AddStringToObject(event, "op", op);
    cJSON_AddStringToObject(event, "source", "device");
    cJSON_AddStringToObject(event, "deviceSn", s_config.device_id);
    cJSON_AddItemToObject(event, "payload", payload);
    cJSON_AddItemToArray(events, event);
    return true;
}

static bool add_storage_doc_event(cJSON *events,
                                  const char *domain,
                                  const char *op,
                                  const char *json_text,
                                  const char *client_suffix)
{
    return add_storage_doc_event_ex(events, domain, op, json_text, client_suffix, false);
}

static bool merge_json_object_text(cJSON *target, const char *json_text)
{
    if (!target || !json_text || json_text[0] == '\0') {
        return false;
    }
    cJSON *source = cJSON_Parse(json_text);
    if (!source || !cJSON_IsObject(source)) {
        cJSON_Delete(source);
        return false;
    }
    bool ok = true;
    for (cJSON *item = source->child; item; item = item->next) {
        if (!item->string) {
            continue;
        }
        if (strcmp(item->string, "configUpdatedAt") == 0) {
            cJSON *current = cJSON_GetObjectItemCaseSensitive(target, item->string);
            if (cJSON_IsNumber(current) && cJSON_IsNumber(item)) {
                if (item->valuedouble > current->valuedouble) {
                    cJSON_SetNumberValue(current, item->valuedouble);
                }
                continue;
            }
        }
        cJSON *copy = cJSON_Duplicate(item, true);
        if (!copy) {
            ok = false;
            break;
        }
        if (cJSON_GetObjectItemCaseSensitive(target, item->string)) {
            cJSON_ReplaceItemInObjectCaseSensitive(target, item->string, copy);
        } else {
            cJSON_AddItemToObject(target, item->string, copy);
        }
    }
    cJSON_Delete(source);
    return ok;
}

static bool add_ai_config_sync_event(cJSON *events, bool force_import)
{
    char *ai_doc = calloc(1, MQTT_SYNC_CONFIG_JSON_LEN);
    char *aux_doc = calloc(1, 1024);
    if (!ai_doc || !aux_doc) {
        free(ai_doc);
        free(aux_doc);
        return false;
    }
    if (fridge_ai_client_get_sync_payload(ai_doc, MQTT_SYNC_CONFIG_JSON_LEN) != ESP_OK) {
        free(ai_doc);
        free(aux_doc);
        return false;
    }

    cJSON *payload = cJSON_Parse(ai_doc);
    if (!payload || !cJSON_IsObject(payload)) {
        cJSON_Delete(payload);
        free(ai_doc);
        free(aux_doc);
        return false;
    }
    if (fridge_asr_get_sync_payload(aux_doc, 1024) == ESP_OK) {
        (void)merge_json_object_text(payload, aux_doc);
    }
    memset(aux_doc, 0, 1024);
    if (fridge_tts_get_sync_payload(aux_doc, 1024) == ESP_OK) {
        (void)merge_json_object_text(payload, aux_doc);
    }

    char *printed = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    free(ai_doc);
    free(aux_doc);
    if (!printed) {
        return false;
    }
    bool ok = add_storage_doc_event_ex(events, "ai_config", "replace", printed, "ai_config", force_import);
    cJSON_free(printed);
    return ok;
}

static esp_err_t publish_device_sync_documents(bool force_import)
{
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    esp_err_t ret = ESP_OK;
    char *doc = NULL;
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create device sync root failed");
    add_common_fields(root, "sync");
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    ESP_GOTO_ON_FALSE(events, ESP_ERR_NO_MEM, cleanup, TAG, "create sync events failed");

    doc = calloc(1, FRIDGE_STORAGE_MAX_JSON_LEN);
    ESP_GOTO_ON_FALSE(doc, ESP_ERR_NO_MEM, cleanup, TAG, "allocate sync doc failed");

    if (fridge_storage_get_shopping_list(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event_ex(events, "shopping_list", "replace", doc, "shopping", force_import);
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_recipe_cache(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event_ex(events, "recipe_cache", "replace", doc, "recipe", force_import);
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_nutrition_cache(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event_ex(events, "nutrition_cache", "replace", doc, "nutrition", force_import);
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_reminder_queue(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event_ex(events, "reminder", "replace", doc, "reminder", force_import);
    }
    memset(doc, 0, FRIDGE_STORAGE_MAX_JSON_LEN);
    if (fridge_storage_get_user_preferences(doc, FRIDGE_STORAGE_MAX_JSON_LEN) == ESP_OK) {
        (void)add_storage_doc_event_ex(events, "settings", "replace", doc, "settings", force_import);
    }
    (void)add_ai_config_sync_event(events, force_import);
    free(doc);
    doc = NULL;

    // 出口兜底：任何本地文档里的半个 UTF-8 字符都会导致后端严格 decode 丢弃整包。
    mqtt_sanitize_json_strings(root);
    char *payload = cJSON_PrintUnformatted(root);
    ESP_GOTO_ON_FALSE(payload, ESP_ERR_NO_MEM, cleanup, TAG, "print device sync docs failed");
    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    esp_err_t err = make_topic(topic, sizeof(topic), MQTT_TOPIC_SYNC);
    if (err == ESP_OK) {
        // 同步文档可能包含 AI/ASR/TTS Key 和本地缓存，首版比赛链路优先在线立即发送。
        // 使用 QoS0 可以避免较大的配置文档占满 MQTT outbox；失败时下次重连还会再次上报。
        int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
        err = msg_id >= 0 ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) {
            s_published_count++;
            ESP_LOGI(TAG,
                     "publish sync documents queued msg_id=%d len=%u force=%s",
                     msg_id,
                     (unsigned)strlen(payload),
                     force_import ? "true" : "false");
        }
    }
    cJSON_free(payload);
    cJSON_Delete(root);
    return err;

cleanup:
    free(doc);
    cJSON_Delete(root);
    return ret == ESP_OK ? ESP_FAIL : ret;
}

esp_err_t fridge_mqtt_publish_chat_history_delta(void)
{
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    // 读取上行游标，取出尚未上报的会话增量；无增量直接返回，避免空 publish。
    int64_t synced_at = 0;
    (void)fridge_storage_get_chat_synced_at(&synced_at);
    fridge_storage_chat_history_t *delta = calloc(1, sizeof(*delta));
    ESP_RETURN_ON_FALSE(delta, ESP_ERR_NO_MEM, TAG, "allocate chat delta failed");
    esp_err_t err = fridge_storage_get_chat_history_since(synced_at, delta);
    if (err != ESP_OK) {
        free(delta);
        ESP_RETURN_ON_ERROR(err, TAG, "get chat history delta failed");
    }
    if (delta->count == 0) {
        free(delta);
        return ESP_OK;
    }

    // 构造 ai_history payload：固定 sessionId=miniapp-default，messages 只放后端要读的 role/content。
    // 后端 sync_service.ai_history 分支只追加 user/assistant 且非空的消息，op=append 不触发整窗替换。
    cJSON *payload = cJSON_CreateObject();
    cJSON *messages = payload ? cJSON_AddArrayToObject(payload, "messages") : NULL;
    if (!payload || !messages) {
        cJSON_Delete(payload);
        free(delta);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "create ai_history payload failed");
    }
    cJSON_AddStringToObject(payload, "sessionId", "miniapp-default");
    int64_t max_created_at = synced_at;
    for (size_t i = 0; i < delta->count && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
        const fridge_storage_chat_message_t *msg = &delta->messages[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "role", msg->role);
        cJSON_AddStringToObject(item, "content", msg->content);
        cJSON_AddItemToArray(messages, item);
        if (msg->created_at > max_created_at) {
            max_created_at = msg->created_at;
        }
    }

    char *payload_text = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    free(delta);
    ESP_RETURN_ON_FALSE(payload_text, ESP_ERR_NO_MEM, TAG, "print ai_history payload failed");

    // 复用设备同步文档的 root/events/make_topic/publish 结构，单事件 ai_history/append。
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_free(payload_text);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "create ai_history sync root failed");
    }
    add_common_fields(root, "sync");
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    if (!events || !add_storage_doc_event(events, "ai_history", "append", payload_text, "ai_history")) {
        cJSON_free(payload_text);
        cJSON_Delete(root);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "build ai_history sync event failed");
    }
    cJSON_free(payload_text);

    char *publish_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(publish_text, ESP_ERR_NO_MEM, TAG, "print ai_history sync doc failed");

    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    err = make_topic(topic, sizeof(topic), MQTT_TOPIC_SYNC);
    if (err == ESP_OK) {
        int msg_id = esp_mqtt_client_publish(s_client, topic, publish_text, 0, 0, 0);
        err = msg_id >= 0 ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) {
            s_published_count++;
            // 仅在 publish 成功交给 MQTT 后才推进游标；失败保持游标不动，靠下次重连补发整段增量。
            (void)fridge_storage_set_chat_synced_at(max_created_at);
            ESP_LOGI(TAG, "published ai_history delta up to created_at=%lld", (long long)max_created_at);
        }
    }
    cJSON_free(publish_text);
    return err;
}

static esp_err_t publish_ai_config_retained(void)
{
    ESP_RETURN_ON_FALSE(s_client && s_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is not connected");

    char *payload_text = calloc(1, MQTT_SYNC_CONFIG_JSON_LEN);
    char *aux_text = calloc(1, 1024);
    if (!payload_text || !aux_text) {
        free(payload_text);
        free(aux_text);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "allocate AI config payload failed");
    }
    esp_err_t err = fridge_ai_client_get_sync_payload(payload_text, MQTT_SYNC_CONFIG_JSON_LEN);
    if (err != ESP_OK) {
        free(payload_text);
        free(aux_text);
        return err;
    }
    cJSON *root = cJSON_Parse(payload_text);
    free(payload_text);
    if (!root) {
        free(aux_text);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_ARG, TAG, "parse AI config sync payload failed");
    }
    // retained ai_config 是后端启动后恢复设备本地 AI/ASR/TTS 配置的种子，三个配置共享同一个后写覆盖时间戳。
    if (fridge_asr_get_sync_payload(aux_text, 1024) == ESP_OK) {
        (void)merge_json_object_text(root, aux_text);
    }
    memset(aux_text, 0, 1024);
    if (fridge_tts_get_sync_payload(aux_text, 1024) == ESP_OK) {
        (void)merge_json_object_text(root, aux_text);
    }
    free(aux_text);
    add_common_fields(root, "ai_config");

    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    err = make_topic(topic, sizeof(topic), MQTT_TOPIC_AI_CONFIG);
    if (err == ESP_OK) {
        // retained 配置是后端启动恢复种子，发布前确保所有字符串都是合法 UTF-8。
        mqtt_sanitize_json_strings(root);
        char *published = cJSON_PrintUnformatted(root);
        if (!published) {
            err = ESP_ERR_NO_MEM;
        } else {
            // AI 配置 retained 给后端启动/重连时恢复设备本地配置状态；仅用于比赛受控 broker。
            int msg_id = esp_mqtt_client_publish(s_client, topic, published, 0, 1, 1);
            err = msg_id >= 0 ? ESP_OK : ESP_FAIL;
            if (err == ESP_OK) {
                s_published_count++;
            }
            cJSON_free(published);
        }
    }
    cJSON_Delete(root);
    return err;
}

static void publish_reconnect_sync_seed(void)
{
    fridge_storage_inventory_sync_status_t inventory_sync = {0};
    if (fridge_storage_get_inventory_sync_status(&inventory_sync) == ESP_OK) {
        // 首次接入云端时，本地库存可能已经存在但未标脏；必须允许设备快照作为云端种子导入。
        bool force_import = (inventory_sync.server_revision == 0 || inventory_sync.last_sync_at_ms == 0);
        if (inventory_sync.dirty || force_import) {
            ESP_LOGI(TAG,
                     "publish inventory seed on reconnect: dirty=%s server_rev=%" PRIu32 " last_sync=%lld force=%s",
                     inventory_sync.dirty ? "true" : "false",
                     inventory_sync.server_revision,
                     (long long)inventory_sync.last_sync_at_ms,
                     force_import ? "true" : "false");
            esp_err_t inv_err = fridge_mqtt_request_inventory_publish(force_import);
            if (inv_err != ESP_OK) {
                ESP_LOGW(TAG, "queue inventory seed on reconnect failed: %s", esp_err_to_name(inv_err));
            }
        }
    } else {
        esp_err_t inv_err = fridge_mqtt_request_inventory_publish(true);
        if (inv_err != ESP_OK) {
            ESP_LOGW(TAG, "queue default inventory seed on reconnect failed: %s", esp_err_to_name(inv_err));
        }
    }
    esp_err_t docs_err = request_sync_documents_publish(false);
    if (docs_err != ESP_OK) {
        ESP_LOGW(TAG, "queue sync documents on reconnect failed: %s", esp_err_to_name(docs_err));
    }
    // 重连时把离线期间攒下的会话增量一次性补发；游标保证不重复上报已落库的对话。
    esp_err_t chat_err = fridge_mqtt_publish_chat_history_delta();
    if (chat_err != ESP_OK) {
        ESP_LOGW(TAG, "publish chat history delta on reconnect failed: %s", esp_err_to_name(chat_err));
    }
    esp_err_t ai_cfg_err = publish_ai_config_retained();
    if (ai_cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "publish retained AI config on reconnect failed: %s", esp_err_to_name(ai_cfg_err));
    }
}

static esp_err_t subscribe_command_topic(void)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[FRIDGE_MQTT_MAX_TOPIC_LEN + 1];
    ESP_RETURN_ON_ERROR(make_topic(topic, sizeof(topic), MQTT_TOPIC_CMD), TAG, "make cmd topic failed");
    int msg_id = esp_mqtt_client_subscribe(s_client, topic, 1);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "subscribe command topic failed");
    ESP_LOGI(TAG, "subscribed command topic, msg_id=%d", msg_id);
    return ESP_OK;
}

static void handle_command_payload(const char *payload, int payload_len)
{
    s_received_count++;
    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root) {
        fridge_mqtt_publish_command_ack_ex("unknown", "invalid_json", "completed", false, "invalid_json", 0, 0, false, "invalid command JSON");
        return;
    }

    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *command = cJSON_GetObjectItem(root, "command");
    const char *request_text = cJSON_IsString(request_id) ? request_id->valuestring : "unknown";
    const char *command_text = cJSON_IsString(command) ? command->valuestring : "unknown";

    // 云端命令采用 received + completed 两阶段 ACK，便于后端区分「已投递」和「设备已写入本地」。
    ESP_LOGI(TAG, "received cloud command: %s request_id=%s", command_text, request_text);
    fridge_mqtt_publish_command_ack_ex(request_text, command_text, "received", true, NULL, 0, 0, false, "command received by device");

    const cJSON *command_payload = json_object_payload_or_root(root);
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    const char *error_code = "not_supported";
    bool applied = false;
    uint32_t local_revision = 0;
    uint32_t server_revision = 0;

    if (strcmp(command_text, "inventory_refresh") == 0) {
        bool force_import = false;
        const cJSON *force_import_json = cJSON_GetObjectItemCaseSensitive(command_payload, "acceptCleanSnapshot");
        if (!cJSON_IsBool(force_import_json)) {
            force_import_json = cJSON_GetObjectItemCaseSensitive(command_payload, "forceImport");
        }
        force_import = cJSON_IsBool(force_import_json) && cJSON_IsTrue(force_import_json);
        err = fridge_mqtt_request_inventory_publish(force_import);
        error_code = "publish_inventory_failed";
        fridge_storage_inventory_sync_status_t status = {0};
        if (fridge_storage_get_inventory_sync_status(&status) == ESP_OK) {
            local_revision = status.snapshot_version;
            server_revision = status.server_revision;
        }
    } else if (strcmp(command_text, "inventory_replace") == 0) {
        err = apply_inventory_replace_payload(command_payload, &local_revision, &server_revision, &applied);
        error_code = "inventory_replace_failed";
    } else if (strcmp(command_text, "sync_documents_refresh") == 0) {
        bool force_import = false;
        const cJSON *force_import_json = cJSON_GetObjectItemCaseSensitive(command_payload, "acceptCleanSnapshot");
        if (!cJSON_IsBool(force_import_json)) {
            force_import_json = cJSON_GetObjectItemCaseSensitive(command_payload, "forceImport");
        }
        force_import = cJSON_IsBool(force_import_json) && cJSON_IsTrue(force_import_json);
        err = request_sync_documents_publish(force_import);
        error_code = "sync_documents_refresh_failed";
    } else if (strcmp(command_text, "ai_chat") == 0) {
        err = start_ai_chat_worker(request_text, command_text, command_payload);
        error_code = "ai_chat_start_failed";
        if (err == ESP_OK) {
            cJSON_Delete(root);
            return;
        }
    } else if (strcmp(command_text, "fridge_zones_update") == 0) {
        err = apply_fridge_zones_payload(command_payload, &local_revision, &server_revision, &applied);
        error_code = "fridge_zones_update_failed";
    } else if (strcmp(command_text, "ai_config_update") == 0) {
        err = apply_ai_config_payload(command_payload);
        error_code = "ai_config_update_failed";
    } else if (strcmp(command_text, "asr_config_update") == 0) {
        err = apply_asr_config_payload(command_payload);
        error_code = "asr_config_update_failed";
    } else if (strcmp(command_text, "tts_config_update") == 0) {
        err = apply_tts_config_payload(command_payload);
        error_code = "tts_config_update_failed";
    } else if (strcmp(command_text, "shopping_list_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_shopping_list);
        error_code = "shopping_list_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "recipe_cache_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_recipe_cache);
        error_code = "recipe_cache_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "nutrition_cache_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_nutrition_cache);
        error_code = "nutrition_cache_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "reminder_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_reminder_queue);
        error_code = "reminder_update_failed";
        applied = err == ESP_OK;
    } else if (strcmp(command_text, "preferences_update") == 0) {
        err = apply_storage_document_payload(command_payload, fridge_storage_set_user_preferences);
        error_code = "preferences_update_failed";
        applied = err == ESP_OK;
    }

    fridge_mqtt_publish_command_ack_ex(request_text,
                                       command_text,
                                       "completed",
                                       err == ESP_OK,
                                       err == ESP_OK ? NULL : error_code,
                                       local_revision,
                                       server_revision,
                                       applied,
                                       err == ESP_OK ? "command completed" : esp_err_to_name(err));
    cJSON_Delete(root);
}

static void command_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, 0);
        drain_publish_requests();

        mqtt_command_job_t *job = NULL;
        if (xQueueReceive(s_command_queue, &job, pdMS_TO_TICKS(1000)) != pdTRUE || !job) {
            continue;
        }
        // MQTT 事件回调栈较小，库存/配置同步会解析 JSON 并写 LittleFS/NVS；
        // 常驻内部栈 worker 串行执行，避免每条命令临时申请 16KB 内部 SRAM 失败。
        handle_command_payload(job->payload, job->payload_len);
        free(job);
        ESP_LOGI(TAG,
                 "MQTT command worker high watermark=%u words",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static esp_err_t dispatch_command_payload_async(const char *payload, int payload_len)
{
    ESP_RETURN_ON_FALSE(payload && payload_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid command payload");
    ESP_RETURN_ON_ERROR(ensure_command_worker_started(), TAG, "MQTT command worker not ready");
    mqtt_command_job_t *job = heap_caps_calloc(1, sizeof(*job) + (size_t)payload_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) {
        job = calloc(1, sizeof(*job) + (size_t)payload_len + 1);
    }
    ESP_RETURN_ON_FALSE(job, ESP_ERR_NO_MEM, TAG, "allocate command job failed");
    job->payload_len = payload_len;
    memcpy(job->payload, payload, (size_t)payload_len);
    job->payload[payload_len] = '\0';
    if (xQueueSend(s_command_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(job);
        ESP_LOGW(TAG, "MQTT command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void handle_command_event_data(const esp_mqtt_event_handle_t event)
{
    if (!event || !event->data || event->data_len <= 0) {
        return;
    }
    int total_len = event->total_data_len > 0 ? event->total_data_len : event->data_len;
    int offset = event->current_data_offset;
    if (total_len <= 0 || offset < 0 || offset + event->data_len > total_len) {
        fridge_mqtt_publish_command_ack_ex("unknown", "invalid_fragment", "completed", false, "invalid_fragment", 0, 0, false, "invalid MQTT data fragment");
        reset_command_fragment_buffer();
        return;
    }
    if (total_len == event->data_len && offset == 0) {
        esp_err_t err = dispatch_command_payload_async(event->data, event->data_len);
        if (err != ESP_OK) {
            fridge_mqtt_publish_command_ack_ex("unknown", "dispatch_failed", "completed", false, "dispatch_failed", 0, 0, false, esp_err_to_name(err));
        }
        return;
    }

    if (!s_command_fragment_buffer || s_command_fragment_total_len != total_len || offset == 0) {
        reset_command_fragment_buffer();
        s_command_fragment_buffer = alloc_mqtt_payload_buffer((size_t)total_len + 1);
        if (!s_command_fragment_buffer) {
            fridge_mqtt_publish_command_ack_ex("unknown", "fragment_oom", "completed", false, "fragment_oom", 0, 0, false, "allocate MQTT command buffer failed");
            return;
        }
        s_command_fragment_total_len = total_len;
        s_command_fragment_received_len = 0;
    }
    memcpy(s_command_fragment_buffer + offset, event->data, event->data_len);
    int next_received = offset + event->data_len;
    if (next_received > s_command_fragment_received_len) {
        s_command_fragment_received_len = next_received;
    }
    if (s_command_fragment_received_len >= s_command_fragment_total_len) {
        s_command_fragment_buffer[s_command_fragment_total_len] = '\0';
        ESP_LOGI(TAG, "assembled fragmented MQTT command len=%d", s_command_fragment_total_len);
        esp_err_t err = dispatch_command_payload_async(s_command_fragment_buffer, s_command_fragment_total_len);
        if (err != ESP_OK) {
            fridge_mqtt_publish_command_ack_ex("unknown", "dispatch_failed", "completed", false, "dispatch_failed", 0, 0, false, esp_err_to_name(err));
        }
        reset_command_fragment_buffer();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        set_status_text("connected");
        ESP_LOGI(TAG, "MQTT connected");
        subscribe_command_topic();
        fridge_mqtt_publish_state(true);
        publish_reconnect_sync_seed();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_reconnect_count++;
        set_status_text("disconnected");
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_command_event_data(event);
        break;
    case MQTT_EVENT_ERROR:
        s_last_error = event->error_handle ? event->error_handle->esp_tls_last_esp_err : ESP_FAIL;
        set_status_text("error");
        ESP_LOGW(TAG, "MQTT error, esp_tls_last_esp_err=%d", s_last_error);
        break;
    default:
        break;
    }
}

esp_err_t fridge_mqtt_protocol_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(load_config(&s_config), TAG, "load MQTT config failed");
    ESP_RETURN_ON_ERROR(ensure_command_worker_started(), TAG, "start MQTT command worker failed");
    s_initialized = true;
    set_status_text(config_is_complete(&s_config) ? "configured" : "not configured");
    ESP_LOGI(TAG, "MQTT protocol initialized, enabled=%s configured=%s",
             s_config.enabled ? "yes" : "no",
             config_is_complete(&s_config) ? "yes" : "no");
    return ESP_OK;
}

esp_err_t fridge_mqtt_get_config(fridge_mqtt_config_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    *out = s_config;
    return ESP_OK;
}

esp_err_t fridge_mqtt_set_config(const fridge_mqtt_config_t *config, bool update_password)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");

    fridge_mqtt_config_t next = *config;
    if (!update_password) {
        strlcpy(next.password, s_config.password, sizeof(next.password));
    }
    if (next.keepalive_seconds == 0) {
        next.keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    }
    next.use_tls = strncmp(next.broker_uri, "mqtts://", 8) == 0 || strncmp(next.broker_uri, "wss://", 6) == 0;
    ESP_RETURN_ON_ERROR(save_config(&next, update_password), TAG, "save MQTT config failed");
    s_config = next;
    set_status_text(config_is_complete(&s_config) ? "configured" : "not configured");

    if (s_started) {
        fridge_mqtt_stop();
        if (config_is_complete(&s_config)) {
            return fridge_mqtt_start();
        }
    }
    return ESP_OK;
}

esp_err_t fridge_mqtt_clear_secret(void)
{
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open MQTT NVS failed");
    esp_err_t err = nvs_erase_key(handle, MQTT_NVS_KEY_PASS);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    s_config.password[0] = '\0';
    set_status_text("secret cleared");
    return err;
}

esp_err_t fridge_mqtt_get_status(fridge_mqtt_status_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->enabled = s_config.enabled;
    out->configured = config_is_complete(&s_config);
    out->connected = s_connected;
    out->has_password = s_config.password[0] != '\0';
    out->reconnect_count = s_reconnect_count;
    out->published_count = s_published_count;
    out->received_count = s_received_count;
    out->last_error = s_last_error;
    strlcpy(out->broker_uri, s_config.broker_uri, sizeof(out->broker_uri));
    strlcpy(out->home_id, s_config.home_id, sizeof(out->home_id));
    strlcpy(out->device_id, s_config.device_id, sizeof(out->device_id));
    strlcpy(out->username, s_config.username, sizeof(out->username));
    strlcpy(out->status_text, s_status_text, sizeof(out->status_text));
    return ESP_OK;
}

esp_err_t fridge_mqtt_start(void)
{
    ESP_RETURN_ON_ERROR(fridge_mqtt_protocol_init(), TAG, "MQTT init failed");
    ESP_RETURN_ON_FALSE(config_is_complete(&s_config), ESP_ERR_INVALID_STATE, TAG, "MQTT config incomplete");

    fridge_network_status_t network = {0};
    if (fridge_network_get_status(&network) == ESP_OK && !network.connected) {
        set_status_text("waiting for Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client) {
        return ESP_OK;
    }

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "starting MQTT, internal=%u KB largest=%u KB psram=%u KB",
             (unsigned)(free_internal / 1024),
             (unsigned)(largest_internal / 1024),
             (unsigned)(free_psram / 1024));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_config.broker_uri,
        .credentials.username = s_config.username,
        .credentials.authentication.password = s_config.password,
        .session.keepalive = s_config.keepalive_seconds ? s_config.keepalive_seconds : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS,
        .network.disable_auto_reconnect = false,
        .task.stack_size = FRIDGE_MQTT_TASK_STACK_BYTES,
        .buffer.size = FRIDGE_MQTT_BUFFER_BYTES,
        .buffer.out_size = FRIDGE_MQTT_BUFFER_BYTES,
        .outbox.limit = FRIDGE_MQTT_OUTBOX_LIMIT_BYTES,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_ERR_NO_MEM, TAG, "MQTT client init failed");
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "register MQTT event failed");
    }
    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        // 创建 MQTT 任务失败时必须释放半初始化 client，否则下一次自动重试会误判已经启动。
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_started = false;
        s_connected = false;
        set_status_text("start failed");
        ESP_RETURN_ON_ERROR(err, TAG, "start MQTT failed");
    }
    s_started = true;
    set_status_text("connecting");
    return ESP_OK;
}

esp_err_t fridge_mqtt_stop(void)
{
    if (!s_client) {
        s_started = false;
        s_connected = false;
        return ESP_OK;
    }
    esp_err_t err = esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_started = false;
    s_connected = false;
    set_status_text("stopped");
    return err;
}

esp_err_t fridge_mqtt_publish_state(bool online)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create state JSON failed");
    add_common_fields(root, "state");

    fridge_device_status_t diag = {0};
    fridge_network_status_t net = {0};
    fridge_diagnostics_get_status(&diag);
    fridge_network_get_status(&net);

    cJSON_AddBoolToObject(root, "online", online);
    cJSON_AddStringToObject(root, "firmware", "s3-alpha.0.1.0");
    cJSON_AddStringToObject(root, "ip", net.ip);
    cJSON_AddNumberToObject(root, "rssi", net.rssi);
    cJSON_AddStringToObject(root, "wifi_status", net.connected ? "ok" : "offline");
    cJSON_AddStringToObject(root, "mqtt_status", s_connected ? "ok" : "offline");
    cJSON_AddNumberToObject(root, "free_heap_kb", diag.free_heap_kb);
    cJSON_AddNumberToObject(root, "free_psram_kb", diag.free_psram_kb);
    fridge_storage_inventory_sync_status_t inventory_sync = {0};
    if (fridge_storage_get_inventory_sync_status(&inventory_sync) == ESP_OK) {
        cJSON_AddNumberToObject(root, "inventory_revision", inventory_sync.snapshot_version);
        cJSON_AddNumberToObject(root, "server_revision", inventory_sync.server_revision);
        cJSON_AddBoolToObject(root, "inventory_dirty", inventory_sync.dirty);
        cJSON_AddNumberToObject(root, "last_sync_at_ms", (double)inventory_sync.last_sync_at_ms);
    }

    esp_err_t err = publish_json(MQTT_TOPIC_STATE, root, 1, true);
    cJSON_Delete(root);
    return err;
}

esp_err_t fridge_mqtt_publish_sensor_snapshot(void)
{
    fridge_sensor_snapshot_t sensor = {0};
    ESP_RETURN_ON_ERROR(fridge_sensors_get_snapshot(&sensor), TAG, "get sensor snapshot failed");

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create sensor JSON failed");
    add_common_fields(root, "sensor");
    cJSON_AddBoolToObject(root, "presence", sensor.radar.stable_presence);
    cJSON_AddNumberToObject(root, "lux", sensor.light_value_10bit);
    cJSON_AddNumberToObject(root, "light_percent", sensor.light_percent);
    cJSON_AddNumberToObject(root, "light_delta", sensor.light_delta);
    cJSON_AddNumberToObject(root, "angle_delta", sensor.angle_delta);
    cJSON_AddNumberToObject(root, "vibration_peak", sensor.vibration_peak);
    cJSON_AddNumberToObject(root, "presence_distance_cm", sensor.radar.smoothed_distance_raw);

    esp_err_t err = publish_json(MQTT_TOPIC_SENSOR, root, 1, false);
    cJSON_Delete(root);
    return err;
}

esp_err_t fridge_mqtt_publish_command_ack(const char *request_id, const char *command, bool ok, const char *message)
{
    return fridge_mqtt_publish_command_ack_ex(request_id,
                                             command,
                                             "completed",
                                             ok,
                                             ok ? NULL : "command_failed",
                                             0,
                                             0,
                                             false,
                                             message);
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
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create ack JSON failed");
    add_common_fields(root, "cmd_ack");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "unknown");
    cJSON_AddStringToObject(root, "command", command ? command : "unknown");
    cJSON_AddStringToObject(root, "stage", stage ? stage : "completed");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "error_code", error_code ? error_code : "");
    cJSON_AddNumberToObject(root, "local_revision", local_revision);
    cJSON_AddNumberToObject(root, "server_revision", server_revision);
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddStringToObject(root, "message", message ? message : "");

    esp_err_t err = publish_json(MQTT_TOPIC_ACK, root, 1, false);
    cJSON_Delete(root);
    return err;
}
