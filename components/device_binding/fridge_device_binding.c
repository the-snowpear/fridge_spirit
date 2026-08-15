// 冰箱小精灵 P4 二维码绑定实现。
//
// 网络线程只执行短 HTTP 请求并更新脱敏快照；LVGL 由上层回调在 UI 任务中刷新。
// 当前比赛联调允许 http:// 公网 IP，bootstrap secret 会在链路上明文传输，不能视为生产安全。

#include "fridge_device_binding.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "fridge_agent_storage.h"
#include "fridge_ai_client.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define BINDING_NVS_NAMESPACE "fridge_bind"
#define BINDING_NVS_API "api_base"
#define BINDING_NVS_SN "device_sn"
#define BINDING_NVS_KEY_ID "key_id"
#define BINDING_NVS_SECRET "bootstrap"
#define BINDING_NVS_TOKEN "device_token"
#define BINDING_RESPONSE_MAX (64U * 1024U)
#define BINDING_AGENT_FILE_MAX (192U * 1024U)
#define BINDING_SEED_MAX (2U * 1024U * 1024U)
#define BINDING_SNAPSHOT_MAX (2U * 1024U * 1024U)
#define BINDING_TASK_STACK 7168
#define BINDING_RETRY_MIN_MS 2000U
#define BINDING_RETRY_MAX_MS 30000U
#define BINDING_DEFAULT_TTL_SECONDS 300U
#define BINDING_DEFAULT_POLL_MS 2000U

static const char *TAG = "fridge_binding";

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} binding_http_response_t;

static SemaphoreHandle_t s_lock;
static fridge_device_provisioning_t s_provisioning;
static char s_device_token[FRIDGE_BINDING_DEVICE_TOKEN_MAX + 1];
static fridge_device_binding_status_t s_status;
static fridge_device_binding_status_cb_t s_callback;
static void *s_callback_ctx;
static atomic_bool s_refresh_requested = ATOMIC_VAR_INIT(false);
static atomic_bool s_initialized = ATOMIC_VAR_INIT(false);
static uint32_t s_poll_after_ms = BINDING_DEFAULT_POLL_MS;
static int64_t s_session_deadline_us;

static void binding_secure_zero(void *data, size_t size)
{
    if (!data) return;
    volatile uint8_t *cursor = (volatile uint8_t *)data;
    while (size--) *cursor++ = 0;
}

static esp_err_t nvs_read_string(nvs_handle_t handle, const char *key,
                                 char *out, size_t out_size)
{
    size_t length = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    if (err != ESP_OK) out[0] = '\0';
    return err;
}

static bool provisioning_valid(const fridge_device_provisioning_t *value)
{
    return value && value->api_base_url[0] && value->device_sn[0] &&
           value->bootstrap_key_id[0] && value->bootstrap_secret[0] &&
           (strncmp(value->api_base_url, "http://", 7) == 0 ||
            strncmp(value->api_base_url, "https://", 8) == 0);
}

static esp_err_t load_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BINDING_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "open binding NVS failed");
    esp_err_t result = nvs_read_string(handle, BINDING_NVS_API,
                                       s_provisioning.api_base_url,
                                       sizeof(s_provisioning.api_base_url));
    if (result == ESP_OK) {
        result = nvs_read_string(handle, BINDING_NVS_SN,
                                 s_provisioning.device_sn,
                                 sizeof(s_provisioning.device_sn));
    }
    if (result == ESP_OK) {
        result = nvs_read_string(handle, BINDING_NVS_KEY_ID,
                                 s_provisioning.bootstrap_key_id,
                                 sizeof(s_provisioning.bootstrap_key_id));
    }
    if (result == ESP_OK) {
        result = nvs_read_string(handle, BINDING_NVS_SECRET,
                                 s_provisioning.bootstrap_secret,
                                 sizeof(s_provisioning.bootstrap_secret));
    }
    if (result == ESP_OK) {
        result = nvs_read_string(handle, BINDING_NVS_TOKEN,
                                 s_device_token, sizeof(s_device_token));
    }
    nvs_close(handle);
    return result;
}

