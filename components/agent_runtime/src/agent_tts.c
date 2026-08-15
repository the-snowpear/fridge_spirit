// 冰箱小精灵 P4 Agent 云端 TTS。
// 旧 speaker 组件绑定 S3/MAX98357A GPIO，不能在 P4 使用；本文件仅兼容其 fridge_tts NVS
// 配置和 SiliconFlow MOSS 请求格式，输出固定为 ES8311 所需的 16kHz/16-bit/mono PCM。

#include "agent_tts.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_codec_es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define AGENT_TTS_NS "fridge_tts"
#define AGENT_TTS_URL_KEY "url"
#define AGENT_TTS_MODEL_KEY "model"
#define AGENT_TTS_VOICE_KEY "voice"
#define AGENT_TTS_KEY_KEY "api_key"
#define AGENT_TTS_TIMEOUT_KEY "timeout_ms"
#define AGENT_TTS_SAMPLE_RATE 16000
#define AGENT_TTS_MAX_TEXT 240
#define AGENT_TTS_MAX_AUDIO (192U * 1024U)
#define AGENT_TTS_READ_CHUNK 2048U

typedef struct {
    char url[193];
    char model[65];
    char voice[33];
    char key[257];
    uint32_t timeout_ms;
} agent_tts_config_t;

static const char *TAG = "agent_tts";
static atomic_bool s_stop_requested = ATOMIC_VAR_INIT(false);
static atomic_bool s_active = ATOMIC_VAR_INIT(false);
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static fridge_agent_runtime_tts_status_t s_status = {
    .state = "idle",
    .sample_rate = AGENT_TTS_SAMPLE_RATE,
};

static void set_status(const char *state, const char *error)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.state, state ? state : "idle", sizeof(s_status.state));
    strlcpy(s_status.error, error ? error : "", sizeof(s_status.error));
    portEXIT_CRITICAL(&s_status_lock);
}

