// 冰箱小精灵 FY-SP703 降噪麦克风驱动。
// 模组是 I2S 主机：LRCK=GPIO7、BCLK=GPIO8、SDIO=GPIO9；P4 使用 I2S1 从机 RX。

#include "fridge_fy_sp703.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "fridge_board_pins.h"

#define FY_I2S_PORT I2S_NUM_1
#define FY_DMA_DESC_NUM 6
#define FY_DMA_FRAME_NUM 256

static const char *TAG = "fy_sp703";
// I2S 从机内部接收时序沿用此前已验证能采到有效数据位的 48k 配置。
// 外部真实帧率仍由 FY-SP703 的 16k LRCK 决定，上层不得据此重采样。
#define FY_SP703_I2S_RX_TIMING_RATE 48000
static i2s_chan_handle_t s_rx_chan;
static bool s_initialized;
static bool s_active;
static atomic_uint_fast32_t s_overflow_count = ATOMIC_VAR_INIT(0);

// DMA 接收队列溢出只累加计数，不能在 ISR 回调里打印日志或执行耗时操作。
static bool on_recv_queue_overflow(i2s_chan_handle_t handle,
                                   i2s_event_data_t *event,
                                   void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    atomic_fetch_add_explicit(&s_overflow_count, 1, memory_order_relaxed);
    return false;
}

esp_err_t fridge_fy_sp703_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(FY_I2S_PORT, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = FY_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = FY_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create I2S1 slave RX failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FY_SP703_I2S_RX_TIMING_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = FRIDGE_PIN_FY_I2S_BCLK,
            .ws = FRIDGE_PIN_FY_I2S_LRCK,
            .dout = I2S_GPIO_UNUSED,
            .din = FRIDGE_PIN_FY_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // 保留 APLL 与已验证的 48k 接收时序配置；真实采样速度仍由 FY-SP703
    // 输出的 16kHz LRCK/BCLK 决定，P4 不会反向输出时钟或重采样。
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initialize FY-SP703 I2S1 STD mode failed: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    const i2s_event_callbacks_t callbacks = {
        .on_recv_q_ovf = on_recv_queue_overflow,
    };
    err = i2s_channel_register_event_callback(s_rx_chan, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register FY-SP703 I2S callbacks failed: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "FY-SP703 ready: I2S1 slave RX, LRCK=GPIO%d BCLK=GPIO%d DIN=GPIO%d, %luHz/32-bit/Philips/stereo slots",
             FRIDGE_PIN_FY_I2S_LRCK,
             FRIDGE_PIN_FY_I2S_BCLK,
             FRIDGE_PIN_FY_I2S_DIN,
             (unsigned long)FRIDGE_FY_SP703_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t fridge_fy_sp703_start(void)
{
    esp_err_t err = fridge_fy_sp703_init();
    if (err != ESP_OK) {
        return err;
    }
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_overflow_count, 0, memory_order_relaxed);
    err = i2s_channel_enable(s_rx_chan);
    if (err == ESP_OK) {
        s_active = true;
    }
    return err;
}

esp_err_t fridge_fy_sp703_stop(void)
{
    if (!s_initialized || !s_rx_chan || !s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_active = false;
    return i2s_channel_disable(s_rx_chan);
}

esp_err_t fridge_fy_sp703_read(int32_t *out_interleaved,
                               size_t max_frames,
                               size_t *out_frames,
                               TickType_t timeout_ticks)
{
    if (!out_interleaved || !out_frames || max_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_frames = 0;
    if (!s_active || !s_rx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t timeout_ms = timeout_ticks == portMAX_DELAY
                              ? UINT32_MAX
                              : (uint32_t)pdTICKS_TO_MS(timeout_ticks);
    if (timeout_ticks > 0 && timeout_ms == 0) {
        timeout_ms = 1;
    }

    size_t bytes_read = 0;
    const size_t frame_bytes = FRIDGE_FY_SP703_CHANNELS * sizeof(int32_t);
    esp_err_t err = i2s_channel_read(s_rx_chan,
                                     out_interleaved,
                                     max_frames * frame_bytes,
                                     &bytes_read,
                                     timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if ((bytes_read % frame_bytes) != 0) {
        ESP_LOGE(TAG, "unaligned FY-SP703 I2S frame length: %u", (unsigned)bytes_read);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_frames = bytes_read / frame_bytes;
    return ESP_OK;
}

void fridge_fy_sp703_get_status(fridge_fy_sp703_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->active = s_active;
    out->overflow_count = (uint32_t)atomic_load_explicit(&s_overflow_count,
                                                         memory_order_relaxed);
}