static esp_err_t save_provisioning_nvs(const fridge_device_provisioning_t *value)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(BINDING_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open binding NVS for write failed");
    esp_err_t err = nvs_set_str(handle, BINDING_NVS_API, value->api_base_url);
    if (err == ESP_OK) err = nvs_set_str(handle, BINDING_NVS_SN, value->device_sn);
    if (err == ESP_OK) err = nvs_set_str(handle, BINDING_NVS_KEY_ID,
                                         value->bootstrap_key_id);
    if (err == ESP_OK) err = nvs_set_str(handle, BINDING_NVS_SECRET,
                                         value->bootstrap_secret);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t save_device_token_nvs(const char *token)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(BINDING_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open binding token NVS failed");
    esp_err_t err = nvs_set_str(handle, BINDING_NVS_TOKEN, token ? token : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void notify_status(void)
{
    fridge_device_binding_status_t snapshot = {0};
    fridge_device_binding_status_cb_t callback = NULL;
    void *callback_ctx = NULL;
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot = s_status;
    callback = s_callback;
    callback_ctx = s_callback_ctx;
    xSemaphoreGive(s_lock);
    if (callback) callback(&snapshot, callback_ctx);
}

static void set_status(const char *status, const char *detail,
                       esp_err_t error, int http_status)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.last_error = error;
    s_status.last_http_status = http_status;
    if (status) strlcpy(s_status.status, status, sizeof(s_status.status));
    if (detail) strlcpy(s_status.detail, detail, sizeof(s_status.detail));
    xSemaphoreGive(s_lock);
    notify_status();
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    binding_http_response_t *response =
        event ? (binding_http_response_t *)event->user_data : NULL;
    if (!event || event->event_id != HTTP_EVENT_ON_DATA || !response ||
        !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    if (response->length + (size_t)event->data_len >= response->capacity) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->data + response->length, event->data,
           (size_t)event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t build_url(char *out, size_t out_size, const char *path)
{
    if (!out || !out_size || !path) return ESP_ERR_INVALID_ARG;
    size_t base_len = strnlen(s_provisioning.api_base_url,
                              sizeof(s_provisioning.api_base_url));
    if (!base_len) return ESP_ERR_INVALID_STATE;
    bool base_slash = s_provisioning.api_base_url[base_len - 1U] == '/';
    bool path_slash = path[0] == '/';
    int written = snprintf(out, out_size, "%s%s%s",
                           s_provisioning.api_base_url,
                           base_slash == path_slash ? (base_slash ? "" : "/") : "",
                           base_slash && path_slash ? path + 1 : path);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t http_json_with_limit(esp_http_client_method_t method,
                                      const char *path,
                                      const char *body,
                                      bool authenticate,
                                      size_t response_max,
                                      char **out_json,
                                      int *out_http_status)
{
    if (!path || !out_json || !out_http_status || response_max < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_http_status = 0;

    char url[FRIDGE_BINDING_API_BASE_MAX + 160] = {0};
    ESP_RETURN_ON_ERROR(build_url(url, sizeof(url), path), TAG,
                        "build binding URL failed");

    char *response_data = heap_caps_calloc(1, response_max,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_data) {
        response_data = heap_caps_calloc(1, response_max,
                                         MALLOC_CAP_8BIT);
    }
    if (!response_data) return ESP_ERR_NO_MEM;

    binding_http_response_t response = {
        .data = response_data,
        .capacity = response_max,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(response_data);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (authenticate) {
        if (s_device_token[0]) {
            char auth[FRIDGE_BINDING_DEVICE_TOKEN_MAX + 16] = {0};
            snprintf(auth, sizeof(auth), "Device %s", s_device_token);
            esp_http_client_set_header(client, "Authorization", auth);
            binding_secure_zero(auth, sizeof(auth));
        } else {
            esp_http_client_set_header(client, "X-Bootstrap-Key-Id",
                                       s_provisioning.bootstrap_key_id);
            esp_http_client_set_header(client, "X-Bootstrap-Secret",
                                       s_provisioning.bootstrap_secret);
        }
    }
    if (body) esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    *out_http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        binding_secure_zero(response_data, response_max);
        free(response_data);
        return err;
    }
    if (response.overflow) {
        binding_secure_zero(response_data, response_max);
        free(response_data);
        return ESP_ERR_INVALID_SIZE;
    }
    *out_json = response_data;
    return ESP_OK;
}

static esp_err_t http_json(esp_http_client_method_t method, const char *path,
                           const char *body, bool authenticate,
                           char **out_json, int *out_http_status)
{
    return http_json_with_limit(method, path, body, authenticate,
                                BINDING_RESPONSE_MAX,
                                out_json, out_http_status);
}

static const char *json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static uint32_t json_u32(cJSON *object, const char *name, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) && item->valuedouble >= 0 ?
               (uint32_t)item->valuedouble : fallback;
}

// 后端所有 v2 HTTP 接口统一返回 ApiResponse 包壳；固件只解析 data 对象，
// 避免把响应包壳误当成绑定会话本身。
static cJSON *api_response_data(cJSON *root)
{
    if (!cJSON_IsObject(root)) return NULL;
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    return cJSON_IsTrue(ok) && cJSON_IsObject(data) ? data : NULL;
}

typedef struct {
    const char *domain;
    const char *path;
    const char *array_name;
    const char *task_kind;
} binding_seed_source_t;

static cJSON *seed_domain_array(cJSON *domains, const char *domain)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(domains, domain);
    if (cJSON_IsArray(array)) return array;
    array = cJSON_CreateArray();
    if (array) cJSON_AddItemToObject(domains, domain, array);
    return array;
}

// 把 TF 卡上的 P4 Agent 权威结构化数据装入首次绑定 seed。
// 文本日志、图片、音频等媒体不进入该流程；JSON 读取缓冲和最终请求体优先使用 PSRAM。
static esp_err_t seed_copy_file(cJSON *domains,
                                const binding_seed_source_t *source)
{
    char *text = heap_caps_calloc(1, BINDING_AGENT_FILE_MAX + 1U,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) return ESP_ERR_NO_MEM;
    size_t text_size = 0;
    esp_err_t err = fridge_agent_storage_json_read(source->path, text,
                                                   BINDING_AGENT_FILE_MAX + 1U,
                                                   &text_size, NULL);
    if (err != ESP_OK) {
        free(text);
        return err;
    }
    cJSON *root = cJSON_ParseWithLength(text, text_size);
    binding_secure_zero(text, BINDING_AGENT_FILE_MAX + 1U);
    free(text);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, source->array_name);
    cJSON *target = seed_domain_array(domains, source->domain);
    if (!cJSON_IsArray(items) || !target) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (!cJSON_IsObject(item)) continue;
        if (source->task_kind) {
            const char *kind = json_string(item, "kind");
            if (!kind || strcmp(kind, source->task_kind) != 0) continue;
        }
        cJSON *copy = cJSON_Duplicate(item, true);
        if (!copy) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(target, copy);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t seed_copy_agent_session(cJSON *domains)
{
    char *text = heap_caps_calloc(1, BINDING_AGENT_FILE_MAX + 1U,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) return ESP_ERR_NO_MEM;
    size_t text_size = 0;
    esp_err_t err = fridge_agent_storage_json_read(
        "agent/data/sessions.json", text, BINDING_AGENT_FILE_MAX + 1U,
        &text_size, NULL);
    if (err != ESP_OK) {
        free(text);
        return err;
    }
    cJSON *root = cJSON_ParseWithLength(text, text_size);
    binding_secure_zero(text, BINDING_AGENT_FILE_MAX + 1U);
    free(text);
    cJSON *foreground = root
                            ? cJSON_GetObjectItemCaseSensitive(root, "foreground")
                            : NULL;
    cJSON *target = seed_domain_array(domains, "agent_sessions");
    if (!cJSON_IsObject(foreground) || !target) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *item = cJSON_CreateObject();
    cJSON *data = cJSON_Duplicate(foreground, true);
    if (!item || !data) {
        cJSON_Delete(item);
        cJSON_Delete(data);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(item, "id", "foreground");
    cJSON_AddItemToObject(item, "data", data);
    cJSON_AddItemToArray(target, item);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t add_current_ai_config(cJSON *domains)
{
    fridge_ai_config_update_t *config =
        heap_caps_calloc(1, sizeof(*config), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!config) config = calloc(1, sizeof(*config));
    if (!config) return ESP_ERR_NO_MEM;
    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }

    cJSON *array = seed_domain_array(domains, "ai_config");
    cJSON *item = cJSON_CreateObject();
    if (!array || !item) {
        cJSON_Delete(item);
        binding_secure_zero(config, sizeof(*config));
        free(config);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(item, "id", "active-profile");
    cJSON_AddNumberToObject(item, "profileId", config->profile_id);
    cJSON_AddStringToObject(item, "profileName", config->profile_name);
    cJSON_AddStringToObject(item, "apiBaseUrl", config->api_base_url);
    cJSON_AddStringToObject(item, "model", config->model);
    cJSON_AddStringToObject(item, "systemPrompt", config->system_prompt);
    cJSON_AddNumberToObject(item, "timeoutMs", config->timeout_ms);
    cJSON_AddNumberToObject(item, "apiFormat", config->api_format);
    // 用户已确认比赛开发阶段允许跨端同步 API Key；这里只进入鉴权设备 seed，
    // 不写日志、不进入普通状态接口，并在请求完成后立即清零内存。
    if (config->api_key[0]) cJSON_AddStringToObject(item, "apiKey", config->api_key);
    cJSON_AddItemToArray(array, item);
    binding_secure_zero(config, sizeof(*config));
    free(config);
    return ESP_OK;
}

static esp_err_t post_initial_agent_seed(bool *out_cloud_exists)
{
    if (!out_cloud_exists) return ESP_ERR_INVALID_ARG;
    *out_cloud_exists = false;
    fridge_agent_storage_status_t storage = {0};
    fridge_agent_storage_get_status(&storage);
    if (!storage.ready) return ESP_ERR_INVALID_STATE;

    static const binding_seed_source_t sources[] = {
        {"inventory", "agent/data/inventory.json", "items", NULL},
        {"reminders", "agent/data/tasks.json", "tasks", "reminder"},
        {"timers", "agent/data/tasks.json", "tasks", "timer"},
        {"alarms", "agent/data/tasks.json", "tasks", "alarm"},
        {"stopwatch", "agent/data/stopwatch.json", "items", NULL},
        {"shopping", "agent/data/shopping.json", "items", NULL},
        {"recipes", "agent/data/recipes.json", "items", NULL},
        {"nutrition", "agent/data/nutrition.json", "items", NULL},
        {"agent_memory", "agent/data/memory.json", "items", NULL},
        {"device_settings", "agent/config/device_settings.json", "items", NULL},
    };

    cJSON *root = cJSON_CreateObject();
    cJSON *domains = root ? cJSON_AddObjectToObject(root, "domains") : NULL;
    if (!domains) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        esp_err_t seed_err = seed_copy_file(domains, &sources[i]);
        if (seed_err != ESP_OK) {
            cJSON_Delete(root);
            return seed_err;
        }
    }
    esp_err_t session_err = seed_copy_agent_session(domains);
    if (session_err != ESP_OK) {
        cJSON_Delete(root);
        return session_err;
    }
    esp_err_t ai_err = add_current_ai_config(domains);
    if (ai_err != ESP_OK) {
        cJSON_Delete(root);
        return ai_err;
    }

    char *body = heap_caps_calloc(1, BINDING_SEED_MAX,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_PrintPreallocated(root, body, BINDING_SEED_MAX, false)) {
        binding_secure_zero(body, BINDING_SEED_MAX);
        free(body);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);

    char *response = NULL;
    int http_status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST, "/api/v2/device-sync/seed",
                              body, true, &response, &http_status);
    binding_secure_zero(body, BINDING_SEED_MAX);
    free(body);
    if (response) {
        binding_secure_zero(response, BINDING_RESPONSE_MAX);
        free(response);
    }
    if (err != ESP_OK) return err;
    // 409 表示该家庭已有 P4 Agent 权威数据，随后改为下载云端快照。
    if (http_status == 409) {
        *out_cloud_exists = true;
        return ESP_OK;
    }
    return http_status >= 200 && http_status < 300 ?
               ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t download_and_apply_agent_snapshot(void)
{
    char *response = NULL;
    int http_status = 0;
    esp_err_t err = http_json_with_limit(
        HTTP_METHOD_GET, "/api/v2/device-sync/snapshot", NULL, true,
        BINDING_SNAPSHOT_MAX, &response, &http_status);
    if (err == ESP_OK && http_status >= 200 && http_status < 300) {
        err = fridge_mqtt_apply_snapshot_json(response);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (response) {
        binding_secure_zero(response, BINDING_SNAPSHOT_MAX);
        free(response);
    }
    return err;
}

static esp_err_t request_session(void)
{
    cJSON *request = cJSON_CreateObject();
    if (!request) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(request, "deviceSn", s_provisioning.device_sn);
    if (s_device_token[0]) {
        cJSON_AddStringToObject(request, "deviceToken", s_device_token);
    } else {
        cJSON_AddStringToObject(request, "keyId",
                               s_provisioning.bootstrap_key_id);
        cJSON_AddStringToObject(request, "bootstrapSecret",
                               s_provisioning.bootstrap_secret);
    }
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) return ESP_ERR_NO_MEM;

    char *response = NULL;
    int http_status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST,
                              "/api/v2/device-binding/sessions",
                              body, s_device_token[0] != '\0',
                              &response, &http_status);
    binding_secure_zero(body, strlen(body));
    cJSON_free(body);
    if (err != ESP_OK) return err;
    if (http_status < 200 || http_status >= 300) {
        binding_secure_zero(response, BINDING_RESPONSE_MAX);
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(response);
    binding_secure_zero(response, BINDING_RESPONSE_MAX);
    free(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON *data = api_response_data(root);
    if (!data) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *session_id = json_string(data, "sessionId");
    const char *qr_payload = json_string(data, "qrPayload");
    const char *remote_status = json_string(data, "status");
    uint32_t ttl = json_u32(data, "expiresInSeconds",
                            BINDING_DEFAULT_TTL_SECONDS);
    uint32_t poll_ms = json_u32(data, "pollAfterMs",
                                BINDING_DEFAULT_POLL_MS);
    if (!session_id || !qr_payload) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.session_id, session_id, sizeof(s_status.session_id));
    strlcpy(s_status.qr_payload, qr_payload, sizeof(s_status.qr_payload));
    s_status.qr_ready = true;
    s_status.expires_in_seconds = ttl;
    strlcpy(s_status.status,
            s_status.bound ? "设备已连接，可扫码加入当前家庭" : "等待小程序扫码",
            sizeof(s_status.status));
    strlcpy(s_status.detail,
            s_status.bound ? "每次二维码仅能加入一个账号，成功后会自动刷新。"
                           : "首次扫码会把当前 P4 Agent 数据作为云端初始数据。",
            sizeof(s_status.detail));
    if (remote_status && strcmp(remote_status, "claimed") == 0) {
        strlcpy(s_status.status, "绑定信息待设备领取", sizeof(s_status.status));
    }
    s_status.last_http_status = http_status;
    s_status.last_error = ESP_OK;
    xSemaphoreGive(s_lock);
    s_poll_after_ms = poll_ms < 500U ? 500U : (poll_ms > 10000U ? 10000U : poll_ms);
    s_session_deadline_us = esp_timer_get_time() + (int64_t)ttl * 1000000LL;
    cJSON_Delete(root);
    notify_status();
    return ESP_OK;
}

static esp_err_t apply_claimed_bundle(cJSON *root)
{
    const char *home_id = json_string(root, "homeId");
    const char *delivered_device_token = json_string(root, "deviceToken");
    const char *device_token = delivered_device_token ?
                                   delivered_device_token : s_device_token;
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (!home_id || !device_token || !cJSON_IsObject(mqtt)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *broker_uri = json_string(mqtt, "brokerUri");
    const char *client_id = json_string(mqtt, "clientId");
    const char *username = json_string(mqtt, "username");
    const char *password = json_string(mqtt, "password");
    if (!broker_uri) return ESP_ERR_INVALID_RESPONSE;

    fridge_mqtt_config_t config = {0};
    strlcpy(config.broker_uri, broker_uri, sizeof(config.broker_uri));
    strlcpy(config.home_id, home_id, sizeof(config.home_id));
    strlcpy(config.device_id, s_provisioning.device_sn,
            sizeof(config.device_id));
    if (username) strlcpy(config.username, username, sizeof(config.username));
    if (password) strlcpy(config.password, password, sizeof(config.password));
    config.binding_epoch = json_u32(root, "bindingEpoch", 1U);
    config.keepalive_seconds = FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    config.use_tls = strncmp(config.broker_uri, "mqtts://", 8) == 0;
    config.enabled = true;
    ESP_RETURN_ON_ERROR(fridge_mqtt_set_config(&config, true), TAG,
                        "save claimed MQTT config failed");

    if (delivered_device_token) {
        strlcpy(s_device_token, delivered_device_token, sizeof(s_device_token));
        ESP_RETURN_ON_ERROR(save_device_token_nvs(s_device_token), TAG,
                            "save device HTTP token failed");
    }
    bool cloud_exists = false;
    ESP_RETURN_ON_ERROR(post_initial_agent_seed(&cloud_exists), TAG,
                        "seed P4 Agent structured data failed");
    // 无论首次种子被接受还是云端已有数据，都立即回拉一次权威快照：
    // 首次种子中的本地条目原本没有 entityRevision，若不回填，设备下一次本地
    // 修改会错误地以 baseEntityRevision=0 上报并与云端 revision 1 冲突。
    esp_err_t snapshot_err = download_and_apply_agent_snapshot();
    if (snapshot_err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s",
                 cloud_exists ? "apply existing cloud Agent snapshot failed"
                              : "apply seeded cloud Agent snapshot failed",
                 esp_err_to_name(snapshot_err));
        return snapshot_err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.bound = true;
    s_status.binding_epoch = config.binding_epoch;
    s_status.qr_ready = false;
    s_status.qr_payload[0] = '\0';
    strlcpy(s_status.status, "设备绑定成功", sizeof(s_status.status));
    strlcpy(s_status.detail, "正在连接 MQTT 并同步 P4 Agent 数据",
            sizeof(s_status.detail));
    xSemaphoreGive(s_lock);
    notify_status();

    binding_secure_zero(config.password, sizeof(config.password));
    esp_err_t start_err = fridge_mqtt_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "MQTT start after binding failed: %s",
                 esp_err_to_name(start_err));
    }
    (void)client_id; // clientId 由 MQTT 组件根据 device/home/bindingEpoch 稳定生成。
    return ESP_OK;
}

static esp_err_t acknowledge_session(void)
{
    char path[160] = {0};
    snprintf(path, sizeof(path), "/api/v2/device-binding/sessions/%s/ack",
             s_status.session_id);
    char *response = NULL;
    int http_status = 0;
    esp_err_t err = http_json(HTTP_METHOD_POST, path, "{}",
                              true, &response, &http_status);
    if (response) {
        binding_secure_zero(response, BINDING_RESPONSE_MAX);
        free(response);
    }
    return err == ESP_OK && http_status >= 200 && http_status < 300 ?
               ESP_OK : (err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err);
}

static esp_err_t poll_session(bool *out_claimed)
{
    if (!out_claimed) return ESP_ERR_INVALID_ARG;
    *out_claimed = false;
    char session_id[FRIDGE_BINDING_SESSION_ID_MAX + 1] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(session_id, s_status.session_id, sizeof(session_id));
    xSemaphoreGive(s_lock);
    if (!session_id[0]) return ESP_ERR_INVALID_STATE;

    char path[256] = {0};
    snprintf(path, sizeof(path),
             "/api/v2/device-binding/sessions/%s?deviceSn=%s",
             session_id, s_provisioning.device_sn);
    char *response = NULL;
    int http_status = 0;
    esp_err_t err = http_json(HTTP_METHOD_GET, path, NULL, true,
                              &response, &http_status);
    if (err != ESP_OK) return err;
    if (http_status == 410) {
        binding_secure_zero(response, BINDING_RESPONSE_MAX);
        free(response);
        return ESP_ERR_TIMEOUT;
    }
    if (http_status < 200 || http_status >= 300) {
        binding_secure_zero(response, BINDING_RESPONSE_MAX);
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_Parse(response);
    binding_secure_zero(response, BINDING_RESPONSE_MAX);
    free(response);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON *data = api_response_data(root);
    if (!data) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *remote_status = json_string(data, "status");
    uint32_t expires = json_u32(data, "expiresInSeconds",
                                s_status.expires_in_seconds);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.expires_in_seconds = expires;
    s_status.last_http_status = http_status;
    s_status.last_error = ESP_OK;
    xSemaphoreGive(s_lock);

    if (remote_status && strcmp(remote_status, "claimed") == 0) {
        err = apply_claimed_bundle(data);
        if (err == ESP_OK) {
            err = acknowledge_session();
            if (err == ESP_OK) *out_claimed = true;
        }
    } else {
        notify_status();
    }
    cJSON_Delete(root);
    return err;
}

static bool network_connected(void)
{
    fridge_network_status_t network = {0};
    return fridge_network_get_status(&network) == ESP_OK && network.connected;
}

static void binding_task(void *argument)
{
    (void)argument;
    uint32_t retry_ms = BINDING_RETRY_MIN_MS;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.worker_running = true;
    xSemaphoreGive(s_lock);
    notify_status();

    while (true) {
        if (!provisioning_valid(&s_provisioning)) {
            set_status("尚未预置设备身份",
                       "请通过 USB 写入 API 地址、deviceSn 和 bootstrap key。",
                       ESP_ERR_INVALID_STATE, 0);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!network_connected()) {
            set_status("等待网络连接", "连接 Wi-Fi 后自动申请绑定二维码。",
                       ESP_ERR_INVALID_STATE, 0);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        bool refresh = atomic_exchange_explicit(&s_refresh_requested, false,
                                                memory_order_acq_rel);
        bool expired = s_session_deadline_us > 0 &&
                       esp_timer_get_time() >= s_session_deadline_us;
        bool has_session = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        has_session = s_status.session_id[0] != '\0';
        xSemaphoreGive(s_lock);
        if (refresh || expired) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.session_id[0] = '\0';
            s_status.qr_payload[0] = '\0';
            s_status.qr_ready = false;
            xSemaphoreGive(s_lock);
            has_session = false;
        }

        esp_err_t err = ESP_OK;
        if (!has_session) {
            set_status("正在申请二维码", "请保持 Wi-Fi 连接。",
                       ESP_OK, 0);
            err = request_session();
        } else {
            bool claimed = false;
            err = poll_session(&claimed);
            if (claimed) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.session_id[0] = '\0';
                s_status.qr_payload[0] = '\0';
                s_status.qr_ready = false;
                xSemaphoreGive(s_lock);
                s_session_deadline_us = 0;
                // 成功后短暂展示结果，再为下一个小程序账号生成加入二维码。
                vTaskDelay(pdMS_TO_TICKS(3000));
                retry_ms = BINDING_RETRY_MIN_MS;
                continue;
            }
        }

        if (err != ESP_OK) {
            set_status("绑定服务暂不可用", esp_err_to_name(err), err,
                       s_status.last_http_status);
            if (err == ESP_ERR_TIMEOUT) {
                atomic_store_explicit(&s_refresh_requested, true,
                                      memory_order_release);
            }
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = retry_ms < BINDING_RETRY_MAX_MS / 2U ?
                           retry_ms * 2U : BINDING_RETRY_MAX_MS;
        } else {
            retry_ms = BINDING_RETRY_MIN_MS;
            vTaskDelay(pdMS_TO_TICKS(s_poll_after_ms));
        }
    }
}

esp_err_t fridge_device_binding_init(fridge_device_binding_status_cb_t callback,
                                     void *user_ctx)
{
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_callback = callback;
        s_callback_ctx = user_ctx;
        xSemaphoreGive(s_lock);
        notify_status();
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = load_nvs();
    if (err != ESP_OK) return err;
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.provisioned = provisioning_valid(&s_provisioning);
    s_status.bound = s_device_token[0] != '\0';
    strlcpy(s_status.api_base_url, s_provisioning.api_base_url,
            sizeof(s_status.api_base_url));
    strlcpy(s_status.device_sn, s_provisioning.device_sn,
            sizeof(s_status.device_sn));
    strlcpy(s_status.status,
            s_status.provisioned ? "准备连接绑定服务" : "尚未预置设备身份",
            sizeof(s_status.status));
    s_callback = callback;
    s_callback_ctx = user_ctx;
    atomic_store_explicit(&s_initialized, true, memory_order_release);

    if (xTaskCreate(binding_task, "device_binding", BINDING_TASK_STACK,
                    NULL, 3, NULL) != pdPASS) {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    notify_status();
    return ESP_OK;
}

esp_err_t fridge_device_binding_set_provisioning(
    const fridge_device_provisioning_t *provisioning)
{
    if (!provisioning_valid(provisioning)) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(save_provisioning_nvs(provisioning), TAG,
                        "save provisioning failed");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    binding_secure_zero(&s_provisioning, sizeof(s_provisioning));
    s_provisioning = *provisioning;
    s_provisioning.api_base_url[sizeof(s_provisioning.api_base_url) - 1U] = '\0';
    s_provisioning.device_sn[sizeof(s_provisioning.device_sn) - 1U] = '\0';
    s_provisioning.bootstrap_key_id[
        sizeof(s_provisioning.bootstrap_key_id) - 1U] = '\0';
    s_provisioning.bootstrap_secret[
        sizeof(s_provisioning.bootstrap_secret) - 1U] = '\0';
    s_status.provisioned = true;
    strlcpy(s_status.api_base_url, s_provisioning.api_base_url,
            sizeof(s_status.api_base_url));
    strlcpy(s_status.device_sn, s_provisioning.device_sn,
            sizeof(s_status.device_sn));
    s_status.session_id[0] = '\0';
    s_status.qr_payload[0] = '\0';
    s_status.qr_ready = false;
    xSemaphoreGive(s_lock);
    atomic_store_explicit(&s_refresh_requested, true, memory_order_release);
    notify_status();
    return ESP_OK;
}

esp_err_t fridge_device_binding_get_status(fridge_device_binding_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    if (s_session_deadline_us > 0 && esp_timer_get_time() < s_session_deadline_us) {
        out->expires_in_seconds =
            (uint32_t)((s_session_deadline_us - esp_timer_get_time() + 999999LL) /
                       1000000LL);
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_device_binding_refresh_session(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_refresh_requested, true, memory_order_release);
    return ESP_OK;
}
