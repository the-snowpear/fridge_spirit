// 冰箱小精灵 SHT40 温湿度传感器驱动。
// 挂载在共享 I2C0（GPIO16 SCL / GPIO17 SDA），与 GT911 触摸共用同一 bus handle。
// 必须在 fridge_touch_gt911_init() 之后调用，确保 I2C0 总线已由 GT911 建立。
// SHT40 协议：地址 0x44（备用 0x45），命令 0xFD 高精度测量，无 clock stretching，
// 发命令后延迟 ≥10ms，读 6 字节（T_MSB T_LSB T_CRC RH_MSB RH_LSB RH_CRC），
// CRC-8 poly=0x31 init=0xFF；换算 T=-45+175*t/65535，RH=-6+125*rh/65535 clamp 0~100。

#include "fridge_sht4x.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SHT4X_CMD_MEASURE_HIGH  0xFD   // 高精度测量，无加热
#define SHT4X_MEASURE_DELAY_MS  10     // 发命令后等待时间（高精度 ≥8.3ms，取 10ms）
#define SHT4X_POLL_INTERVAL_MS  5000   // 轮询周期
#define SHT4X_IO_TIMEOUT_MS     50
#define SHT4X_INIT_TIMEOUT_MS   500
#define SHT4X_I2C_SPEED_HZ      100000 // 共享总线保守速率

static const char *TAG = "fridge_sht4x";
static SemaphoreHandle_t s_mutex;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static bool s_initialized;
static fridge_sht4x_snapshot_t s_snapshot;

// CRC-8：poly=0x31，init=0xFF，对 2 字节数据校验。
static uint8_t sht4x_crc8(uint8_t msb, uint8_t lsb)
{
    uint8_t crc = 0xFF;
    uint8_t data[2] = {msb, lsb};
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// 发送单字节命令，等待测量完成，读取 6 字节响应。
static esp_err_t sht4x_measure_locked(float *temp_c, float *humidity_rh)
{
    uint8_t cmd = SHT4X_CMD_MEASURE_HIGH;
    esp_err_t ret = i2c_master_transmit(s_device, &cmd, 1, SHT4X_IO_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(SHT4X_MEASURE_DELAY_MS));

    uint8_t buf[6] = {0};
    ret = i2c_master_receive(s_device, buf, sizeof(buf), SHT4X_IO_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;

    // CRC 校验：T 和 RH 各自独立校验。
    if (sht4x_crc8(buf[0], buf[1]) != buf[2]) {
        ESP_LOGW(TAG, "温度 CRC 校验失败");
        return ESP_ERR_INVALID_CRC;
    }
    if (sht4x_crc8(buf[3], buf[4]) != buf[5]) {
        ESP_LOGW(TAG, "湿度 CRC 校验失败");
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_t  = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_rh = ((uint16_t)buf[3] << 8) | buf[4];

    float t  = -45.0f + 175.0f * raw_t  / 65535.0f;
    float rh = -6.0f  + 125.0f * raw_rh / 65535.0f;
    if (rh < 0.0f)   rh = 0.0f;
    if (rh > 100.0f) rh = 100.0f;

    *temp_c      = t;
    *humidity_rh = rh;
    return ESP_OK;
}

// 轮询任务：每 5s 读一次，写入 mutex 保护的快照。
static void sht4x_poll_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SHT4X_POLL_INTERVAL_MS));

        float t = 0.0f, rh = 0.0f;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SHT4X_IO_TIMEOUT_MS)) != pdTRUE) continue;

        esp_err_t err = sht4x_measure_locked(&t, &rh);
        if (err == ESP_OK) {
            s_snapshot.valid          = true;
            s_snapshot.temperature_c  = t;
            s_snapshot.humidity_rh    = rh;
            s_snapshot.last_update_us = esp_timer_get_time();
            s_snapshot.last_err       = ESP_OK;
        } else {
            s_snapshot.valid    = false;
            s_snapshot.last_err = err;
        }
        xSemaphoreGive(s_mutex);
    }
}

// 探测指定地址：发测量命令并读 6 字节，CRC 通过则认为在位。
static esp_err_t sht4x_probe_address(uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = SHT4X_I2C_SPEED_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(s_bus, &cfg, &dev);
    if (ret != ESP_OK) return ret;

    uint8_t cmd = SHT4X_CMD_MEASURE_HIGH;
    ret = i2c_master_transmit(dev, &cmd, 1, SHT4X_IO_TIMEOUT_MS);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHT4X_MEASURE_DELAY_MS));
        uint8_t buf[6] = {0};
        ret = i2c_master_receive(dev, buf, sizeof(buf), SHT4X_IO_TIMEOUT_MS);
        if (ret == ESP_OK) {
            if (sht4x_crc8(buf[0], buf[1]) != buf[2] ||
                sht4x_crc8(buf[3], buf[4]) != buf[5]) {
                ret = ESP_ERR_INVALID_CRC;
            }
        }
    }

    if (ret == ESP_OK) {
        s_device = dev;
        s_snapshot.i2c_address = addr;
        ESP_LOGI(TAG, "SHT40 就绪 addr=0x%02X", addr);
    } else {
        (void)i2c_master_bus_rm_device(dev);
    }
    return ret;
}

esp_err_t fridge_sht4x_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SHT4X_INIT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    // 复用 GT911 已建的 I2C0 共享总线，不重新创建总线（共享 I2C0 红线）。
    esp_err_t ret = i2c_master_get_bus_handle(I2C_NUM_0, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "获取 I2C0 共享总线失败: %s（需先调用 fridge_touch_gt911_init）",
                 esp_err_to_name(ret));
        goto out;
    }

    // 先探测 0x44，再探测 0x45。
    static const uint8_t addrs[] = {0x44, 0x45};
    ret = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(addrs) && ret != ESP_OK; i++) {
        ret = sht4x_probe_address(addrs[i]);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT40 未找到（0x44/0x45）: %s", esp_err_to_name(ret));
        goto out;
    }

    // 首次读数写入快照。
    float t = 0.0f, rh = 0.0f;
    esp_err_t mret = sht4x_measure_locked(&t, &rh);
    if (mret == ESP_OK) {
        s_snapshot.valid          = true;
        s_snapshot.temperature_c  = t;
        s_snapshot.humidity_rh    = rh;
        s_snapshot.last_update_us = esp_timer_get_time();
        s_snapshot.last_err       = ESP_OK;
    }

    s_initialized = true;
    xSemaphoreGive(s_mutex);

    // 启动轮询任务。新 I2C master 驱动的事务调用层级较深，3072 字节在 P4
    // 截图 TF 读回期间实测触发栈保护；这里仅增加任务栈，不改变总线时序。
    if (xTaskCreate(sht4x_poll_task, "sht4x_poll", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "SHT40 轮询任务创建失败");
    }
    return ESP_OK;

out:
    s_initialized = false;
    s_snapshot.last_err = ret;
    xSemaphoreGive(s_mutex);
    return ret;
}

esp_err_t fridge_sht4x_get_snapshot(fridge_sht4x_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(SHT4X_IO_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool fridge_sht4x_is_ready(void)
{
    return s_initialized;
}
