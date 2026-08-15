// 冰箱小精灵智谱 GLM-Realtime 实现。
// 单任务 glm_rt 独占 WebSocket；USB 仅投递命令；工具必须经 Tool Broker。
// 音频：ES8311 16k 上行 / 24k 下行重采样到 16k 播放；PA_CTRL 由 codec 控制。

#include "fridge_glm_realtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "fridge_audio.h"
#include "fridge_network.h"
#include "fridge_tool_broker.h"
#include "fridge_tool_registry.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define RT_NVS_NS "fridge_rt"
#define RT_NVS_KEY "key"
#define RT_NVS_URL "url"
#define RT_NVS_MODEL "model"
#define RT_NVS_VOICE "voice"
#define RT_NVS_INSTR "instr"
#define RT_NVS_VAD "vad_mode"
#define RT_NVS_TIMEOUT "timeout"
#define RT_NVS_ENABLED "enabled"
#define RT_NVS_UPDATED "updated_ms"

#define RT_TASK_STACK 12288
#define RT_TASK_PRIO 5
#define RT_AUDIO_TASK_STACK 4096
#define RT_AUDIO_TASK_PRIO 6
#define RT_TOOL_TASK_STACK 8192
#define RT_TOOL_TASK_PRIO 4
#define RT_CMD_QUEUE_LEN 8
#define RT_RX_QUEUE_LEN 12
#define RT_TOOL_QUEUE_LEN 2
#define RT_RX_INITIAL_BYTES (256 * 1024)
#define RT_RX_MAX_BYTES (2 * 1024 * 1024)
#define RT_AUDIO_FRAME_SAMPLES 1600  // 100ms @ 16k
#define RT_AUDIO_FRAME_BYTES (RT_AUDIO_FRAME_SAMPLES * sizeof(int16_t))
#define RT_MIC_MESSAGE_BUFFER_BYTES ((RT_AUDIO_FRAME_BYTES + 16) * 3 + 64)
// 智谱会把数秒 PCM24 音频放进单个 JSON 事件，且不同轮次的分片大小并不固定。
// 工作缓冲只增不减并强制使用 PSRAM：初始覆盖常见响应，必要时动态扩容到约 32 秒音频。
#define RT_PLAY_RING_INITIAL_SAMPLES (FRIDGE_AUDIO_SAMPLE_RATE * 12)
#define RT_PLAY_RING_MAX_SAMPLES (FRIDGE_AUDIO_SAMPLE_RATE * 40)
#define RT_AUDIO_DECODE_INITIAL_BYTES (192 * 1024)
#define RT_AUDIO_DECODE_MAX_BYTES ((RT_RX_MAX_BYTES / 4) * 3)
#define RT_AUDIO_RESAMPLE_INITIAL_SAMPLES (64 * 1024)
#define RT_AUDIO_RESAMPLE_MAX_SAMPLES \
    ((RT_AUDIO_DECODE_MAX_BYTES / sizeof(int16_t) / 3) * 2 + 4)
#define RT_MAX_TOOLS_EXPORT 24
#define RT_OUTPUT_MAX_LEN 1800
#define RT_HEARTBEAT_STALE_MS 90000
#define RT_TOOL_ONE_PER_RESPONSE 1
#define RT_LOCAL_VAD_RMS_THRESHOLD 80
#define RT_LOCAL_VAD_PEAK_THRESHOLD 400
#define RT_LOCAL_VAD_WARMUP_FRAMES 10  // ES8311 切入双工后的前 1s 仅预热，忽略 I2S 启动瞬态。
#define RT_LOCAL_VAD_START_FRAMES 2    // 连续 200ms 达到语音门槛才开始一轮，抑制瞬态噪声。
#define RT_LOCAL_VAD_SILENCE_FRAMES 20 // 服务端 VAD 未结束时，连续 2s 静音后兜底提交，避免短暂停顿被截断。
#define RT_LOCAL_VAD_MAX_FRAMES 150    // 单轮最长 15s，持续噪声下也能释放服务端输入缓冲。
#define RT_SERVER_SPEECH_GATE_MS 5000  // speech_started 超过 5s 未获本地语音确认则视为误触发。
#define RT_WS_OPCODE_CONTINUATION 0x0
#define RT_WS_OPCODE_TEXT 0x1

_Static_assert(RT_RX_INITIAL_BYTES < RT_RX_MAX_BYTES, "Realtime RX 初始容量必须小于最大容量");
_Static_assert(RT_AUDIO_DECODE_INITIAL_BYTES < RT_AUDIO_DECODE_MAX_BYTES,
               "Realtime 音频解码初始容量必须小于最大容量");
_Static_assert(RT_PLAY_RING_INITIAL_SAMPLES < RT_PLAY_RING_MAX_SAMPLES,
               "Realtime 播放环形区初始容量必须小于最大容量");

#if CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN < 16384
#error "GLM Realtime requires a 16KB mbedTLS RX buffer for the server handshake"
#endif

#ifndef CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
#error "GLM Realtime requires mbedTLS allocations in PSRAM on ESP32-P4"
#endif

static const char *TAG = "glm_realtime";

typedef enum {
    RT_CMD_START = 1,
    RT_CMD_STOP,
    RT_CMD_CANCEL,
    RT_CMD_SEND_TEXT,
} rt_cmd_type_t;

typedef struct {
    rt_cmd_type_t type;
    char text[FRIDGE_GLM_RT_MAX_TEXT_LEN + 1];
} rt_cmd_t;

typedef struct {
    char *data;
    size_t len;
} rt_rx_msg_t;

typedef struct {
    uint32_t session_generation;
    uint16_t samples;
    int16_t pcm[RT_AUDIO_FRAME_SAMPLES];
} rt_mic_message_t;

typedef struct {
    uint32_t session_generation;
    uint32_t response_generation;
    fridge_tool_call_t call;
} rt_tool_job_t;

typedef struct {
    uint32_t session_generation;
    uint32_t response_generation;
    fridge_tool_call_t call;
    fridge_tool_result_t result;
    esp_err_t err;
    uint32_t latency_ms;
} rt_tool_done_t;

static bool s_initialized;
static TaskHandle_t s_task;
static TaskHandle_t s_mic_task;
static TaskHandle_t s_play_task;
static TaskHandle_t s_tool_task;
static QueueHandle_t s_cmd_q;
static QueueHandle_t s_rx_q;
static QueueHandle_t s_tool_job_q;
static QueueHandle_t s_tool_done_q;
static MessageBufferHandle_t s_mic_msg;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_play_lock;
static fridge_glm_rt_config_update_t *s_config_cache;
static esp_websocket_client_handle_t s_ws;
static fridge_glm_rt_state_t s_state = FRIDGE_GLM_RT_STATE_IDLE;
static char s_last_error[FRIDGE_GLM_RT_MAX_ERROR_LEN + 1];
static fridge_glm_rt_event_cb_t s_event_cb;
static void *s_event_ctx;
static atomic_bool s_session_active;
static atomic_bool s_session_updated;
static bool s_session_update_sent;
static bool s_cancel_pending;
static bool s_cancel_waiting_ack;
static bool s_response_request_pending;
static atomic_bool s_audio_up_active;
static atomic_bool s_audio_down_active;
static bool s_wake_dialog_claimed;
static atomic_bool s_response_active;
static bool s_tool_used_this_response;
static bool s_response_has_content;
static bool s_local_speech_active;
static bool s_server_speech_active;
static int64_t s_server_speech_started_us;
static uint16_t s_local_warmup_frames;
static uint16_t s_local_voice_frames;
static uint16_t s_local_silence_frames;
static uint16_t s_local_utterance_frames;
static uint32_t s_tools_exported;
static uint32_t s_audio_up_bytes;
static uint32_t s_audio_down_bytes;
static uint32_t s_tool_ok;
static uint32_t s_tool_fail;
static atomic_uint s_session_generation;
static atomic_uint s_response_generation;
static uint32_t s_rx_drop_count;
static int64_t s_last_event_us;
static char s_active_response_id[64];
static char s_cancelled_response_id[64];
static char s_deferred_text[FRIDGE_GLM_RT_MAX_TEXT_LEN + 1];
static rt_mic_message_t *s_mic_frame;
static rt_mic_message_t *s_mic_capture;
static rt_tool_done_t *s_tool_done_work;
static rt_tool_done_t *s_tool_done_receive;
static int16_t *s_play_ring;
static size_t s_play_ring_cap;
static size_t s_play_w;
static size_t s_play_r;
static size_t s_play_count;
static char *s_rx_assemble;
static size_t s_rx_assemble_len;
static size_t s_rx_assemble_cap;
static char *s_b64_buf;
static size_t s_b64_cap;
static uint8_t *s_audio_decode;
static size_t s_audio_decode_cap;
static int16_t *s_audio_resample;
static size_t s_audio_resample_cap;
static int16_t s_resample_group[3];
static size_t s_resample_group_len;
static bool s_rx_message_active;
static size_t s_rx_frame_base;
// WebSocket 握手 Authorization 头必须在 client 生命周期内保持有效（库侧可能只保存指针）。
static char s_ws_headers[FRIDGE_GLM_RT_MAX_API_KEY_LEN + 48];

// ---- 基础工具 ----

