// 冰箱小精灵 ESP32-P4 音频采集组件。
// 统一管理板载 ES8311 与外接 FY-SP703 两路麦克风。
// ES8311 使用 I2S0 采集 16kHz PCM；FY-SP703 使用 I2S1 从机接收 16kHz/32-bit 双时隙，
// 再转换为 16kHz/16-bit/mono 公共 PCM。两路手动录音与 WakeNet 通过同一状态锁互斥。
#include "fridge_audio.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_codec_es8311.h"
#include "fridge_fy_sp703.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_TASK_STACK 6144
#define AUDIO_READ_SAMPLES 256
#define AUDIO_FY_READ_FRAMES 256
#define AUDIO_FY_PROBE_FRAMES (FRIDGE_AUDIO_FY_SAMPLE_RATE / 2)
#define AUDIO_WAKE_READ_SAMPLES 256
#define AUDIO_CLIP_THRESHOLD 32000
#define AUDIO_TIMEOUT_BACKOFF_MS 20
#define AUDIO_I2C_GATE_TIMEOUT_MS 200

// FY 读取一次返回的帧数必须能完整写入 mono 临时缓冲；修改任一宏时让编译器立即阻止栈溢出。
_Static_assert(AUDIO_FY_READ_FRAMES <= AUDIO_READ_SAMPLES,
               "FY 每次读取帧数不得超过 samples[] 容量，否则栈溢出");

static const char *TAG = "fridge_audio";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_record_task;
static bool s_record_active;
static bool s_wake_stream_active;
static bool s_realtime_stream_active;
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
static int32_t s_live_rms;
static int32_t s_live_peak_abs;
static uint32_t s_clip_count;
static uint32_t s_sample_count;
static uint64_t s_sum_squares;
static int64_t s_sum_samples;
static fridge_audio_state_t s_state = FRIDGE_AUDIO_STATE_IDLE;
static char s_error[128];
static uint32_t s_i2s_timeout_count;
static esp_err_t s_stream_stop_result = ESP_OK;
static fridge_audio_source_t s_source = FRIDGE_AUDIO_DEFAULT_SOURCE;
static fridge_audio_channel_t s_selected_channel = FRIDGE_AUDIO_CHANNEL_LEFT;
static uint64_t s_fy_left_sum_squares;
static uint64_t s_fy_right_sum_squares;
static uint32_t s_fy_channel_frames;
static bool s_common_initialized;
static bool s_es8311_ready;
// WakeNet 管理任务只有 4096 字节栈；FY 原始 32-bit stereo 批量缓冲放静态区，
// 每次读取 256 帧以跟上外部 16k LRCK，避免 64 帧碎片读取造成 I2S DMA overflow。
// 唤醒流与录音/Realtime 互斥，因此该缓冲只有一个读者。
static int32_t s_wake_fy_frames[AUDIO_WAKE_READ_SAMPLES * FRIDGE_FY_SP703_CHANNELS];

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

static uint32_t active_sample_rate_locked(void)
{
    return s_source == FRIDGE_AUDIO_SOURCE_FY_SP703
               ? FRIDGE_AUDIO_FY_SAMPLE_RATE
               : FRIDGE_AUDIO_SAMPLE_RATE;
}

static int16_t fy_raw_to_pcm16(int32_t value)
{
    return (int16_t)(value >> 16);
}

static int16_t mix_i16_saturated(int16_t left, int16_t right)
{
    int32_t mixed = ((int32_t)left + (int32_t)right) / 2;
    if (mixed > INT16_MAX) {
        return INT16_MAX;
    }
    if (mixed < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)mixed;
}

const char *fridge_audio_source_name(fridge_audio_source_t source)
{
    return source == FRIDGE_AUDIO_SOURCE_FY_SP703 ? "fy_sp703" : "es8311";
}

