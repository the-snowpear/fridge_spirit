// 冰箱小精灵 ESP32-P4 GT911 触摸驱动。
// 对齐慧勤智远 7 寸综合例程（comprehensive_routine_70inch_mipilcd / gt9xxx）：
//   I2C0 SDA=17 SCL=16、优先地址 0x14、400kHz、RST/INT 时序、0x8040 软复位、0x8150 读点。
// GT911 使用 16-bit 大端寄存器地址；本实现只轮询首个触点，避免在 LVGL 回调中做复杂解析。

#include "fridge_touch_gt911.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fridge_board_pins.h"

#define GT911_REG_CTRL       0x8040
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS     0x814E
#define GT911_REG_POINT_1    0x8150
#define GT911_STATUS_READY   0x80
#define GT911_MAX_POINTS     5
#define GT911_I2C_SPEED_HZ   400000
#define GT911_I2C_SPEED_FALLBACK_HZ 100000
#define GT911_IO_TIMEOUT_MS  50
#define GT911_INIT_TIMEOUT_MS 500
#define GT911_RESET_LOW_MS   10
#define GT911_RESET_HIGH_MS  10
#define GT911_RESET_SETTLE_MS 100
#define GT911_SOFT_RESET_MS  10

static const char *TAG = "fridge_gt911";
static SemaphoreHandle_t s_mutex;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static uint8_t s_address;
static bool s_initialized;
static char s_product_id[8];
static uint8_t s_last_status;
static uint16_t s_last_raw_x;
static uint16_t s_last_raw_y;
static uint16_t s_last_x;
static uint16_t s_last_y;
static bool s_last_pressed;
static uint8_t s_last_points;
static esp_err_t s_last_read_err = ESP_ERR_INVALID_STATE;

// WKS70 1024x600：厂商 EK79007 分支不做镜像翻转，默认 1:1 映射。
static fridge_touch_gt911_calibration_t s_calibration = {
    .raw_x_min = 0,
    .raw_x_max = 1023,
    .raw_y_min = 0,
    .raw_y_max = 599,
    .swap_xy = false,
    .invert_x = false,
    .invert_y = false,
};

static uint16_t scale_axis(uint16_t raw, uint16_t min, uint16_t max, uint16_t output_max)
{
    if (raw <= min) return 0;
    if (raw >= max) return output_max;
    return (uint16_t)(((uint32_t)(raw - min) * output_max) / (max - min));
}

static void transform_point(uint16_t raw_x, uint16_t raw_y, uint16_t *x, uint16_t *y)
{
    uint16_t tx = scale_axis(raw_x, s_calibration.raw_x_min, s_calibration.raw_x_max, 1023);
    uint16_t ty = scale_axis(raw_y, s_calibration.raw_y_min, s_calibration.raw_y_max, 599);
    if (s_calibration.swap_xy) {
        uint16_t swapped_x = scale_axis(raw_y, s_calibration.raw_y_min, s_calibration.raw_y_max, 1023);
        uint16_t swapped_y = scale_axis(raw_x, s_calibration.raw_x_min, s_calibration.raw_x_max, 599);
        tx = swapped_x;
        ty = swapped_y;
    }
    if (s_calibration.invert_x) tx = 1023U - tx;
    if (s_calibration.invert_y) ty = 599U - ty;
    *x = tx;
    *y = ty;
}

static bool product_id_is_gt9xxx(const uint8_t id[4])
{
    char text[5] = {0};
    memcpy(text, id, 4);
    // 厂商例程接受 GT911/9147/1158/9271/928；本工程 WKS70 固定为 911。
    if (strcmp(text, "911") == 0) return true;
    if (strcmp(text, "9147") == 0) return true;
    if (strcmp(text, "1158") == 0) return true;
    if (strcmp(text, "9271") == 0) return true;
    if (strcmp(text, "928") == 0) return true;
    // 兼容仅前三字节有效、第四字节非零填充的读法。
    if (id[0] == '9' && id[1] == '1' && id[2] == '1') return true;
    return false;
}

