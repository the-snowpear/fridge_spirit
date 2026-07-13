// 冰箱小精灵 ESP32-P4 板载 ES8311 编解码器适配层。
// 本组件只启用 ADC 录音；NS4150B 功放保持关闭，并统一管理摄像头/编解码器共享 I2C1。
#include "fridge_codec_es8311.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "fridge_board_pins.h"
#include "freertos/semphr.h"

#define CODEC_I2C_PORT I2C_NUM_1
#define CODEC_I2S_PORT I2S_NUM_0
#define CODEC_SAMPLE_RATE_HZ 16000
#define CODEC_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_384
#define CODEC_MIC_GAIN_DB 30.0f
#define CODEC_DMA_DESC_NUM 4
#define CODEC_DMA_FRAME_NUM 256

static const char *TAG = "fridge_es8311";

static SemaphoreHandle_t s_i2c_gate;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static fridge_codec_i2c_gate_owner_t s_i2c_gate_owner = FRIDGE_CODEC_I2C_GATE_OWNER_NONE;
static i2c_master_bus_handle_t s_i2c_bus;

static i2s_chan_handle_t s_rx_chan;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_codec_dev;
static bool s_initialized;
static bool s_input_active;

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
    return gpio_set_level(FRIDGE_PIN_PA_CTRL, 0);
}

static esp_err_t codec_result_to_esp(int result)
{
    return result == ESP_CODEC_DEV_OK ? ESP_OK : (esp_err_t)result;
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
    if (s_rx_chan) {
        esp_err_t err = i2s_del_channel(s_rx_chan);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "delete I2S RX channel failed: %s", esp_err_to_name(err));
        }
        s_rx_chan = NULL;
    }
}

// 创建 16-bit I2S0 RX。ES8311 的 ADC 数据位于左时隙，驱动直接输出紧凑 int16 PCM。
static esp_err_t init_i2s_rx_locked(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CODEC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CODEC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = CODEC_DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create I2S RX channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = FRIDGE_PIN_I2S_MCLK,
            .bclk = FRIDGE_PIN_I2S_SCLK,
            .ws = FRIDGE_PIN_I2S_LRCK,
            // 当前仅创建 RX 通道，GPIO3 保留为板载 ES8311 DAC 数据方向，不启用功放。
            .dout = FRIDGE_PIN_I2S_DSDIN,
            .din = FRIDGE_PIN_I2S_ASDOUT,
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
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_init(void)
{
    // 无论后续哪个初始化步骤失败，板载 NS4150B 都必须保持关闭。
    esp_err_t err = force_pa_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "force PA_CTRL low failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_initialized) {
        return ESP_OK;
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
        err = init_i2s_rx_locked();
    }

    if (err == ESP_OK) {
        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = CODEC_I2S_PORT,
            .rx_handle = s_rx_chan,
            .tx_handle = NULL,
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
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
            // GPIO11 由本组件固定拉低；传 -1 可阻止 esp_codec_dev 自动使能功放。
            .pa_pin = GPIO_NUM_NC,
            .pa_reverted = false,
            .master_mode = false,
            .use_mclk = true,
            .digital_mic = false,
            .invert_mclk = false,
            .invert_sclk = false,
            .no_dac_ref = true,
            .mclk_div = CODEC_MCLK_MULTIPLE,
        };
        s_codec_if = es8311_codec_new(&codec_cfg);
        if (!s_codec_if) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
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
                 "ES8311 ready: I2C1=%d/%d I2S0 MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d, 16k/16bit, MCLKx384, PA off",
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

    err = fridge_codec_shared_i2c_gate_lock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO, gate_timeout_ticks);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    bool already_active = s_input_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (already_active) {
        esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
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
        return err;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_input_active = true;
    portEXIT_CRITICAL(&s_state_mux);
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

    portENTER_CRITICAL(&s_state_mux);
    bool active = s_input_active;
    portEXIT_CRITICAL(&s_state_mux);
    if (!active || !s_rx_chan) {
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
        return err;
    }
    if ((bytes_read % sizeof(samples[0])) != 0) {
        ESP_LOGE(TAG, "unaligned I2S RX length: %u", (unsigned)bytes_read);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *sample_count = bytes_read / sizeof(samples[0]);
    return ESP_OK;
}

esp_err_t fridge_codec_es8311_stop_input(void)
{
    portENTER_CRITICAL(&s_state_mux);
    bool was_active = s_input_active;
    s_input_active = false;
    portEXIT_CRITICAL(&s_state_mux);
    if (!was_active || !s_codec_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = codec_result_to_esp(esp_codec_dev_close(s_codec_dev));
    esp_err_t pa_err = force_pa_off();
    esp_err_t unlock_err = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO);
    if (err != ESP_OK) {
        return err;
    }
    if (pa_err != ESP_OK) {
        return pa_err;
    }
    return unlock_err;
}