static void *rt_psram_only_calloc(size_t n, size_t sz)
{
    if (n == 0 || sz == 0 || n > SIZE_MAX / sz) {
        return NULL;
    }
    size_t total = n * sz;
    return heap_caps_calloc(1, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *rt_psram_calloc(size_t n, size_t sz)
{
    if (n == 0 || sz == 0 || n > SIZE_MAX / sz) {
        return NULL;
    }
    void *p = rt_psram_only_calloc(n, sz);
    if (!p) {
        p = heap_caps_calloc(n, sz, MALLOC_CAP_8BIT);
    }
    return p;
}

// 动态扩容只允许进入 PSRAM，禁止大消息在压力下迁入内部 RAM 挤占 I2S DMA 和任务栈。
static void *rt_psram_realloc(void *ptr, size_t bytes)
{
    if (!ptr || bytes == 0) {
        return NULL;
    }
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// 以 2 倍增长减少大音频热路径的反复分配；到达最大值后精确夹紧。
static size_t growth_capacity(size_t current, size_t required, size_t maximum)
{
    if (required == 0 || required > maximum) {
        return 0;
    }
    size_t next = current > 0 ? current : 1;
    while (next < required) {
        if (next > maximum / 2) {
            next = maximum;
            break;
        }
        next *= 2;
    }
    return next >= required ? next : 0;
}

static bool ensure_rx_assemble_capacity(size_t required)
{
    if (required <= s_rx_assemble_cap) {
        return true;
    }
    size_t new_cap = growth_capacity(s_rx_assemble_cap, required, RT_RX_MAX_BYTES);
    if (new_cap == 0) {
        ESP_LOGW(TAG, "rx message exceeds hard limit: required=%u max=%u bytes",
                 (unsigned)required, (unsigned)RT_RX_MAX_BYTES);
        return false;
    }
    char *expanded = rt_psram_realloc(s_rx_assemble, new_cap);
    if (!expanded) {
        ESP_LOGW(TAG, "expand rx assembly PSRAM failed: required=%u target=%u bytes",
                 (unsigned)required, (unsigned)new_cap);
        return false;
    }
    ESP_LOGI(TAG, "rx assembly expanded in PSRAM: %u -> %u bytes",
             (unsigned)s_rx_assemble_cap, (unsigned)new_cap);
    s_rx_assemble = expanded;
    s_rx_assemble_cap = new_cap;
    return true;
}

static bool ensure_audio_work_capacity(size_t decode_required, size_t resample_required)
{
    if (decode_required > RT_AUDIO_DECODE_MAX_BYTES ||
        resample_required > RT_AUDIO_RESAMPLE_MAX_SAMPLES) {
        ESP_LOGW(TAG, "audio delta exceeds hard limit: decode=%u/%u resample=%u/%u",
                 (unsigned)decode_required, (unsigned)RT_AUDIO_DECODE_MAX_BYTES,
                 (unsigned)resample_required, (unsigned)RT_AUDIO_RESAMPLE_MAX_SAMPLES);
        return false;
    }

    if (decode_required > s_audio_decode_cap) {
        size_t new_cap = growth_capacity(s_audio_decode_cap, decode_required,
                                         RT_AUDIO_DECODE_MAX_BYTES);
        uint8_t *expanded = rt_psram_realloc(s_audio_decode, new_cap);
        if (!expanded) {
            ESP_LOGW(TAG, "expand audio decode PSRAM failed: required=%u target=%u bytes",
                     (unsigned)decode_required, (unsigned)new_cap);
            return false;
        }
        ESP_LOGI(TAG, "audio decode expanded in PSRAM: %u -> %u bytes",
                 (unsigned)s_audio_decode_cap, (unsigned)new_cap);
        s_audio_decode = expanded;
        s_audio_decode_cap = new_cap;
    }

    if (resample_required > s_audio_resample_cap) {
        size_t new_cap = growth_capacity(s_audio_resample_cap, resample_required,
                                         RT_AUDIO_RESAMPLE_MAX_SAMPLES);
        if (new_cap == 0 || new_cap > SIZE_MAX / sizeof(int16_t)) {
            return false;
        }
        int16_t *expanded = rt_psram_realloc(s_audio_resample,
                                             new_cap * sizeof(int16_t));
        if (!expanded) {
            ESP_LOGW(TAG, "expand audio resample PSRAM failed: required=%u target=%u samples",
                     (unsigned)resample_required, (unsigned)new_cap);
            return false;
        }
        ESP_LOGI(TAG, "audio resample expanded in PSRAM: %u -> %u samples",
                 (unsigned)s_audio_resample_cap, (unsigned)new_cap);
        s_audio_resample = expanded;
        s_audio_resample_cap = new_cap;
    }
    return true;
}

// API Key 会经过临时 PSRAM 配置对象；释放前使用 volatile 擦除，避免被编译器优化掉。
static void rt_secure_zero(void *data, size_t size)
{
    volatile uint8_t *ptr = (volatile uint8_t *)data;
    while (size-- > 0) {
        *ptr++ = 0;
    }
}

static void set_error(const char *msg)
{
    strlcpy(s_last_error, msg ? msg : "", sizeof(s_last_error));
    if (msg && msg[0]) {
        ESP_LOGW(TAG, "%s", msg);
    }
}

static int64_t now_ms(void)
{
    time_t t = time(NULL);
    if (t >= 1735689600LL) {
        return (int64_t)t * 1000;
    }
    return esp_timer_get_time() / 1000;
}

static uint32_t clamp_timeout(uint32_t ms)
{
    if (ms < 10000) {
        return 10000;
    }
    if (ms > 120000) {
        return 120000;
    }
    return ms;
}

const char *fridge_glm_rt_state_name(fridge_glm_rt_state_t state)
{
    switch (state) {
    case FRIDGE_GLM_RT_STATE_CONNECTING:
        return "connecting";
    case FRIDGE_GLM_RT_STATE_SESSION_READY:
        return "session_ready";
    case FRIDGE_GLM_RT_STATE_LISTENING:
        return "listening";
    case FRIDGE_GLM_RT_STATE_RESPONDING:
        return "responding";
    case FRIDGE_GLM_RT_STATE_TOOL_CALL:
        return "tool_call";
    case FRIDGE_GLM_RT_STATE_SPEAKING:
        return "speaking";
    case FRIDGE_GLM_RT_STATE_ERROR:
        return "error";
    case FRIDGE_GLM_RT_STATE_IDLE:
    default:
        return "idle";
    }
}

bool fridge_glm_realtime_is_active(void)
{
    return s_session_active;
}

static void make_key_preview(const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!key || !key[0]) {
        return;
    }
    size_t len = strlen(key);
    if (len <= 8) {
        strlcpy(out, "saved", out_size);
        return;
    }
    snprintf(out, out_size, "%.*s...%s", (int)(strncmp(key, "sk-", 3) == 0 ? 3 : 4), key, key + len - 4);
}

static void emit_event(const fridge_glm_rt_event_t *ev)
{
    if (!ev) {
        return;
    }
    fridge_glm_rt_event_cb_t cb = NULL;
    void *ctx = NULL;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        cb = s_event_cb;
        ctx = s_event_ctx;
        xSemaphoreGive(s_lock);
    }
    if (cb) {
        cb(ev, ctx);
    }
}

static void set_state(fridge_glm_rt_state_t st, const char *reason)
{
    s_state = st;
    s_last_event_us = esp_timer_get_time();
    fridge_glm_rt_event_t ev = {0};
    ev.type = FRIDGE_GLM_RT_EVENT_STATE;
    strlcpy(ev.name, "realtime_state", sizeof(ev.name));
    strlcpy(ev.state, fridge_glm_rt_state_name(st), sizeof(ev.state));
    if (reason) {
        strlcpy(ev.summary, reason, sizeof(ev.summary));
    }
    emit_event(&ev);
}

static void emit_error(const char *code, const char *message)
{
    set_error(message);
    fridge_glm_rt_event_t ev = {0};
    ev.type = FRIDGE_GLM_RT_EVENT_ERROR;
    strlcpy(ev.name, "realtime_error", sizeof(ev.name));
    strlcpy(ev.code, code ? code : "ERROR", sizeof(ev.code));
    strlcpy(ev.summary, message ? message : "error", sizeof(ev.summary));
    strlcpy(ev.text, message ? message : "", sizeof(ev.text));
    emit_event(&ev);
}

// ---- NVS 配置 ----

static void read_nvs_str(nvs_handle_t h, const char *key, char *out, size_t out_size, const char *fallback)
{
    size_t len = out_size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback ? fallback : "", out_size);
    }
}

static esp_err_t load_config_internal(fridge_glm_rt_config_update_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->url, FRIDGE_GLM_RT_DEFAULT_URL, sizeof(cfg->url));
    strlcpy(cfg->model, FRIDGE_GLM_RT_DEFAULT_MODEL, sizeof(cfg->model));
    strlcpy(cfg->voice, FRIDGE_GLM_RT_DEFAULT_VOICE, sizeof(cfg->voice));
    strlcpy(cfg->instructions, FRIDGE_GLM_RT_DEFAULT_INSTRUCTIONS, sizeof(cfg->instructions));
    strlcpy(cfg->vad_mode, "server_vad", sizeof(cfg->vad_mode));
    cfg->timeout_ms = FRIDGE_GLM_RT_DEFAULT_TIMEOUT_MS;
    cfg->enabled = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(RT_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    read_nvs_str(h, RT_NVS_URL, cfg->url, sizeof(cfg->url), FRIDGE_GLM_RT_DEFAULT_URL);
    read_nvs_str(h, RT_NVS_MODEL, cfg->model, sizeof(cfg->model), FRIDGE_GLM_RT_DEFAULT_MODEL);
    read_nvs_str(h, RT_NVS_VOICE, cfg->voice, sizeof(cfg->voice), FRIDGE_GLM_RT_DEFAULT_VOICE);
    // 长系统指令优先 blob，兼容旧 str。
    {
        size_t blen = sizeof(cfg->instructions);
        if (nvs_get_blob(h, RT_NVS_INSTR, cfg->instructions, &blen) != ESP_OK) {
            read_nvs_str(h, RT_NVS_INSTR, cfg->instructions, sizeof(cfg->instructions),
                         FRIDGE_GLM_RT_DEFAULT_INSTRUCTIONS);
        } else if (blen < sizeof(cfg->instructions)) {
            cfg->instructions[blen] = '\0';
        } else {
            cfg->instructions[sizeof(cfg->instructions) - 1] = '\0';
        }
        if (!cfg->instructions[0]) {
            strlcpy(cfg->instructions, FRIDGE_GLM_RT_DEFAULT_INSTRUCTIONS, sizeof(cfg->instructions));
        }
    }
    read_nvs_str(h, RT_NVS_VAD, cfg->vad_mode, sizeof(cfg->vad_mode), "server_vad");
    // MVP 固定 server_vad，忽略历史 client_vad 值。
    strlcpy(cfg->vad_mode, "server_vad", sizeof(cfg->vad_mode));
    size_t key_len = sizeof(cfg->api_key);
    if (nvs_get_str(h, RT_NVS_KEY, cfg->api_key, &key_len) == ESP_OK && cfg->api_key[0]) {
        cfg->update_api_key = true;
    }
    uint32_t timeout = FRIDGE_GLM_RT_DEFAULT_TIMEOUT_MS;
    if (nvs_get_u32(h, RT_NVS_TIMEOUT, &timeout) == ESP_OK) {
        cfg->timeout_ms = clamp_timeout(timeout);
    }
    uint8_t enabled = 1;
    if (nvs_get_u8(h, RT_NVS_ENABLED, &enabled) == ESP_OK) {
        cfg->enabled = enabled != 0;
    }
    int64_t updated = 0;
    if (nvs_get_i64(h, RT_NVS_UPDATED, &updated) == ESP_OK) {
        cfg->config_updated_at_ms = updated;
    }
    nvs_close(h);
    return ESP_OK;
}

// Realtime 主任务使用 PSRAM 栈，运行期间禁止直接访问 NVS。配置只在 init/set/clear 的
// 内部栈调用路径读写 NVS，网络任务从受锁保护的 PSRAM 缓存复制快照。
static esp_err_t rt_config_alloc_load(fridge_glm_rt_config_update_t **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = rt_psram_calloc(1, sizeof(**out));
    if (!*out) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_config_cache || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        rt_secure_zero(*out, sizeof(**out));
        heap_caps_free(*out);
        *out = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    **out = *s_config_cache;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void rt_config_free(fridge_glm_rt_config_update_t *config)
{
    if (!config) {
        return;
    }
    rt_secure_zero(config, sizeof(*config));
    heap_caps_free(config);
}

esp_err_t fridge_glm_realtime_get_config(fridge_glm_rt_config_view_t *out)
{
    ESP_RETURN_ON_FALSE(out && s_initialized, ESP_ERR_INVALID_STATE, TAG, "not ready");
    // 结构体含 2KB instructions 和 API Key；放 PSRAM，避免调用方任务栈与 NVS 调用叠加。
    fridge_glm_rt_config_update_t *cfg = NULL;
    esp_err_t err = rt_config_alloc_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load config failed: %s", esp_err_to_name(err));
        return err;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->url, cfg->url, sizeof(out->url));
    strlcpy(out->model, cfg->model, sizeof(out->model));
    strlcpy(out->voice, cfg->voice, sizeof(out->voice));
    strlcpy(out->instructions, cfg->instructions, sizeof(out->instructions));
    strlcpy(out->vad_mode, "server_vad", sizeof(out->vad_mode));
    out->timeout_ms = cfg->timeout_ms;
    out->enabled = cfg->enabled;
    out->has_api_key = cfg->update_api_key && cfg->api_key[0] != '\0';
    out->ready = out->has_api_key && out->url[0] && out->model[0];
    make_key_preview(cfg->api_key, out->api_key_preview, sizeof(out->api_key_preview));
    rt_config_free(cfg);
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    return ESP_OK;
}

esp_err_t fridge_glm_realtime_set_config(const fridge_glm_rt_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config && s_initialized, ESP_ERR_INVALID_ARG, TAG, "bad config");
    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }

    fridge_glm_rt_config_update_t *cur = NULL;
    esp_err_t err = rt_config_alloc_load(&cur);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load realtime config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (config->url[0]) {
        strlcpy(cur->url, config->url, sizeof(cur->url));
    }
    if (config->model[0]) {
        strlcpy(cur->model, config->model, sizeof(cur->model));
    }
    if (config->voice[0]) {
        strlcpy(cur->voice, config->voice, sizeof(cur->voice));
    }
    if (config->instructions[0]) {
        strlcpy(cur->instructions, config->instructions, sizeof(cur->instructions));
    }
    if (config->timeout_ms) {
        cur->timeout_ms = clamp_timeout(config->timeout_ms);
    }
    if (config->update_enabled) {
        cur->enabled = config->enabled;
    }
    if (config->update_api_key && config->api_key[0]) {
        strlcpy(cur->api_key, config->api_key, sizeof(cur->api_key));
        cur->update_api_key = true;
    }
    strlcpy(cur->vad_mode, "server_vad", sizeof(cur->vad_mode));
    cur->config_updated_at_ms = now_ms();

    nvs_handle_t h;
    err = nvs_open(RT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        rt_config_free(cur);
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, RT_NVS_URL, cur->url);
    if (err == ESP_OK) {
        err = nvs_set_str(h, RT_NVS_MODEL, cur->model);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, RT_NVS_VOICE, cur->voice);
    }
    if (err == ESP_OK) {
        // 指令可能较长：用 blob，避免 nvs_set_str 单页限制。
        err = nvs_set_blob(h, RT_NVS_INSTR, cur->instructions, strlen(cur->instructions) + 1);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, RT_NVS_VAD, "server_vad");
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, RT_NVS_TIMEOUT, cur->timeout_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, RT_NVS_ENABLED, cur->enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_i64(h, RT_NVS_UPDATED, cur->config_updated_at_ms);
    }
    if (err == ESP_OK && config->update_api_key && config->api_key[0]) {
        err = nvs_set_str(h, RT_NVS_KEY, config->api_key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        set_error("save realtime config failed");
    } else if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        *s_config_cache = *cur;
        xSemaphoreGive(s_lock);
    }
    rt_config_free(cur);
    return err;
}