static esp_err_t gt911_read_locked(uint16_t reg, uint8_t *data, size_t length)
{
    if (!s_device || !data || length == 0) return ESP_ERR_INVALID_ARG;
    uint8_t address[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_master_transmit_receive(s_device, address, sizeof(address), data, length,
                                       GT911_IO_TIMEOUT_MS);
}

static esp_err_t gt911_write_u8_locked(uint16_t reg, uint8_t value)
{
    if (!s_device) return ESP_ERR_INVALID_STATE;
    uint8_t frame[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    return i2c_master_transmit(s_device, frame, sizeof(frame), GT911_IO_TIMEOUT_MS);
}

static esp_err_t gt911_probe_locked(uint8_t address, uint32_t scl_hz)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_hz,
    };
    i2c_master_dev_handle_t candidate = NULL;
    esp_err_t ret = i2c_master_bus_add_device(s_bus, &cfg, &candidate);
    if (ret != ESP_OK) return ret;

    uint8_t id[4] = {0};
    uint8_t addr_buf[2] = {(uint8_t)(GT911_REG_PRODUCT_ID >> 8), (uint8_t)GT911_REG_PRODUCT_ID};
    ret = i2c_master_transmit_receive(candidate, addr_buf, sizeof(addr_buf), id, sizeof(id),
                                       GT911_IO_TIMEOUT_MS);
    if (ret == ESP_OK && !product_id_is_gt9xxx(id)) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        s_device = candidate;
        s_address = address;
        memset(s_product_id, 0, sizeof(s_product_id));
        memcpy(s_product_id, id, 4);
        // 去掉尾部非打印填充，便于日志与 JSON。
        for (int i = 3; i >= 0; --i) {
            if (s_product_id[i] < 0x20 || s_product_id[i] > 0x7E) {
                s_product_id[i] = '\0';
            } else {
                break;
            }
        }
        ESP_LOGI(TAG, "GT911 ready addr=0x%02X product=%s scl=%lu",
                 address, s_product_id, (unsigned long)scl_hz);
        return ESP_OK;
    }
    (void)i2c_master_bus_rm_device(candidate);
    return ret;
}

// 厂商复位：RST 脉冲后把 INT 设为浮空输入，再等待芯片启动；地址由模组上电时序决定（本板期望 0x14）。
static esp_err_t gt911_reset_locked(void)
{
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << FRIDGE_PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "GT911 RST config failed");

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << FRIDGE_PIN_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "GT911 INT config failed");

    ESP_RETURN_ON_ERROR(gpio_set_level(FRIDGE_PIN_TOUCH_RST, 0), TAG, "GT911 RST low failed");
    vTaskDelay(pdMS_TO_TICKS(GT911_RESET_LOW_MS));
    ESP_RETURN_ON_ERROR(gpio_set_level(FRIDGE_PIN_TOUCH_RST, 1), TAG, "GT911 RST high failed");
    vTaskDelay(pdMS_TO_TICKS(GT911_RESET_HIGH_MS));

    // 释放 INT：进入读坐标阶段后由芯片驱动中断脚。
    int_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    int_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "GT911 INT float config failed");
    vTaskDelay(pdMS_TO_TICKS(GT911_RESET_SETTLE_MS));
    return ESP_OK;
}

static esp_err_t gt911_soft_reset_locked(void)
{
    // 0x02：软复位；0x00：结束复位并进入读坐标状态（厂商 gt9xxx_init）。
    esp_err_t ret = gt911_write_u8_locked(GT911_REG_CTRL, 0x02);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(GT911_SOFT_RESET_MS));
    return gt911_write_u8_locked(GT911_REG_CTRL, 0x00);
}

