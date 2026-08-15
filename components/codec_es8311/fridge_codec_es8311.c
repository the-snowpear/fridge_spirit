// 冰箱小精灵 ESP32-P4 板载 ES8311 编解码器适配层。
// 支持 ADC 录音与 Realtime 双工（ADC+DAC）；NS4150B 的 PA_CTRL 默认拉低，仅在写 PCM 时短暂打开。
// 统一管理摄像头/编解码器共享 I2C1 gate。
#include "fridge_codec_es8311.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "fridge_board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CODEC_I2C_PORT I2C_NUM_1
#define CODEC_I2S_PORT I2S_NUM_0
#define CODEC_SAMPLE_RATE_HZ 16000
#define CODEC_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_384
#define CODEC_MIC_GAIN_DB 30.0f
#define CODEC_OUT_VOL 70
#define CODEC_DMA_DESC_NUM 4
#define CODEC_DMA_FRAME_NUM 256

static const char *TAG = "fridge_es8311";

static SemaphoreHandle_t s_i2c_gate;
// RX/TX 分锁允许 I2S 全双工并行；启动/停止同时持有两把锁，避免关闭正在读写的 codec。
// 固定顺序为 RX -> TX，禁止反向获取，防止停止路径与业务读写形成死锁。
static SemaphoreHandle_t s_rx_io_lock;
static SemaphoreHandle_t s_tx_io_lock;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static fridge_codec_i2c_gate_owner_t s_i2c_gate_owner = FRIDGE_CODEC_I2C_GATE_OWNER_NONE;
static i2c_master_bus_handle_t s_i2c_bus;

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_codec_dev;
static bool s_initialized;
static bool s_input_active;
static bool s_duplex_active;
static bool s_output_active;
static bool s_pa_on;

// 二值 gate 允许启动任务申请、录音任务释放，因此不能改成要求同任务释放的 mutex。
static esp_err_t ensure_i2c_gate(void)
{
    if (s_i2c_gate) {
        return ESP_OK;
    }

    SemaphoreHandle_t new_gate = xSemaphoreCreateBinary();
    if (!new_gate) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreGive(new_gate) != pdTRUE) {
        vSemaphoreDelete(new_gate);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_state_mux);
    if (!s_i2c_gate) {
        s_i2c_gate = new_gate;
        new_gate = NULL;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (new_gate) {
        vSemaphoreDelete(new_gate);
    }
    return ESP_OK;
}

esp_err_t fridge_codec_shared_i2c_gate_lock(fridge_codec_i2c_gate_owner_t owner, TickType_t timeout_ticks)
{
    if (owner == FRIDGE_CODEC_I2C_GATE_OWNER_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_i2c_gate();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_i2c_gate, timeout_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "shared I2C gate busy: requested=%d current=%d",
                 owner, fridge_codec_shared_i2c_gate_get_owner());
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_i2c_gate_owner = owner;
    portEXIT_CRITICAL(&s_state_mux);
    return ESP_OK;
}