esp_err_t fridge_glm_realtime_clear_key(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not ready");
    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(RT_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, RT_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_set_i64(h, RT_NVS_UPDATED, now_ms());
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        rt_secure_zero(s_config_cache->api_key, sizeof(s_config_cache->api_key));
        s_config_cache->update_api_key = false;
        s_config_cache->config_updated_at_ms = now_ms();
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t fridge_glm_realtime_get_status(fridge_glm_rt_status_t *out)
{
    ESP_RETURN_ON_FALSE(out && s_initialized, ESP_ERR_INVALID_STATE, TAG, "not ready");
    memset(out, 0, sizeof(*out));
    fridge_network_status_t net = {0};
    (void)fridge_network_get_status(&net);
    out->ready = s_initialized;
    out->session_active = s_session_active;
    out->network_ok = net.connected;
    out->internet_ready = net.internet_ready;
    out->audio_up_active = s_audio_up_active;
    out->audio_down_active = s_audio_down_active;
    out->tools_ready = fridge_tool_registry_count() > 0;
    out->state = s_state;
    strlcpy(out->state_name, fridge_glm_rt_state_name(s_state), sizeof(out->state_name));
    fridge_glm_rt_config_update_t *cfg = NULL;
    if (rt_config_alloc_load(&cfg) == ESP_OK) {
        strlcpy(out->model, cfg->model, sizeof(out->model));
        strlcpy(out->voice, cfg->voice, sizeof(out->voice));
    }
    rt_config_free(cfg);
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    out->tools_exported = s_tools_exported;
    out->audio_up_bytes = s_audio_up_bytes;
    out->audio_down_bytes = s_audio_down_bytes;
    out->tool_calls_ok = s_tool_ok;
    out->tool_calls_fail = s_tool_fail;
    if (s_last_event_us > 0) {
        out->last_event_age_ms = (uint32_t)((esp_timer_get_time() - s_last_event_us) / 1000);
    }
    return ESP_OK;
}

esp_err_t fridge_glm_realtime_set_event_callback(fridge_glm_rt_event_cb_t cb, void *user_ctx)
{
    // USB 可能在组件 init 之前启动：允许先登记回调，init 后立即生效。
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_event_cb = cb;
        s_event_ctx = user_ctx;
        xSemaphoreGive(s_lock);
    } else {
        s_event_cb = cb;
        s_event_ctx = user_ctx;
    }
    return ESP_OK;
}

// ---- Base64 ----

static char *b64_encode_pcm(const uint8_t *data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || len == 0) {
        return NULL;
    }
    size_t out_len = ((len + 2) / 3) * 4;
    if (out_len + 1 > s_b64_cap || !s_b64_buf) {
        return NULL;
    }
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        s_b64_buf[j++] = table[(triple >> 18) & 0x3F];
        s_b64_buf[j++] = table[(triple >> 12) & 0x3F];
        s_b64_buf[j++] = table[(triple >> 6) & 0x3F];
        s_b64_buf[j++] = table[triple & 0x3F];
    }
    if ((len % 3) == 1) {
        s_b64_buf[out_len - 1] = '=';
        s_b64_buf[out_len - 2] = '=';
    } else if ((len % 3) == 2) {
        s_b64_buf[out_len - 1] = '=';
    }
    s_b64_buf[out_len] = '\0';
    return s_b64_buf;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode_to(const char *src, uint8_t *out, size_t out_cap)
{
    if (!src || !out) {
        return 0;
    }
    size_t len = strlen(src);
    size_t written = 0;
    for (size_t i = 0; i + 3 < len; i += 4) {
        int a = b64_val(src[i]);
        int b = b64_val(src[i + 1]);
        int c = src[i + 2] == '=' ? 0 : b64_val(src[i + 2]);
        int d = src[i + 3] == '=' ? 0 : b64_val(src[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            break;
        }
        uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        size_t bytes = src[i + 2] == '=' ? 1 : (src[i + 3] == '=' ? 2 : 3);
        if (written + bytes > out_cap) {
            break;
        }
        out[written++] = (uint8_t)(triple >> 16);
        if (bytes > 1) {
            out[written++] = (uint8_t)(triple >> 8);
        }
        if (bytes > 2) {
            out[written++] = (uint8_t)triple;
        }
    }
    return written;
}

// 24k mono s16 -> 16k mono s16。保留跨 delta 的三采样相位，避免每段丢尾样本。
static size_t resample_24k_to_16k(const int16_t *in, size_t in_samples, int16_t *out, size_t out_cap)
{
    size_t out_n = 0;
    size_t i = 0;
    while (i < in_samples || s_resample_group_len == 3) {
        if (s_resample_group_len == 3) {
            if (out_n + 2 > out_cap) {
                // 输出缓冲不足时保留完整三采样组，下一次调用先继续输出。
                break;
            }
            out[out_n++] = s_resample_group[0];
            out[out_n++] = (int16_t)(((int32_t)s_resample_group[1] +
                                      (int32_t)s_resample_group[2]) / 2);
            s_resample_group_len = 0;
        } else if (i < in_samples) {
            s_resample_group[s_resample_group_len++] = in[i++];
        }
    }
    return out_n;
}

// 播放任务只在持锁期间访问环形区，因此 owner 可先在锁外分配新 PSRAM，再短暂持锁搬运未播样本。
// 缓冲只扩容不缩容，避免每轮语音产生 PSRAM 碎片。
static bool play_ring_ensure_capacity(size_t incoming)
{
    if (!s_play_ring || !s_play_lock || incoming == 0) {
        return false;
    }

    if (xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    size_t current_cap = s_play_ring_cap;
    size_t current_count = s_play_count;
    bool fits = current_cap > 0 && current_count <= current_cap &&
                incoming <= current_cap - current_count;
    xSemaphoreGive(s_play_lock);
    if (fits) {
        return true;
    }
    if (current_count > SIZE_MAX - incoming) {
        return false;
    }

    size_t required = current_count + incoming;
    size_t new_cap = growth_capacity(current_cap, required, RT_PLAY_RING_MAX_SAMPLES);
    if (new_cap == 0 || new_cap > SIZE_MAX / sizeof(int16_t)) {
        ESP_LOGW(TAG, "play ring request exceeds limit: required=%u max=%u samples",
                 (unsigned)required, (unsigned)RT_PLAY_RING_MAX_SAMPLES);
        return false;
    }
    int16_t *new_ring = heap_caps_calloc(new_cap, sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_ring) {
        ESP_LOGW(TAG, "expand play ring PSRAM failed: required=%u target=%u samples",
                 (unsigned)required, (unsigned)new_cap);
        return false;
    }

    if (xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        heap_caps_free(new_ring);
        return false;
    }
    int16_t *old_ring = s_play_ring;
    size_t old_cap = s_play_ring_cap;
    for (size_t i = 0; i < s_play_count; ++i) {
        new_ring[i] = old_ring[(s_play_r + i) % old_cap];
    }
    s_play_ring = new_ring;
    s_play_ring_cap = new_cap;
    s_play_r = 0;
    s_play_w = s_play_count % new_cap;
    xSemaphoreGive(s_play_lock);
    heap_caps_free(old_ring);
    ESP_LOGI(TAG, "play ring expanded in PSRAM: %u -> %u samples",
             (unsigned)old_cap, (unsigned)new_cap);
    return true;
}

static bool play_ring_push(const int16_t *samples, size_t n)
{
    if (!s_play_ring || !s_play_lock || !samples || n == 0) {
        return false;
    }
    bool complete = play_ring_ensure_capacity(n);
    if (xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_rx_drop_count++;
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s_play_count >= s_play_ring_cap) {
            // 超过 40 秒硬上限或 PSRAM 扩容失败时保留最新音频，并明确向上层报告不完整。
            s_play_r = (s_play_r + 1) % s_play_ring_cap;
            s_play_count--;
            complete = false;
        }
        s_play_ring[s_play_w] = samples[i];
        s_play_w = (s_play_w + 1) % s_play_ring_cap;
        s_play_count++;
    }
    xSemaphoreGive(s_play_lock);
    return complete;
}

static size_t play_ring_pop(int16_t *out, size_t max_n)
{
    if (!s_play_lock || xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    size_t n = 0;
    while (n < max_n && s_play_count > 0) {
        out[n++] = s_play_ring[s_play_r];
        s_play_r = (s_play_r + 1) % s_play_ring_cap;
        s_play_count--;
    }
    xSemaphoreGive(s_play_lock);
    return n;
}

static bool play_ring_has_data(void)
{
    bool has_data = false;
    if (s_play_lock && xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        has_data = s_play_count > 0;
        xSemaphoreGive(s_play_lock);
    }
    return has_data;
}

static void play_ring_reset(void)
{
    if (s_play_lock && xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_play_w = s_play_r = s_play_count = 0;
        xSemaphoreGive(s_play_lock);
    }
    s_resample_group_len = 0;
}

// ---- WebSocket 发送 ----

static esp_err_t ws_send_json(const char *json)
{
    if (!s_ws || !json || !esp_websocket_client_is_connected(s_ws)) {
        return ESP_ERR_INVALID_STATE;
    }
    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_ws, json, len, pdMS_TO_TICKS(5000));
    return sent == len ? ESP_OK : ESP_FAIL;
}

// 智谱 Realtime 对工具 JSON Schema 的约束比本地 Broker 更严格：
// 每个参数必须有 description，对象必须有 properties，required 必须是数组。
// 只在导出副本上补齐兼容字段，不改变 Tool Registry 的本地参数校验语义。
static bool normalize_realtime_tool_schema(cJSON *schema, const char *property_name,
                                           bool root_object)
{
    if (!cJSON_IsObject(schema)) {
        return false;
    }

    if (property_name && property_name[0]) {
        cJSON *description = cJSON_GetObjectItemCaseSensitive(schema, "description");
        if (!cJSON_IsString(description) || !description->valuestring || !description->valuestring[0]) {
            cJSON_DeleteItemFromObjectCaseSensitive(schema, "description");
            char fallback[96];
            snprintf(fallback, sizeof(fallback), "参数 %s", property_name);
            if (!cJSON_AddStringToObject(schema, "description", fallback)) {
                return false;
            }
        }
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    const bool is_object = root_object ||
                           (cJSON_IsString(type) && strcmp(type->valuestring, "object") == 0) ||
                           cJSON_GetObjectItemCaseSensitive(schema, "properties") != NULL;
    if (is_object) {
        if (!cJSON_IsString(type)) {
            cJSON_DeleteItemFromObjectCaseSensitive(schema, "type");
            if (!cJSON_AddStringToObject(schema, "type", "object")) {
                return false;
            }
        }

        cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
        if (!cJSON_IsObject(properties)) {
            cJSON_DeleteItemFromObjectCaseSensitive(schema, "properties");
            properties = cJSON_AddObjectToObject(schema, "properties");
            if (!properties) {
                return false;
            }
        }
        // 智谱语音工具校验会把空对象归一化为 null；无参数工具无法用空 Schema 表达，
        // 因此本轮不导出这类工具，本地 Registry/Broker 仍完整保留。
        if (!properties->child) {
            return false;
        }

        cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
        if (!cJSON_IsArray(required)) {
            cJSON_DeleteItemFromObjectCaseSensitive(schema, "required");
            required = cJSON_AddArrayToObject(schema, "required");
            if (!required) {
                return false;
            }
        }

        // 空数组同样会在语音请求中变为 null。只对原本没有必填项的 Schema，
        // 将现有参数全部列为必填；工具实现已有范围校验，模型必须显式给出值。
        if (!required->child) {
            cJSON *required_property = NULL;
            cJSON_ArrayForEach(required_property, properties) {
                if (!required_property->string ||
                    !cJSON_AddItemToArray(required, cJSON_CreateString(required_property->string))) {
                    return false;
                }
            }
        }

        cJSON *property = NULL;
        cJSON_ArrayForEach(property, properties) {
            if (!normalize_realtime_tool_schema(property, property->string, false)) {
                return false;
            }
        }
    }

    type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "array") == 0) {
        cJSON *items = cJSON_GetObjectItemCaseSensitive(schema, "items");
        if (cJSON_IsObject(items) && !normalize_realtime_tool_schema(items, NULL, false)) {
            return false;
        }
    }
    return true;
}

