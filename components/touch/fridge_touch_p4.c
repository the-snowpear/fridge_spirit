// 冰箱小精灵 ESP32-P4 FT6336U 触摸驱动。
// 本驱动使用 ESP-IDF 6 新 i2c_master API 独占 I2C0，避免继承 S3 的 sensors/legacy I2C 总线关系。
// 硬件注意：SDA/SCL 为开漏信号，内部上拉只用于辅助；FPC 转接板或屏模组必须提供到 3.3V 的外部上拉，禁止上拉到 5V。

#include "fridge_touch.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fridge_board_pins.h"

#define FT6336U_REG_TD_STATUS 0x02
#define FT6336U_REG_CHIP_ID 0xA3
#define FT6336U_REG_VENDOR_ID 0xA8

#define TOUCH_I2C_PORT I2C_NUM_0
#define TOUCH_I2C_SPEED_HZ 100000
#define TOUCH_READ_TIMEOUT_MS 50
#define TOUCH_INIT_LOCK_TIMEOUT_MS 500
#define TOUCH_RESET_LOW_MS 10
#define TOUCH_RESET_SETTLE_MS 200

#define TOUCH_PIN_SDA ((gpio_num_t)FRIDGE_PIN_TOUCH_SDA)
#define TOUCH_PIN_SCL ((gpio_num_t)FRIDGE_PIN_TOUCH_SCL)
#define TOUCH_PIN_INT ((gpio_num_t)FRIDGE_PIN_TOUCH_INT)
#define TOUCH_PIN_RST ((gpio_num_t)FRIDGE_PIN_TOUCH_RST)

static const char *TAG = "fridge_touch";
static SemaphoreHandle_t s_mutex;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static bool s_initialized;

// 释放本组件持有的 I2C 资源，使初始化失败后可以再次调用 fridge_touch_init() 重试。
// 调用者必须已经持有 s_mutex，避免释放过程中出现并发读寄存器。
static esp_err_t touch_release_i2c_locked(void)
{
    esp_err_t first_err = ESP_OK;

    if (s_device != NULL) {
        esp_err_t ret = i2c_master_bus_rm_device(s_device);
        if (ret == ESP_OK) {
            s_device = NULL;
        } else {
            ESP_LOGW(TAG, "remove FT6336U device failed: %s", esp_err_to_name(ret));
            first_err = ret;
        }
    }

    if (s_bus != NULL && s_device == NULL) {
        esp_err_t ret = i2c_del_master_bus(s_bus);
        if (ret == ESP_OK) {
            s_bus = NULL;
        } else {
            ESP_LOGW(TAG, "delete touch I2C0 bus failed: %s", esp_err_to_name(ret));
            if (first_err == ESP_OK) {
                first_err = ret;
            }
        }
    }

    return first_err;
}

// 在已持有互斥锁时读取一个连续寄存器区间；每次传输最多等待 50ms，防止接线异常时长期阻塞 UI。
static esp_err_t touch_read_reg_locked(uint8_t reg, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(s_device,
                                       &reg,
                                       sizeof(reg),
                                       data,
                                       len,
                                       TOUCH_READ_TIMEOUT_MS);
}

// 串行化单次 FT6336U 寄存器读取；取锁与 I2C 传输都使用有限超时。
static esp_err_t touch_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL || !s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(TOUCH_READ_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = touch_read_reg_locked(reg, data, len);
    xSemaphoreGive(s_mutex);
    return ret;
}

// 配置触摸中断与复位引脚，并执行明确的低到高硬复位脉冲。
// INT 目前只作为带辅助上拉的输入，读点路径采用轮询，不在中断上下文访问 I2C。
static esp_err_t touch_reset(void)
{
    gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&int_config);
    if (ret != ESP_OK) {
        return ret;
    }

    gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&reset_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(TOUCH_PIN_RST, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_RESET_LOW_MS));

    ret = gpio_set_level(TOUCH_PIN_RST, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_RESET_SETTLE_MS));
    return ESP_OK;
}

