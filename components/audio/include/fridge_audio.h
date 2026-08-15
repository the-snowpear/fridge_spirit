#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_AUDIO_SAMPLE_RATE 16000
#define FRIDGE_AUDIO_FY_SAMPLE_RATE 16000
#define FRIDGE_AUDIO_MAX_RECORD_SECONDS 6
#define FRIDGE_AUDIO_MAX_PCM_BYTES (FRIDGE_AUDIO_FY_SAMPLE_RATE * sizeof(int16_t) * FRIDGE_AUDIO_MAX_RECORD_SECONDS)

typedef enum {
    FRIDGE_AUDIO_SOURCE_ES8311 = 0,
    FRIDGE_AUDIO_SOURCE_FY_SP703,
} fridge_audio_source_t;

// 默认录音/唤醒音源：当前为外接 FY-SP703（I2S1 从机、16k 直通）。
// 显式指定 ES8311 仍可用作回退；FY 内部 48kHz RX timing/APLL 裕量不得改动。
#define FRIDGE_AUDIO_DEFAULT_SOURCE FRIDGE_AUDIO_SOURCE_FY_SP703

typedef enum {
    FRIDGE_AUDIO_CHANNEL_LEFT = 0,
    FRIDGE_AUDIO_CHANNEL_RIGHT,
    FRIDGE_AUDIO_CHANNEL_MIX,
    FRIDGE_AUDIO_CHANNEL_PROBING,
} fridge_audio_channel_t;

typedef enum {
    FRIDGE_AUDIO_STATE_IDLE = 0,
    FRIDGE_AUDIO_STATE_RECORDING,
    FRIDGE_AUDIO_STATE_WAKE_LISTENING,
    FRIDGE_AUDIO_STATE_READY,
    FRIDGE_AUDIO_STATE_ERROR,
} fridge_audio_state_t;

typedef struct {
    fridge_audio_state_t state;
    fridge_audio_source_t source;
    size_t pcm_bytes;
    uint32_t duration_ms;
    int32_t rms;
    int16_t min_sample;
    int16_t max_sample;
    int32_t mean_sample;
    int32_t peak_abs;
    // 最近一帧（约 16ms）的能量，仅用于本地 VAD；不替代整段录音质量统计。
    int32_t live_rms;
    int32_t live_peak_abs;
    uint32_t clip_count;
    uint32_t timeout_count;
    uint32_t overflow_count;
    uint32_t sample_count;
    uint32_t native_sample_rate;
    uint8_t native_bits_per_sample;
    uint8_t native_channels;
    fridge_audio_channel_t selected_channel;
    int32_t left_rms;
    int32_t right_rms;
    char quality_hint[24];
    char error[128];
} fridge_audio_status_t;

typedef struct {
    fridge_audio_source_t source;
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
} fridge_audio_pcm_format_t;

// 初始化麦克风采集链路。
// P4：板载驻极体 MIC + ES8311，I2C1=GPIO28/29、I2S0=GPIO2-6，输出 16kHz/16-bit/mono PCM；
//      录音期间持续持有摄像头共享 I2C gate，PA_CTRL=GPIO11 始终保持低。
// S3 存档：外接 INMP441，SCK=GPIO40、WS=GPIO41、SD=GPIO42、L/R=GND。
esp_err_t fridge_audio_init(void);

// 开始录音；录音缓冲优先使用 PSRAM，最长 6 秒，避免占用过多内部 RAM。
// 无 source 参数时默认使用 FY-SP703，显式 ES8311 仍可经 start_recording_source 选择。
esp_err_t fridge_audio_start_recording(void);

// 按指定音源开始一次录音。FY-SP703 使用 I2S1 从机接收 16k/32-bit 双时隙，
// 公共缓冲保存转换后的 16k/16-bit/mono PCM；无 source 参数的旧调用默认 FY-SP703，
// 显式 ES8311 仍可选作回退。
esp_err_t fridge_audio_start_recording_source(fridge_audio_source_t source);

// 停止录音并保留 PCM 数据，供 ASR 组件封装 WAV 上传。
esp_err_t fridge_audio_stop_recording(void);

esp_err_t fridge_audio_get_status(fridge_audio_status_t *out);

// 读取录音缓冲指针。调用者只读，不负责释放；下一次 start 会覆盖旧数据。
esp_err_t fridge_audio_get_pcm(const int16_t **pcm, size_t *pcm_bytes, uint32_t *duration_ms);

// 返回最近录音公共 PCM 的实际格式；ES8311 与 FY-SP703 均为 16k/16-bit/mono。
esp_err_t fridge_audio_get_pcm_format(fridge_audio_pcm_format_t *out);

// 兼容旧 FY-SP703 48kHz 固件：以固定 63 阶低通 FIR 抽取为 16kHz。
// 当前 16kHz 固件必须直接使用原始 PCM，不得调用该函数重复抽取。
esp_err_t fridge_audio_downsample_48k_to_16k(const int16_t *input,
                                             size_t input_samples,
                                             int16_t *output,
                                             size_t output_capacity,
                                             size_t *output_samples);

const char *fridge_audio_source_name(fridge_audio_source_t source);
const char *fridge_audio_channel_name(fridge_audio_channel_t channel);

// 启动唤醒词监听用的连续 PCM 流（默认使用 FY-SP703，16k/16-bit/mono 直通）。
// 该模式和短录音互斥，避免同一 I2S RX 被重复 enable；FY 不占用摄像头共享 I2C。
esp_err_t fridge_audio_wake_stream_start(void);
esp_err_t fridge_audio_wake_stream_read(int16_t *out_samples, size_t max_samples, size_t *sample_count, TickType_t timeout_ticks);
esp_err_t fridge_audio_wake_stream_stop(void);

// ---- GLM Realtime 连续双工流（与短录音/WakeNet 互斥）----
// 内部走 ES8311 duplex：16kHz mono 上行读取 + 16kHz mono 下行写入；
// 会话期间持有 I2C gate，摄像头访问应失败/等待。
esp_err_t fridge_audio_realtime_stream_start(void);
esp_err_t fridge_audio_realtime_stream_read(int16_t *out_samples,
                                            size_t max_samples,
                                            size_t *sample_count,
                                            TickType_t timeout_ticks);
esp_err_t fridge_audio_realtime_stream_stop(void);
esp_err_t fridge_audio_realtime_play_pcm(const int16_t *samples, size_t sample_count);
esp_err_t fridge_audio_realtime_playback_stop(void);
bool fridge_audio_realtime_is_active(void);

// 播放板载扬声器硬件自检音：1kHz、约 500ms。
// 该接口临时占用 ES8311 双工链路和 PA_CTRL=GPIO11，与录音/WakeNet/Realtime 互斥。
esp_err_t fridge_audio_speaker_beep_test(void);

#ifdef __cplusplus
}
#endif