static esp_err_t send_session_update(const fridge_glm_rt_config_update_t *cfg)
{
    // 导出 READ + WRITE；排除 DANGEROUS（如 inventory.delete）。
    fridge_tool_definition_t *defs = rt_psram_calloc(FRIDGE_TOOL_MAX_TOOLS, sizeof(*defs));
    if (!defs) {
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    esp_err_t err = fridge_tool_registry_list(defs, FRIDGE_TOOL_MAX_TOOLS, &count);
    if (err != ESP_OK) {
        heap_caps_free(defs);
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    cJSON *tools = cJSON_CreateArray();
    cJSON *turn = cJSON_CreateObject();
    cJSON *noise_reduction = cJSON_CreateObject();
    cJSON *beta = cJSON_CreateObject();
    if (!root || !session || !modalities || !tools || !turn || !noise_reduction || !beta) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(modalities);
        cJSON_Delete(tools);
        cJSON_Delete(turn);
        cJSON_Delete(noise_reduction);
        cJSON_Delete(beta);
        heap_caps_free(defs);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddItemToObject(root, "session", session);
    cJSON_AddStringToObject(session, "model", cfg->model);
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "modalities", modalities);
    cJSON_AddStringToObject(session, "instructions", cfg->instructions);
    cJSON_AddStringToObject(session, "voice", cfg->voice);
    cJSON_AddStringToObject(session, "input_audio_format", "pcm16");
    cJSON_AddStringToObject(session, "output_audio_format", "pcm");
    // 板载驻极体麦克风属于远场采集，显式启用服务端远场降噪。
    // 1.5s 尾静音可容纳自然停顿；服务端负责提交，本地 VAD 仅在其失效时兜底。
    cJSON_AddStringToObject(noise_reduction, "type", "far_field");
    cJSON_AddItemToObject(session, "input_audio_noise_reduction", noise_reduction);
    cJSON_AddStringToObject(turn, "type", "server_vad");
    cJSON_AddNumberToObject(turn, "threshold", 0.5);
    cJSON_AddNumberToObject(turn, "prefix_padding_ms", 300);
    cJSON_AddNumberToObject(turn, "silence_duration_ms", 1500);
    // 关闭服务端自动创建响应：收到 committed 后由本地显式 response.create；
    // 若服务端一直不结束，本地能量 VAD 会在 2s 尾静音后执行 commit + response.create。
    cJSON_AddBoolToObject(turn, "create_response", false);
    cJSON_AddBoolToObject(turn, "interrupt_response", true);
    cJSON_AddItemToObject(session, "turn_detection", turn);
    cJSON_AddStringToObject(beta, "chat_mode", "audio");
    cJSON_AddStringToObject(beta, "tts_source", "e2e");
    cJSON_AddItemToObject(session, "beta_fields", beta);

    s_tools_exported = 0;
    for (size_t i = 0; i < count && s_tools_exported < RT_MAX_TOOLS_EXPORT; i++) {
        if (defs[i].permission == FRIDGE_TOOL_PERMISSION_DANGEROUS) {
            continue;
        }
        cJSON *schema = cJSON_Parse(defs[i].input_schema[0] ? defs[i].input_schema : "{\"type\":\"object\"}");
        if (!schema) {
            ESP_LOGW(TAG, "skip tool %s: invalid schema", defs[i].name);
            continue;
        }
        if (!normalize_realtime_tool_schema(schema, NULL, true)) {
            ESP_LOGW(TAG, "skip tool %s: schema normalization failed", defs[i].name);
            cJSON_Delete(schema);
            continue;
        }
        cJSON *tool = cJSON_CreateObject();
        if (!tool) {
            cJSON_Delete(schema);
            continue;
        }
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddStringToObject(tool, "name", defs[i].name);
        cJSON_AddStringToObject(tool, "description", defs[i].description);
        cJSON_AddItemToObject(tool, "parameters", schema);
        cJSON_AddItemToArray(tools, tool);
        s_tools_exported++;
    }
    cJSON_AddItemToObject(session, "tools", tools);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    heap_caps_free(defs);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    err = ws_send_json(json);
    cJSON_free(json);
    ESP_LOGI(TAG, "session.update sent, tools=%u", (unsigned)s_tools_exported);
    return err;
}

static esp_err_t send_text_item(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    if (!root || !item || !content || !part) {
        cJSON_Delete(root);
        cJSON_Delete(item);
        cJSON_Delete(content);
        cJSON_Delete(part);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddStringToObject(item, "object", "realtime.item");
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    cJSON_AddStringToObject(part, "type", "input_text");
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToObject(item, "content", content);
    cJSON_AddItemToObject(root, "item", item);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ws_send_json(json);
    cJSON_free(json);
    if (err != ESP_OK) {
        return err;
    }
    err = ws_send_json("{\"type\":\"response.create\"}");
    if (err == ESP_OK) {
        s_response_request_pending = true;
    }
    return err;
}

static esp_err_t send_audio_append(const int16_t *pcm, size_t samples)
{
    char *b64 = b64_encode_pcm((const uint8_t *)pcm, samples * sizeof(int16_t));
    if (!b64) {
        return ESP_ERR_NO_MEM;
    }
    // 手工拼 JSON，避免再拷贝巨大 base64。
    size_t need = strlen(b64) + 96;
    char *msg = rt_psram_calloc(1, need);
    if (!msg) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(msg, need, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
    esp_err_t err = ws_send_json(msg);
    heap_caps_free(msg);
    if (err == ESP_OK) {
        s_audio_up_bytes += (uint32_t)(samples * sizeof(int16_t));
    }
    return err;
}

// 重置本地端点检测。这里只保存少量计数器，不额外占用内部 SRAM 或 PSRAM。
static void local_vad_reset(void)
{
    s_local_speech_active = false;
    s_local_voice_frames = 0;
    s_local_silence_frames = 0;
    s_local_utterance_frames = 0;
}

// 使用与麦克风硬件测试一致的 RMS/Peak 门槛判断 100ms PCM 帧是否包含有效语音。
// 用平方和比较代替 sqrt，避免在音频热路径增加浮点开销。
static bool local_vad_frame_is_voice(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) {
        return false;
    }
    uint64_t sum_squares = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t sample = pcm[i];
        int32_t abs_sample = sample < 0 ? -sample : sample;
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        sum_squares += (uint64_t)(sample * sample);
    }
    const uint64_t rms_squared_threshold =
        (uint64_t)RT_LOCAL_VAD_RMS_THRESHOLD * RT_LOCAL_VAD_RMS_THRESHOLD;
    return peak >= RT_LOCAL_VAD_PEAK_THRESHOLD &&
           sum_squares >= rms_squared_threshold * samples;
}

// 主动提交一轮音频并创建响应。真机和 PC 对照均验证：服务端自动 VAD 卡住时，
// input_audio_buffer.commit + response.create 可正常返回用户转写、文本和 24k PCM 音频。
static esp_err_t local_vad_commit_response(void)
{
    esp_err_t err = ws_send_json("{\"type\":\"input_audio_buffer.commit\"}");
    if (err != ESP_OK) {
        return err;
    }
    err = ws_send_json("{\"type\":\"response.create\"}");
    if (err == ESP_OK) {
        s_response_request_pending = true;
        set_state(FRIDGE_GLM_RT_STATE_LISTENING, "audio_committed");
    }
    return err;
}

// 本地能量 VAD 只决定服务端 VAD 失效时何时兜底提交，原始 PCM 仍持续上送给模型。
// 两帧起始 + 二十帧尾静音允许句中自然停顿，同时避免页面永久停留在录音状态。
static void local_vad_observe(const int16_t *pcm, size_t samples)
{
    if (s_local_warmup_frames < RT_LOCAL_VAD_WARMUP_FRAMES) {
        s_local_warmup_frames++;
        s_local_voice_frames = 0;
        return;
    }
    const bool voiced = local_vad_frame_is_voice(pcm, samples);
    if (!s_local_speech_active) {
        if (s_server_speech_active && s_server_speech_started_us > 0 &&
            (esp_timer_get_time() - s_server_speech_started_us) / 1000 > RT_SERVER_SPEECH_GATE_MS) {
            s_server_speech_active = false;
            s_server_speech_started_us = 0;
        }
        if (voiced) {
            if (s_local_voice_frames < RT_LOCAL_VAD_START_FRAMES) {
                s_local_voice_frames++;
            }
        } else {
            s_local_voice_frames = 0;
        }
        if (!s_server_speech_active || s_local_voice_frames < RT_LOCAL_VAD_START_FRAMES) {
            return;
        }
        s_local_speech_active = true;
        s_local_silence_frames = 0;
        s_local_utterance_frames = s_local_voice_frames;
        ESP_LOGI(TAG, "local VAD speech started");
        set_state(FRIDGE_GLM_RT_STATE_LISTENING, "speech_started");
        return;
    }

    s_local_utterance_frames++;
    s_local_silence_frames = voiced ? 0 : (uint16_t)(s_local_silence_frames + 1);
    if (s_local_silence_frames < RT_LOCAL_VAD_SILENCE_FRAMES &&
        s_local_utterance_frames < RT_LOCAL_VAD_MAX_FRAMES) {
        return;
    }

    ESP_LOGI(TAG, "local VAD commit: frames=%u silence=%u",
             (unsigned)s_local_utterance_frames, (unsigned)s_local_silence_frames);
    esp_err_t err = local_vad_commit_response();
    local_vad_reset();
    s_server_speech_active = false;
    s_server_speech_started_us = 0;
    if (err != ESP_OK) {
        emit_error("AUDIO_COMMIT", esp_err_to_name(err));
    }
}

static esp_err_t send_function_output(const char *output_json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    if (!root || !item) {
        cJSON_Delete(root);
        cJSON_Delete(item);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddStringToObject(item, "object", "realtime.item");
    cJSON_AddStringToObject(item, "type", "function_call_output");
    // 智谱当前协议只接受 object/type/output；本地 call_id 仅用于追踪，不上送未知字段。
    cJSON_AddStringToObject(item, "output", output_json ? output_json : "{}");
    cJSON_AddItemToObject(root, "item", item);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ws_send_json(json);
    cJSON_free(json);
    if (err != ESP_OK) {
        return err;
    }
    err = ws_send_json("{\"type\":\"response.create\"}");
    if (err == ESP_OK) {
        s_response_request_pending = true;
    }
    return err;
}

// ---- 工具执行 ----

static uint32_t stable_text_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash;
}

static void tool_send_failed(const char *context, esp_err_t err)
{
    char detail[96];
    snprintf(detail, sizeof(detail), "%s: %s", context, esp_err_to_name(err));
    emit_error("TOOL_OUTPUT_SEND", detail);
    set_state(FRIDGE_GLM_RT_STATE_ERROR, "tool_output_send");
}

static esp_err_t send_compact_tool_output(const char *status, const char *code, const char *summary)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "status", status ? status : "error");
    cJSON_AddStringToObject(root, "code", code ? code : "ERROR");
    cJSON_AddStringToObject(root, "summary", summary ? summary : "工具执行失败");
    cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = send_function_output(json);
    cJSON_free(json);
    return err;
}

