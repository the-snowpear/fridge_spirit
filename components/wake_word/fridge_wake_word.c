// 冰箱小精灵本地语音唤醒组件。
// 使用 ESP-SR WakeNet 在 ESP32-S3 本地识别“小冰小冰”，只负责唤醒事件上报，不直接触发 ASR 或 AI。
#include "fridge_wake_word.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_audio.h"
#include "fridge_speaker.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"
#include "nvs.h"

#define WAKE_TASK_STACK 8192
#define WAKE_TASK_PRIORITY 5
#define WAKE_STOP_WAIT_MS 1500
#define WAKE_TRIGGER_GUARD_MS 1200

// 策略管理任务：常驻轻量任务，独占 start/stop/模型加载等可能阻塞或耗时的调用，
// 把这些动作从 UI 任务上下文剥离出去，保证 policy_update 永远不阻塞调用者。
#define WAKE_MANAGER_TASK_STACK 4096
#define WAKE_MANAGER_TASK_PRIORITY 4
#define WAKE_MANAGER_TICK_MS 200

// 启动前内部 DMA 连续块门槛：经验初值 64KB，须用真机实测 WakeNet 实际占用后校准。
// 低于该门槛时拒绝拉起 WakeNet，避免内部 SRAM 紧张时强行启动引发不稳定。
#define WAKE_MIN_INTERNAL_DMA_BLOCK (64 * 1024)
// 最小驻留时间：已进入监听后 30s 内忽略“内存余量”边界抖动，避免反复 start/stop 抢占 I2S。
#define WAKE_MIN_DWELL_MS 30000

#define WAKE_NVS_NAMESPACE "fridge_wake"
#define WAKE_NVS_KEY_USER_EN "user_en"

static const char *TAG = "fridge_wake";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static bool s_initialized;
static bool s_stop_requested;
static bool s_starting;   // 正在创建 wake 任务，防止并发 start 重复建任务
static bool s_enabled;
static fridge_wake_word_state_t s_state = FRIDGE_WAKE_WORD_STATE_IDLE;
static uint32_t s_trigger_count;
static uint32_t s_last_trigger_ms;
static int32_t s_vad_state = -1;
static int32_t s_rms;
static int32_t s_peak_abs;
static uint32_t s_timeout_count;
static char s_error[128];
static fridge_wake_word_event_cb_t s_event_cb;
static void *s_event_ctx;

// 自动策略与用户开关相关状态。
static bool s_user_enabled;                 // 用户开关缓存（NVS 持久化，默认 false）
static bool s_policy_want_listen;           // 策略层期望是否监听
static fridge_wake_policy_reason_t s_policy_reason = FRIDGE_WAKE_POLICY_DISABLED_BY_USER;
static size_t s_largest_internal_dma_block; // 最近一次测得的内部 DMA 最大连续块
static bool s_dialog_in_progress;           // 唤醒后自动对话进行中，期间不自动恢复监听
static int64_t s_listen_since_ms;           // 进入监听的时间戳，用于最小驻留反横跳
static TaskHandle_t s_manager_task;
static fridge_wake_word_dialog_cb_t s_dialog_cb;
static void *s_dialog_ctx;

// 确保互斥锁已创建。init/start/对外 setter 都先调用它，避免启动早期顺序问题导致空指针。
static esp_err_t ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void set_error_locked(const char *error)
{
    strlcpy(s_error, error ? error : "", sizeof(s_error));
}

static void set_state(fridge_wake_word_state_t state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    if (error) {
        set_error_locked(error);
    }
    xSemaphoreGive(s_lock);
}

const char *fridge_wake_word_state_text(fridge_wake_word_state_t state)
{
    switch (state) {
    case FRIDGE_WAKE_WORD_STATE_LISTENING:
        return "listening";
    case FRIDGE_WAKE_WORD_STATE_ERROR:
        return "error";
    case FRIDGE_WAKE_WORD_STATE_IDLE:
    default:
        return "idle";
    }
}

static void update_audio_stats_locked(const int16_t *samples, size_t sample_count)
{
    uint64_t sum_squares = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        int32_t abs_sample = sample == INT16_MIN ? 32768 : (sample < 0 ? -sample : sample);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        sum_squares += (uint64_t)((int32_t)sample * (int32_t)sample);
    }
    s_peak_abs = peak;
    s_rms = sample_count > 0 ? (int32_t)sqrt((double)sum_squares / (double)sample_count) : 0;
}