const char *fridge_audio_channel_name(fridge_audio_channel_t channel)
{
    switch (channel) {
    case FRIDGE_AUDIO_CHANNEL_RIGHT:
        return "right";
    case FRIDGE_AUDIO_CHANNEL_MIX:
        return "mix";
    case FRIDGE_AUDIO_CHANNEL_PROBING:
        return "probing";
    case FRIDGE_AUDIO_CHANNEL_LEFT:
    default:
        return "left";
    }
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
    s_live_rms = 0;
    s_live_peak_abs = 0;
    s_clip_count = 0;
    s_sample_count = 0;
    s_sum_squares = 0;
    s_sum_samples = 0;
    s_i2s_timeout_count = 0;
    s_stream_stop_result = ESP_OK;
    s_fy_left_sum_squares = 0;
    s_fy_right_sum_squares = 0;
    s_fy_channel_frames = 0;
    s_selected_channel = s_source == FRIDGE_AUDIO_SOURCE_FY_SP703
                             ? FRIDGE_AUDIO_CHANNEL_PROBING
                             : FRIDGE_AUDIO_CHANNEL_LEFT;
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

// 保存最近一次 DMA 读取的短窗能量。连续语音仅根据该短窗判断“仍在说话”，
// 不能使用整段累计 RMS，否则说过一句后累计值会让静音尾段误判为持续说话。
static void update_live_level_locked(const int16_t *samples, size_t sample_count)
{
    uint64_t sum_squares = 0;
    int32_t peak = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t sample = samples[i];
        int32_t absolute = abs_i16(samples[i]);
        if (absolute > peak) peak = absolute;
        sum_squares += (uint64_t)(sample * sample);
    }
    s_live_peak_abs = peak;
    s_live_rms = sample_count > 0 ?
                     (int32_t)sqrt((double)sum_squares / (double)sample_count) : 0;
}

// FY-SP703 前 500ms 同时观察左右时隙：一路能量比另一路高 12dB 以上时选择强通道，
// 否则使用饱和平均。探测完成前先用平均值，避免丢失录音开头。
static void update_fy_channel_locked(int16_t left, int16_t right)
{
    s_fy_left_sum_squares += (uint64_t)((int32_t)left * (int32_t)left);
    s_fy_right_sum_squares += (uint64_t)((int32_t)right * (int32_t)right);
    s_fy_channel_frames++;
    if (s_selected_channel != FRIDGE_AUDIO_CHANNEL_PROBING ||
        s_fy_channel_frames < AUDIO_FY_PROBE_FRAMES) {
        return;
    }

    if (s_fy_left_sum_squares > s_fy_right_sum_squares * 16ULL) {
        s_selected_channel = FRIDGE_AUDIO_CHANNEL_LEFT;
    } else if (s_fy_right_sum_squares > s_fy_left_sum_squares * 16ULL) {
        s_selected_channel = FRIDGE_AUDIO_CHANNEL_RIGHT;
    } else {
        s_selected_channel = FRIDGE_AUDIO_CHANNEL_MIX;
    }
    ESP_LOGI(TAG,
             "FY-SP703 channel selected: %s, left_energy=%llu right_energy=%llu",
             fridge_audio_channel_name(s_selected_channel),
             (unsigned long long)s_fy_left_sum_squares,
             (unsigned long long)s_fy_right_sum_squares);
}

static int16_t select_fy_sample_locked(int16_t left, int16_t right)
{
    update_fy_channel_locked(left, right);
    // 探测阶段输出 L/R 平均；探测完成后即使选择单通道也保留 /2 增益，
    // 使静音通道的“信号/2”与后续强通道输出电平连续，避免 500ms 幅度台阶。
    switch (s_selected_channel) {
    case FRIDGE_AUDIO_CHANNEL_LEFT:
        return (int16_t)((int32_t)left / 2);
    case FRIDGE_AUDIO_CHANNEL_RIGHT:
        return (int16_t)((int32_t)right / 2);
    case FRIDGE_AUDIO_CHANNEL_MIX:
    case FRIDGE_AUDIO_CHANNEL_PROBING:
    default:
        return mix_i16_saturated(left, right);
    }
}

// 63 阶 Hamming-windowed sinc，截止频率约 6kHz；Q15 系数在编译期固定，
// 对 48kHz 输入做 3:1 抽取时可明显抑制 8kHz 以上噪声混叠，且不引入额外 DSP 依赖。
static const int16_t s_fy_downsample_fir_q15[63] = {
    -4, 20, 32, 19, -17, -53, -54, 0, 80, 118, 56, -88, -207, -174, 38, 291,
    361, 117, -320, -607, -428, 211, 878, 966, 173, -1126, -1946, -1253, 1301,
    4983, 8244, 9546, 8244, 4983, 1301, -1253, -1946, -1126, 173, 966, 878, 211,
    -428, -607, -320, 117, 361, 291, 38, -174, -207, -88, 56, 118, 80, 0, -54,
    -53, -17, 19, 32, 20, -4,
};