static void handle_function_call(const char *name, const char *arguments, const char *response_id, int output_index)
{
    if (!name || !name[0]) {
        return;
    }
    if ((response_id && s_cancelled_response_id[0] &&
         strcmp(response_id, s_cancelled_response_id) == 0) ||
        (response_id && s_active_response_id[0] &&
         strcmp(response_id, s_active_response_id) != 0)) {
        ESP_LOGW(TAG, "discard stale/cancelled tool call response=%s tool=%s", response_id, name);
        return;
    }
#if RT_TOOL_ONE_PER_RESPONSE
    if (s_tool_used_this_response) {
        ESP_LOGW(TAG, "ignore extra tool call in same response: %s", name);
        fridge_glm_rt_event_t ev = {0};
        ev.type = FRIDGE_GLM_RT_EVENT_TOOL_RESULT;
        strlcpy(ev.name, "realtime_tool_result", sizeof(ev.name));
        strlcpy(ev.tool_name, name, sizeof(ev.tool_name));
        strlcpy(ev.status, "skipped", sizeof(ev.status));
        strlcpy(ev.summary, "MVP 限制每轮仅一个工具调用", sizeof(ev.summary));
        emit_event(&ev);
        esp_err_t send_err = send_compact_tool_output(
            "error", "MULTI_TOOL_LIMIT", "本轮已执行过工具，请先用已有结果继续回答");
        if (send_err != ESP_OK) {
            tool_send_failed("multi-tool result", send_err);
        }
        return;
    }
#endif

    // DANGEROUS 不导出；若仍被调用则拒绝。
    fridge_tool_definition_t def = {0};
    if (fridge_tool_registry_find(name, &def) == ESP_OK &&
        def.permission == FRIDGE_TOOL_PERMISSION_DANGEROUS) {
        s_tool_fail++;
        esp_err_t send_err = send_compact_tool_output(
            "denied", "DANGEROUS_BLOCKED", "危险工具未导出且不可通过 Realtime 执行");
        if (send_err != ESP_OK) {
            tool_send_failed("dangerous-tool result", send_err);
        }
        return;
    }

    size_t arguments_len = arguments ? strlen(arguments) : 0;
    if (arguments_len > FRIDGE_TOOL_MAX_ARGS_LEN) {
        s_tool_fail++;
        esp_err_t send_err = send_compact_tool_output(
            "error", "ARG_TOO_LARGE", "工具参数超过设备端上限，已拒绝执行");
        if (send_err != ESP_OK) {
            tool_send_failed("oversized-args result", send_err);
        }
        return;
    }

    set_state(FRIDGE_GLM_RT_STATE_TOOL_CALL, name);
    s_tool_used_this_response = true;

    rt_tool_job_t *job = rt_psram_calloc(1, sizeof(*job));
    if (!job) {
        s_tool_fail++;
        emit_error("TOOL_QUEUE", "allocate tool job failed");
        esp_err_t send_err = send_compact_tool_output(
            "error", "NO_MEMORY", "设备内存不足，工具未执行");
        if (send_err != ESP_OK) {
            tool_send_failed("tool allocation result", send_err);
        } else {
            set_state(FRIDGE_GLM_RT_STATE_RESPONDING, "tool_allocation_failed");
        }
        return;
    }
    fridge_tool_call_t *call = &job->call;
    uint32_t response_hash = stable_text_hash(response_id);
    snprintf(call->call_id, sizeof(call->call_id), "rt_%08lx_%d",
             (unsigned long)response_hash, output_index);
    strlcpy(call->run_id, "glm_rt", sizeof(call->run_id));
    strlcpy(call->tool_name, name, sizeof(call->tool_name));
    strlcpy(call->arguments_json, arguments && arguments[0] ? arguments : "{}",
            sizeof(call->arguments_json));
    // 幂等键：防 Realtime 重试导致重复写入。
    snprintf(call->idempotency_key, sizeof(call->idempotency_key), "rt_%08lx_%d",
             (unsigned long)response_hash, output_index);
    // Realtime 只导出 READ/WRITE；WRITE 的自动执行授权只作用于本次调用，不修改全局 Broker 模式。
    call->user_confirmed = (def.permission == FRIDGE_TOOL_PERMISSION_WRITE);
    job->session_generation = s_session_generation;
    job->response_generation = s_response_generation;

    fridge_glm_rt_event_t call_ev = {0};
    call_ev.type = FRIDGE_GLM_RT_EVENT_TOOL_CALL;
    strlcpy(call_ev.name, "realtime_tool_call", sizeof(call_ev.name));
    strlcpy(call_ev.tool_name, name, sizeof(call_ev.tool_name));
    strlcpy(call_ev.call_id, call->call_id, sizeof(call_ev.call_id));
    strlcpy(call_ev.arguments_json, call->arguments_json, sizeof(call_ev.arguments_json));
    emit_event(&call_ev);

    if (xQueueSend(s_tool_job_q, &job, pdMS_TO_TICKS(20)) != pdTRUE) {
        heap_caps_free(job);
        s_tool_fail++;
        emit_error("TOOL_QUEUE", "tool worker queue full");
        esp_err_t send_err = send_compact_tool_output(
            "error", "TOOL_BUSY", "工具执行队列繁忙，本次未执行");
        if (send_err != ESP_OK) {
            tool_send_failed("tool queue result", send_err);
        } else {
            set_state(FRIDGE_GLM_RT_STATE_RESPONDING, "tool_queue_busy");
        }
    }
}

static void handle_tool_done(rt_tool_done_t *done)
{
    if (!done) {
        return;
    }
    if (!s_session_active || done->session_generation != s_session_generation ||
        done->response_generation != s_response_generation) {
        ESP_LOGW(TAG, "discard stale tool result: %s", done->call.tool_name);
        return;
    }

    fridge_tool_result_t *result = &done->result;

    if (result->status == FRIDGE_TOOL_RESULT_OK) {
        s_tool_ok++;
    } else {
        s_tool_fail++;
    }

    fridge_glm_rt_event_t res_ev = {0};
    res_ev.type = FRIDGE_GLM_RT_EVENT_TOOL_RESULT;
    strlcpy(res_ev.name, "realtime_tool_result", sizeof(res_ev.name));
    strlcpy(res_ev.tool_name, done->call.tool_name, sizeof(res_ev.tool_name));
    strlcpy(res_ev.call_id, done->call.call_id, sizeof(res_ev.call_id));
    strlcpy(res_ev.status, fridge_tool_result_status_name(result->status), sizeof(res_ev.status));
    strlcpy(res_ev.code, result->code, sizeof(res_ev.code));
    strlcpy(res_ev.summary, result->summary, sizeof(res_ev.summary));
    res_ev.latency_ms = done->latency_ms;
    emit_event(&res_ev);

    // 组装 output（限制长度）。
    cJSON *out = cJSON_CreateObject();
    if (out) {
        cJSON_AddStringToObject(out, "status", fridge_tool_result_status_name(result->status));
        cJSON_AddStringToObject(out, "code", result->code);
        cJSON_AddStringToObject(out, "summary", result->summary);
        cJSON *data = cJSON_Parse(result->data_json[0] ? result->data_json : "{}");
        if (data) {
            cJSON_AddItemToObject(out, "data", data);
        } else {
            cJSON_AddStringToObject(out, "data", result->data_json);
        }
        if (done->err != ESP_OK && result->status != FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION) {
            cJSON_AddStringToObject(out, "esp_err", esp_err_to_name(done->err));
        }
        char *payload = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);
        if (payload) {
            esp_err_t send_err;
            if (strlen(payload) > RT_OUTPUT_MAX_LEN) {
                // 超长：只保留 status/summary。
                cJSON_free(payload);
                send_err = send_compact_tool_output(fridge_tool_result_status_name(result->status),
                                                    result->code, result->summary);
            } else {
                send_err = send_function_output(payload);
                cJSON_free(payload);
            }
            if (send_err != ESP_OK) {
                tool_send_failed("tool result", send_err);
                return;
            }
        } else {
            tool_send_failed("serialize tool result", ESP_ERR_NO_MEM);
            return;
        }
    } else {
        tool_send_failed("allocate tool result", ESP_ERR_NO_MEM);
        return;
    }
    set_state(FRIDGE_GLM_RT_STATE_RESPONDING, "after_tool");
}

// ---- 入站事件解析 ----

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

// 从完成的 assistant item 中提取服务端最终文本。
// 部分模型/网关可能只在 output_item.done 或 response.done.output 中携带完整内容，
// 即使没有发送 delta 事件，也必须让 Web 对话预览拿到结果。
static bool emit_assistant_item_content(const cJSON *item)
{
    const char *role = cJSON_IsObject(item) ? json_str(item, "role") : NULL;
    if (!role || strcmp(role, "assistant") != 0) {
        return false;
    }
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
    if (!cJSON_IsArray(content)) {
        return false;
    }
    const cJSON *part = NULL;
    cJSON_ArrayForEach(part, content) {
        const char *transcript = json_str(part, "transcript");
        const char *text = json_str(part, "text");
        const char *final_text = transcript && transcript[0] ? transcript : text;
        if (!final_text || !final_text[0]) {
            continue;
        }
        fridge_glm_rt_event_t ev = {0};
        const bool audio_transcript = transcript && transcript[0];
        ev.type = audio_transcript ? FRIDGE_GLM_RT_EVENT_TRANSCRIPT : FRIDGE_GLM_RT_EVENT_TEXT_DELTA;
        strlcpy(ev.name, audio_transcript ? "realtime_transcript" : "realtime_text_done",
                sizeof(ev.name));
        strlcpy(ev.role, "assistant", sizeof(ev.role));
        strlcpy(ev.text, final_text, sizeof(ev.text));
        ev.is_final = true;
        emit_event(&ev);
        s_response_has_content = true;
        return true;
    }
    return false;
}

static void send_deferred_text_if_ready(void)
{
    if (s_cancel_pending || !s_deferred_text[0] || !s_session_active || !s_session_updated) {
        return;
    }
    char text[sizeof(s_deferred_text)];
    strlcpy(text, s_deferred_text, sizeof(text));
    s_deferred_text[0] = '\0';
    esp_err_t err = send_text_item(text);
    if (err != ESP_OK) {
        emit_error("SEND_TEXT", "failed to send deferred text");
    }
}