static esp_err_t gt911_create_bus_locked(void)
{
    if (s_bus) return ESP_OK;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = FRIDGE_PIN_TOUCH_SDA,
        .scl_io_num = FRIDGE_PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

static void gt911_release_device_locked(void)
{
    if (s_device) {
        (void)i2c_master_bus_rm_device(s_device);
        s_device = NULL;
    }
}

static void gt911_release_bus_locked(void)
{
    gt911_release_device_locked();
    if (s_bus) {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_address = 0;
    s_initialized = false;
    memset(s_product_id, 0, sizeof(s_product_id));
}

esp_err_t fridge_touch_gt911_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(GT911_INIT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    const bool bus_existed = s_bus != NULL;
    esp_err_t ret = gt911_create_bus_locked();
    if (ret != ESP_OK) goto out;
    ret = gt911_reset_locked();
    if (ret != ESP_OK) goto fail;

    // 厂商固定 0x14；少数批次可能为 0x5D。先 400kHz，再 100kHz 兜底。
    static const uint8_t addrs[] = {0x14, 0x5D};
    static const uint32_t speeds[] = {GT911_I2C_SPEED_HZ, GT911_I2C_SPEED_FALLBACK_HZ};
    ret = ESP_ERR_NOT_FOUND;
    for (size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]) && ret != ESP_OK; ++s) {
        for (size_t a = 0; a < sizeof(addrs) / sizeof(addrs[0]); ++a) {
            ret = gt911_probe_locked(addrs[a], speeds[s]);
            if (ret == ESP_OK) break;
        }
    }
    if (ret != ESP_OK) goto fail;

    ret = gt911_soft_reset_locked();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GT911 soft reset failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    s_last_pressed = false;
    s_last_points = 0;
    s_initialized = true;
    s_last_read_err = ESP_OK;
    xSemaphoreGive(s_mutex);
    return ESP_OK;

fail:
    if (bus_existed) {
        // 运行期 deinit 后的再次初始化会复用仍挂 SHT40/MPU6050 的共享总线；
        // 此时 GT911 探测失败也只能移除自己的 device handle，不能删除 I2C0。
        gt911_release_device_locked();
        s_address = 0;
        memset(s_product_id, 0, sizeof(s_product_id));
    } else {
        // 首次启动阶段还没有其他设备挂载，失败时可释放本组件刚创建的总线。
        gt911_release_bus_locked();
    }
out:
    s_initialized = false;
    s_last_read_err = ret;
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "GT911 init failed: %s (check 26-pin FPC touch pins INT/SCL/SDA/RST)",
             esp_err_to_name(ret));
    return ret;
}

esp_err_t fridge_touch_gt911_read(fridge_touch_point_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_initialized || !s_mutex) {
        s_last_read_err = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(GT911_IO_TIMEOUT_MS)) != pdTRUE) {
        s_last_read_err = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    uint8_t status = 0;
    esp_err_t ret = gt911_read_locked(GT911_REG_STATUS, &status, 1);
    s_last_status = status;

    if (ret == ESP_OK && (status & GT911_STATUS_READY) != 0) {
        const uint8_t points = status & 0x0F;
        s_last_points = points;
        if (points > 0 && points <= GT911_MAX_POINTS) {
            // 厂商从 0x8150 读 4 字节：X 低/高、Y 低/高。
            uint8_t point[4] = {0};
            ret = gt911_read_locked(GT911_REG_POINT_1, point, sizeof(point));
            if (ret == ESP_OK) {
                uint16_t raw_x = (uint16_t)point[0] | ((uint16_t)point[1] << 8);
                uint16_t raw_y = (uint16_t)point[2] | ((uint16_t)point[3] << 8);
                s_last_raw_x = raw_x;
                s_last_raw_y = raw_y;
                if (raw_x <= 4095 && raw_y <= 4095) {
                    uint16_t x = 0;
                    uint16_t y = 0;
                    transform_point(raw_x, raw_y, &x, &y);
                    out->pressed = true;
                    out->points = points;
                    out->x = x;
                    out->y = y;
                    s_last_pressed = true;
                    s_last_points = points;
                    s_last_x = x;
                    s_last_y = y;
                }
            }
        } else if (points == 0) {
            // GT911 只有在“数据就绪且触点数为 0”时才表示真实抬手。
            // 不能把单纯的 READY=0 当成释放，否则轮询间隙会让 LVGL 收到假 CLICKED。
            s_last_pressed = false;
        }
        // 清除数据就绪标记；失败只影响下一次轮询。
        (void)gt911_write_u8_locked(GT911_REG_STATUS, 0);
    } else if (ret == ESP_OK && s_last_pressed) {
        // READY=0 表示本轮没有新数据，保持上一次触摸状态和坐标。
        // 这与手机触摸栈的连续按压语义一致，滚动仲裁才能等到真正抬手。
        out->pressed = true;
        out->points = s_last_points;
        out->x = s_last_x;
        out->y = s_last_y;
    }

    s_last_read_err = ret;
    xSemaphoreGive(s_mutex);
    return ret;
}