esp_err_t fridge_codec_shared_i2c_gate_unlock(fridge_codec_i2c_gate_owner_t owner)
{
    if (!s_i2c_gate || owner == FRIDGE_CODEC_I2C_GATE_OWNER_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    bool owner_matches = false;
    portENTER_CRITICAL(&s_state_mux);
    if (s_i2c_gate_owner == owner) {
        s_i2c_gate_owner = FRIDGE_CODEC_I2C_GATE_OWNER_NONE;
        owner_matches = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (!owner_matches) {
        ESP_LOGE(TAG, "shared I2C gate owner mismatch: release=%d current=%d",
                 owner, fridge_codec_shared_i2c_gate_get_owner());
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreGive(s_i2c_gate) == pdTRUE ? ESP_OK : ESP_FAIL;
}

fridge_codec_i2c_gate_owner_t fridge_codec_shared_i2c_gate_get_owner(void)
{
    fridge_codec_i2c_gate_owner_t owner;
    portENTER_CRITICAL(&s_state_mux);
    owner = s_i2c_gate_owner;
    portEXIT_CRITICAL(&s_state_mux);
    return owner;
}

// 调用者必须已经持有共享 I2C gate，避免两个初始化路径重复创建同一端口。
static esp_err_t shared_i2c_init_locked(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CODEC_I2C_PORT,
        .sda_io_num = FRIDGE_PIN_CODEC_I2C_SDA,
        .scl_io_num = FRIDGE_PIN_CODEC_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // 板载线路应有 3.3V 上拉；内部上拉仅作为弱兜底，禁止外接到 5V。
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create shared I2C1 bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "shared I2C1 ready: SDA=GPIO%d SCL=GPIO%d",
             FRIDGE_PIN_CODEC_I2C_SDA, FRIDGE_PIN_CODEC_I2C_SCL);
    return ESP_OK;
}

esp_err_t fridge_codec_shared_i2c_init(void)
{
    // 总线句柄创建后不会在运行期销毁；已就绪时直接返回，避免音频长期持有 gate 时，
    // 摄像头预览任务为了重复“初始化”而在 portMAX_DELAY 上永久阻塞。
    if (s_i2c_bus) {
        return ESP_OK;
    }

    esp_err_t err = fridge_codec_shared_i2c_gate_lock(FRIDGE_CODEC_I2C_GATE_OWNER_INIT, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t init_err = shared_i2c_init_locked();
    esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_INIT);
    return init_err != ESP_OK ? init_err : unlock_err;
}

i2c_master_bus_handle_t fridge_codec_shared_i2c_get_bus(void)
{
    return s_i2c_bus;
}

// PA_CTRL 高有效。先写低电平锁存值，再切换为输出，尽量避免初始化瞬间的爆音脉冲。
static esp_err_t force_pa_off(void)
{
    esp_err_t err = gpio_set_level(FRIDGE_PIN_PA_CTRL, 0);
    if (err != ESP_OK) {
        return err;
    }
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << FRIDGE_PIN_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&pa_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(FRIDGE_PIN_PA_CTRL, 0);
    s_pa_on = false;
    return err;
}

static esp_err_t pa_on(void)
{
    esp_err_t err = gpio_set_level(FRIDGE_PIN_PA_CTRL, 1);
    if (err == ESP_OK) {
        s_pa_on = true;
    }
    return err;
}

static esp_err_t codec_result_to_esp(int result)
{
    return result == ESP_CODEC_DEV_OK ? ESP_OK : (esp_err_t)result;
}

// 调用者已持有初始化 gate；两把锁要么同时可用，要么全部回收后返回失败。
static esp_err_t ensure_io_locks_locked(void)
{
    if (s_rx_io_lock && s_tx_io_lock) {
        return ESP_OK;
    }
    if (!s_rx_io_lock) {
        s_rx_io_lock = xSemaphoreCreateMutex();
    }
    if (!s_tx_io_lock) {
        s_tx_io_lock = xSemaphoreCreateMutex();
    }
    if (!s_rx_io_lock || !s_tx_io_lock) {
        if (s_rx_io_lock) {
            vSemaphoreDelete(s_rx_io_lock);
            s_rx_io_lock = NULL;
        }
        if (s_tx_io_lock) {
            vSemaphoreDelete(s_tx_io_lock);
            s_tx_io_lock = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// 初始化失败时按依赖逆序回收；所有删除错误都记录，主调用仍返回最早的初始化错误。
static void cleanup_partial_codec_locked(void)
{
    if (s_codec_dev) {
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    if (s_codec_if) {
        int ret = audio_codec_delete_codec_if(s_codec_if);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "delete codec interface failed: %d", ret);
        }
        s_codec_if = NULL;
    }
    if (s_gpio_if) {
        int ret = audio_codec_delete_gpio_if(s_gpio_if);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "delete gpio interface failed: %d", ret);
        }
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        int ret = audio_codec_delete_ctrl_if(s_ctrl_if);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "delete I2C control interface failed: %d", ret);
        }
        s_ctrl_if = NULL;
    }
    if (s_data_if) {
        int ret = audio_codec_delete_data_if(s_data_if);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "delete I2S data interface failed: %d", ret);
        }
        s_data_if = NULL;
    }
    if (s_tx_chan) {
        esp_err_t err = i2s_del_channel(s_tx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete I2S TX channel failed: %s", esp_err_to_name(err));
        }
        s_tx_chan = NULL;
    }
    if (s_rx_chan) {
        esp_err_t err = i2s_del_channel(s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete I2S RX channel failed: %s", esp_err_to_name(err));
        }
        s_rx_chan = NULL;
    }
}