static void handle_server_json(const char *json_text)
{
    if (!json_text || !json_text[0]) {
        return;
    }
    s_last_event_us = esp_timer_get_time();
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        ESP_LOGW(TAG, "invalid server json");
        return;
    }
    const char *type = json_str(root, "type");
    if (!type) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "heartbeat") == 0) {
        // 仅刷新活跃时间。
    } else if (strcmp(type, "session.created") == 0) {
        if (!s_session_update_sent) {
            fridge_glm_rt_config_update_t *cfg = NULL;
            esp_err_t config_err = rt_config_alloc_load(&cfg);
            if (config_err == ESP_OK) {
                esp_err_t update_err = send_session_update(cfg);
                if (update_err == ESP_OK) {
                    s_session_update_sent = true;
                } else {
                    emit_error("SESSION_UPDATE", esp_err_to_name(update_err));
                }
            } else {
                emit_error("SESSION_CONFIG", esp_err_to_name(config_err));
            }
            rt_config_free(cfg);
        }
    } else if (strcmp(type, "session.updated") == 0) {
        s_session_updated = true;
        set_state(FRIDGE_GLM_RT_STATE_LISTENING, "session.updated");
        // 会话就绪后启动上行音频。
        if (!s_audio_up_active) {
            esp_err_t aerr = fridge_audio_realtime_stream_start();
            if (aerr == ESP_OK) {
                s_audio_up_active = true;
                fridge_glm_rt_event_t aev = {0};
                aev.type = FRIDGE_GLM_RT_EVENT_AUDIO_STATE;
                strlcpy(aev.name, "realtime_audio_state", sizeof(aev.name));
                strlcpy(aev.state, "up_active", sizeof(aev.state));
                emit_event(&aev);
            } else {
                emit_error("AUDIO_UP", "realtime mic stream start failed");
            }
        }
    } else if (strcmp(type, "error") == 0) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *msg = err ? json_str(err, "message") : json_str(root, "message");
        const char *code = err ? json_str(err, "code") : "server_error";
        emit_error(code ? code : "server_error", msg ? msg : "server error");
        set_state(FRIDGE_GLM_RT_STATE_ERROR, msg);
        if (s_cancel_pending) {
            s_cancel_pending = false;
            s_cancel_waiting_ack = false;
            s_response_request_pending = false;
            send_deferred_text_if_ready();
        }
    } else if (strcmp(type, "response.created") == 0) {
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
        const char *id = resp ? json_str(resp, "id") : json_str(root, "response_id");
        if (s_cancel_pending ||
            (id && s_cancelled_response_id[0] && strcmp(id, s_cancelled_response_id) == 0)) {
            ESP_LOGW(TAG, "discard response.created for cancelled response %s", id ? id : "unknown");
            if (id) {
                strlcpy(s_cancelled_response_id, id, sizeof(s_cancelled_response_id));
            }
            s_response_request_pending = false;
            s_cancel_waiting_ack = true;
            if (s_ws && esp_websocket_client_is_connected(s_ws)) {
                (void)ws_send_json("{\"type\":\"response.cancel\"}");
            }
        } else {
            s_response_request_pending = false;
            s_response_generation++;
            s_response_active = true;
            s_tool_used_this_response = false;
            s_response_has_content = false;
            strlcpy(s_active_response_id, id ? id : "", sizeof(s_active_response_id));
            s_cancelled_response_id[0] = '\0';
            set_state(FRIDGE_GLM_RT_STATE_RESPONDING, "response.created");
        }
    } else if (strcmp(type, "response.done") == 0 || strcmp(type, "response.cancelled") == 0) {
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
        const char *done_id = resp ? json_str(resp, "id") : json_str(root, "response_id");
        if (done_id && s_active_response_id[0] && strcmp(done_id, s_active_response_id) != 0) {
            ESP_LOGW(TAG, "discard done for stale response %s", done_id);
            cJSON_Delete(root);
            return;
        }
        // 官方说明 response.done 无论成功、取消或不完整都会发送；不能只看事件名判断成功。
        // 同时从 response.output 做最终内容兜底，兼容没有 delta 的服务端实现。
        const char *server_status = resp ? json_str(resp, "status") : NULL;
        const cJSON *output = resp ? cJSON_GetObjectItemCaseSensitive(resp, "output") : NULL;
        if (!s_response_has_content && cJSON_IsArray(output)) {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, output) {
                if (emit_assistant_item_content(item)) {
                    break;
                }
            }
        }
        const cJSON *usage = resp ? cJSON_GetObjectItemCaseSensitive(resp, "usage") : NULL;
        const cJSON *output_tokens_json = usage ? cJSON_GetObjectItemCaseSensitive(usage, "output_tokens") : NULL;
        int output_tokens = cJSON_IsNumber(output_tokens_json) ? output_tokens_json->valueint : -1;
        int output_items = cJSON_IsArray(output) ? cJSON_GetArraySize(output) : 0;
        ESP_LOGI(TAG, "response done: status=%s output_items=%d output_tokens=%d content=%s",
                 server_status ? server_status : "unknown", output_items, output_tokens,
                 s_response_has_content ? "yes" : "no");
        s_response_active = false;
        s_cancel_pending = false;
        s_cancel_waiting_ack = false;
        s_response_request_pending = false;
        fridge_glm_rt_event_t ev = {0};
        ev.type = FRIDGE_GLM_RT_EVENT_DONE;
        strlcpy(ev.name, "realtime_done", sizeof(ev.name));
        strlcpy(ev.status, server_status ? server_status : type, sizeof(ev.status));
        snprintf(ev.summary, sizeof(ev.summary), "output_items=%d output_tokens=%d content=%s",
                 output_items, output_tokens, s_response_has_content ? "yes" : "no");
        emit_event(&ev);
        if (!s_response_has_content && strcmp(type, "response.cancelled") != 0) {
            // 服务端可能以 completed 结束但不给任何文本或音频；明确上报，避免 Web 误判为仍在生成。
            emit_error("EMPTY_RESPONSE", "server completed without text or audio");
        }
        if (s_session_active) {
            set_state(FRIDGE_GLM_RT_STATE_LISTENING, type);
        }
        send_deferred_text_if_ready();
    } else if (strcmp(type, "response.text.delta") == 0 ||
               strcmp(type, "response.audio_transcript.delta") == 0) {
        const char *delta = json_str(root, "delta");
        if (delta && delta[0]) {
            fridge_glm_rt_event_t ev = {0};
            if (strcmp(type, "response.text.delta") == 0) {
                ev.type = FRIDGE_GLM_RT_EVENT_TEXT_DELTA;
                strlcpy(ev.name, "realtime_text_delta", sizeof(ev.name));
            } else {
                ev.type = FRIDGE_GLM_RT_EVENT_TRANSCRIPT;
                strlcpy(ev.name, "realtime_transcript", sizeof(ev.name));
                strlcpy(ev.role, "assistant", sizeof(ev.role));
            }
            strlcpy(ev.text, delta, sizeof(ev.text));
            emit_event(&ev);
            s_response_has_content = true;
        }
    } else if (strcmp(type, "response.text.done") == 0 ||
               strcmp(type, "response.audio_transcript.done") == 0) {
        // 智谱会在流式 delta 后发送完整文本；Web 对话预览以该事件定稿，
        // 可避免丢失 delta 时只看到 response.done 而没有具体回复。
        const bool audio_transcript = strcmp(type, "response.audio_transcript.done") == 0;
        const char *final_text = json_str(root, audio_transcript ? "transcript" : "text");
        if (final_text && final_text[0]) {
            fridge_glm_rt_event_t ev = {0};
            ev.type = audio_transcript ? FRIDGE_GLM_RT_EVENT_TRANSCRIPT : FRIDGE_GLM_RT_EVENT_TEXT_DELTA;
            strlcpy(ev.name, audio_transcript ? "realtime_transcript" : "realtime_text_done",
                    sizeof(ev.name));
            strlcpy(ev.role, "assistant", sizeof(ev.role));
            strlcpy(ev.text, final_text, sizeof(ev.text));
            ev.is_final = true;
            emit_event(&ev);
            s_response_has_content = true;
        }
    } else if (strcmp(type, "response.output_item.done") == 0) {
        (void)emit_assistant_item_content(cJSON_GetObjectItemCaseSensitive(root, "item"));
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char *tr = json_str(root, "transcript");
        if (tr) {
            fridge_glm_rt_event_t ev = {0};
            ev.type = FRIDGE_GLM_RT_EVENT_TRANSCRIPT;
            strlcpy(ev.name, "realtime_transcript", sizeof(ev.name));
            strlcpy(ev.role, "user", sizeof(ev.role));
            strlcpy(ev.text, tr, sizeof(ev.text));
            ev.is_final = true;
            emit_event(&ev);
        }
    } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
        // 输入转写失败不终止 Realtime 会话，但要让 Web 明确显示本轮无法预览用户原话。
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *code = err ? json_str(err, "code") : "ASR_FAILED";
        const char *message = err ? json_str(err, "message") : "input audio transcription failed";
        emit_error(code ? code : "ASR_FAILED",
                   message ? message : "input audio transcription failed");
    } else if (strcmp(type, "response.audio.delta") == 0) {
        const char *delta = json_str(root, "delta");
        if (delta && delta[0]) {
            // 热路径复用 PSRAM 缓冲，避免每个音频 delta 都分配/释放造成碎片。
            size_t encoded_len = strlen(delta);
            size_t decoded_upper = (encoded_len / 4) * 3;
            size_t samples_24k_upper = decoded_upper / sizeof(int16_t);
            // 加上跨 delta 最多遗留的 2 个输入样本，再向上取整为 3:2 重采样输出组。
            size_t resample_required = ((samples_24k_upper + 4) / 3) * 2;
            if ((encoded_len % 4) != 0) {
                s_rx_drop_count++;
                emit_error("AUDIO_DELTA_BASE64", "audio delta base64 length is invalid");
            } else if (!ensure_audio_work_capacity(decoded_upper, resample_required)) {
                s_rx_drop_count++;
                emit_error("AUDIO_DELTA_SIZE", "audio delta exceeds PSRAM work buffer limit");
            } else if (s_audio_decode && s_audio_resample) {
                size_t raw_len = b64_decode_to(delta, s_audio_decode, s_audio_decode_cap);
                size_t samples_24k = raw_len / sizeof(int16_t);
                if (samples_24k > 0) {
                    size_t n16 = resample_24k_to_16k((const int16_t *)s_audio_decode,
                                                     samples_24k, s_audio_resample,
                                                     s_audio_resample_cap);
                    if (!play_ring_push(s_audio_resample, n16)) {
                        s_rx_drop_count++;
                        emit_error("AUDIO_RING_CAPACITY", "audio playback ring could not retain full delta");
                    }
                    s_audio_down_bytes += (uint32_t)raw_len;
                    s_response_has_content = true;
                    bool first_audio_delta = !s_audio_down_active;
                    s_audio_down_active = true;
                    if (first_audio_delta) {
                        // 首包到达才切换状态，避免每个音频分片都刷一条串口事件。
                        ESP_LOGI(TAG, "first response audio received: %u bytes", (unsigned)raw_len);
                        set_state(FRIDGE_GLM_RT_STATE_SPEAKING, "audio.delta");
                    }
                }
            }
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        const char *name = json_str(root, "name");
        const char *args = json_str(root, "arguments");
        const char *resp_id = json_str(root, "response_id");
        if (!resp_id) {
            resp_id = s_active_response_id;
        }
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(root, "output_index");
        int output_index = cJSON_IsNumber(idx) ? idx->valueint : 0;
        handle_function_call(name, args, resp_id, output_index);
    } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
        // create_response=false：服务端事件只作辅助状态，本地 VAD 负责 commit + response.create。
        s_server_speech_active = false;
        s_server_speech_started_us = 0;
        set_state(FRIDGE_GLM_RT_STATE_LISTENING, "speech_stopped");
    } else if (strcmp(type, "input_audio_buffer.committed") == 0) {
        // Server VAD 会自动提交音频，但 create_response=false 时仍需客户端显式创建响应。
        // 本地兜底提交会提前置 pending 并复位 VAD，因此这里不会重复创建响应。
        set_state(FRIDGE_GLM_RT_STATE_LISTENING, "audio_committed");
        if (s_local_speech_active && !s_response_request_pending && !s_response_active) {
            esp_err_t err = ws_send_json("{\"type\":\"response.create\"}");
            if (err == ESP_OK) {
                s_response_request_pending = true;
                ESP_LOGI(TAG, "server VAD committed audio, response.create sent");
            } else {
                emit_error("RESPONSE_CREATE", esp_err_to_name(err));
            }
            local_vad_reset();
            s_server_speech_active = false;
            s_server_speech_started_us = 0;
        }
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        // 打断：清空本地下行播放。
        s_server_speech_active = true;
        s_server_speech_started_us = esp_timer_get_time();
        s_audio_down_active = false;
        play_ring_reset();
        (void)fridge_audio_realtime_playback_stop();
        set_state(FRIDGE_GLM_RT_STATE_LISTENING, "speech_started");
    }

    cJSON_Delete(root);
}

