#pragma once

// ESP32-P4 MPU6050 公共快照接口。
// 硬件固定复用 I2C0：GPIO16=SCL、GPIO17=SDA，模块只允许 3.3V 供电。
// 旧 S3 GPIO1 光敏 ADC、GPIO4/5 legacy I2C 和跨组件软件锁接口已移除；
// P4 光敏模块改走 GPIO19=ADC1_CH3（oneshot 12bit，见下方光照字段说明）。

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_MPU6050_PRIMARY_ADDR 0x68
#define FRIDGE_MPU6050_SECONDARY_ADDR 0x69

// 光照业务换算常量：light_raw12bit 为 12-bit ADC 原始值（0..4095），
// light_value10bit 为缩放到 0..1023 的相对光敏读数（低=亮，高=暗）；禁止把本组数值换算成物理 lux。
#define FRIDGE_LIGHT_ADC_MAX_RAW 4095
#define FRIDGE_LIGHT_VALUE_MAX_10BIT 1023
// 协议极性：实机确认 raw_high_dark，ADC 原始值越高表示环境越暗。
#define FRIDGE_LIGHT_POLARITY_RAW_HIGH_DARK "raw_high_dark"

// MQ-5 可燃气体/烟雾传感器工作状态机。
// 状态优先级（严格从高到低）：
//   DISABLED            -> CONFIG_FRIDGE_MQ5_ENABLE 关闭
//   ERROR               -> ADC 初始化失败或持续读取失败且从未得到有效样本
//   WARMING_UP          -> 上电预热中（固定 60 秒）
//   NEEDS_BURN_IN       -> 预热完成但用户尚未确认首次老化（烧机）
//   READY_TO_CALIBRATE  -> 老化已确认、基线未标定（等待 capture_baseline）
//   MONITORING          -> 基线已标定，进入日常监测
// 读取偶发失败但已有旧样本时保留语义合理的状态，错误通过 mq5_error 单独暴露。
typedef enum {
    FRIDGE_MQ5_STATE_DISABLED = 0,
    FRIDGE_MQ5_STATE_ERROR,
    FRIDGE_MQ5_STATE_WARMING_UP,
    FRIDGE_MQ5_STATE_NEEDS_BURN_IN,
    FRIDGE_MQ5_STATE_READY_TO_CALIBRATE,
    FRIDGE_MQ5_STATE_MONITORING,
} fridge_mq5_state_t;

// USB、WebUI 和 P4 本机 UI 共用的最近一次 MPU6050 快照。
// angle_delta 是相对本次驱动初始化后首个有效样本的安装姿态变化，不是相对水平面的绝对倾角。
typedef struct {
    bool ready;
    bool imu_ready;
    bool baseline_ready;
    uint8_t imu_address;
    uint8_t imu_who_am_i;
    esp_err_t imu_error;
    uint32_t consecutive_failures;
    uint32_t sample_count;
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float imu_temperature_c;
    float pitch_deg;
    float roll_deg;
    float angle_delta;
    float vibration_peak;
    // ---- 光照（GPIO19 光敏模块，ADC1_CH3，3.3V 模拟输入）----
    // 极性 raw_high_dark：ADC 原始值越高表示越暗，当前模块的关灯判定阈值为 light_value10bit > 600。
    // light_value10bit/light_percent/light_delta 均为相对光敏读数，不是物理 lux；
    // light_delta 为本次读数相对上次有效值的有符号变化，正值表示变暗，首次有效值固定为 0；
    // 单次采样全部失败时不覆盖上次有效值，仅更新 light_error 供诊断。
    bool light_ready;
    esp_err_t light_error;
    uint16_t light_raw12bit;
    uint16_t light_value10bit;
    uint8_t light_percent;
    int16_t light_delta;
    const char *light_polarity;  // 指向静态协议串，如 "raw_high_dark"
    int64_t updated_at_ms;
    // ---- MQ-5 可燃气体/烟雾（GPIO20 = ADC1_CH4，AO 经 22k/33k 分压 + 100nF）----
    // 首阶段只用 AO：raw_12bit 为 12-bit ADC 原始均值，value_10bit 为缩放到 0..1023 的
    // 相对读数，adc_percent 只是 ADC 满量程相对百分比，三者都不是 ppm，禁止换算成浓度。
    // 预热固定 60 秒；老化确认、基线标定持久化在 NVS namespace "fridge_mq5"。
    fridge_mq5_state_t mq5_state;
    bool mq5_ready;               // ADC 可用且至少有一个有效样本
    esp_err_t mq5_error;          // 最近一次采样/初始化错误；ESP_OK 表示正常
    uint16_t mq5_raw_12bit;       // 本周期 8 次 oneshot 均值（0..4095）
    uint16_t mq5_value_10bit;     // (raw*1023+2047)/4095 的相对读数
    uint8_t mq5_adc_percent;      // min(100,(raw*100+2047)/4095)，ADC 满量程相对值
    uint32_t mq5_sample_count;    // 累计有效样本数
    uint32_t mq5_updated_at_ms;   // 最近一次有效样本的单调毫秒时间
    uint32_t mq5_warmup_elapsed_sec;  // 自初始化起的预热经过秒数
    bool mq5_daily_warmup_complete;   // 预热 >= 60 秒
    bool mq5_burn_in_confirmed;       // 首次老化已由用户确认（NVS burn_in_ok）
    bool mq5_baseline_ready;          // 基线已标定（NVS baseline_raw 且本机校验通过）
    uint16_t mq5_baseline_raw_12bit;  // 基线原始均值（NVS baseline_raw）
    int32_t mq5_delta_from_baseline;  // 当前 raw 相对基线有符号差，无基线时为 0
} fridge_sensor_snapshot_t;

// 初始化 P4 MPU6050 轮询任务。必须在 GT911 创建 I2C0 后调用。
// 模块首次不在线时任务仍会启动，并通过已保留的 0x68 device handle 每秒恢复探测。
esp_err_t fridge_sensors_init(void);

// 读取内存快照，不直接访问 I2C，适合 USB/Web Serial 高频轮询。
esp_err_t fridge_sensors_get_snapshot(fridge_sensor_snapshot_t *out);

// ---- MQ-5 校准公共接口（由 USB 协议任务调用；均线程安全）----
// 三个函数统一在快照互斥锁内完成前置校验和 NVS set/erase+commit，并在锁内同步
// 更新 RAM 状态后再释放锁，保证 NVS/RAM 原子一致；代价是人工操作期间会短暂暂停采样。
// 失败返回错误码且 RAM 保持不变，绝不伪装成功。

// 确认首次老化（烧机）已完成：显式写 NVS key burn_in_ok=1 并 commit。
// 前置：CONFIG_FRIDGE_MQ5_ENABLE 开启且 MQ 采样已运行；否则返回 ESP_ERR_INVALID_STATE。
esp_err_t fridge_sensors_mq5_confirm_burn_in(void);

// 标定基线：取满 64 样本滚动窗口整数均值写入 NVS key baseline_raw 并 commit。
// 前置：MQ 启用、采样运行、预热 60 秒完成、老化已确认、窗口已满 64；否则 ESP_ERR_INVALID_STATE。
esp_err_t fridge_sensors_mq5_capture_baseline(void);

// 复位校准：擦除 NVS 的 burn_in_ok 与 baseline_raw 并 commit，清空老化/基线/窗口；
// 状态回到 WARMING_UP 或 NEEDS_BURN_IN。只擦本组件 namespace，不影响其他数据。
esp_err_t fridge_sensors_mq5_reset_calibration(void);

#ifdef __cplusplus
}
#endif
