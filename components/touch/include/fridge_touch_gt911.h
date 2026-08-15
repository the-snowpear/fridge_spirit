// 冰箱小精灵 P4 GT911 触摸驱动公共接口。
// 26-pin MIPI 屏座的触摸信号固定使用 GPIO15/16/17/18，逻辑电平必须为 3.3V。
// 目标模组：WKS70WSV114-WCT（1024x600，EK79007 + GT911）；行为对齐厂商 7 寸综合例程。

#pragma once

#include <stddef.h>

#include "fridge_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t raw_x_min;
    uint16_t raw_x_max;
    uint16_t raw_y_min;
    uint16_t raw_y_max;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
} fridge_touch_gt911_calibration_t;

// GT911 诊断快照：供 USB touch_status 与串口排查使用，不包含密钥等敏感信息。
typedef struct {
    bool ready;
    uint8_t i2c_address;
    char product_id[8];
    uint8_t last_status;
    uint16_t last_raw_x;
    uint16_t last_raw_y;
    uint16_t last_x;
    uint16_t last_y;
    bool last_pressed;
    uint8_t last_points;
    esp_err_t last_read_err;
} fridge_touch_gt911_status_t;

// 初始化 GT911 并读取 Product ID；未读到有效 ID 时不报告初始化成功。
// 复位时序、优先地址 0x14 与 0x8040 软复位对齐厂商 70 寸例程。
esp_err_t fridge_touch_gt911_init(void);

// 读取 GT911 当前第一个触点。函数只做有限时长 I2C 事务，不在 LVGL 回调中等待中断。
esp_err_t fridge_touch_gt911_read(fridge_touch_point_t *out);

// 是否已成功完成 Product ID 探测与软复位。
bool fridge_touch_gt911_is_ready(void);

// 返回当前生效的 7-bit I2C 地址；未就绪时返回 0。
uint8_t fridge_touch_gt911_get_address(void);

// 填充诊断快照；可在未初始化时调用（ready=false）。
esp_err_t fridge_touch_gt911_get_status(fridge_touch_gt911_status_t *out);

// 在触摸 I2C 总线上探测 0x14/0x5D（以及可选全扫摘要）。
// 若驱动尚未初始化，会临时创建总线并在返回前释放；已初始化则复用现有总线。
// found_mask bit0=0x14, bit1=0x5D。
esp_err_t fridge_touch_gt911_scan(uint8_t *found_mask, uint8_t *found_addrs, size_t max_addrs, size_t *found_count);

// 设置一次性横屏坐标校准。范围非法时拒绝更新；参数只保存在 RAM。
esp_err_t fridge_touch_gt911_set_calibration(const fridge_touch_gt911_calibration_t *calibration);

// 释放 GT911 占用的 I2C 总线资源，供维护和失败重试使用。
esp_err_t fridge_touch_gt911_deinit(void);

#ifdef __cplusplus
}
#endif
