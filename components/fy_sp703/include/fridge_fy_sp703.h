#pragma once

// FY-SP703 降噪麦克风 I2S 接收驱动。
// 当前模组固件输出 16kHz/32-bit/Philips 双时隙音频，并负责产生 LRCK/BCLK；
// ESP32-P4 的 I2S1 只作为从机接收，禁止向 GPIO7/8 输出时钟。

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_FY_SP703_SAMPLE_RATE 16000
#define FRIDGE_FY_SP703_BITS_PER_SAMPLE 32
#define FRIDGE_FY_SP703_CHANNELS 2

typedef struct {
    uint32_t overflow_count;
    bool initialized;
    bool active;
} fridge_fy_sp703_status_t;

// 仅创建 I2S1 RX 通道，不启动 DMA。FY-SP703 未接线时初始化仍可成功，
// 实际外部时钟是否存在由 read 的超时统计判断。
esp_err_t fridge_fy_sp703_init(void);

// 开启/关闭 I2S1 RX DMA。每次开始录音时清零 DMA 溢出计数。
esp_err_t fridge_fy_sp703_start(void);
esp_err_t fridge_fy_sp703_stop(void);

// 读取交错的 32-bit 左/右时隙帧；out_interleaved 至少容纳 max_frames * 2 个 int32_t。
esp_err_t fridge_fy_sp703_read(int32_t *out_interleaved,
                               size_t max_frames,
                               size_t *out_frames,
                               TickType_t timeout_ticks);

void fridge_fy_sp703_get_status(fridge_fy_sp703_status_t *out);

#ifdef __cplusplus
}
#endif
