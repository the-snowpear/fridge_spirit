// 冰箱小精灵 ESP32-P4 音频采集组件。
// 通过板载 ES8311 + 驻极体麦克风采集 16kHz/16-bit/mono PCM；录音期间独占摄像头共享 I2C gate。
#include "fridge_audio.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_codec_es8311.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_TASK_STACK 4096
#define AUDIO_READ_SAMPLES 256
#define AUDIO_WAKE_READ_SAMPLES 256
#define AUDIO_CLIP_THRESHOLD 32000
#define AUDIO_TIMEOUT_BACKOFF_MS 20
#define AUDIO_I2C_GATE_TIMEOUT_MS 200

static const char *TAG = "fridge_audio";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_record_task;
static bool s_wake_stream_active;
static int16_t *s_pcm;
static size_t s_pcm_capacity;
static size_t s_pcm_bytes;
static int64_t s_record_start_us;
static uint32_t s_duration_ms;
static int32_t s_rms;
static int16_t s_min_sample;
static int16_t s_max_sample;
static int32_t s_mean_sample;
static int32_t s_peak_abs;
static uint32_t s_clip_count;
static uint32_t s_sample_count;
static uint64_t s_sum_squares;
static int64_t s_sum_samples;
static fridge_audio_state_t s_state = FRIDGE_AUDIO_STATE_IDLE;
static char s_error[128];
static bool s_initialized;
static uint32_t s_i2s_timeout_count;
static esp_err_t s_stream_stop_result = ESP_OK;

static void set_state(fridge_audio_state_t state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    strlcpy(s_error, error ? error : "", sizeof(s_error));
    xSemaphoreGive(s_lock);
}

static int32_t abs_i16(int16_t value)
{
    return value == INT16_MIN ? 32768 : (value < 0 ? -value : value);
}

// 清空一次录音/唤醒监听的诊断统计；PCM 数据长度由各启动入口单独清零。
static void reset_stats_locked(void)
{
    s_duration_ms = 0;
    s_rms = 0;
    s_min_sample = INT16_MAX;
    s_max_sample = INT16_MIN;
    s_mean_sample = 0;
    s_peak_abs = 0;
    s_clip_count = 0;
    s_sample_count = 0;
    s_sum_squares = 0;
    s_sum_samples = 0;
    s_i2s_timeout_count = 0;
    s_stream_stop_result = ESP_OK;
}

static void update_stats_locked(const int16_t *samples, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; i++) {
        int16_t sample = samples[i];
        int32_t abs_sample = abs_i16(sample);
        if (sample < s_min_sample) {
            s_min_sample = sample;
        }
        if (sample > s_max_sample) {
            s_max_sample = sample;
        }
        if (abs_sample > s_peak_abs) {
            s_peak_abs = abs_sample;
        }
        if (abs_sample >= AUDIO_CLIP_THRESHOLD) {
            s_clip_count++;
        }
        s_sum_samples += sample;
        s_sum_squares += (uint64_t)((int32_t)sample * (int32_t)sample);
        s_sample_count++;
    }
    if (s_sample_count > 0) {
        s_mean_sample = (int32_t)(s_sum_samples / (int64_t)s_sample_count);
        s_rms = (int32_t)sqrt((double)s_sum_squares / (double)s_sample_count);
    }
}

