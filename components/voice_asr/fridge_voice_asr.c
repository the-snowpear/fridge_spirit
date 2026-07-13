// 冰箱小精灵 ESP32-P4 唤醒后 ASR 编排。
// WakeNet 回调仅投递信号；内部 SRAM 协调任务负责 NVS/状态，PSRAM 大栈任务负责 HTTPS。

#include "fridge_voice_asr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_audio.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define VOICE_ASR_AUTO_RECORD_MS 5000
#define VOICE_ASR_POLICY_TICK_MS 500
#define VOICE_ASR_AUTO_TASK_STACK 8192
#define VOICE_ASR_POLICY_TASK_STACK 4096
#define VOICE_ASR_HTTP_TASK_STACK 20480

static const char *TAG = "voice_asr";

typedef struct {
    const fridge_asr_config_update_t *config;
    fridge_asr_result_t *result;
    SemaphoreHandle_t done;
    esp_err_t err;
} voice_asr_http_job_t;

static SemaphoreHandle_t s_run_lock;
static SemaphoreHandle_t s_wake_signal;
static SemaphoreHandle_t s_state_lock;
static TaskHandle_t s_auto_task;
static TaskHandle_t s_policy_task;
static bool s_initialized;
static bool s_hardware_ready;
static bool s_manual_session;
static uint32_t s_trigger_id;
static fridge_voice_asr_event_cb_t s_event_cb;
static void *s_event_ctx;

const char *fridge_voice_asr_state_text(fridge_voice_asr_state_t state)
{
    switch (state) {
    case FRIDGE_VOICE_ASR_STATE_RECORDING:
        return "recording";
    case FRIDGE_VOICE_ASR_STATE_TRANSCRIBING:
        return "transcribing";
    case FRIDGE_VOICE_ASR_STATE_DONE:
        return "done";
    case FRIDGE_VOICE_ASR_STATE_ERROR:
        return "error";
    case FRIDGE_VOICE_ASR_STATE_IDLE:
    default:
        return "idle";
    }
}

static void emit_event(fridge_voice_asr_state_t state,
                       uint32_t trigger_id,
                       const fridge_asr_result_t *result,
                       const char *error)
{
    fridge_voice_asr_event_cb_t cb = NULL;
    void *ctx = NULL;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    cb = s_event_cb;
    ctx = s_event_ctx;
    xSemaphoreGive(s_state_lock);
    if (!cb) {
        return;
    }

    fridge_voice_asr_event_t event = {
        .state = state,
        .trigger_id = trigger_id,
    };
    if (result) {
        event.result = *result;
    }
    strlcpy(event.error,
            error ? error : (result ? result->error : ""),
            sizeof(event.error));
    cb(&event, ctx);
}

