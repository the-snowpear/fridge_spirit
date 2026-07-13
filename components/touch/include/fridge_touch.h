// 冰箱小精灵 FT6336U 触摸驱动公共接口。
// 接线注意：TP-INT、TP-SDA、TP-SCL、TP-RST 这四根触摸信号只在屏幕 20-pin FPC 上引出，
// 必须通过逐脚核对过的 FPC 转接板/排线连接；屏背 2x7 14-pin 排针不含这四根线。
// 如果当前只用 2x7 14-pin 排针接屏，触摸不可用属于硬件接口限制，但显示仍可独立点亮测试。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TOUCH_I2C_ADDR 0x48
#define FRIDGE_TOUCH_INT_GPIO 15

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
    uint8_t points;
} fridge_touch_point_t;

// 初始化 FT6336U 最小读点驱动。
// P4 使用独立 I2C0（SDA=GPIO17、SCL=GPIO16）并由驱动控制 GPIO18 复位；
// S3 存档路径仍复用 sensors 的 I2C0（SDA=GPIO4、SCL=GPIO5），公共 API 保持不变。
esp_err_t fridge_touch_init(void);

// 读取最近触摸坐标；该函数只做一次 I2C 寄存器读取，不阻塞等待中断。
esp_err_t fridge_touch_read(fridge_touch_point_t *out);

#ifdef __cplusplus
}
#endif