static bool should_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stop = s_stop_requested;
    xSemaphoreGive(s_lock);
    return stop;
}

static void notify_wake_detected(void)
{
    fridge_wake_word_event_cb_t cb = NULL;
    void *ctx = NULL;
    fridge_wake_word_dialog_cb_t dialog_cb = NULL;
    void *dialog_ctx = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_last_trigger_ms == 0 || now_ms - s_last_trigger_ms >= WAKE_TRIGGER_GUARD_MS) {
        s_trigger_count++;
        s_last_trigger_ms = now_ms;
        cb = s_event_cb;
        ctx = s_event_ctx;
        // 标记“对话进行中”，策略层在对话期间不会重复拉起监听；由对话流程结束时清除该标记。
        if (s_dialog_cb && !s_dialog_in_progress) {
            s_dialog_in_progress = true;
            dialog_cb = s_dialog_cb;
            dialog_ctx = s_dialog_ctx;
        }
    }
    xSemaphoreGive(s_lock);

    // 回调在 wake 任务上下文执行，必须保持非阻塞：事件回调只输出 USB 事件，
    // 对话回调只投递信号给独立任务，绝不在此做录音或 HTTPS。
    if (cb) {
        cb(ctx);
    }
    if (dialog_cb) {
        dialog_cb(dialog_ctx);
    }
}

static void wake_task(void *arg)
{
    (void)arg;
    int feed_chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    int feed_channel = s_afe_handle->get_feed_channel_num(s_afe_data);
    size_t frame_samples = (size_t)feed_chunksize * (size_t)feed_channel;
    int16_t *frame = heap_caps_malloc(frame_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        frame = heap_caps_malloc(frame_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (!frame) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake frame allocation failed");
        goto done;
    }

    esp_err_t err = fridge_audio_wake_stream_start();
    if (err != ESP_OK) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, esp_err_to_name(err));
        goto done;
    }

    set_state(FRIDGE_WAKE_WORD_STATE_LISTENING, "");
    // 记录进入监听的时间戳，供策略层最小驻留反横跳使用。
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_listen_since_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_lock);
    while (!should_stop()) {
        size_t sample_count = 0;
        err = fridge_audio_wake_stream_read(frame, frame_samples, &sample_count, pdMS_TO_TICKS(250));
        if (err != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_timeout_count++;
            xSemaphoreGive(s_lock);
            if (err != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "wake stream read failed: %s", esp_err_to_name(err));
            }
            continue;
        }
        if (sample_count != frame_samples) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        update_audio_stats_locked(frame, frame_samples);
        xSemaphoreGive(s_lock);

        int feed_ret = s_afe_handle->feed(s_afe_data, frame);
        if (feed_ret < 0) {
            ESP_LOGW(TAG, "AFE feed failed: %d", feed_ret);
            continue;
        }

        afe_fetch_result_t *result = s_afe_handle->fetch_with_delay
                                         ? s_afe_handle->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(20))
                                         : s_afe_handle->fetch(s_afe_data);
        if (!result || result->ret_value == ESP_FAIL) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_vad_state = result->vad_state;
        xSemaphoreGive(s_lock);
        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected: %s", FRIDGE_WAKE_WORD_TEXT);
            notify_wake_detected();
        }
    }

    (void)fridge_audio_wake_stream_stop();
    set_state(FRIDGE_WAKE_WORD_STATE_IDLE, "");

done:
    free(frame);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_enabled = false;
    s_stop_requested = false;
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

esp_err_t fridge_wake_word_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t lock_ret = ensure_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    // ESP-SR 模型由固定 label 为 model 的分区加载，烧录前必须确认当前自定义分区表中存在该分区。
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "load ESP-SR models failed");
        return ESP_FAIL;
    }
    char model_name[] = FRIDGE_WAKE_WORD_MODEL;
    if (esp_srmodel_exists(s_models, model_name) < 0) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake model not found in model partition");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    // 单麦克风 INMP441 只提供一个 16kHz mono 通道，因此输入格式必须是 "M"，不能声明播放参考通道。
    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE config init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NO_MEM;
    }
    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->vad_init = true;
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = FRIDGE_WAKE_WORD_MODEL;
    // 用户实测单纯放大麦克风输入会降低触发率；这里改用 WakeNet 自带的 aggressive 检测模式提高灵敏度。
    afe_config->wakenet_mode = DET_MODE_95;
    afe_config->agc_init = true;
    afe_config->fixed_first_channel = true;
    afe_config->fixed_output_channel = true;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = WAKE_TASK_PRIORITY;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (!s_afe_handle) {
        afe_config_free(afe_config);
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE handle init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    if (!s_afe_data) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE data create failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    if (s_afe_handle->get_samp_rate(s_afe_data) != FRIDGE_AUDIO_SAMPLE_RATE) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "AFE sample rate mismatch");
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "wake word ready: %s / %s", FRIDGE_WAKE_WORD_TEXT, FRIDGE_WAKE_WORD_MODEL);
    s_afe_handle->print_pipeline(s_afe_data);
    return ESP_OK;
}