void fridge_agent_tts_get_status(fridge_agent_runtime_tts_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

static void emit(fridge_agent_tts_state_cb_t cb, const char *state, const char *detail, void *ctx)
{
    if (cb) cb(state, detail ? detail : "", ctx);
}

static void read_string(nvs_handle_t handle, const char *name, char *out, size_t out_size,
                        const char *fallback)
{
    size_t len = out_size;
    if (nvs_get_str(handle, name, out, &len) != ESP_OK || !out[0]) {
        strlcpy(out, fallback, out_size);
    }
}

static esp_err_t load_config(agent_tts_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    strlcpy(out->url, "https://api.siliconflow.cn/v1/audio/speech", sizeof(out->url));
    strlcpy(out->model, "fnlp/MOSS-TTSD-v0.5", sizeof(out->model));
    strlcpy(out->voice, "fnlp/MOSS-TTSD-v0.5:alex", sizeof(out->voice));
    out->timeout_ms = 45000;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AGENT_TTS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    read_string(handle, AGENT_TTS_URL_KEY, out->url, sizeof(out->url), out->url);
    read_string(handle, AGENT_TTS_MODEL_KEY, out->model, sizeof(out->model), out->model);
    read_string(handle, AGENT_TTS_VOICE_KEY, out->voice, sizeof(out->voice), out->voice);
    size_t key_len = sizeof(out->key);
    (void)nvs_get_str(handle, AGENT_TTS_KEY_KEY, out->key, &key_len);
    uint32_t timeout = out->timeout_ms;
    if (nvs_get_u32(handle, AGENT_TTS_TIMEOUT_KEY, &timeout) == ESP_OK && timeout >= 5000 && timeout <= 120000) {
        out->timeout_ms = timeout;
    }
    nvs_close(handle);
    return ESP_OK;
}

static bool moss_model(const char *model)
{
    return model && strcmp(model, "fnlp/MOSS-TTSD-v0.5") == 0;
}

static void normalized_voice(const agent_tts_config_t *config, char *out, size_t out_size)
{
    if (moss_model(config->model)) {
        if (!config->voice[0] || strcmp(config->voice, "alloy") == 0) {
            strlcpy(out, "fnlp/MOSS-TTSD-v0.5:alex", out_size);
        } else if (strchr(config->voice, ':')) {
            strlcpy(out, config->voice, out_size);
        } else {
            snprintf(out, out_size, "%s:%s", config->model, config->voice);
        }
        return;
    }
    strlcpy(out, config->voice, out_size);
}

static char *make_request(const agent_tts_config_t *config, const char *text)
{
    // MOSS 允许把未带前缀的音色规范化为 "model:voice"，长度不能再按单独 voice 字段分配。
    char voice[sizeof(config->model) + sizeof(config->voice) + 2U] = {0};
    normalized_voice(config, voice, sizeof(voice));
    char content[AGENT_TTS_MAX_TEXT + 8] = {0};
    if (moss_model(config->model) && strncmp(text, "[S1]", 4) != 0 && strncmp(text, "[S2]", 4) != 0) {
        snprintf(content, sizeof(content), "[S1] %s", text);
    } else {
        strlcpy(content, text, sizeof(content));
    }
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    cJSON_AddStringToObject(body, "model", config->model);
    cJSON_AddStringToObject(body, "voice", voice);
    cJSON_AddStringToObject(body, "input", content);
    cJSON_AddStringToObject(body, "response_format", "pcm");
    cJSON_AddNumberToObject(body, "sample_rate", AGENT_TTS_SAMPLE_RATE);
    // MOSS 已验证使用流式 PCM；服务端可能没有 Content-Length，并以 TLS close 表示结束。
    cJSON_AddBoolToObject(body, "stream", moss_model(config->model));
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return json;
}

static esp_err_t download_pcm(const agent_tts_config_t *config, const char *body,
                              uint8_t **out_pcm, size_t *out_len,
                              int *out_http_status, uint32_t *out_latency_ms)
{
    *out_pcm = NULL;
    *out_len = 0;
    if (out_http_status) *out_http_status = 0;
    if (out_latency_ms) *out_latency_ms = 0;
    uint8_t *pcm = heap_caps_malloc(AGENT_TTS_MAX_AUDIO, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) pcm = heap_caps_malloc(AGENT_TTS_MAX_AUDIO, MALLOC_CAP_8BIT);
    if (!pcm) return ESP_ERR_NO_MEM;

    esp_http_client_config_t http = {
        .url = config->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)config->timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client) { free(pcm); return ESP_FAIL; }
    char auth[274] = {0};
    snprintf(auth, sizeof(auth), "Bearer %s", config->key);
    esp_http_client_set_header(client, "Accept", "audio/pcm, application/octet-stream");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    int64_t started_us = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err == ESP_OK && esp_http_client_write(client, body, (int)strlen(body)) != (int)strlen(body)) err = ESP_FAIL;
    int status = 0;
    if (err == ESP_OK) {
        int64_t headers = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (out_http_status) *out_http_status = status;
        if (headers < 0) err = ESP_FAIL;
        if (status < 200 || status >= 300) err = ESP_FAIL;
        if (headers > (int64_t)AGENT_TTS_MAX_AUDIO) err = ESP_ERR_NO_MEM;
        ESP_LOGI(TAG, "TTS HTTP headers: status=%d content_length=%lld",
                 status, (long long)headers);
    }
    size_t used = 0;
    while (err == ESP_OK && !atomic_load_explicit(&s_stop_requested, memory_order_acquire) &&
           used < AGENT_TTS_MAX_AUDIO) {
        int n = esp_http_client_read(client, (char *)pcm + used,
                                     (int)((AGENT_TTS_MAX_AUDIO - used) > AGENT_TTS_READ_CHUNK ?
                                           AGENT_TTS_READ_CHUNK : (AGENT_TTS_MAX_AUDIO - used)));
        if (n > 0) { used += (size_t)n; continue; }
        if (n == 0) break;
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (used > 0 && status >= 200 && status < 300) {
            // 流式 2xx 响应可能以 TLS close 收尾；已有偶数字节 PCM 时按正常 EOF 处理。
            ESP_LOGI(TAG, "TTS stream ended by TLS close after %u bytes", (unsigned)used);
            break;
        }
        err = ESP_FAIL;
    }
    if (out_latency_ms) {
        *out_latency_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (atomic_load_explicit(&s_stop_requested, memory_order_acquire)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && (used == 0 || (used & 1U) != 0U)) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS download failed status=%d bytes=%u err=%s", status, (unsigned)used,
                 esp_err_to_name(err));
        free(pcm);
        return err;
    }
    *out_pcm = pcm;
    *out_len = used;
    return ESP_OK;
}

void fridge_agent_tts_stop(void)
{
    atomic_store_explicit(&s_stop_requested, true, memory_order_release);
    (void)fridge_codec_es8311_pa_off();
}

bool fridge_agent_tts_is_active(void)
{
    return atomic_load_explicit(&s_active, memory_order_acquire);
}