// 创建 16-bit I2S0 全双工（RX+TX）。ADC/DAC 均使用左时隙；PA 仍由本组件独立控制。
static esp_err_t init_i2s_full_duplex_locked(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CODEC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CODEC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CODEC_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create I2S TX/RX channels failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = FRIDGE_PIN_I2S_MCLK,
            .bclk = FRIDGE_PIN_I2S_SCLK,
            .ws = FRIDGE_PIN_I2S_LRCK,
            .dout = FRIDGE_PIN_I2S_DSDIN,   // P4 -> ES8311 DAC
            .din = FRIDGE_PIN_I2S_ASDOUT,   // ES8311 ADC -> P4
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = CODEC_MCLK_MULTIPLE;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initialize I2S STD RX failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initialize I2S STD TX failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    // 无论后续哪个初始化步骤失败，板载 NS4150B 都必须保持关闭。
    esp_err_t err = force_pa_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "force PA_CTRL low failed: %s", esp_err_to_name(err));
        return err;
    }
    err = fridge_codec_shared_i2c_gate_lock(FRIDGE_CODEC_I2C_GATE_OWNER_INIT, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    if (s_initialized) {
        return fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_INIT);
    }

    err = shared_i2c_init_locked();
    if (err == ESP_OK) {
        err = ensure_io_locks_locked();
    }
    if (err == ESP_OK) {
        err = init_i2s_full_duplex_locked();
    }

    if (err == ESP_OK) {
        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = CODEC_I2S_PORT,
            .rx_handle = s_rx_chan,
            .tx_handle = s_tx_chan,
            .clk_src = I2S_CLK_SRC_DEFAULT,
        };
        s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
        if (!s_data_if) {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        audio_codec_i2c_cfg_t i2c_cfg = {
            .port = CODEC_I2C_PORT,
            .addr = ES8311_CODEC_DEFAULT_ADDR,
            .bus_handle = s_i2c_bus,
        };
        s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        if (!s_ctrl_if) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        s_gpio_if = audio_codec_new_gpio();
        if (!s_gpio_if) {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        es8311_codec_cfg_t codec_cfg = {
            .ctrl_if = s_ctrl_if,
            .gpio_if = s_gpio_if,
            // BOTH：允许 Realtime 双工；PA 仍由本组件 GPIO11 控制，不交给 esp_codec_dev。
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin = GPIO_NUM_NC,
            .pa_reverted = false,
            .master_mode = false,
            .use_mclk = true,
            .digital_mic = false,
            .invert_mclk = false,
            .invert_sclk = false,
            .no_dac_ref = false,
            .mclk_div = CODEC_MCLK_MULTIPLE,
        };
        s_codec_if = es8311_codec_new(&codec_cfg);
        if (!s_codec_if) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = s_codec_if,
            .data_if = s_data_if,
        };
        s_codec_dev = esp_codec_dev_new(&dev_cfg);
        if (!s_codec_dev) {
            err = ESP_ERR_NO_MEM;
        }
    }

    if (err == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(TAG,
                 "ES8311 ready (duplex-capable): I2C1=%d/%d I2S0 MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d, 16k/16bit, PA off",
                 FRIDGE_PIN_CODEC_I2C_SDA, FRIDGE_PIN_CODEC_I2C_SCL,
                 FRIDGE_PIN_I2S_MCLK, FRIDGE_PIN_I2S_SCLK, FRIDGE_PIN_I2S_LRCK,
                 FRIDGE_PIN_I2S_DSDIN, FRIDGE_PIN_I2S_ASDOUT);
    } else {
        cleanup_partial_codec_locked();
    }

    esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_INIT);
    return err != ESP_OK ? err : unlock_err;
}