// 初始化 P4 独立触摸 I2C0、复位 FT6336U，并读取芯片与厂商 ID 验证 0x48 地址链路。
// 函数成功后可重复调用；失败时会释放已创建的 I2C 资源，后续可修正接线后重试。
esp_err_t fridge_touch_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(TOUCH_INIT_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // 若上一次清理曾被底层驱动拒绝，先完成清理再重新创建 I2C0。
    if (s_device != NULL || s_bus != NULL) {
        esp_err_t cleanup_ret = touch_release_i2c_locked();
        if (cleanup_ret != ESP_OK) {
            xSemaphoreGive(s_mutex);
            return cleanup_ret;
        }
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_PIN_SDA,
        .scl_io_num = TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // 内部上拉只作无源辅助，不能代替转接板/模组上到 3.3V 的外部 I2C 上拉电阻。
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create I2C0 failed, SDA=GPIO%d SCL=GPIO%d: %s",
                 TOUCH_PIN_SDA, TOUCH_PIN_SCL, esp_err_to_name(ret));
        goto fail;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FRIDGE_TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_SPEED_HZ,
    };
    ret = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add FT6336U at 0x%02X failed: %s",
                 FRIDGE_TOUCH_I2C_ADDR, esp_err_to_name(ret));
        goto fail;
    }

    ret = touch_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT6336U reset pulse failed on GPIO%d: %s",
                 TOUCH_PIN_RST, esp_err_to_name(ret));
        goto fail;
    }

    uint8_t chip_id = 0;
    uint8_t vendor_id = 0;
    ret = touch_read_reg_locked(FT6336U_REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read FT6336U chip ID at 0x%02X failed: %s",
                 FRIDGE_TOUCH_I2C_ADDR, esp_err_to_name(ret));
        goto fail;
    }
    ret = touch_read_reg_locked(FT6336U_REG_VENDOR_ID, &vendor_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read FT6336U vendor ID at 0x%02X failed: %s",
                 FRIDGE_TOUCH_I2C_ADDR, esp_err_to_name(ret));
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "FT6336U ready addr=0x%02X chip=0x%02X vendor=0x%02X I2C0=GPIO%d/%d 100kHz INT=GPIO%d RST=GPIO%d",
             FRIDGE_TOUCH_I2C_ADDR,
             chip_id,
             vendor_id,
             TOUCH_PIN_SDA,
             TOUCH_PIN_SCL,
             TOUCH_PIN_INT,
             TOUCH_PIN_RST);
    xSemaphoreGive(s_mutex);
    return ESP_OK;

fail:
    s_initialized = false;
    {
        esp_err_t cleanup_ret = touch_release_i2c_locked();
        if (cleanup_ret != ESP_OK) {
            ESP_LOGW(TAG, "touch init cleanup failed: %s", esp_err_to_name(cleanup_ret));
        }
    }
    xSemaphoreGive(s_mutex);
    return ret;
}

// 读取 FT6336U 第一个触点坐标；一次事务读取状态和坐标，避免跨事务拿到不同帧的数据。
esp_err_t fridge_touch_read(fridge_touch_point_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[5] = {0};
    esp_err_t ret = touch_read_reg(FT6336U_REG_TD_STATUS, data, sizeof(data));
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t points = data[0] & 0x0F;
    if (points == 0 || points > 2) {
        return ESP_OK;
    }

    uint16_t x = (uint16_t)(((data[1] & 0x0F) << 8) | data[2]);
    uint16_t y = (uint16_t)(((data[3] & 0x0F) << 8) | data[4]);
    if (x >= 720 || y >= 720) {
        ESP_LOGD(TAG, "ignore out-of-range touch x=%u y=%u points=%u", x, y, points);
        return ESP_OK;
    }

    out->pressed = true;
    out->points = points;
    out->x = x;
    out->y = y;
    return ESP_OK;
}