// ---- WebSocket 回调：只投递，不解析 ----

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "websocket connected");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "websocket disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!data || !data->data_ptr || data->data_len <= 0) {
            break;
        }
        // esp_websocket_client 会把一个 frame 按 buffer 分多次回调，也会用 continuation 表示跨 frame message。
        if (data->payload_offset == 0) {
            if (data->op_code == RT_WS_OPCODE_TEXT) {
                if (s_rx_message_active && s_rx_assemble_len > 0) {
                    ESP_LOGW(TAG, "new text frame before previous message finished, drop previous");
                    s_rx_drop_count++;
                }
                s_rx_assemble_len = 0;
                s_rx_frame_base = 0;
                s_rx_message_active = true;
            } else if (data->op_code == RT_WS_OPCODE_CONTINUATION) {
                if (!s_rx_message_active) {
                    ESP_LOGW(TAG, "orphan websocket continuation frame, drop");
                    s_rx_drop_count++;
                    break;
                }
                s_rx_frame_base = s_rx_assemble_len;
            } else {
                // binary/ping/pong/close 均不是 GLM JSON 事件，不参与消息重组。
                break;
            }
        }
        if (!s_rx_message_active || !s_rx_assemble || data->payload_len <= 0) {
            break;
        }
        size_t payload_offset = (size_t)data->payload_offset;
        size_t payload_len = (size_t)data->payload_len;
        size_t data_len = (size_t)data->data_len;
        if (s_rx_frame_base > SIZE_MAX - payload_offset ||
            s_rx_frame_base + payload_offset > SIZE_MAX - data_len ||
            s_rx_frame_base > SIZE_MAX - payload_len) {
            ESP_LOGW(TAG, "rx payload length overflow, drop");
            s_rx_assemble_len = 0;
            s_rx_message_active = false;
            s_rx_drop_count++;
            break;
        }
        size_t write_offset = s_rx_frame_base + payload_offset;
        size_t write_end = write_offset + data_len;
        size_t frame_end = s_rx_frame_base + payload_len;
        size_t required = (write_end > frame_end ? write_end : frame_end) + 1;
        if (required == 0 || !ensure_rx_assemble_capacity(required)) {
            ESP_LOGW(TAG, "rx payload capacity unavailable, drop: required=%u cap=%u",
                     (unsigned)required, (unsigned)s_rx_assemble_cap);
            s_rx_assemble_len = 0;
            s_rx_message_active = false;
            s_rx_drop_count++;
            break;
        }
        memcpy(s_rx_assemble + write_offset, data->data_ptr, (size_t)data->data_len);
        if (payload_offset + data_len >= payload_len) {
            s_rx_assemble_len = frame_end;
        }
        if (data->fin &&
            payload_offset + data_len >= payload_len) {
            s_rx_assemble[s_rx_assemble_len] = '\0';
            // 拷贝到队列消息（PSRAM）。
            rt_rx_msg_t msg = {0};
            msg.data = rt_psram_only_calloc(1, s_rx_assemble_len + 1);
            if (msg.data) {
                memcpy(msg.data, s_rx_assemble, s_rx_assemble_len + 1);
                msg.len = s_rx_assemble_len;
                if (xQueueSend(s_rx_q, &msg, 0) != pdTRUE) {
                    heap_caps_free(msg.data);
                    s_rx_drop_count++;
                    ESP_LOGW(TAG, "realtime rx queue full, drop message count=%lu",
                             (unsigned long)s_rx_drop_count);
                }
            } else {
                s_rx_drop_count++;
                ESP_LOGW(TAG, "allocate realtime rx message failed");
            }
            s_rx_assemble_len = 0;
            s_rx_frame_base = 0;
            s_rx_message_active = false;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket error event");
        break;
    default:
        break;
    }
}

static void cleanup_session_resources(void)
{
    // 先推进 generation，使正在结束的麦克风读取和工具结果即使稍后返回也会被判为过期。
    s_session_generation++;
    s_response_generation++;
    if (s_audio_up_active) {
        s_audio_up_active = false;
        (void)fridge_audio_realtime_stream_stop();
    }
    s_audio_down_active = false;
    play_ring_reset();
    (void)fridge_audio_realtime_playback_stop();
    if (s_mic_msg) {
        xMessageBufferReset(s_mic_msg);
    }

    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    memset(s_ws_headers, 0, sizeof(s_ws_headers));
    // 排空 RX 队列。
    if (s_rx_q) {
        rt_rx_msg_t msg;
        while (xQueueReceive(s_rx_q, &msg, 0) == pdTRUE) {
            heap_caps_free(msg.data);
        }
    }
    s_session_active = false;
    s_session_updated = false;
    s_session_update_sent = false;
    s_response_active = false;
    s_tool_used_this_response = false;
    s_cancel_pending = false;
    s_cancel_waiting_ack = false;
    s_response_request_pending = false;
    local_vad_reset();
    s_server_speech_active = false;
    s_server_speech_started_us = 0;
    s_local_warmup_frames = 0;
    s_rx_assemble_len = 0;
    s_rx_frame_base = 0;
    s_rx_message_active = false;
    s_active_response_id[0] = '\0';
    s_cancelled_response_id[0] = '\0';
    s_deferred_text[0] = '\0';

    if (s_wake_dialog_claimed) {
        fridge_wake_word_notify_dialog_finished();
        s_wake_dialog_claimed = false;
    }
}

static esp_err_t start_session_locked(void)
{
    if (s_session_active) {
        return ESP_ERR_INVALID_STATE;
    }

    // 配置包含长系统指令和 API Key，优先放 PSRAM，避免与 WebSocket/TLS 调用栈叠加。
    fridge_glm_rt_config_update_t *cfg = NULL;
    esp_err_t result = rt_config_alloc_load(&cfg);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "load session config failed: %s", esp_err_to_name(result));
        return result;
    }
    if (!cfg->update_api_key || !cfg->api_key[0]) {
        set_error("API Key missing");
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (!cfg->enabled) {
        set_error("realtime disabled in config");
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }

    fridge_network_status_t net = {0};
    if (fridge_network_get_status(&net) != ESP_OK || !net.connected) {
        set_error("Wi-Fi not connected");
        result = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (!net.internet_ready) {
        // 尽量校时；失败仍允许尝试（TLS 可能因时间失败）。
        (void)fridge_network_sync_time();
        (void)fridge_network_get_status(&net);
        if (!net.internet_ready) {
            set_error("internet/SNTP not ready");
            result = ESP_ERR_INVALID_STATE;
            goto done;
        }
    }

    // 声明对话占用并同步停止 WakeNet；不改用户 NVS 开关，避免会话结束后篡改偏好。
    fridge_wake_word_status_t wake = {0};
    if (fridge_wake_word_get_status(&wake) == ESP_OK) {
        fridge_wake_word_notify_dialog_started();
        s_wake_dialog_claimed = true;
        if (wake.enabled || wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING) {
            esp_err_t wake_err = fridge_wake_word_stop();
            if (wake_err != ESP_OK && wake_err != ESP_ERR_INVALID_STATE) {
                set_error("failed to stop WakeNet before realtime");
                cleanup_session_resources();
                result = wake_err;
                goto done;
            }
        }
    }

    set_state(FRIDGE_GLM_RT_STATE_CONNECTING, "start");

    // Authorization 通过 config.headers 注入；禁止写入日志；会话结束前不得清零该缓冲。
    snprintf(s_ws_headers, sizeof(s_ws_headers), "Authorization: Bearer %s\r\n", cfg->api_key);
    rt_secure_zero(cfg->api_key, sizeof(cfg->api_key));

    esp_websocket_client_config_t ws_cfg = {
        .uri = cfg->url,
        .disable_auto_reconnect = true,  // 阶段一：禁止 stop 后自动重连
        .buffer_size = 4096,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = (int)cfg->timeout_ms,
        .reconnect_timeout_ms = 10000,
        .headers = s_ws_headers,
    };

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) {
        set_error("websocket init failed");
        cleanup_session_resources();
        set_state(FRIDGE_GLM_RT_STATE_ERROR, "ws_init");
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        set_error("websocket start failed");
        cleanup_session_resources();
        set_state(FRIDGE_GLM_RT_STATE_ERROR, "ws_start");
        result = err;
        goto done;
    }

    // 等待连接。
    const int wait_ms = (int)cfg->timeout_ms;
    const int step = 100;
    int waited = 0;
    while (waited < wait_ms && !esp_websocket_client_is_connected(s_ws)) {
        rt_cmd_t pending;
        while (xQueueReceive(s_cmd_q, &pending, 0) == pdTRUE) {
            if (pending.type == RT_CMD_STOP || pending.type == RT_CMD_CANCEL) {
                set_error("realtime start cancelled");
                cleanup_session_resources();
                result = ESP_ERR_INVALID_STATE;
                goto done;
            }
            if (pending.type == RT_CMD_SEND_TEXT) {
                emit_error("SEND_TEXT", "session is still connecting");
            } else if (pending.type == RT_CMD_START) {
                emit_error("START", "realtime connection already in progress");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    if (!esp_websocket_client_is_connected(s_ws)) {
        set_error("websocket connect timeout");
        cleanup_session_resources();
        set_state(FRIDGE_GLM_RT_STATE_ERROR, "connect_timeout");
        result = ESP_ERR_TIMEOUT;
        goto done;
    }

    s_session_active = true;
    s_session_generation++;
    s_session_updated = false;
    s_session_update_sent = false;
    s_audio_up_bytes = 0;
    s_audio_down_bytes = 0;
    s_last_event_us = esp_timer_get_time();
    // session.update 只在官方 session.created 事件到达后发送一次，由主循环处理该入站事件。
    result = ESP_OK;

done:
    rt_config_free(cfg);
    return result;
}

static void stop_session_locked(void)
{
    if (s_response_active && s_ws && esp_websocket_client_is_connected(s_ws)) {
        (void)ws_send_json("{\"type\":\"response.cancel\"}");
    }
    cleanup_session_resources();
    set_state(FRIDGE_GLM_RT_STATE_IDLE, "stopped");
}

// ---- 主任务 ----

static bool drain_play_queue(void)
{
    int16_t chunk[320];  // 20ms @ 16k
    size_t n = play_ring_pop(chunk, sizeof(chunk) / sizeof(chunk[0]));
    if (n == 0) {
        return false;
    }
    esp_err_t err = fridge_audio_realtime_play_pcm(chunk, n);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "play pcm failed: %s", esp_err_to_name(err));
        emit_error("AUDIO_PLAY", esp_err_to_name(err));
    }
    return true;
}

static void uplink_mic_once(void)
{
    if (!s_audio_up_active || !s_mic_frame || !s_mic_msg || !s_session_updated) {
        return;
    }
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        return;
    }
    size_t bytes = xMessageBufferReceive(s_mic_msg, s_mic_frame, sizeof(*s_mic_frame), 0);
    if (bytes == 0) {
        return;
    }
    size_t header_bytes = offsetof(rt_mic_message_t, pcm);
    if (bytes < header_bytes || s_mic_frame->samples > RT_AUDIO_FRAME_SAMPLES ||
        bytes != header_bytes + s_mic_frame->samples * sizeof(int16_t)) {
        emit_error("MIC_QUEUE", "invalid microphone frame size");
        return;
    }
    if (s_mic_frame->session_generation != s_session_generation) {
        ESP_LOGW(TAG, "discard stale microphone frame generation=%lu current=%lu",
                 (unsigned long)s_mic_frame->session_generation,
                 (unsigned long)s_session_generation);
        return;
    }
    esp_err_t err = send_audio_append(s_mic_frame->pcm, s_mic_frame->samples);
    if (err != ESP_OK) {
        emit_error("MIC_SEND", esp_err_to_name(err));
    } else {
        local_vad_observe(s_mic_frame->pcm, s_mic_frame->samples);
    }
}

// 麦克风任务只负责可能阻塞约 100ms 的 I2S 采集；WebSocket 发送仍由 glm_rt 单一所有者完成。
static void glm_mic_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_audio_up_active || !s_session_updated || !s_mic_capture || !s_mic_msg) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t generation = s_session_generation;
        size_t samples = 0;
        esp_err_t err = fridge_audio_realtime_stream_read(s_mic_capture->pcm, RT_AUDIO_FRAME_SAMPLES,
                                                          &samples, pdMS_TO_TICKS(120));
        if (err == ESP_OK) {
            // stop 可能在阻塞读取返回后改变会话代次；此时 samples=0 或状态已失效都不是采集错误。
            if (samples > 0 && s_audio_up_active && s_session_updated &&
                generation == s_session_generation) {
                s_mic_capture->session_generation = generation;
                s_mic_capture->samples = (uint16_t)samples;
                size_t bytes = offsetof(rt_mic_message_t, pcm) + samples * sizeof(int16_t);
                if (xMessageBufferSend(s_mic_msg, s_mic_capture, bytes, 0) != bytes) {
                    // 上行拥塞时丢当前帧，不能反压 I2S 采集造成录音任务长期阻塞。
                    s_rx_drop_count++;
                }
            }
        } else if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "realtime mic read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// 播放任务独立消费 20ms PCM，避免上行采集或工具执行拖慢扬声器节拍。
// 注意：即使 played=true 也必须让出 CPU；连续 drain 不 delay 会饿死 IDLE 触发 task_wdt。
static void glm_play_task(void *arg)
{
    (void)arg;
    while (true) {
        bool busy = false;
        if (s_session_active && s_audio_down_active) {
            bool played = drain_play_queue();
            if (!played && !s_response_active && !play_ring_has_data()) {
                // response.done 后等本地环形缓冲播完再关 PA，既不截断尾音也不让功放长期保持使能。
                s_audio_down_active = false;
                (void)fridge_audio_realtime_playback_stop();
            }
            busy = played;
        }
        // 有音频时约 5ms 一轮（接近 20ms 帧的 1/4）；空闲时 20ms，保证 IDLE 喂狗。
        vTaskDelay(pdMS_TO_TICKS(busy ? 5 : 20));
    }
}