// 录音任务直接接收 ES8311 的 int16 PCM，不再执行 INMP441 的 32-bit 右移缩放。
static void record_task(void *arg)
{
    (void)arg;
    int16_t samples[AUDIO_READ_SAMPLES];
    esp_err_t stream_result = ESP_OK;

    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool recording = s_state == FRIDGE_AUDIO_STATE_RECORDING;
        size_t offset = s_pcm_bytes;
        xSemaphoreGive(s_lock);
        if (!recording || offset >= s_pcm_capacity) {
            break;
        }

        size_t samples_read = 0;
        esp_err_t err = fridge_codec_es8311_read(samples,
                                                 AUDIO_READ_SAMPLES,
                                                 &samples_read,
                                                 pdMS_TO_TICKS(250));
        if (err != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (err == ESP_ERR_TIMEOUT) {
                // 时钟未稳定或编解码器链路异常时限频记录，避免录音任务刷爆串口并饿死其它任务。
                s_i2s_timeout_count++;
                strlcpy(s_error, "ES8311 i2s read timeout", sizeof(s_error));
                if (s_i2s_timeout_count == 1 || (s_i2s_timeout_count % 32) == 0) {
                    ESP_LOGW(TAG, "ES8311 I2S read timeout, count=%lu",
                             (unsigned long)s_i2s_timeout_count);
                }
            } else {
                stream_result = err;
                strlcpy(s_error, esp_err_to_name(err), sizeof(s_error));
                ESP_LOGE(TAG, "ES8311 I2S read failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(s_lock);
            if (err != ESP_ERR_TIMEOUT) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(AUDIO_TIMEOUT_BACKOFF_MS));
            continue;
        }
        if (samples_read == 0) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t writable = (s_pcm_capacity - s_pcm_bytes) / sizeof(int16_t);
        if (samples_read > writable) {
            samples_read = writable;
        }
        int16_t *write_ptr = &s_pcm[s_pcm_bytes / sizeof(int16_t)];
        memcpy(write_ptr, samples, samples_read * sizeof(samples[0]));
        s_pcm_bytes += samples_read * sizeof(int16_t);
        update_stats_locked(write_ptr, samples_read);
        if (s_peak_abs >= 400 && s_rms >= 80) {
            s_error[0] = '\0';
        }
        s_duration_ms = (uint32_t)(s_sample_count * 1000 / FRIDGE_AUDIO_SAMPLE_RATE);
        bool full = s_pcm_bytes >= s_pcm_capacity;
        xSemaphoreGive(s_lock);
        if (full) {
            ESP_LOGI(TAG, "record buffer full, auto stop");
            break;
        }
    }

    // 必须先关闭 ES8311 ADC，再释放 GPIO28/29 gate；摄像头只能在此后访问 SCCB。
    esp_err_t stop_result = fridge_codec_es8311_stop_input();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stream_stop_result = stream_result != ESP_OK ? stream_result : stop_result;
    if (s_stream_stop_result != ESP_OK) {
        s_state = FRIDGE_AUDIO_STATE_ERROR;
        strlcpy(s_error, esp_err_to_name(s_stream_stop_result), sizeof(s_error));
    } else if (s_state == FRIDGE_AUDIO_STATE_RECORDING) {
        s_state = s_pcm_bytes > 0 ? FRIDGE_AUDIO_STATE_READY : FRIDGE_AUDIO_STATE_IDLE;
    }
    s_record_task = NULL;
    xSemaphoreGive(s_lock);

    if (stop_result != ESP_OK) {
        ESP_LOGE(TAG, "stop ES8311 input failed: %s", esp_err_to_name(stop_result));
    }
    vTaskDelete(NULL);
}

esp_err_t fridge_audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_pcm) {
        s_pcm_capacity = FRIDGE_AUDIO_MAX_PCM_BYTES;
        s_pcm = heap_caps_malloc(s_pcm_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm) {
            s_pcm = heap_caps_malloc(s_pcm_capacity, MALLOC_CAP_8BIT);
        }
        if (!s_pcm) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = fridge_codec_es8311_init();
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "board ES8311 microphone ready: 16000Hz/16-bit/mono, direct int16 PCM");
    return ESP_OK;
}