esp_err_t fridge_audio_downsample_48k_to_16k(const int16_t *input,
                                             size_t input_samples,
                                             int16_t *output,
                                             size_t output_capacity,
                                             size_t *output_samples)
{
    if (!input || !output || !output_samples) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t count = input_samples / 3;
    if (count > output_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int center = 63 / 2;
    for (size_t out_index = 0; out_index < count; out_index++) {
        const int64_t center_input = (int64_t)(out_index * 3);
        int64_t acc = 0;
        for (int tap = 0; tap < 63; tap++) {
            int64_t input_index = center_input + tap - center;
            if (input_index >= 0 && (size_t)input_index < input_samples) {
                acc += (int64_t)input[input_index] * s_fy_downsample_fir_q15[tap];
            }
        }
        int64_t sample = (acc + (acc >= 0 ? 16384 : -16384)) / 32768;
        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        output[out_index] = (int16_t)sample;
    }
    *output_samples = count;
    return ESP_OK;
}

static void finalize_fy_channel_locked(void)
{
    if (s_selected_channel != FRIDGE_AUDIO_CHANNEL_PROBING || s_fy_channel_frames == 0) {
        return;
    }
    if (s_fy_left_sum_squares > s_fy_right_sum_squares * 16ULL) {
        s_selected_channel = FRIDGE_AUDIO_CHANNEL_LEFT;
    } else if (s_fy_right_sum_squares > s_fy_left_sum_squares * 16ULL) {
        s_selected_channel = FRIDGE_AUDIO_CHANNEL_RIGHT;
    } else {
        s_selected_channel = FRIDGE_AUDIO_CHANNEL_MIX;
    }
}

// 录音任务按当前音源读取数据。FY-SP703 的原生 32-bit 双时隙数据只在任务栈短暂存在，
// 公共 PSRAM 缓冲保存确定性 raw>>16 后的 mono PCM，供 Web 回放和 ASR 重采样。
static void record_task(void *arg)
{
    (void)arg;
    int16_t samples[AUDIO_READ_SAMPLES];
    int32_t fy_frames[AUDIO_FY_READ_FRAMES * FRIDGE_FY_SP703_CHANNELS];
    for (;;) {
        // 录音任务在音频初始化早期一次创建，平时阻塞等待通知。
        // 避免 WakeNet/ASR/TTS 已加载后再临时申请连续内部 SRAM 栈而启动失败。
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_err_t stream_result = ESP_OK;
        fridge_audio_source_t source;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        source = s_source;
        xSemaphoreGive(s_lock);

        while (true) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool recording = s_state == FRIDGE_AUDIO_STATE_RECORDING;
            size_t offset = s_pcm_bytes;
            xSemaphoreGive(s_lock);
            if (!recording || offset >= s_pcm_capacity) {
                break;
            }

            size_t samples_read = 0;
            esp_err_t err;
            if (source == FRIDGE_AUDIO_SOURCE_FY_SP703) {
                size_t frames_read = 0;
                err = fridge_fy_sp703_read(fy_frames,
                                           AUDIO_FY_READ_FRAMES,
                                           &frames_read,
                                           pdMS_TO_TICKS(250));
                if (err == ESP_OK) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    for (size_t i = 0; i < frames_read; i++) {
                        int16_t left = fy_raw_to_pcm16(fy_frames[i * 2]);
                        int16_t right = fy_raw_to_pcm16(fy_frames[i * 2 + 1]);
                        samples[i] = select_fy_sample_locked(left, right);
                    }
                    xSemaphoreGive(s_lock);
                    samples_read = frames_read;
                }
            } else {
                err = fridge_codec_es8311_read(samples,
                                               AUDIO_READ_SAMPLES,
                                               &samples_read,
                                               pdMS_TO_TICKS(250));
            }
            if (err != ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (err == ESP_ERR_TIMEOUT) {
                    // 时钟未稳定或编解码器链路异常时限频记录，避免录音任务刷爆串口并饿死其它任务。
                    s_i2s_timeout_count++;
                    strlcpy(s_error,
                            source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                ? "FY-SP703 external I2S clock/data timeout"
                                : "ES8311 i2s read timeout",
                            sizeof(s_error));
                    if (s_i2s_timeout_count == 1 || (s_i2s_timeout_count % 32) == 0) {
                        ESP_LOGW(TAG, "%s I2S read timeout, count=%lu",
                                 fridge_audio_source_name(source),
                                 (unsigned long)s_i2s_timeout_count);
                    }
                } else {
                    stream_result = err;
                    strlcpy(s_error, esp_err_to_name(err), sizeof(s_error));
                    ESP_LOGE(TAG, "%s I2S read failed: %s",
                             fridge_audio_source_name(source), esp_err_to_name(err));
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
            update_live_level_locked(write_ptr, samples_read);
            if (s_peak_abs >= 400 && s_rms >= 80) {
                s_error[0] = '\0';
            }
            s_duration_ms = (uint32_t)(s_sample_count * 1000 / active_sample_rate_locked());
            bool full = s_pcm_bytes >= s_pcm_capacity;
            xSemaphoreGive(s_lock);
            if (full) {
                ESP_LOGI(TAG, "record buffer full, auto stop");
                break;
            }
        }

        // ES8311 必须先关闭 ADC 再释放摄像头共享 I2C gate；FY-SP703 只关闭独立 I2S1 DMA。
        if (source == FRIDGE_AUDIO_SOURCE_FY_SP703) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            finalize_fy_channel_locked();
            xSemaphoreGive(s_lock);
        }
        esp_err_t stop_result = source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                    ? fridge_fy_sp703_stop()
                                    : fridge_codec_es8311_stop_input();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_stream_stop_result = stream_result != ESP_OK ? stream_result : stop_result;
        if (s_stream_stop_result != ESP_OK) {
            s_state = FRIDGE_AUDIO_STATE_ERROR;
            strlcpy(s_error, esp_err_to_name(s_stream_stop_result), sizeof(s_error));
        } else if (s_state == FRIDGE_AUDIO_STATE_RECORDING) {
            s_state = s_pcm_bytes > 0 ? FRIDGE_AUDIO_STATE_READY : FRIDGE_AUDIO_STATE_IDLE;
        }
        s_record_active = false;
        xSemaphoreGive(s_lock);

        if (stop_result != ESP_OK) {
            ESP_LOGE(TAG, "stop %s input failed: %s",
                     fridge_audio_source_name(source), esp_err_to_name(stop_result));
        }
    }
}