// Tool Broker 可能访问 TF/NVS 并长时间阻塞，必须与音频和 WebSocket owner 隔离。
static void glm_tool_task(void *arg)
{
    (void)arg;
    while (true) {
        rt_tool_job_t *job = NULL;
        if (xQueueReceive(s_tool_job_q, &job, portMAX_DELAY) != pdTRUE || !job) {
            continue;
        }
        if (job->session_generation != s_session_generation ||
            job->response_generation != s_response_generation) {
            ESP_LOGW(TAG, "discard cancelled tool job before execution: %s", job->call.tool_name);
            heap_caps_free(job);
            continue;
        }
        rt_tool_done_t *done = s_tool_done_work;
        memset(done, 0, sizeof(*done));
        done->session_generation = job->session_generation;
        done->response_generation = job->response_generation;
        done->call = job->call;
        int64_t t0 = esp_timer_get_time();
        done->err = fridge_tool_broker_call(&done->call, &done->result);
        done->latency_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        heap_caps_free(job);
        // 工具（尤其 WRITE）一旦执行，结果绝不能静默丢失；worker 可等待 owner 消费。
        (void)xQueueSend(s_tool_done_q, done, portMAX_DELAY);
    }
}

static void glm_rt_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "glm_rt task started");
    while (true) {
        // 1) 处理命令
        rt_cmd_t cmd;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case RT_CMD_START:
                if (start_session_locked() != ESP_OK) {
                    if (strcmp(s_last_error, "realtime start cancelled") == 0) {
                        set_state(FRIDGE_GLM_RT_STATE_IDLE, "start_cancelled");
                    } else {
                        set_state(FRIDGE_GLM_RT_STATE_ERROR, s_last_error);
                    }
                }
                break;
            case RT_CMD_STOP:
                stop_session_locked();
                break;
            case RT_CMD_CANCEL:
                if (s_ws && esp_websocket_client_is_connected(s_ws)) {
                    (void)ws_send_json("{\"type\":\"response.cancel\"}");
                }
                if (s_active_response_id[0]) {
                    strlcpy(s_cancelled_response_id, s_active_response_id,
                            sizeof(s_cancelled_response_id));
                }
                s_active_response_id[0] = '\0';
                s_response_generation++;
                bool may_have_response = s_response_active || s_response_request_pending ||
                                         uxQueueMessagesWaiting(s_rx_q) > 0;
                s_cancel_pending = may_have_response;
                s_cancel_waiting_ack = s_response_active;
                s_response_active = false;
                s_tool_used_this_response = false;
                s_audio_down_active = false;
                play_ring_reset();
                (void)fridge_audio_realtime_playback_stop();
                if (s_session_active) {
                    set_state(FRIDGE_GLM_RT_STATE_LISTENING, "cancelled");
                }
                break;
            case RT_CMD_SEND_TEXT:
                if (s_session_active && s_session_updated && cmd.text[0]) {
                    if (s_cancel_pending) {
                        // cancel 与服务端 ack 串行化；避免旧 response.created 被误认成新文本响应。
                        strlcpy(s_deferred_text, cmd.text, sizeof(s_deferred_text));
                    } else if (send_text_item(cmd.text) != ESP_OK) {
                        emit_error("SEND_TEXT", "failed to send text");
                    }
                } else {
                    emit_error("SEND_TEXT", "session not ready");
                }
                break;
            default:
                break;
            }
        }

        // 2) 处理入站消息
        rt_rx_msg_t msg;
        while (xQueueReceive(s_rx_q, &msg, 0) == pdTRUE) {
            if (msg.data) {
                handle_server_json(msg.data);
                heap_caps_free(msg.data);
            }
        }
        if (s_cancel_pending && !s_cancel_waiting_ack && !s_response_active &&
            !s_response_request_pending && uxQueueMessagesWaiting(s_rx_q) == 0) {
            // 队列里没有发现待创建响应：视为空闲 cancel，不阻塞后续文本。
            s_cancel_pending = false;
            send_deferred_text_if_ready();
        }

        // 3) 处理异步工具结果；只有 glm_rt 能发送 WebSocket 消息。
        while (xQueueReceive(s_tool_done_q, s_tool_done_receive, 0) == pdTRUE) {
            handle_tool_done(s_tool_done_receive);
        }

        // 4) 会话活跃：发送已采集的上行音频；下行由独立播放任务消费。
        if (s_session_active) {
            if (s_ws && !esp_websocket_client_is_connected(s_ws)) {
                emit_error("WS_DOWN", "websocket disconnected");
                stop_session_locked();
            } else {
                // heartbeat 超时
                if (s_last_event_us > 0) {
                    int64_t age_ms = (esp_timer_get_time() - s_last_event_us) / 1000;
                    if (age_ms > RT_HEARTBEAT_STALE_MS) {
                        emit_error("STALE", "no events / heartbeat timeout");
                        stop_session_locked();
                    }
                }
                uplink_mic_once();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---- 公共异步 API ----

static esp_err_t post_cmd(rt_cmd_type_t type, const char *text)
{
    ESP_RETURN_ON_FALSE(s_initialized && s_cmd_q, ESP_ERR_INVALID_STATE, TAG, "not ready");
    rt_cmd_t cmd = {.type = type};
    if (text) {
        strlcpy(cmd.text, text, sizeof(cmd.text));
    }
    if (xQueueSend(s_cmd_q, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t fridge_glm_realtime_start(void)
{
    return post_cmd(RT_CMD_START, NULL);
}

esp_err_t fridge_glm_realtime_stop(void)
{
    return post_cmd(RT_CMD_STOP, NULL);
}

esp_err_t fridge_glm_realtime_cancel(void)
{
    return post_cmd(RT_CMD_CANCEL, NULL);
}

esp_err_t fridge_glm_realtime_send_text(const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_cmd(RT_CMD_SEND_TEXT, text);
}

static void cleanup_init_resources(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_mic_task) {
        vTaskDelete(s_mic_task);
        s_mic_task = NULL;
    }
    if (s_play_task) {
        vTaskDelete(s_play_task);
        s_play_task = NULL;
    }
    if (s_tool_task) {
        vTaskDelete(s_tool_task);
        s_tool_task = NULL;
    }
    if (s_cmd_q) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
    }
    if (s_rx_q) {
        vQueueDelete(s_rx_q);
        s_rx_q = NULL;
    }
    if (s_tool_job_q) {
        vQueueDelete(s_tool_job_q);
        s_tool_job_q = NULL;
    }
    if (s_tool_done_q) {
        vQueueDelete(s_tool_done_q);
        s_tool_done_q = NULL;
    }
    if (s_mic_msg) {
        vMessageBufferDelete(s_mic_msg);
        s_mic_msg = NULL;
    }
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    if (s_play_lock) {
        vSemaphoreDelete(s_play_lock);
        s_play_lock = NULL;
    }
    if (s_config_cache) {
        rt_config_free(s_config_cache);
        s_config_cache = NULL;
    }
    heap_caps_free(s_mic_frame);
    heap_caps_free(s_mic_capture);
    heap_caps_free(s_play_ring);
    heap_caps_free(s_rx_assemble);
    heap_caps_free(s_b64_buf);
    heap_caps_free(s_audio_decode);
    heap_caps_free(s_audio_resample);
    heap_caps_free(s_tool_done_work);
    heap_caps_free(s_tool_done_receive);
    s_mic_frame = NULL;
    s_mic_capture = NULL;
    s_play_ring = NULL;
    s_play_ring_cap = 0;
    s_rx_assemble = NULL;
    s_rx_assemble_cap = 0;
    s_b64_buf = NULL;
    s_audio_decode = NULL;
    s_audio_decode_cap = 0;
    s_audio_resample = NULL;
    s_audio_resample_cap = 0;
    s_tool_done_work = NULL;
    s_tool_done_receive = NULL;
}

esp_err_t fridge_glm_realtime_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_play_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(RT_CMD_QUEUE_LEN, sizeof(rt_cmd_t));
    s_rx_q = xQueueCreate(RT_RX_QUEUE_LEN, sizeof(rt_rx_msg_t));
    s_tool_job_q = xQueueCreate(RT_TOOL_QUEUE_LEN, sizeof(rt_tool_job_t *));
    s_tool_done_q = xQueueCreate(RT_TOOL_QUEUE_LEN, sizeof(rt_tool_done_t));
    s_mic_msg = xMessageBufferCreate(RT_MIC_MESSAGE_BUFFER_BYTES);
    if (!s_lock || !s_play_lock || !s_cmd_q || !s_rx_q || !s_tool_job_q ||
        !s_tool_done_q || !s_mic_msg) {
        cleanup_init_resources();
        return ESP_ERR_NO_MEM;
    }
    s_config_cache = rt_psram_calloc(1, sizeof(*s_config_cache));
    if (!s_config_cache) {
        cleanup_init_resources();
        return ESP_ERR_NO_MEM;
    }
    // 初始化由 main 的内部 RAM 栈调用；只在这里读取一次 NVS，之后 glm_rt 不再碰 Flash。
    esp_err_t config_err = load_config_internal(s_config_cache);
    if (config_err != ESP_OK) {
        ESP_LOGE(TAG, "load realtime config cache failed: %s", esp_err_to_name(config_err));
        cleanup_init_resources();
        return config_err;
    }
    s_mic_frame = rt_psram_calloc(1, sizeof(*s_mic_frame));
    s_mic_capture = rt_psram_calloc(1, sizeof(*s_mic_capture));
    s_play_ring_cap = RT_PLAY_RING_INITIAL_SAMPLES;
    s_play_ring = rt_psram_only_calloc(s_play_ring_cap, sizeof(int16_t));
    s_rx_assemble_cap = RT_RX_INITIAL_BYTES;
    s_rx_assemble = rt_psram_only_calloc(1, s_rx_assemble_cap);
    s_b64_cap = ((RT_AUDIO_FRAME_SAMPLES * 2 + 2) / 3) * 4 + 8;
    s_b64_buf = rt_psram_calloc(1, s_b64_cap);
    s_audio_decode_cap = RT_AUDIO_DECODE_INITIAL_BYTES;
    s_audio_decode = rt_psram_only_calloc(1, s_audio_decode_cap);
    s_audio_resample_cap = RT_AUDIO_RESAMPLE_INITIAL_SAMPLES;
    s_audio_resample = rt_psram_only_calloc(s_audio_resample_cap, sizeof(int16_t));
    s_tool_done_work = rt_psram_calloc(1, sizeof(*s_tool_done_work));
    s_tool_done_receive = rt_psram_calloc(1, sizeof(*s_tool_done_receive));
    if (!s_mic_frame || !s_mic_capture || !s_play_ring || !s_rx_assemble || !s_b64_buf ||
        !s_audio_decode || !s_audio_resample || !s_tool_done_work || !s_tool_done_receive) {
        cleanup_init_resources();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok_main = xTaskCreate(glm_rt_task, "glm_rt", RT_TASK_STACK, NULL,
                                    RT_TASK_PRIO, &s_task);
    BaseType_t ok_mic = xTaskCreate(glm_mic_task, "glm_mic", RT_AUDIO_TASK_STACK, NULL,
                                   RT_AUDIO_TASK_PRIO, &s_mic_task);
    BaseType_t ok_play = xTaskCreate(glm_play_task, "glm_play", RT_AUDIO_TASK_STACK, NULL,
                                    RT_AUDIO_TASK_PRIO, &s_play_task);
    BaseType_t ok_tool = xTaskCreate(glm_tool_task, "glm_tool", RT_TOOL_TASK_STACK, NULL,
                                    RT_TOOL_TASK_PRIO, &s_tool_task);
    if (ok_main != pdPASS || ok_mic != pdPASS || ok_play != pdPASS || ok_tool != pdPASS) {
        cleanup_init_resources();
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG,
             "GLM Realtime ready (WSS, Tool Broker, ES8311 duplex; TLS RX=%d TX=%d, heap=PSRAM)",
             CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN,
             CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN);
    return ESP_OK;
}