esp_err_t fridge_wake_word_set_event_callback(fridge_wake_word_event_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "wake word not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_event_cb = cb;
    s_event_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_wake_word_start(void)
{
    esp_err_t err = fridge_wake_word_init();
    if (err != ESP_OK) {
        return err;
    }

    fridge_speaker_status_t speaker = {0};
    if (fridge_speaker_get_status(&speaker) == ESP_OK &&
        (speaker.state == FRIDGE_SPEAKER_STATE_SYNTHESIZING || speaker.state == FRIDGE_SPEAKER_STATE_PLAYING)) {
        return ESP_ERR_INVALID_STATE;
    }

    // 启动前内部 DMA 余量门槛：WakeNet/AFE 的实时缓冲、锁与 I2S DMA 需要连续内部 SRAM。
    // 连续块低于门槛时拒绝启动，避免在内部 SRAM 紧张时强拉 WakeNet 诱发屏幕/TLS 抢内存。
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_largest_internal_dma_block = largest;
    xSemaphoreGive(s_lock);
    if (largest < WAKE_MIN_INTERNAL_DMA_BLOCK) {
        ESP_LOGW(TAG, "skip wake start: largest internal DMA block %u < %u",
                 (unsigned)largest, (unsigned)WAKE_MIN_INTERNAL_DMA_BLOCK);
        set_state(FRIDGE_WAKE_WORD_STATE_IDLE, "low internal dma");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task || s_starting) {
        // 已在监听或正在创建任务：保持幂等，避免管理任务与 UI 录音恢复并发各建一个 wake 任务。
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_starting = true;
    s_stop_requested = false;
    s_enabled = true;
    s_timeout_count = 0;
    s_error[0] = '\0';
    xSemaphoreGive(s_lock);

    BaseType_t ok = xTaskCreatePinnedToCore(wake_task, "wake_word", WAKE_TASK_STACK, NULL, WAKE_TASK_PRIORITY, &s_task, 1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_starting = false;
    if (ok != pdPASS) {
        s_enabled = false;
        s_task = NULL;
        xSemaphoreGive(s_lock);
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake task create failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_wake_word_stop(void)
{
    if (!s_initialized || !s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    TaskHandle_t task = s_task;
    s_stop_requested = true;
    xSemaphoreGive(s_lock);

    for (int i = 0; task && i < (WAKE_STOP_WAIT_MS / 20); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        task = s_task;
        xSemaphoreGive(s_lock);
    }
    if (task) {
        set_state(FRIDGE_WAKE_WORD_STATE_ERROR, "wake task stop timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t fridge_wake_word_reset_stats(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "wake word not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_trigger_count = 0;
    s_last_trigger_ms = 0;
    s_timeout_count = 0;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_wake_word_get_status(fridge_wake_word_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        out->state = FRIDGE_WAKE_WORD_STATE_IDLE;
        out->policy_reason = FRIDGE_WAKE_POLICY_DISABLED_BY_USER;
        out->min_dma_block_threshold = WAKE_MIN_INTERNAL_DMA_BLOCK;
        strlcpy(out->wake_word, FRIDGE_WAKE_WORD_TEXT, sizeof(out->wake_word));
        strlcpy(out->model, FRIDGE_WAKE_WORD_MODEL, sizeof(out->model));
        strlcpy(out->error, "wake word not initialized", sizeof(out->error));
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->enabled = s_enabled;
    out->state = s_state;
    out->trigger_count = s_trigger_count;
    out->last_trigger_ms = s_last_trigger_ms;
    out->vad_state = s_vad_state;
    out->rms = s_rms;
    out->peak_abs = s_peak_abs;
    out->timeout_count = s_timeout_count;
    out->user_enabled = s_user_enabled;
    out->auto_allowed = s_policy_want_listen;
    out->policy_reason = s_policy_reason;
    out->largest_internal_dma_block = s_largest_internal_dma_block;
    out->min_dma_block_threshold = WAKE_MIN_INTERNAL_DMA_BLOCK;
    strlcpy(out->wake_word, FRIDGE_WAKE_WORD_TEXT, sizeof(out->wake_word));
    strlcpy(out->model, FRIDGE_WAKE_WORD_MODEL, sizeof(out->model));
    strlcpy(out->error, s_error, sizeof(out->error));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

// 把策略原因枚举转成稳定的英文短串，供 USB/Web 显示与脚本匹配。
const char *fridge_wake_policy_reason_text(fridge_wake_policy_reason_t reason)
{
    switch (reason) {
    case FRIDGE_WAKE_POLICY_DISPLAY_NOT_READY:
        return "display_not_ready";
    case FRIDGE_WAKE_POLICY_SCREEN_OFF:
        return "screen_off";
    case FRIDGE_WAKE_POLICY_AUDIO_BUSY:
        return "audio_busy";
    case FRIDGE_WAKE_POLICY_SPEAKER_BUSY:
        return "speaker_busy";
    case FRIDGE_WAKE_POLICY_LOW_INTERNAL_DMA:
        return "low_internal_dma";
    case FRIDGE_WAKE_POLICY_STARTING:
        return "starting";
    case FRIDGE_WAKE_POLICY_LISTENING:
        return "listening";
    case FRIDGE_WAKE_POLICY_STOPPING:
        return "stopping";
    case FRIDGE_WAKE_POLICY_DISABLED_BY_USER:
    default:
        return "disabled_by_user";
    }
}

// 读取用户开关缓存。启动早期 NVS 尚未加载时返回默认 false。
bool fridge_wake_word_get_user_enabled(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool enabled = s_user_enabled;
    xSemaphoreGive(s_lock);
    return enabled;
}

// 从 NVS 加载用户开关到缓存；找不到键时保持默认 false。仅在策略首次评估时惰性加载一次。
static void load_user_enabled_locked(void)
{
    nvs_handle_t handle;
    if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t value = 0;
    if (nvs_get_u8(handle, WAKE_NVS_KEY_USER_EN, &value) == ESP_OK) {
        s_user_enabled = value != 0;
    }
    nvs_close(handle);
}

esp_err_t fridge_wake_word_set_user_enabled(bool enabled)
{
    esp_err_t lock_ret = ensure_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }

    // 持久化到 NVS，保证重启后保持用户选择。
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, WAKE_NVS_KEY_USER_EN, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_user_enabled = enabled;
    if (!enabled) {
        // 关闭开关立即取消监听意图；实际停止由策略管理任务执行，避免在此阻塞调用者。
        s_policy_want_listen = false;
        s_policy_reason = FRIDGE_WAKE_POLICY_DISABLED_BY_USER;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_wake_word_set_dialog_callback(fridge_wake_word_dialog_cb_t cb, void *user_ctx)
{
    esp_err_t lock_ret = ensure_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dialog_cb = cb;
    s_dialog_ctx = user_ctx;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void fridge_wake_word_notify_dialog_finished(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dialog_in_progress = false;
    xSemaphoreGive(s_lock);
}

// 策略管理任务：常驻轮询“期望监听状态”，独占可能阻塞/耗时的 start/stop 调用。
// 这样 policy_update 只需设标志位即可立即返回，绝不在 UI 任务上下文里等模型加载或 stop 退出。
static void wake_manager_task(void *arg)
{
    (void)arg;
    int64_t next_start_attempt_ms = 0;  // 启动失败后的退避时间点，避免 200ms 高频重试刷日志
    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool want = s_policy_want_listen;
        bool dialog = s_dialog_in_progress;
        bool listening = (s_task != NULL);
        xSemaphoreGive(s_lock);

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (want && !dialog && !listening && now_ms >= next_start_attempt_ms) {
            // 期望监听但当前未监听：尝试启动（start 内部含 TTS/内存/幂等防护）。
            esp_err_t err = fridge_wake_word_start();
            if (err == ESP_ERR_NO_MEM) {
                // 内部 DMA 余量不足：记录原因并退避 2s，避免高频重试和日志刷屏。
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_policy_reason = FRIDGE_WAKE_POLICY_LOW_INTERNAL_DMA;
                xSemaphoreGive(s_lock);
                next_start_attempt_ms = now_ms + 2000;
            } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "manager start wake failed: %s", esp_err_to_name(err));
                next_start_attempt_ms = now_ms + 2000;
            }
        } else if (!want && listening) {
            // 期望停止但仍在监听：在本任务里执行可能阻塞 1.5s 的 stop，不影响 UI 任务。
            (void)fridge_wake_word_stop();
        }
        vTaskDelay(pdMS_TO_TICKS(WAKE_MANAGER_TICK_MS));
    }
}

void fridge_wake_word_policy_update(bool display_ready, bool screen_awake)
{
    if (ensure_lock() != ESP_OK) {
        return;
    }

    // 首次调用时惰性加载用户开关，并拉起常驻管理任务。
    static bool s_user_loaded;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_user_loaded) {
        load_user_enabled_locked();
        s_user_loaded = true;
    }
    bool user_enabled = s_user_enabled;
    bool dialog = s_dialog_in_progress;
    bool listening = (s_task != NULL);
    int64_t listen_since = s_listen_since_ms;
    xSemaphoreGive(s_lock);

    if (!s_manager_task) {
        BaseType_t ok = xTaskCreate(wake_manager_task, "wake_mgr", WAKE_MANAGER_TASK_STACK,
                                    NULL, WAKE_MANAGER_TASK_PRIORITY, &s_manager_task);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "create wake manager task failed");
        }
    }

    // 综合判定是否允许自动监听；任一条件不满足都给出对应原因，便于真机定位。
    fridge_wake_policy_reason_t reason;
    bool want;
    if (!user_enabled) {
        want = false;
        reason = FRIDGE_WAKE_POLICY_DISABLED_BY_USER;
    } else if (!display_ready) {
        want = false;
        reason = FRIDGE_WAKE_POLICY_DISPLAY_NOT_READY;
    } else if (!screen_awake) {
        want = false;
        reason = FRIDGE_WAKE_POLICY_SCREEN_OFF;
    } else {
        // 亮屏且用户开启：检查音频/扬声器忙闲，避免和录音/TTS 抢 I2S。
        fridge_audio_status_t audio = {0};
        bool audio_busy = fridge_audio_get_status(&audio) == ESP_OK &&
                          (audio.state == FRIDGE_AUDIO_STATE_RECORDING);
        fridge_speaker_status_t speaker = {0};
        bool speaker_busy = fridge_speaker_get_status(&speaker) == ESP_OK &&
                            (speaker.state == FRIDGE_SPEAKER_STATE_SYNTHESIZING ||
                             speaker.state == FRIDGE_SPEAKER_STATE_PLAYING);
        if (dialog) {
            // 唤醒后自动对话进行中：保持期望停止，待对话结束再由策略恢复。
            want = false;
            reason = FRIDGE_WAKE_POLICY_AUDIO_BUSY;
        } else if (audio_busy) {
            want = false;
            reason = FRIDGE_WAKE_POLICY_AUDIO_BUSY;
        } else if (speaker_busy) {
            want = false;
            reason = FRIDGE_WAKE_POLICY_SPEAKER_BUSY;
        } else {
            want = true;
            reason = listening ? FRIDGE_WAKE_POLICY_LISTENING : FRIDGE_WAKE_POLICY_STARTING;
        }
    }

    // 最小驻留反横跳：已在监听且本轮想停，但停因仅是“忙/内存”这类可能抖动的边界，
    // 且监听时长还没到最小驻留时间，则维持监听，避免反复 start/stop 抢 I2S。
    if (!want && listening && listen_since > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool transient = (reason == FRIDGE_WAKE_POLICY_AUDIO_BUSY ||
                          reason == FRIDGE_WAKE_POLICY_SPEAKER_BUSY ||
                          reason == FRIDGE_WAKE_POLICY_LOW_INTERNAL_DMA);
        // 注意：用户关开关(disabled_by_user)、灭屏(screen_off)是明确事件，不在抖动豁免之列。
        if (transient && !dialog && (now_ms - listen_since) < WAKE_MIN_DWELL_MS) {
            want = true;
            reason = FRIDGE_WAKE_POLICY_LISTENING;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_policy_want_listen = want;
    s_policy_reason = reason;
    xSemaphoreGive(s_lock);
}
