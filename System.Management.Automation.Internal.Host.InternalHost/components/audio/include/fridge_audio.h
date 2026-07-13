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
#define FRIDGE_AUDIO_MAX_RECORD_SECONDS 6
#define FRIDGE_AUDIO_MAX_PCM_BYTES (FRIDGE_AUDIO_SAMPLE_RATE * sizeof(int16_t) * FRIDGE_AUDIO_MAX_RECORD_SECONDS)

typedef enum {
    FRIDGE_AUDIO_STATE_IDLE = 0,
    FRIDGE_AUDIO_STATE_RECORDING,
    FRIDGE_AUDIO_STATE_WAKE_LISTENING,
    FRIDGE_AUDIO_STATE_READY,
    FRIDGE_AUDIO_STATE_ERROR,
} fridge_audio_state_t;

typedef struct {
    fridge_audio_state_t state;
    size_t pcm_bytes;
    uint32_t duration_ms;
    int32_t rms;
    int16_t min_sample;
    int16_t max_sample;
    int32_t mean_sample;
    int32_t peak_abs;
    uint32_t clip_count;
    uint32_t timeout_count;
    uint32_t sample_count;
    char quality_hint[24];
    char error[128];
} fridge_audio_status_t;

// 初始化麦克风采集链路。
// P4：板载驻极体 MIC + ES8311，I2C1=GPIO28/29、I2S0=GPIO2-6，输出 16kHz/16-bit/mono PCM；
//      录音期间持续持有摄像头共享 I2C gate，PA_CTRL=GPIO11 始终保持低。
// S3 存档：外接 INMP441，SCK=GPIO40、WS=GPIO41、SD=GPIO42、L/R=GND。
esp_err_t fridge_audio_init(void);

// 开始录音；录音缓冲优先使用 PSRAM，最长 6 秒，避免占用过多内部 RAM。
esp_err_t fridge_audio_start_recording(void);

// 停止录音并保留 PCM 数据，供 ASR 组件封装 WAV 上传。
esp_err_t fridge_audio_stop_recording(void);

esp_err_t fridge_audio_get_status(fridge_audio_status_t *out);

// 读取录音缓冲指针。调用者只读，不负责释放；下一次 start 会覆盖旧数据。
esp_err_t fridge_audio_get_pcm(const int16_t **pcm, size_t *pcm_bytes, uint32_t *duration_ms);

// 启动唤醒词监听用的连续 PCM 流。该模式和短录音互斥，避免同一 I2S RX 被重复 enable。
esp_err_t fridge_audio_wake_stream_start(void);
esp_err_t fridge_audio_wake_stream_read(int16_t *out_samples, size_t max_samples, size_t *sample_count, TickType_t timeout_ticks);
esp_err_t fridge_audio_wake_stream_stop(void);

#ifdef __cplusplus
}
#endif