bool fridge_touch_gt911_is_ready(void)
{
    return s_initialized;
}

uint8_t fridge_touch_gt911_get_address(void)
{
    return s_initialized ? s_address : 0;
}

esp_err_t fridge_touch_gt911_get_status(fridge_touch_gt911_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->ready = s_initialized;
    out->i2c_address = s_address;
    if (s_product_id[0]) {
        strlcpy(out->product_id, s_product_id, sizeof(out->product_id));
    }
    out->last_status = s_last_status;
    out->last_raw_x = s_last_raw_x;
    out->last_raw_y = s_last_raw_y;
    out->last_x = s_last_x;
    out->last_y = s_last_y;
    out->last_pressed = s_last_pressed;
    out->last_points = s_last_points;
    out->last_read_err = s_last_read_err;
    return ESP_OK;
}

esp_err_t fridge_touch_gt911_scan(uint8_t *found_mask, uint8_t *found_addrs, size_t max_addrs, size_t *found_count)
{
    if (found_mask) *found_mask = 0;
    if (found_count) *found_count = 0;

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(GT911_INIT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool temporary_bus = false;
    esp_err_t ret = ESP_OK;
    if (!s_bus) {
        ret = gt911_create_bus_locked();
        if (ret != ESP_OK) {
            xSemaphoreGive(s_mutex);
            return ret;
        }
        temporary_bus = true;
        (void)gt911_reset_locked();
    }

    static const uint8_t candidates[] = {0x14, 0x5D};
    size_t count = 0;
    uint8_t mask = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const uint8_t addr = candidates[i];
        // 运行期只做线程安全的地址 ACK 探测，禁止并发 add/rm 临时 device 泄漏句柄。
        const bool present = i2c_master_probe(s_bus, addr, GT911_IO_TIMEOUT_MS) == ESP_OK;
        if (present) {
            mask |= (uint8_t)(1U << i);
            if (found_addrs && count < max_addrs) {
                found_addrs[count++] = addr;
            } else if (!found_addrs) {
                count++;
            }
        }
    }

    if (found_mask) *found_mask = mask;
    if (found_count) *found_count = count;

    if (temporary_bus && !s_initialized) {
        gt911_release_bus_locked();
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t fridge_touch_gt911_set_calibration(const fridge_touch_gt911_calibration_t *calibration)
{
    if (!calibration || calibration->raw_x_max <= calibration->raw_x_min ||
        calibration->raw_y_max <= calibration->raw_y_min) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(GT911_IO_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_calibration = *calibration;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t fridge_touch_gt911_deinit(void)
{
    if (!s_mutex) return ESP_OK;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(GT911_INIT_TIMEOUT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    // I2C0 还挂载 SHT40 与 MPU6050。运行期反初始化只能移除 GT911 自己的设备，
    // 共享 bus handle 保持到固件生命周期结束；否则会破坏其他传感器并让再次初始化失效。
    gt911_release_device_locked();
    s_address = 0;
    s_initialized = false;
    memset(s_product_id, 0, sizeof(s_product_id));
    s_last_pressed = false;
    s_last_points = 0;
    s_last_read_err = ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
