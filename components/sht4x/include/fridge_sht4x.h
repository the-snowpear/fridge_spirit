// 冰箱小精灵 SHT40 温湿度传感器驱动公共接口。
// 挂载在共享 I2C0（GPIO16 SCL / GPIO17 SDA），与 GT911 触摸共用同一 bus handle。
// 地址 0x44（备用 0x45），命令 0xFD 高精度测量，6 字节响应，CRC-8 校验。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// SHT40 读数快照；valid=false 时其余字段无意义。
typedef struct {
    bool      valid;           // 最近一次读数 CRC 通过且换算合理
    float     temperature_c;   // 摄氏温度（-40 ~ +125 °C）
    float     humidity_rh;     // 相对湿度 %（已 clamp 到 0~100）
    uint8_t   i2c_address;     // 实际命中的 I2C 地址（0x44 / 0x45）
    esp_err_t last_err;        // 最近一次 I2C 操作返回值
    int64_t   last_update_us;  // 最近成功更新时间戳（esp_timer_get_time）
} fridge_sht4x_snapshot_t;

// 初始化：复用 GT911 已建的 I2C0 共享总线，探测地址，启动轮询任务。
// 必须在 fridge_touch_gt911_init() 之后调用（确保 I2C0 总线已建立）。
esp_err_t fridge_sht4x_init(void);

// 读取最新快照（mutex 保护，可在任意任务调用）。
esp_err_t fridge_sht4x_get_snapshot(fridge_sht4x_snapshot_t *out);

// 返回驱动是否已成功初始化。
bool fridge_sht4x_is_ready(void);

#ifdef __cplusplus
}
#endif