esp_err_t fridge_audio_init(void)
{
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

    if (!s_record_task) {
        // 录音 worker 开机早期一次创建并使用 PSRAM 栈，避免运行期临时建任务失败，
        // 同时把内部 DMA SRAM 留给 I2S、SDMMC 和 WakeNet；I2S DMA 缓冲仍由驱动分配在内部内存。
        BaseType_t task_ok = xTaskCreateWithCaps(record_task,
                                                 "audio_record",
                                                 AUDIO_TASK_STACK,
                                                 NULL,
                                                 6,
                                                 &s_record_task,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "create persistent audio record worker failed: psram_free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "persistent audio record worker ready (PSRAM stack)");
    }

    s_common_initialized = true;

    if (s_es8311_ready) {
        return ESP_OK;
    }

    esp_err_t err = fridge_codec_es8311_init();
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        return err;
    }

    s_es8311_ready = true;
    ESP_LOGI(TAG, "board ES8311 microphone ready: 16000Hz/16-bit/mono, direct int16 PCM");
    return ESP_OK;
}

esp_err_t fridge_audio_start_recording(void)
{
    // 无 source 参数默认使用 FY-SP703；显式 ES8311 仍可经 start_recording_source 选择。
    return fridge_audio_start_recording_source(FRIDGE_AUDIO_DEFAULT_SOURCE);
}