esp_err_t fridge_codec_es8311_start_input(TickType_t gate_timeout_ticks)
{
    esp_err_t err = fridge_codec_es8311_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);

    err = fridge_codec_shared_i2c_gate_lock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO, gate_timeout_ticks);
    if (err != ESP_OK) {
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    // 单向 TTS 输出同样占用 codec/I2S0 与 GPIO28/29 AUDIO gate；不能在其播放期间
    // 再打开 ADC，否则会重复打开 codec 时钟并破坏共享总线所有权。
    bool already_active = s_input_active || s_duplex_active || s_output_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (already_active) {
        esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
        xSemaphoreGive(s_rx_io_lock);
        return unlock_err == ESP_OK ? ESP_ERR_INVALID_STATE : unlock_err;
    }

    err = force_pa_off();
    if (err == ESP_OK) {
        esp_codec_dev_sample_info_t sample_cfg = {
            .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
            .channel = 1,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = CODEC_SAMPLE_RATE_HZ,
            .mclk_multiple = CODEC_MCLK_MULTIPLE,
        };
        err = codec_result_to_esp(esp_codec_dev_open(s_codec_dev, &sample_cfg));
    }
    if (err == ESP_OK) {
        err = codec_result_to_esp(esp_codec_dev_set_in_gain(s_codec_dev, CODEC_MIC_GAIN_DB));
    }

    if (err != ESP_OK) {
        int close_result = esp_codec_dev_close(s_codec_dev);
        if (close_result != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec cleanup after start failure returned %d", close_result);
        }
        esp_err_t pa_err = force_pa_off();
        if (pa_err != ESP_OK) {
            ESP_LOGW(TAG, "force PA off after start failure failed: %s", esp_err_to_name(pa_err));
        }
        esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
        if (unlock_err != ESP_OK) {
            ESP_LOGE(TAG, "release shared I2C gate after start failure failed: %s", esp_err_to_name(unlock_err));
        }
        ESP_LOGE(TAG, "start ES8311 input failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_input_active = true;
    portEXIT_CRITICAL(&s_state_mux);
    xSemaphoreGive(s_rx_io_lock);
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_read(int16_t *samples,
                                   size_t max_samples,
                                   size_t *sample_count,
                                   TickType_t timeout_ticks)
{
    if (!samples || !sample_count || max_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample_count = 0;

    if (!s_rx_io_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);

    portENTER_CRITICAL(&s_state_mux);
    bool active = s_input_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (!active || !s_rx_chan) {
        xSemaphoreGive(s_rx_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    // I2S 新驱动的超时参数单位是毫秒，而上层公共 API 沿用 FreeRTOS tick。
    // 显式换算，避免 CONFIG_FREERTOS_HZ 不是 1000 时超时被缩短。
    uint32_t timeout_ms = timeout_ticks == portMAX_DELAY
                              ? UINT32_MAX
                              : (uint32_t)pdTICKS_TO_MS(timeout_ticks);
    if (timeout_ticks > 0 && timeout_ms == 0) {
        timeout_ms = 1;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan,
                                     samples,
                                     max_samples * sizeof(samples[0]),
                                     &bytes_read,
                                     timeout_ms);
    if (err != ESP_OK) {
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }
    if ((bytes_read % sizeof(samples[0])) != 0) {
        ESP_LOGE(TAG, "unaligned I2S RX length: %u", (unsigned)bytes_read);
        xSemaphoreGive(s_rx_io_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *sample_count = bytes_read / sizeof(samples[0]);
    xSemaphoreGive(s_rx_io_lock);
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_stop_input(void)
{
    if (!s_rx_io_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);

    portENTER_CRITICAL(&s_state_mux);
    bool was_active = s_input_active;
    bool duplex = s_duplex_active;
    if (!duplex) {
        s_input_active = false;
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (duplex) {
        // 双工会话由 stop_duplex 统一收尾。
        xSemaphoreGive(s_rx_io_lock);
        return ESP_OK;
    }
    if (!was_active || !s_codec_dev) {
        xSemaphoreGive(s_rx_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    // 先关闭 NS4150B，再关闭 codec/I2S，避免停机时的尾音脉冲。
    esp_err_t pa_err = force_pa_off();
    esp_err_t err = codec_result_to_esp(esp_codec_dev_close(s_codec_dev));
    esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
    xSemaphoreGive(s_rx_io_lock);
    if (err != ESP_OK) {
        return err;
    }
    if (pa_err != ESP_OK) {
        return pa_err;
    }
    return unlock_err;
}

esp_err_t fridge_codec_es8311_start_duplex(TickType_t gate_timeout_ticks)
{
    esp_err_t err = fridge_codec_es8311_init();
    if (err != ESP_OK) {
        return err;
    }

    // 启动与停止都按 RX -> TX 获取，读写任务仍可分别并行。
    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);
    xSemaphoreTake(s_tx_io_lock, portMAX_DELAY);

    err = fridge_codec_shared_i2c_gate_lock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO, gate_timeout_ticks);
    if (err != ESP_OK) {
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    bool busy = s_input_active || s_duplex_active || s_output_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (busy) {
        esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return unlock_err == ESP_OK ? ESP_ERR_INVALID_STATE : unlock_err;
    }

    // 打开前强制功放关闭，避免初始化脉冲。
    err = force_pa_off();
    if (err == ESP_OK) {
        esp_codec_dev_sample_info_t sample_cfg = {
            .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
            .channel = 1,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = CODEC_SAMPLE_RATE_HZ,
            .mclk_multiple = CODEC_MCLK_MULTIPLE,
        };
        err = codec_result_to_esp(esp_codec_dev_open(s_codec_dev, &sample_cfg));
    }
    if (err == ESP_OK) {
        err = codec_result_to_esp(esp_codec_dev_set_in_gain(s_codec_dev, CODEC_MIC_GAIN_DB));
    }
    if (err == ESP_OK) {
        err = codec_result_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, CODEC_OUT_VOL));
    }

    if (err != ESP_OK) {
        (void)esp_codec_dev_close(s_codec_dev);
        (void)force_pa_off();
        (void)fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
        ESP_LOGE(TAG, "start ES8311 duplex failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_input_active = true;
    s_duplex_active = true;
    portEXIT_CRITICAL(&s_state_mux);
    xSemaphoreGive(s_tx_io_lock);
    xSemaphoreGive(s_rx_io_lock);
    ESP_LOGI(TAG, "ES8311 duplex started (ADC+DAC 16k mono, PA still off until first write)");
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_stop_duplex(void)
{
    if (!s_rx_io_lock || !s_tx_io_lock) {
        (void)force_pa_off();
        return ESP_OK;
    }
    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);
    xSemaphoreTake(s_tx_io_lock, portMAX_DELAY);

    portENTER_CRITICAL(&s_state_mux);
    bool was = s_duplex_active;
    s_duplex_active = false;
    s_input_active = false;
    portEXIT_CRITICAL(&s_state_mux);
    if (!was || !s_codec_dev) {
        (void)force_pa_off();
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return ESP_OK;
    }

    // PA_CTRL 必须先拉低，再关闭 DAC/I2S，避免 NS4150B 放大停机瞬态。
    esp_err_t pa_err = force_pa_off();
    esp_err_t err = codec_result_to_esp(esp_codec_dev_close(s_codec_dev));
    esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
    xSemaphoreGive(s_tx_io_lock);
    xSemaphoreGive(s_rx_io_lock);
    if (err != ESP_OK) {
        return err;
    }
    if (pa_err != ESP_OK) {
        return pa_err;
    }
    return unlock_err;
}

// TTS 仅使用 TX。启动与停止仍获取 RX->TX 两把锁，确保不会与双工路径的关闭流程
// 并发操作同一套 ES8311/I2S0 资源；PA_CTRL 只会在首块有效 PCM 写入时拉高。
esp_err_t fridge_codec_es8311_start_output(TickType_t gate_timeout_ticks)
{
    esp_err_t err = fridge_codec_es8311_init();
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);
    xSemaphoreTake(s_tx_io_lock, portMAX_DELAY);
    err = fridge_codec_shared_i2c_gate_lock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO,
                                             gate_timeout_ticks);
    if (err != ESP_OK) {
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    bool busy = s_input_active || s_duplex_active || s_output_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (busy) {
        (void)fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    err = force_pa_off();
    if (err == ESP_OK) {
        esp_codec_dev_sample_info_t sample_cfg = {
            .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
            .channel = 1,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = CODEC_SAMPLE_RATE_HZ,
            .mclk_multiple = CODEC_MCLK_MULTIPLE,
        };
        err = codec_result_to_esp(esp_codec_dev_open(s_codec_dev, &sample_cfg));
    }
    if (err == ESP_OK) {
        err = codec_result_to_esp(esp_codec_dev_set_out_vol(s_codec_dev, CODEC_OUT_VOL));
    }
    if (err != ESP_OK) {
        (void)esp_codec_dev_close(s_codec_dev);
        (void)force_pa_off();
        (void)fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
        xSemaphoreGive(s_tx_io_lock);
        xSemaphoreGive(s_rx_io_lock);
        return err;
    }
    portENTER_CRITICAL(&s_state_mux);
    s_output_active = true;
    portEXIT_CRITICAL(&s_state_mux);
    xSemaphoreGive(s_tx_io_lock);
    xSemaphoreGive(s_rx_io_lock);
    ESP_LOGI(TAG, "ES8311 TTS output started (16k mono, PA off until PCM)");
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_stop_output(void)
{
    if (!s_rx_io_lock || !s_tx_io_lock) return force_pa_off();
    xSemaphoreTake(s_rx_io_lock, portMAX_DELAY);
    xSemaphoreTake(s_tx_io_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_state_mux);
    bool was_active = s_output_active;
    s_output_active = false;
    portEXIT_CRITICAL(&s_state_mux);

    // 先关 PA，避免 NS4150B 放大 codec/I2S 停止瞬态。
    esp_err_t pa_err = force_pa_off();
    esp_err_t close_err = ESP_OK;
    esp_err_t unlock_err = ESP_OK;
    if (was_active && s_codec_dev) {
        close_err = codec_result_to_esp(esp_codec_dev_close(s_codec_dev));
        unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
    }
    xSemaphoreGive(s_tx_io_lock);
    xSemaphoreGive(s_rx_io_lock);
    if (close_err != ESP_OK) return close_err;
    if (pa_err != ESP_OK) return pa_err;
    return unlock_err;
}

bool fridge_codec_es8311_output_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_state_mux);
    active = s_output_active;
    portEXIT_CRITICAL(&s_state_mux);
    return active;
}

esp_err_t fridge_codec_es8311_write(const int16_t *samples,
                                    size_t sample_count,
                                    TickType_t timeout_ticks)
{
    if (!samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx_io_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t wait_started = xTaskGetTickCount();
    if (xSemaphoreTake(s_tx_io_lock, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_state_mux);
    bool active = s_duplex_active || s_output_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (!active || !s_codec_dev || !s_tx_chan) {
        xSemaphoreGive(s_tx_io_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_pa_on) {
        esp_err_t pa_err = pa_on();
        if (pa_err != ESP_OK) {
            xSemaphoreGive(s_tx_io_lock);
            return pa_err;
        }
        // 功放上电稳定一小段时间，降低爆音概率。
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    TickType_t remaining_ticks = timeout_ticks;
    if (timeout_ticks != portMAX_DELAY) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - wait_started;
        remaining_ticks = elapsed_ticks < timeout_ticks ? timeout_ticks - elapsed_ticks : 0;
    }
    uint32_t timeout_ms = remaining_ticks == portMAX_DELAY
                              ? UINT32_MAX
                              : (uint32_t)pdTICKS_TO_MS(remaining_ticks);
    if (remaining_ticks > 0 && timeout_ms == 0) {
        timeout_ms = 1;
    }
    size_t bytes_written = 0;
    size_t bytes_requested = sample_count * sizeof(samples[0]);
    // esp_codec_dev_write 内部把超时固定为 1000ms。这里直接使用它最终调用的 I2S TX API，
    // 让 Realtime 上层传入的 200ms 超时真正生效；ES8311 音量仍由硬件寄存器控制。
    esp_err_t err = i2s_channel_write(s_tx_chan,
                                      samples,
                                      bytes_requested,
                                      &bytes_written,
                                      timeout_ms);
    if (err == ESP_OK && bytes_written != bytes_requested) {
        ESP_LOGW(TAG, "partial I2S TX write: requested=%u written=%u",
                 (unsigned)bytes_requested, (unsigned)bytes_written);
        err = ESP_ERR_TIMEOUT;
    }
    if (err != ESP_OK) {
        // 写失败后立即关闭 PA，禁止 NS4150B 在无有效音频时持续导通。
        esp_err_t pa_err = force_pa_off();
        if (pa_err != ESP_OK) {
            ESP_LOGE(TAG, "force PA off after I2S write failure failed: %s", esp_err_to_name(pa_err));
        }
    }
    xSemaphoreGive(s_tx_io_lock);
    return err;
}

esp_err_t fridge_codec_es8311_pa_off(void)
{
    if (!s_tx_io_lock) {
        return force_pa_off();
    }
    xSemaphoreTake(s_tx_io_lock, portMAX_DELAY);
    esp_err_t err = force_pa_off();
    xSemaphoreGive(s_tx_io_lock);
    return err;
}

bool fridge_codec_es8311_duplex_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_state_mux);
    active = s_duplex_active;
    portEXIT_CRITICAL(&s_state_mux);
    return active;
}