// HTTPS 深调用栈放在 PSRAM；任务只使用调用方已加载好的配置，不在 PSRAM 栈上访问 NVS。
static void voice_asr_http_task(void *arg)
{
    voice_asr_http_job_t *job = (voice_asr_http_job_t *)arg;
    job->err = fridge_asr_transcribe_latest_recording_with_config(job->config, job->result);
    ESP_LOGI(TAG,
             "ASR HTTP done: err=%s http=%d stack_hwm=%u words",
             esp_err_to_name(job->err),
             job->result->http_status,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

esp_err_t fridge_voice_asr_transcribe_latest(fridge_asr_result_t *out)
{
    if (!out || !s_run_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (xSemaphoreTake(s_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        strlcpy(out->error, "ASR busy", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }

    // 配置读取在当前内部 SRAM 栈完成；API Key 只存在于设备内存，不写日志或串口。
    fridge_asr_config_update_t *config = heap_caps_calloc(1,
                                                          sizeof(*config),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!config) {
        config = calloc(1, sizeof(*config));
    }
    esp_err_t err = config ? fridge_asr_load_active_config(config) : ESP_ERR_NO_MEM;
    SemaphoreHandle_t done = NULL;
    if (err == ESP_OK) {
        done = xSemaphoreCreateBinary();
        if (!done) {
            err = ESP_ERR_NO_MEM;
        }
    }

    voice_asr_http_job_t job = {
        .config = config,
        .result = out,
        .done = done,
        .err = ESP_FAIL,
    };
    if (err == ESP_OK) {
        BaseType_t ok = xTaskCreateWithCaps(voice_asr_http_task,
                                            "voice_asr_http",
                                            VOICE_ASR_HTTP_TASK_STACK,
                                            &job,
                                            4,
                                            NULL,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            err = ESP_ERR_NO_MEM;
            strlcpy(out->error, "ASR HTTPS worker create failed", sizeof(out->error));
        } else {
            // HTTP 客户端自身有配置超时；必须等 worker 结束，避免栈上 job 生命周期提前结束。
            xSemaphoreTake(done, portMAX_DELAY);
            err = job.err;
        }
    }
    if (err != ESP_OK && out->error[0] == '\0') {
        strlcpy(out->error, esp_err_to_name(err), sizeof(out->error));
    }

    if (done) {
        vSemaphoreDelete(done);
    }
    free(config);
    xSemaphoreGive(s_run_lock);
    return err;
}

static void on_wake_dialog(void *user_ctx)
{
    (void)user_ctx;
    if (s_wake_signal) {
        xSemaphoreGive(s_wake_signal);
    }
}

static void voice_asr_auto_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_wake_signal, portMAX_DELAY);

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        uint32_t trigger_id = ++s_trigger_id;
        xSemaphoreGive(s_state_lock);

        // WakeNet 与录音复用 ES8311/I2S0，必须先完整停止两个 AFE 任务再启动录音。
        esp_err_t err = fridge_wake_word_stop();
        if (err == ESP_OK) {
            emit_event(FRIDGE_VOICE_ASR_STATE_RECORDING, trigger_id, NULL, NULL);
            err = fridge_audio_start_recording();
        }
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_ASR_AUTO_RECORD_MS));
            err = fridge_audio_stop_recording();
        }

        fridge_asr_result_t result = {0};
        if (err == ESP_OK) {
            emit_event(FRIDGE_VOICE_ASR_STATE_TRANSCRIBING, trigger_id, NULL, NULL);
            err = fridge_voice_asr_transcribe_latest(&result);
        }
        if (err == ESP_OK) {
            emit_event(FRIDGE_VOICE_ASR_STATE_DONE, trigger_id, &result, NULL);
        } else {
            const char *detail = result.error[0] ? result.error : esp_err_to_name(err);
            ESP_LOGW(TAG, "wake ASR failed: %s", detail);
            emit_event(FRIDGE_VOICE_ASR_STATE_ERROR, trigger_id, &result, detail);
        }

        // 错误不改变用户持久开关；清除会话标记后由策略任务按音频空闲状态恢复监听。
        fridge_wake_word_notify_dialog_finished();
        ESP_LOGI(TAG,
                 "wake ASR cycle done: trigger=%lu free_internal=%u largest_dma=%u free_psram=%u",
                 (unsigned long)trigger_id,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
}

static void voice_asr_policy_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool ready = s_hardware_ready;
        xSemaphoreGive(s_state_lock);
        if (ready) {
            // P4 最小模式使用静态常亮屏；用户持久开关和音频忙闲由 wake_word 内部综合判断。
            fridge_wake_word_policy_update(true, true);
        }
        vTaskDelay(pdMS_TO_TICKS(VOICE_ASR_POLICY_TICK_MS));
    }
}

esp_err_t fridge_voice_asr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_run_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    s_wake_signal = xSemaphoreCreateBinary();
    if (!s_run_lock || !s_state_lock || !s_wake_signal) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(fridge_asr_init(), TAG, "ASR config init failed");
    ESP_RETURN_ON_ERROR(fridge_wake_word_set_dialog_callback(on_wake_dialog, NULL),
                        TAG,
                        "wake callback registration failed");
    if (xTaskCreate(voice_asr_auto_task,
                    "voice_asr_auto",
                    VOICE_ASR_AUTO_TASK_STACK,
                    NULL,
                    4,
                    &s_auto_task) != pdPASS ||
        xTaskCreate(voice_asr_policy_task,
                    "voice_asr_policy",
                    VOICE_ASR_POLICY_TASK_STACK,
                    NULL,
                    4,
                    &s_policy_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "P4 wake-to-ASR coordinator ready; auto record=%dms", VOICE_ASR_AUTO_RECORD_MS);
    return ESP_OK;
}

void fridge_voice_asr_set_hardware_ready(void)
{
    if (!s_state_lock) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_hardware_ready = true;
    xSemaphoreGive(s_state_lock);
}

esp_err_t fridge_voice_asr_set_event_callback(fridge_voice_asr_event_cb_t cb, void *user_ctx)
{
    if (!s_state_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_event_cb = cb;
    s_event_ctx = user_ctx;
    xSemaphoreGive(s_state_lock);
    return ESP_OK;
}

esp_err_t fridge_voice_asr_manual_record_begin(void)
{
    if (!s_state_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_manual_session) {
        xSemaphoreGive(s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_manual_session = true;
    xSemaphoreGive(s_state_lock);

    fridge_wake_word_notify_dialog_started();
    esp_err_t err = fridge_wake_word_stop();
    if (err != ESP_OK) {
        fridge_voice_asr_manual_session_finish();
    }
    return err;
}

void fridge_voice_asr_manual_session_finish(void)
{
    if (!s_state_lock) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool was_active = s_manual_session;
    s_manual_session = false;
    xSemaphoreGive(s_state_lock);
    if (was_active) {
        fridge_wake_word_notify_dialog_finished();
    }
}