esp_err_t fridge_audio_start_recording(void)
{
    esp_err_t err = fridge_audio_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_state == FRIDGE_AUDIO_STATE_RECORDING ||
                s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING ||
                s_wake_stream_active || s_record_task;
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    err = fridge_codec_es8311_start_input(pdMS_TO_TICKS(AUDIO_I2C_GATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR,
                  err == ESP_ERR_TIMEOUT ? "camera/ES8311 shared I2C busy" : esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    busy = s_state == FRIDGE_AUDIO_STATE_RECORDING ||
           s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING ||
           s_wake_stream_active || s_record_task;
    if (!busy) {
        s_pcm_bytes = 0;
        reset_stats_locked();
        s_error[0] = '\0';
        s_record_start_us = esp_timer_get_time();
        s_state = FRIDGE_AUDIO_STATE_RECORDING;
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        esp_err_t stop_err = fridge_codec_es8311_stop_input();
        return stop_err == ESP_OK ? ESP_ERR_INVALID_STATE : stop_err;
    }

    BaseType_t task_ok = xTaskCreate(record_task, "audio_record", AUDIO_TASK_STACK, NULL, 6, &s_record_task);
    if (task_ok != pdPASS) {
        esp_err_t stop_err = fridge_codec_es8311_stop_input();
        set_state(FRIDGE_AUDIO_STATE_ERROR,
                  stop_err == ESP_OK ? "audio record task create failed" : esp_err_to_name(stop_err));
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "cleanup after record task create failure failed: %s", esp_err_to_name(stop_err));
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fridge_audio_stop_recording(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state != FRIDGE_AUDIO_STATE_RECORDING) {
        bool ready = s_state == FRIDGE_AUDIO_STATE_READY;
        esp_err_t prior_result = s_stream_stop_result;
        xSemaphoreGive(s_lock);
        return ready ? prior_result : ESP_ERR_INVALID_STATE;
    }
    s_state = FRIDGE_AUDIO_STATE_READY;
    s_duration_ms = (uint32_t)((esp_timer_get_time() - s_record_start_us) / 1000);
    xSemaphoreGive(s_lock);

    // 单次 I2S read 最多阻塞 250ms；1s 预算足够任务退出并完成 ES8311/I2C 收尾。
    for (int i = 0; i < 50; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool running = s_record_task != NULL;
        xSemaphoreGive(s_lock);
        if (!running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool still_running = s_record_task != NULL;
    esp_err_t stop_result = s_stream_stop_result;
    xSemaphoreGive(s_lock);
    if (still_running) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, "audio record task stop timeout");
        return ESP_ERR_TIMEOUT;
    }
    return stop_result;
}

esp_err_t fridge_audio_wake_stream_start(void)
{
    esp_err_t err = fridge_audio_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active || s_record_task;
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    err = fridge_codec_es8311_start_input(pdMS_TO_TICKS(AUDIO_I2C_GATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR,
                  err == ESP_ERR_TIMEOUT ? "camera/ES8311 shared I2C busy" : esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    busy = s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active || s_record_task;
    if (!busy) {
        s_wake_stream_active = true;
        s_state = FRIDGE_AUDIO_STATE_WAKE_LISTENING;
        s_pcm_bytes = 0;
        s_error[0] = '\0';
        reset_stats_locked();
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        esp_err_t stop_err = fridge_codec_es8311_stop_input();
        return stop_err == ESP_OK ? ESP_ERR_INVALID_STATE : stop_err;
    }
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_read(int16_t *out_samples,
                                        size_t max_samples,
                                        size_t *sample_count,
                                        TickType_t timeout_ticks)
{
    if (!out_samples || !sample_count || max_samples == 0 || !s_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample_count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_wake_stream_active && s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING;
    xSemaphoreGive(s_lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total_samples = 0;
    while (total_samples < max_samples) {
        size_t wanted = max_samples - total_samples;
        if (wanted > AUDIO_WAKE_READ_SAMPLES) {
            wanted = AUDIO_WAKE_READ_SAMPLES;
        }

        size_t samples_read = 0;
        esp_err_t err = fridge_codec_es8311_read(&out_samples[total_samples],
                                                 wanted,
                                                 &samples_read,
                                                 timeout_ticks);
        if (err != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (err == ESP_ERR_TIMEOUT) {
                s_i2s_timeout_count++;
            } else {
                strlcpy(s_error, esp_err_to_name(err), sizeof(s_error));
            }
            xSemaphoreGive(s_lock);
            if (total_samples > 0) {
                break;
            }
            return err;
        }
        if (samples_read == 0) {
            break;
        }
        total_samples += samples_read;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    update_stats_locked(out_samples, total_samples);
    if (s_peak_abs >= 400 && s_rms >= 80) {
        s_error[0] = '\0';
    }
    s_duration_ms = (uint32_t)(s_sample_count * 1000 / FRIDGE_AUDIO_SAMPLE_RATE);
    xSemaphoreGive(s_lock);
    *sample_count = total_samples;
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_stop(void)
{
    if (!s_initialized || !s_lock) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_active = s_wake_stream_active;
    s_wake_stream_active = false;
    xSemaphoreGive(s_lock);
    if (!was_active) {
        return ESP_OK;
    }

    esp_err_t err = fridge_codec_es8311_stop_input();
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        return err;
    }
    set_state(FRIDGE_AUDIO_STATE_IDLE, NULL);
    return ESP_OK;
}

static const char *quality_hint_locked(void)
{
    if (s_sample_count == 0) {
        return "too_short";
    }
    if (s_clip_count > 0) {
        return "clipping";
    }
    if (s_duration_ms < 500 || s_sample_count < (FRIDGE_AUDIO_SAMPLE_RATE / 2)) {
        return "too_short";
    }
    if (s_rms < 80 || s_peak_abs < 400) {
        return "silent";
    }
    if (s_i2s_timeout_count > 0 && s_sample_count < (FRIDGE_AUDIO_SAMPLE_RATE / 2)) {
        return "i2s_timeout";
    }
    return "ok";
}

esp_err_t fridge_audio_get_status(fridge_audio_status_t *out)
{
    if (!out || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = s_state;
    out->pcm_bytes = s_pcm_bytes;
    out->duration_ms = s_state == FRIDGE_AUDIO_STATE_RECORDING
                           ? (uint32_t)((esp_timer_get_time() - s_record_start_us) / 1000)
                           : s_duration_ms;
    out->rms = s_rms;
    out->min_sample = s_sample_count > 0 ? s_min_sample : 0;
    out->max_sample = s_sample_count > 0 ? s_max_sample : 0;
    out->mean_sample = s_mean_sample;
    out->peak_abs = s_peak_abs;
    out->clip_count = s_clip_count;
    out->timeout_count = s_i2s_timeout_count;
    out->sample_count = s_sample_count;
    strlcpy(out->quality_hint, quality_hint_locked(), sizeof(out->quality_hint));
    if (strcmp(out->quality_hint, "ok") == 0 || strcmp(out->quality_hint, "clipping") == 0) {
        out->error[0] = '\0';
    } else {
        strlcpy(out->error, s_error, sizeof(out->error));
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_audio_get_pcm(const int16_t **pcm, size_t *pcm_bytes, uint32_t *duration_ms)
{
    if (!pcm || !pcm_bytes || !duration_ms || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *pcm = s_pcm;
    *pcm_bytes = s_pcm_bytes;
    *duration_ms = s_duration_ms;
    bool ready = s_state == FRIDGE_AUDIO_STATE_READY && s_pcm_bytes > 0;
    xSemaphoreGive(s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}