esp_err_t fridge_agent_tts_synthesize_and_play(const char *text,
                                                fridge_agent_tts_state_cb_t state_cb,
                                                void *user_ctx)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (atomic_exchange_explicit(&s_active, true, memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }
    char safe_text[AGENT_TTS_MAX_TEXT + 1] = {0};
    strlcpy(safe_text, text, sizeof(safe_text));
    agent_tts_config_t config;
    esp_err_t err = load_config(&config);
    if (err != ESP_OK || !config.key[0]) {
        set_status("error", "未配置 TTS API Key");
        emit(state_cb, "error", "未配置 TTS API Key", user_ctx);
        atomic_store_explicit(&s_active, false, memory_order_release);
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    char voice[sizeof(config.model) + sizeof(config.voice) + 2U] = {0};
    normalized_voice(&config, voice, sizeof(voice));
    portENTER_CRITICAL(&s_status_lock);
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.state, "synthesizing", sizeof(s_status.state));
    s_status.sample_rate = AGENT_TTS_SAMPLE_RATE;
    strlcpy(s_status.model, config.model, sizeof(s_status.model));
    strlcpy(s_status.voice, voice, sizeof(s_status.voice));
    portEXIT_CRITICAL(&s_status_lock);
    atomic_store_explicit(&s_stop_requested, false, memory_order_release);
    emit(state_cb, "synthesizing", "正在生成语音", user_ctx);
    char *body = make_request(&config, safe_text);
    if (!body) {
        set_status("error", "构造 TTS 请求失败");
        atomic_store_explicit(&s_active, false, memory_order_release);
        emit(state_cb, "error", "构造 TTS 请求失败", user_ctx);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    int http_status = 0;
    uint32_t latency_ms = 0;
    err = download_pcm(&config, body, &pcm, &pcm_len, &http_status, &latency_ms);
    cJSON_free(body);
    portENTER_CRITICAL(&s_status_lock);
    s_status.http_status = http_status;
    s_status.latency_ms = latency_ms;
    s_status.audio_bytes = pcm_len;
    s_status.duration_ms = (uint32_t)(pcm_len * 1000U /
                                     (AGENT_TTS_SAMPLE_RATE * sizeof(int16_t)));
    portEXIT_CRITICAL(&s_status_lock);
    if (err != ESP_OK) {
        bool stopped = atomic_load_explicit(&s_stop_requested, memory_order_acquire);
        const char *detail = stopped ? "播报已停止" : "TTS 服务未返回有效 16k PCM";
        set_status(stopped ? "idle" : "error", detail);
        atomic_store_explicit(&s_active, false, memory_order_release);
        emit(state_cb, stopped ? "stopped" : "error", detail, user_ctx);
        return err;
    }
    if (atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
        free(pcm);
        set_status("idle", "播报已停止");
        atomic_store_explicit(&s_active, false, memory_order_release);
        emit(state_cb, "stopped", "播报已停止", user_ctx);
        return ESP_ERR_INVALID_STATE;
    }
    err = fridge_codec_es8311_start_output(pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        const char *detail = err == ESP_ERR_TIMEOUT ? "摄像头正在使用音频共享 I2C" : "扬声器忙";
        free(pcm);
        set_status("error", detail);
        atomic_store_explicit(&s_active, false, memory_order_release);
        emit(state_cb, "error", detail, user_ctx);
        return err;
    }
    set_status("playing", "");
    emit(state_cb, "playing", "正在播报", user_ctx);
    for (size_t offset = 0;
         offset < pcm_len &&
         !atomic_load_explicit(&s_stop_requested, memory_order_acquire); ) {
        size_t bytes = pcm_len - offset;
        if (bytes > AGENT_TTS_READ_CHUNK) bytes = AGENT_TTS_READ_CHUNK;
        err = fridge_codec_es8311_write((const int16_t *)(pcm + offset), bytes / sizeof(int16_t),
                                        pdMS_TO_TICKS(250));
        if (err != ESP_OK) break;
        offset += bytes;
        portENTER_CRITICAL(&s_status_lock);
        s_status.played_bytes = offset;
        portEXIT_CRITICAL(&s_status_lock);
    }
    free(pcm);
    (void)fridge_codec_es8311_stop_output();
    bool stopped = atomic_load_explicit(&s_stop_requested, memory_order_acquire);
    atomic_store_explicit(&s_active, false, memory_order_release);
    if (stopped) {
        set_status("idle", "播报已停止");
        emit(state_cb, "stopped", "播报已停止", user_ctx);
    } else if (err == ESP_OK) {
        set_status("done", "");
        emit(state_cb, "done", "播报完成", user_ctx);
    } else {
        set_status("error", "扬声器播放失败");
        emit(state_cb, "error", "扬声器播放失败", user_ctx);
    }
    return stopped ? ESP_ERR_INVALID_STATE : err;
}