esp_err_t fridge_audio_start_recording_source(fridge_audio_source_t source)
{
    if (source != FRIDGE_AUDIO_SOURCE_ES8311 && source != FRIDGE_AUDIO_SOURCE_FY_SP703) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_FRIDGE_FY_SP703_ENABLE
    if (source == FRIDGE_AUDIO_SOURCE_FY_SP703) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    esp_err_t err;
    if (source == FRIDGE_AUDIO_SOURCE_ES8311) {
        err = fridge_audio_init();
    } else {
        // FY-SP703 即使板载 ES8311 初始化失败也可独立工作；先保证公共锁和 PSRAM 缓冲存在。
        if (!s_common_initialized) {
            err = fridge_audio_init();
            if (err != ESP_OK && (!s_lock || !s_pcm)) {
                return err;
            }
        }
        err = fridge_fy_sp703_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_state == FRIDGE_AUDIO_STATE_RECORDING ||
                s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING ||
                s_wake_stream_active || s_realtime_stream_active || s_record_active;
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    err = source == FRIDGE_AUDIO_SOURCE_FY_SP703
              ? fridge_fy_sp703_start()
              : fridge_codec_es8311_start_input(pdMS_TO_TICKS(AUDIO_I2C_GATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        if (source == FRIDGE_AUDIO_SOURCE_ES8311 && err == ESP_ERR_TIMEOUT) {
            set_state(FRIDGE_AUDIO_STATE_ERROR, "camera/ES8311 shared I2C busy");
        } else {
            set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        }
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    busy = s_state == FRIDGE_AUDIO_STATE_RECORDING ||
           s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING ||
           s_wake_stream_active || s_realtime_stream_active || s_record_active;
    if (!busy) {
        s_source = source;
        s_pcm_bytes = 0;
        reset_stats_locked();
        s_error[0] = '\0';
        s_record_start_us = esp_timer_get_time();
        s_state = FRIDGE_AUDIO_STATE_RECORDING;
        s_record_active = true;
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        esp_err_t stop_err = source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                 ? fridge_fy_sp703_stop()
                                 : fridge_codec_es8311_stop_input();
        return stop_err == ESP_OK ? ESP_ERR_INVALID_STATE : stop_err;
    }

    xTaskNotifyGive(s_record_task);
    ESP_LOGI(TAG, "recording started: source=%s sample_rate=%luHz",
             fridge_audio_source_name(source),
             (unsigned long)(source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                 ? FRIDGE_AUDIO_FY_SAMPLE_RATE
                                 : FRIDGE_AUDIO_SAMPLE_RATE));
    return ESP_OK;
}

esp_err_t fridge_audio_stop_recording(void)
{
    if (!s_common_initialized) {
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
        bool running = s_record_active;
        xSemaphoreGive(s_lock);
        if (!running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool still_running = s_record_active;
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
    // 唤醒监听默认使用 FY-SP703（I2S1 从机、16k 直通），不依赖 ES8311 与摄像头共享 I2C。
    // 配置关闭 FY 时明确返回不支持，避免静默回退到 ES 造成音源不一致。
#if !CONFIG_FRIDGE_FY_SP703_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#endif
    const fridge_audio_source_t source = FRIDGE_AUDIO_DEFAULT_SOURCE;

    esp_err_t err;
    if (source == FRIDGE_AUDIO_SOURCE_ES8311) {
        err = fridge_audio_init();
    } else {
        // FY-SP703 即使板载 ES8311 初始化失败也可独立工作；先保证公共锁和 PSRAM 缓冲存在。
        if (!s_common_initialized) {
            err = fridge_audio_init();
            if (err != ESP_OK && (!s_lock || !s_pcm)) {
                return err;
            }
        }
        err = fridge_fy_sp703_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active ||
                s_realtime_stream_active || s_record_active;
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    err = source == FRIDGE_AUDIO_SOURCE_FY_SP703
              ? fridge_fy_sp703_start()
              : fridge_codec_es8311_start_input(pdMS_TO_TICKS(AUDIO_I2C_GATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        if (source == FRIDGE_AUDIO_SOURCE_ES8311 && err == ESP_ERR_TIMEOUT) {
            set_state(FRIDGE_AUDIO_STATE_ERROR, "camera/ES8311 shared I2C busy");
        } else {
            set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        }
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    busy = s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active ||
           s_realtime_stream_active || s_record_active;
    if (!busy) {
        // 锁内先落实际 source 再清统计，保证 read/stop 能按正确音源对称收尾。
        s_source = source;
        s_wake_stream_active = true;
        s_state = FRIDGE_AUDIO_STATE_WAKE_LISTENING;
        s_pcm_bytes = 0;
        s_error[0] = '\0';
        reset_stats_locked();
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        // 启动前检查通过但启动后出现竞争：按实际源停止已打开的链路，不自动回退。
        esp_err_t stop_err = source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                 ? fridge_fy_sp703_stop()
                                 : fridge_codec_es8311_stop_input();
        return stop_err == ESP_OK ? ESP_ERR_INVALID_STATE : stop_err;
    }

    // 启动成功只打一条包含 source/sample rate 的日志。
    ESP_LOGI(TAG, "wake stream started: source=%s sample_rate=%luHz",
             fridge_audio_source_name(source),
             (unsigned long)(source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                 ? FRIDGE_AUDIO_FY_SAMPLE_RATE
                                 : FRIDGE_AUDIO_SAMPLE_RATE));
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_read(int16_t *out_samples,
                                        size_t max_samples,
                                        size_t *sample_count,
                                        TickType_t timeout_ticks)
{
    if (!out_samples || !sample_count || max_samples == 0 || !s_common_initialized || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample_count = 0;

    // 只在锁内快照激活状态与本次音源；I2S/驱动读取绝不能持锁进行，避免阻塞其它录音入口。
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_wake_stream_active && s_state == FRIDGE_AUDIO_STATE_WAKE_LISTENING;
    fridge_audio_source_t source = s_source;
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

        size_t got = 0;
        esp_err_t err;
        if (source == FRIDGE_AUDIO_SOURCE_FY_SP703) {
            // 与录音任务一致按最多 256 帧批量读取；原始缓冲在静态区，不占 WakeNet 管理任务栈。
            err = fridge_fy_sp703_read(s_wake_fy_frames, wanted, &got, timeout_ticks);
            if (err == ESP_OK && got > 0) {
                // 短锁内完成 L/R 探测与 mono 合并，直接写调用者缓冲，避免额外 mono 临时数组。
                xSemaphoreTake(s_lock, portMAX_DELAY);
                for (size_t i = 0; i < got; i++) {
                    int16_t left = fy_raw_to_pcm16(s_wake_fy_frames[i * 2]);
                    int16_t right = fy_raw_to_pcm16(s_wake_fy_frames[i * 2 + 1]);
                    out_samples[total_samples + i] = select_fy_sample_locked(left, right);
                }
                xSemaphoreGive(s_lock);
                total_samples += got;
            }
        } else {
            err = fridge_codec_es8311_read(&out_samples[total_samples], wanted, &got, timeout_ticks);
            if (err == ESP_OK && got > 0) {
                total_samples += got;
            }
        }

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
        if (got == 0) {
            break;
        }
    }

    // 统一更新累计统计与实时电平；时长按本次实际音源的公共采样率换算。
    xSemaphoreTake(s_lock, portMAX_DELAY);
    update_stats_locked(out_samples, total_samples);
    update_live_level_locked(out_samples, total_samples);
    if (s_peak_abs >= 400 && s_rms >= 80) {
        s_error[0] = '\0';
    }
    s_duration_ms = (uint32_t)(s_sample_count * 1000 / active_sample_rate_locked());
    xSemaphoreGive(s_lock);
    *sample_count = total_samples;
    return ESP_OK;
}

esp_err_t fridge_audio_wake_stream_stop(void)
{
    if (!s_common_initialized || !s_lock) {
        return ESP_OK;
    }

    // 快照实际音源并清 active，再按该音源对称收尾。
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_active = s_wake_stream_active;
    fridge_audio_source_t source = s_source;
    s_wake_stream_active = false;
    xSemaphoreGive(s_lock);
    if (!was_active) {
        return ESP_OK;
    }

    esp_err_t err;
    if (source == FRIDGE_AUDIO_SOURCE_FY_SP703) {
        // FY 先收尾 500ms L/R 通道探测，再关闭独立 I2S1 DMA；与 record_task 停止顺序一致。
        xSemaphoreTake(s_lock, portMAX_DELAY);
        finalize_fy_channel_locked();
        xSemaphoreGive(s_lock);
        err = fridge_fy_sp703_stop();
    } else {
        err = fridge_codec_es8311_stop_input();
    }
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        return err;
    }
    set_state(FRIDGE_AUDIO_STATE_IDLE, NULL);
    return ESP_OK;
}

// GLM Realtime 双工流：与短录音/WakeNet 互斥，内部走 ES8311 duplex。
esp_err_t fridge_audio_realtime_stream_start(void)
{
    esp_err_t err = fridge_audio_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_es8311_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active ||
                s_realtime_stream_active || s_record_active;
    xSemaphoreGive(s_lock);
    if (busy) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, "audio busy (record/wake/realtime)");
        return ESP_ERR_INVALID_STATE;
    }

    err = fridge_codec_es8311_start_duplex(pdMS_TO_TICKS(AUDIO_I2C_GATE_TIMEOUT_MS));
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR,
                  err == ESP_ERR_TIMEOUT ? "camera/ES8311 shared I2C busy" : esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    busy = s_state == FRIDGE_AUDIO_STATE_RECORDING || s_wake_stream_active ||
           s_realtime_stream_active || s_record_active;
    if (!busy) {
        s_source = FRIDGE_AUDIO_SOURCE_ES8311;
        s_realtime_stream_active = true;
        s_state = FRIDGE_AUDIO_STATE_WAKE_LISTENING;  // 复用连续读状态
        s_pcm_bytes = 0;
        s_error[0] = '\0';
        reset_stats_locked();
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        (void)fridge_codec_es8311_stop_duplex();
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "realtime duplex stream started (ES8311 16k mono)");
    return ESP_OK;
}

esp_err_t fridge_audio_realtime_stream_read(int16_t *out_samples,
                                            size_t max_samples,
                                            size_t *sample_count,
                                            TickType_t timeout_ticks)
{
    if (!out_samples || !sample_count || max_samples == 0 || !s_es8311_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample_count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_realtime_stream_active;
    xSemaphoreGive(s_lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = 0;
    while (total < max_samples) {
        size_t wanted = max_samples - total;
        if (wanted > AUDIO_WAKE_READ_SAMPLES) {
            wanted = AUDIO_WAKE_READ_SAMPLES;
        }
        size_t got = 0;
        esp_err_t err = fridge_codec_es8311_read(&out_samples[total], wanted, &got, timeout_ticks);
        if (err != ESP_OK) {
            if (total > 0) {
                break;
            }
            return err;
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    *sample_count = total;
    return ESP_OK;
}

esp_err_t fridge_audio_realtime_stream_stop(void)
{
    if (!s_common_initialized || !s_lock) {
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_realtime_stream_active;
    s_realtime_stream_active = false;
    xSemaphoreGive(s_lock);
    if (!was) {
        return ESP_OK;
    }
    esp_err_t err = fridge_codec_es8311_stop_duplex();
    if (err != ESP_OK) {
        set_state(FRIDGE_AUDIO_STATE_ERROR, esp_err_to_name(err));
        return err;
    }
    set_state(FRIDGE_AUDIO_STATE_IDLE, NULL);
    return ESP_OK;
}

esp_err_t fridge_audio_realtime_play_pcm(const int16_t *samples, size_t sample_count)
{
    if (!samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_realtime_stream_active;
    xSemaphoreGive(s_lock);
    if (!active) {
        return ESP_ERR_INVALID_STATE;
    }
    return fridge_codec_es8311_write(samples, sample_count, pdMS_TO_TICKS(200));
}

esp_err_t fridge_audio_realtime_playback_stop(void)
{
    // 只关 PA，保持 duplex ADC 继续上行，便于 barge-in。
    return fridge_codec_es8311_pa_off();
}

bool fridge_audio_realtime_is_active(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_realtime_stream_active;
    xSemaphoreGive(s_lock);
    return active;
}

// 板载扬声器自检：用 16 点正弦查表产生 1kHz 音调，避免在测试路径引入浮点实时计算。
// PA_CTRL=GPIO11 只会在首个 PCM 写入时拉高，结束后先写静音尾帧再关闭功放和 I2S。
esp_err_t fridge_audio_speaker_beep_test(void)
{
    static const int16_t tone_lut[16] = {
        0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
        0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
    };
    int16_t frame[320];  // 20ms @ 16kHz；仅使用小栈缓冲，避免占用内部 DMA 堆。

    esp_err_t err = fridge_audio_realtime_stream_start();
    if (err != ESP_OK) {
        return err;
    }

    for (size_t offset = 0; offset < FRIDGE_AUDIO_SAMPLE_RATE / 2; offset += 320) {
        for (size_t i = 0; i < 320; ++i) {
            frame[i] = tone_lut[(offset + i) % 16];
        }
        err = fridge_audio_realtime_play_pcm(frame, 320);
        if (err != ESP_OK) {
            break;
        }
    }

    if (err == ESP_OK) {
        // 静音尾帧减轻停机爆音；短暂等待 DMA 把最后一帧送完后再关闭 PA。
        memset(frame, 0, sizeof(frame));
        err = fridge_audio_realtime_play_pcm(frame, 320);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    esp_err_t stop_err = fridge_audio_realtime_stream_stop();
    ESP_LOGI(TAG, "speaker beep test finished: play=%s stop=%s",
             esp_err_to_name(err), esp_err_to_name(stop_err));
    return err != ESP_OK ? err : stop_err;
}

static const char *quality_hint_locked(void)
{
    uint32_t sample_rate = active_sample_rate_locked();
    if (s_sample_count == 0) {
        return "too_short";
    }
    if (s_clip_count > 0) {
        return "clipping";
    }
    if (s_duration_ms < 500 || s_sample_count < (sample_rate / 2)) {
        return "too_short";
    }
    if (s_rms < 80 || s_peak_abs < 400) {
        return "silent";
    }
    if (s_i2s_timeout_count > 0 && s_sample_count < (sample_rate / 2)) {
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
    out->source = s_source;
    out->pcm_bytes = s_pcm_bytes;
    out->duration_ms = s_state == FRIDGE_AUDIO_STATE_RECORDING
                           ? (uint32_t)((esp_timer_get_time() - s_record_start_us) / 1000)
                           : s_duration_ms;
    out->rms = s_rms;
    out->min_sample = s_sample_count > 0 ? s_min_sample : 0;
    out->max_sample = s_sample_count > 0 ? s_max_sample : 0;
    out->mean_sample = s_mean_sample;
    out->peak_abs = s_peak_abs;
    out->live_rms = s_live_rms;
    out->live_peak_abs = s_live_peak_abs;
    out->clip_count = s_clip_count;
    out->timeout_count = s_i2s_timeout_count;
    out->sample_count = s_sample_count;
    out->native_sample_rate = s_source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                  ? FRIDGE_FY_SP703_SAMPLE_RATE
                                  : FRIDGE_AUDIO_SAMPLE_RATE;
    out->native_bits_per_sample = s_source == FRIDGE_AUDIO_SOURCE_FY_SP703
                                      ? FRIDGE_FY_SP703_BITS_PER_SAMPLE
                                      : 16;
    out->native_channels = s_source == FRIDGE_AUDIO_SOURCE_FY_SP703
                               ? FRIDGE_FY_SP703_CHANNELS
                               : 1;
    out->selected_channel = s_selected_channel;
    if (s_source == FRIDGE_AUDIO_SOURCE_FY_SP703 && s_fy_channel_frames > 0) {
        out->left_rms = (int32_t)sqrt((double)s_fy_left_sum_squares /
                                     (double)s_fy_channel_frames);
        out->right_rms = (int32_t)sqrt((double)s_fy_right_sum_squares /
                                      (double)s_fy_channel_frames);
        fridge_fy_sp703_status_t fy_status = {0};
        fridge_fy_sp703_get_status(&fy_status);
        out->overflow_count = fy_status.overflow_count;
    } else {
        out->left_rms = s_rms;
        out->right_rms = 0;
        out->overflow_count = 0;
    }
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

esp_err_t fridge_audio_get_pcm_format(fridge_audio_pcm_format_t *out)
{
    if (!out || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->source = s_source;
    out->sample_rate = active_sample_rate_locked();
    out->bits_per_sample = 16;
    out->channels = 1;
    bool ready = s_state == FRIDGE_AUDIO_STATE_READY && s_pcm_bytes > 0;
    xSemaphoreGive(s_lock);
    return ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}
