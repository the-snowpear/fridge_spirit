// 冰箱小精灵 ESP32-P4 MPU6050 驱动。
// MPU6050 复用 GT911 已创建的 I2C0（GPIO16=SCL、GPIO17=SDA），本组件只添加
// 自己的 device handle，禁止创建、复位或删除共享总线。模块必须使用 3.3V 供电，
// AD0 按扩展板规划接 GND（地址 0x68）；INT 首版不接，使用 50Hz 轮询。

#include "fridge_sensors.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define MPU6050_I2C_PORT I2C_NUM_0
#define MPU6050_I2C_SPEED_HZ 100000
#define MPU6050_IO_TIMEOUT_MS 50
#define MPU6050_RETRY_DELAY_MS 10
#define MPU6050_SAMPLE_PERIOD_MS 20
#define MPU6050_RECOVERY_PERIOD_MS 1000
#define MPU6050_FAILURE_LIMIT 3
#define MPU6050_TASK_STACK_BYTES 4096
#define MPU6050_TASK_PRIORITY 4

// ---- 光照 ADC（GPIO19 = ADC1_CH3，oneshot 12bit，无校准）----
// 业务只需要相对亮度，不做电压校准；ADC_ATTEN_DB_12 覆盖 3.3V 供电光敏模块的 AO 输出范围。
// 本脚只作输入，禁止输出电平、禁止使能内部上下拉（详见 fridge_board_pins.h 约束注释）。
#define LIGHT_ADC_UNIT ADC_UNIT_1
#define LIGHT_ADC_CHANNEL ADC_CHANNEL_3
#define LIGHT_ADC_ATTEN ADC_ATTEN_DB_12
#define LIGHT_ADC_BITWIDTH ADC_BITWIDTH_12
#define SENSOR_TASK_PERIOD_MS 20     // 传感器 owner task 固定 20ms 周期（IMU 恢复 50Hz 主机读取）
#define SENSOR_ADC_PERIOD_MS 100     // 光照/MQ-5 ADC 独立单调时间门控：每 100ms 各采样一次
#define LIGHT_SAMPLES_PER_CYCLE 8    // 每次采样连续读取 8 次做轻量均值

// ---- MQ-5 可燃气体/烟雾（GPIO20 = ADC1_CH4，oneshot 12bit，无校准）----
// 与光照共用同一个 adc_oneshot unit；本组件禁止第二次 adc_oneshot_new_unit。
// AO 经 22kΩ 串联 + 33kΩ 对地分压并 100nF 滤波（详见 fridge_board_pins.h），
// 业务只读相对电平，不做 ppm 换算、不做电压校准。
#define MQ5_ADC_UNIT ADC_UNIT_1
#define MQ5_ADC_CHANNEL ADC_CHANNEL_4
#define MQ5_ADC_ATTEN ADC_ATTEN_DB_12
#define MQ5_ADC_BITWIDTH ADC_BITWIDTH_12
#define MQ5_SAMPLES_PER_CYCLE 8   // 每周期连续读取 8 次取整数均值
#define MQ5_WARMUP_PERIOD_SEC 60  // 日常预热固定 60 秒（模块与整板同上电）
#define MQ5_WINDOW_SIZE 64        // 64 样本滚动窗口，约 6.4 秒覆盖
// 校准公共接口取锁超时：IMU 慢速恢复期可能持锁约 1 秒（I2C 重试），
// 用户触发的校准操作允许等待更久，避免偶发 ESP_ERR_TIMEOUT；持锁时间本身仍然很短。
#define MQ5_CALIBRATION_LOCK_TIMEOUT_MS 1000
// NVS 命名空间与 key 严格固定；baseline_raw=0 约定为“未标定”。
#define MQ5_NVS_NAMESPACE "fridge_mq5"
#define MQ5_NVS_KEY_BURN_IN "burn_in_ok"
#define MQ5_NVS_KEY_BASELINE "baseline_raw"
// Kconfig bool=n 时 CONFIG_FRIDGE_MQ5_ENABLE 不会被定义，运行时表达式不能直接引用，
// 统一通过编译期常量判断启用状态，避免在关闭配置下出现未声明标识符。
#if CONFIG_FRIDGE_MQ5_ENABLE
#define FRIDGE_MQ5_ENABLED 1
#else
#define FRIDGE_MQ5_ENABLED 0
#endif

#define MPU6050_REG_SMPLRT_DIV 0x19
#define MPU6050_REG_CONFIG 0x1A
#define MPU6050_REG_GYRO_CONFIG 0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C
#define MPU6050_REG_ACCEL_XOUT_H 0x3B
#define MPU6050_REG_PWR_MGMT_1 0x6B
#define MPU6050_REG_PWR_MGMT_2 0x6C
#define MPU6050_REG_WHO_AM_I 0x75

#define MPU6050_WHO_AM_I_VALUE 0x68
#define MPU6050_SMPLRT_DIV_50_HZ 19
#define MPU6050_DLPF_42_HZ 0x03
#define MPU6050_ACCEL_RANGE_2G 0x00
#define MPU6050_GYRO_RANGE_250_DPS 0x00
#define MPU6050_PI 3.14159265358979323846f

static const char *TAG = "fridge_mpu6050";
static SemaphoreHandle_t s_mutex;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static adc_oneshot_unit_handle_t s_adc_unit;  // 光照与 MQ-5 共用的唯一 ADC1 unit
static fridge_sensor_snapshot_t s_snapshot;
static bool s_initialized;
static bool s_baseline_ready;
static float s_pitch_baseline_deg;
static float s_roll_baseline_deg;

// ---- MQ-5 状态（互斥锁保护；快照字段在 mq5_update_state_locked 中同步）----
static bool s_mq5_adc_ok;             // MQ 通道初始化成功且可采样
static int64_t s_mq5_start_ms;        // MQ 初始化时刻（预热起点，单调毫秒）
static bool s_mq5_burn_in_confirmed;  // 首次老化已确认（NVS burn_in_ok）
static bool s_mq5_baseline_ready;     // 基线已标定（NVS baseline_raw）
static uint16_t s_mq5_baseline_raw12; // 基线原始均值
static uint16_t s_mq5_window[MQ5_WINDOW_SIZE];  // 64 样本滚动窗口（静态数组，无动态分配）
static uint32_t s_mq5_window_count;   // 窗口已存样本数
static uint32_t s_mq5_window_index;   // 环形写位置
static uint32_t s_mq5_window_sum;     // 窗口总和（用于整数均值）

// 初始化光敏 ADC oneshot 通道。失败只记录 light_error 并保持 light_ready=false，
// 绝不阻断 MPU6050 主链路（ADC 错误不能让 sensors 整体 init 失败）。
static esp_err_t light_adc_init(void)
{
    if (s_adc_unit) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = LIGHT_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_unit);
    if (err != ESP_OK) {
        s_adc_unit = NULL;
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = LIGHT_ADC_ATTEN,
        .bitwidth = LIGHT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc_unit, LIGHT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        (void)adc_oneshot_del_unit(s_adc_unit);
        s_adc_unit = NULL;
    }
    return err;
}

// 在光照已创建的 ADC1 unit 上增加 GPIO20=ADC1_CH4 通道（禁止第二次 adc_oneshot_new_unit）。
// 先用 adc_oneshot_io_to_channel 校验引脚映射；不符合预期则返回 ESP_ERR_INVALID_ARG。
static esp_err_t mq5_adc_init(void)
{
    if (!FRIDGE_MQ5_ENABLED) return ESP_ERR_NOT_SUPPORTED;

    adc_unit_t io_unit = ADC_UNIT_1;
    adc_channel_t io_channel = ADC_CHANNEL_0;
    esp_err_t err = adc_oneshot_io_to_channel(FRIDGE_PIN_MQ5_AO, &io_unit, &io_channel);
    if (err != ESP_OK) return err;
    if (io_unit != MQ5_ADC_UNIT || io_channel != MQ5_ADC_CHANNEL) {
        ESP_LOGE(TAG, "MQ-5 GPIO%d 映射校验失败: unit=%d channel=%d（预期 ADC_UNIT_1/ADC_CHANNEL_4）",
                 FRIDGE_PIN_MQ5_AO, (int)io_unit, (int)io_channel);
        return ESP_ERR_INVALID_ARG;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = MQ5_ADC_ATTEN,
        .bitwidth = MQ5_ADC_BITWIDTH,
    };
    return adc_oneshot_config_channel(s_adc_unit, MQ5_ADC_CHANNEL, &chan_cfg);
}

// 每约 100ms 采样一次：连续 8 次 oneshot 读取取轻量均值，降低单次毛刺影响。
// 本次 8 次全部失败时保留上次有效值（light_ready 与数值都不变），仅更新 light_error 供诊断。
static void light_sample_once(void)
{
    if (!s_adc_unit) return;

    int sum = 0;
    int ok_count = 0;
    esp_err_t last_err = ESP_OK;
    for (int i = 0; i < LIGHT_SAMPLES_PER_CYCLE; i++) {
        int raw = 0;
        const esp_err_t err = adc_oneshot_read(s_adc_unit, LIGHT_ADC_CHANNEL, &raw);
        if (err == ESP_OK) {
            sum += raw;
            ok_count++;
        } else {
            last_err = err;
        }
    }

    if (ok_count == 0) {
        // 全部失败：不覆盖上次有效值，只记录可诊断错误。
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS)) == pdTRUE) {
            s_snapshot.light_error = last_err;
            xSemaphoreGive(s_mutex);
        }
        return;
    }

    int raw_avg = sum / ok_count;
    if (raw_avg < 0) raw_avg = 0;
    if (raw_avg > FRIDGE_LIGHT_ADC_MAX_RAW) raw_avg = FRIDGE_LIGHT_ADC_MAX_RAW;
    // 实机确认极性 raw_high_dark：原始值越高越暗，保留原始读数比例，不做反相。
    const uint16_t value10 =
        (uint16_t)(((uint32_t)raw_avg * FRIDGE_LIGHT_VALUE_MAX_10BIT) / FRIDGE_LIGHT_ADC_MAX_RAW);
    const uint8_t percent =
        (uint8_t)(((uint32_t)value10 * 100U) / FRIDGE_LIGHT_VALUE_MAX_10BIT);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS)) == pdTRUE) {
        // delta 是本次业务亮度相对上次有效值的有符号变化；首次有效值固定为 0。
        const int16_t delta = s_snapshot.light_ready
                                  ? (int16_t)((int)value10 - (int)s_snapshot.light_value10bit)
                                  : 0;
        s_snapshot.light_ready = true;
        s_snapshot.light_error = ESP_OK;
        s_snapshot.light_raw12bit = (uint16_t)raw_avg;
        s_snapshot.light_value10bit = value10;
        s_snapshot.light_percent = percent;
        s_snapshot.light_delta = delta;
        s_snapshot.light_polarity = FRIDGE_LIGHT_POLARITY_RAW_HIGH_DARK;
        s_snapshot.ready = true;
        xSemaphoreGive(s_mutex);
    }
}

// ---- MQ-5 滚动窗口：静态固定数组 uint16_t[64]，无动态分配 ----
// 每个有效 100ms 均值样本进入窗口；窗口满 64 后环形覆盖，约 6.4 秒覆盖。
static void mq5_window_push_locked(uint16_t raw)
{
    if (s_mq5_window_count < MQ5_WINDOW_SIZE) {
        s_mq5_window[s_mq5_window_count] = raw;
        s_mq5_window_sum += raw;
        s_mq5_window_count++;
    } else {
        s_mq5_window_sum -= s_mq5_window[s_mq5_window_index];
        s_mq5_window[s_mq5_window_index] = raw;
        s_mq5_window_sum += raw;
        s_mq5_window_index = (s_mq5_window_index + 1) % MQ5_WINDOW_SIZE;
    }
}

static void mq5_window_clear_locked(void)
{
    s_mq5_window_count = 0;
    s_mq5_window_index = 0;
    s_mq5_window_sum = 0;
}

// 在锁内更新 MQ-5 快照字段与状态机；状态优先级严格为：
// DISABLED > ERROR(无有效样本) > WARMING_UP(60s) > NEEDS_BURN_IN > READY_TO_CALIBRATE > MONITORING。
// 偶发读取失败但已有旧样本时保留语义合理的状态，错误由 mq5_error 单独暴露。
static void mq5_update_state_locked(void)
{
    if (!FRIDGE_MQ5_ENABLED) {
        s_snapshot.mq5_state = FRIDGE_MQ5_STATE_DISABLED;
        s_snapshot.mq5_ready = false;
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const uint32_t elapsed = (now_ms > s_mq5_start_ms)
                                 ? (uint32_t)((now_ms - s_mq5_start_ms) / 1000)
                                 : 0;
    s_snapshot.mq5_warmup_elapsed_sec = elapsed;
    s_snapshot.mq5_daily_warmup_complete = elapsed >= MQ5_WARMUP_PERIOD_SEC;
    s_snapshot.mq5_ready = s_mq5_adc_ok && (s_snapshot.mq5_sample_count > 0);
    s_snapshot.mq5_burn_in_confirmed = s_mq5_burn_in_confirmed;
    s_snapshot.mq5_baseline_ready = s_mq5_baseline_ready;
    s_snapshot.mq5_baseline_raw_12bit = s_mq5_baseline_raw12;
    s_snapshot.mq5_delta_from_baseline =
        s_mq5_baseline_ready ? (int32_t)s_snapshot.mq5_raw_12bit - (int32_t)s_mq5_baseline_raw12
                             : 0;

    if ((!s_mq5_adc_ok || s_snapshot.mq5_error != ESP_OK) &&
        s_snapshot.mq5_sample_count == 0) {
        s_snapshot.mq5_state = FRIDGE_MQ5_STATE_ERROR;
        return;
    }
    if (!s_snapshot.mq5_daily_warmup_complete) {
        s_snapshot.mq5_state = FRIDGE_MQ5_STATE_WARMING_UP;
        return;
    }
    if (!s_mq5_burn_in_confirmed) {
        s_snapshot.mq5_state = FRIDGE_MQ5_STATE_NEEDS_BURN_IN;
        return;
    }
    if (!s_mq5_baseline_ready) {
        s_snapshot.mq5_state = FRIDGE_MQ5_STATE_READY_TO_CALIBRATE;
        return;
    }
    s_snapshot.mq5_state = FRIDGE_MQ5_STATE_MONITORING;
}

// 每 100ms 门控读取 8 次 oneshot 取整数均值；全部失败保留上次有效值并更新 mq5_error，
// 避免一次毛刺把状态永久锁死。读取成功推进滚动窗口并更新快照时间戳。
static void mq5_sample_once(void)
{
    if (!FRIDGE_MQ5_ENABLED || !s_adc_unit || !s_mq5_adc_ok) return;

    int sum = 0;
    int ok_count = 0;
    esp_err_t last_err = ESP_OK;
    for (int i = 0; i < MQ5_SAMPLES_PER_CYCLE; i++) {
        int raw = 0;
        const esp_err_t err = adc_oneshot_read(s_adc_unit, MQ5_ADC_CHANNEL, &raw);
        if (err == ESP_OK) {
            sum += raw;
            ok_count++;
        } else {
            last_err = err;
        }
    }

    if (ok_count == 0) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS)) == pdTRUE) {
            s_snapshot.mq5_error = last_err;
            mq5_update_state_locked();
            xSemaphoreGive(s_mutex);
        }
        return;
    }

    int raw_avg = sum / ok_count;
    if (raw_avg < 0) raw_avg = 0;
    if (raw_avg > FRIDGE_LIGHT_ADC_MAX_RAW) raw_avg = FRIDGE_LIGHT_ADC_MAX_RAW;
    const uint16_t raw12 = (uint16_t)raw_avg;
    // 相对读数与 ADC 满量程百分比，均为相对量，不是 ppm。
    const uint16_t value10 = (uint16_t)(((uint32_t)raw12 * 1023U + 2047U) / 4095U);
    uint8_t percent = (uint8_t)(((uint32_t)raw12 * 100U + 2047U) / 4095U);
    if (percent > 100U) percent = 100U;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS)) == pdTRUE) {
        s_snapshot.mq5_error = ESP_OK;
        s_snapshot.mq5_raw_12bit = raw12;
        s_snapshot.mq5_value_10bit = value10;
        s_snapshot.mq5_adc_percent = percent;
        s_snapshot.mq5_sample_count++;
        s_snapshot.mq5_updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
        mq5_window_push_locked(raw12);
        mq5_update_state_locked();
        xSemaphoreGive(s_mutex);
    }
}

// init 时只读加载老化/基线；namespace/key 不存在属于首次使用，绝不使 init 失败。
// baseline_raw=0 约定为“未标定”，避免与无效存储值混淆。
static void mq5_nvs_load(void)
{
    if (!FRIDGE_MQ5_ENABLED) return;
    nvs_handle_t handle;
    if (nvs_open(MQ5_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t burn_in = 0;
    uint16_t baseline = 0;
    if (nvs_get_u8(handle, MQ5_NVS_KEY_BURN_IN, &burn_in) == ESP_OK && burn_in != 0) {
        s_mq5_burn_in_confirmed = true;
    }
    if (nvs_get_u16(handle, MQ5_NVS_KEY_BASELINE, &baseline) == ESP_OK && baseline != 0) {
        s_mq5_baseline_ready = true;
        s_mq5_baseline_raw12 = baseline;
    }
    nvs_close(handle);
}

// 新 I2C master 驱动会在总线内部串行化事务；这里只做一次有限重试，
// 不调用 i2c_master_bus_reset，避免影响同总线的 GT911 与 SHT40。
static esp_err_t mpu6050_read(i2c_master_dev_handle_t device,
                              uint8_t reg,
                              uint8_t *data,
                              size_t length,
                              bool retry)
{
    if (!device || !data || length == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_transmit_receive(device, &reg, 1, data, length,
                                                MPU6050_IO_TIMEOUT_MS);
    if (err != ESP_OK && retry) {
        vTaskDelay(pdMS_TO_TICKS(MPU6050_RETRY_DELAY_MS));
        err = i2c_master_transmit_receive(device, &reg, 1, data, length,
                                          MPU6050_IO_TIMEOUT_MS);
    }
    return err;
}

static esp_err_t mpu6050_write(uint8_t reg, uint8_t value)
{
    if (!s_device) return ESP_ERR_INVALID_STATE;
    const uint8_t data[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(s_device, data, sizeof(data),
                                        MPU6050_IO_TIMEOUT_MS);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(MPU6050_RETRY_DELAY_MS));
        err = i2c_master_transmit(s_device, data, sizeof(data),
                                  MPU6050_IO_TIMEOUT_MS);
    }
    return err;
}

// 在串行启动阶段探测 0x68/0x69。WHO_AM_I 固定返回 0x68，不能用它推断 AD0。
// 若启动时模块暂时离线，保留规划地址 0x68 的 device handle，后台只通过该句柄慢速恢复。
static esp_err_t mpu6050_attach_locked(void)
{
    static const uint8_t addresses[] = {
        FRIDGE_MPU6050_PRIMARY_ADDR,
        FRIDGE_MPU6050_SECONDARY_ADDR,
    };
    esp_err_t last_err = ESP_ERR_NOT_FOUND;

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[i],
            .scl_speed_hz = MPU6050_I2C_SPEED_HZ,
        };
        i2c_master_dev_handle_t candidate = NULL;
        last_err = i2c_master_bus_add_device(s_bus, &config, &candidate);
        if (last_err != ESP_OK) continue;

        uint8_t who_am_i = 0;
        last_err = mpu6050_read(candidate, MPU6050_REG_WHO_AM_I,
                                &who_am_i, 1, false);
        if (last_err == ESP_OK && who_am_i == MPU6050_WHO_AM_I_VALUE) {
            s_device = candidate;
            s_snapshot.imu_address = addresses[i];
            s_snapshot.imu_who_am_i = who_am_i;
            return ESP_OK;
        }
        if (last_err == ESP_OK) last_err = ESP_ERR_NOT_FOUND;
        (void)i2c_master_bus_rm_device(candidate);
    }

    i2c_device_config_t fallback_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FRIDGE_MPU6050_PRIMARY_ADDR,
        .scl_speed_hz = MPU6050_I2C_SPEED_HZ,
    };
    esp_err_t add_err = i2c_master_bus_add_device(s_bus, &fallback_config, &s_device);
    if (add_err != ESP_OK) return add_err;
    s_snapshot.imu_address = FRIDGE_MPU6050_PRIMARY_ADDR;
    return last_err;
}

// 按官方寄存器手册与 Motion Driver 时序完成 MPU6050 复位与配置的 I2C 事务部分：
// 硬复位、X 轴陀螺 PLL、42Hz DLPF、50Hz 输出、±2g 和 ±250dps，复位后读回关键寄存器
// 校验，避免写失败却误报就绪。本函数不持有快照锁、不写 s_snapshot，可在锁外执行
// （后台恢复探测专用）；成功时通过 out_who 返回 WHO_AM_I，由调用方在短锁内提交快照。
static esp_err_t mpu6050_configure_i2c(bool wait_for_power, uint8_t *out_who)
{
    if (wait_for_power) vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t who_am_i = 0;
    esp_err_t err = mpu6050_read(s_device, MPU6050_REG_WHO_AM_I,
                                 &who_am_i, 1, true);
    if (err != ESP_OK) return err;
    if (who_am_i != MPU6050_WHO_AM_I_VALUE) return ESP_ERR_NOT_FOUND;

    err = mpu6050_write(MPU6050_REG_PWR_MGMT_1, 0x80);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));
    if ((err = mpu6050_write(MPU6050_REG_PWR_MGMT_1, 0x01)) != ESP_OK ||
        (err = mpu6050_write(MPU6050_REG_PWR_MGMT_2, 0x00)) != ESP_OK ||
        (err = mpu6050_write(MPU6050_REG_CONFIG, MPU6050_DLPF_42_HZ)) != ESP_OK ||
        (err = mpu6050_write(MPU6050_REG_SMPLRT_DIV, MPU6050_SMPLRT_DIV_50_HZ)) != ESP_OK ||
        (err = mpu6050_write(MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_RANGE_2G)) != ESP_OK ||
        (err = mpu6050_write(MPU6050_REG_GYRO_CONFIG, MPU6050_GYRO_RANGE_250_DPS)) != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    const uint8_t verify_regs[] = {
        MPU6050_REG_PWR_MGMT_1,
        MPU6050_REG_CONFIG,
        MPU6050_REG_SMPLRT_DIV,
        MPU6050_REG_ACCEL_CONFIG,
        MPU6050_REG_GYRO_CONFIG,
    };
    const uint8_t verify_values[] = {
        0x01,
        MPU6050_DLPF_42_HZ,
        MPU6050_SMPLRT_DIV_50_HZ,
        MPU6050_ACCEL_RANGE_2G,
        MPU6050_GYRO_RANGE_250_DPS,
    };
    for (size_t i = 0; i < sizeof(verify_regs) / sizeof(verify_regs[0]); ++i) {
        uint8_t value = 0;
        err = mpu6050_read(s_device, verify_regs[i], &value, 1, true);
        if (err != ESP_OK) return err;
        if (value != verify_values[i]) return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_who) *out_who = who_am_i;
    return ESP_OK;
}

// 在持有快照锁的前提下完成配置并提交快照（串行 init 路径专用）。
static esp_err_t mpu6050_configure_locked(bool wait_for_power)
{
    uint8_t who_am_i = 0;
    esp_err_t err = mpu6050_configure_i2c(wait_for_power, &who_am_i);
    if (err != ESP_OK) return err;

    s_snapshot.imu_who_am_i = who_am_i;
    s_snapshot.imu_ready = true;
    s_snapshot.imu_error = ESP_OK;
    s_snapshot.consecutive_failures = 0;
    s_baseline_ready = false;
    s_snapshot.baseline_ready = false;
    ESP_LOGI(TAG,
             "MPU6050 就绪: I2C0 SCL=GPIO%d SDA=GPIO%d addr=0x%02X WHO_AM_I=0x%02X 50Hz",
             FRIDGE_PIN_SENSOR_I2C_SCL, FRIDGE_PIN_SENSOR_I2C_SDA,
             s_snapshot.imu_address, who_am_i);
    return ESP_OK;
}

static int16_t mpu6050_be16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static float mpu6050_angle_difference(float current, float baseline)
{
    float delta = current - baseline;
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    return delta;
}

// 一次 burst 读取 14 字节，确保三轴加速度、温度和三轴角速度来自同一个采样时刻。
static esp_err_t mpu6050_sample_locked(void)
{
    uint8_t raw[14] = {0};
    esp_err_t err = mpu6050_read(s_device, MPU6050_REG_ACCEL_XOUT_H,
                                 raw, sizeof(raw), true);
    if (err != ESP_OK) return err;

    s_snapshot.accel_x_g = (float)mpu6050_be16(&raw[0]) / 16384.0f;
    s_snapshot.accel_y_g = (float)mpu6050_be16(&raw[2]) / 16384.0f;
    s_snapshot.accel_z_g = (float)mpu6050_be16(&raw[4]) / 16384.0f;
    s_snapshot.imu_temperature_c = ((float)mpu6050_be16(&raw[6]) / 340.0f) + 36.53f;
    s_snapshot.gyro_x_dps = (float)mpu6050_be16(&raw[8]) / 131.0f;
    s_snapshot.gyro_y_dps = (float)mpu6050_be16(&raw[10]) / 131.0f;
    s_snapshot.gyro_z_dps = (float)mpu6050_be16(&raw[12]) / 131.0f;

    s_snapshot.pitch_deg = atan2f(s_snapshot.accel_y_g,
                                  sqrtf((s_snapshot.accel_x_g * s_snapshot.accel_x_g) +
                                        (s_snapshot.accel_z_g * s_snapshot.accel_z_g))) *
                           (180.0f / MPU6050_PI);
    s_snapshot.roll_deg = atan2f(-s_snapshot.accel_x_g, s_snapshot.accel_z_g) *
                          (180.0f / MPU6050_PI);
    if (!s_baseline_ready) {
        s_pitch_baseline_deg = s_snapshot.pitch_deg;
        s_roll_baseline_deg = s_snapshot.roll_deg;
        s_baseline_ready = true;
        s_snapshot.baseline_ready = true;
    }
    const float pitch_delta = mpu6050_angle_difference(s_snapshot.pitch_deg,
                                                       s_pitch_baseline_deg);
    const float roll_delta = mpu6050_angle_difference(s_snapshot.roll_deg,
                                                      s_roll_baseline_deg);
    s_snapshot.angle_delta = sqrtf((pitch_delta * pitch_delta) +
                                   (roll_delta * roll_delta));

    // 去掉静态 1g 重力分量，避免冰箱门静止时仍被误报为持续震动。
    const float magnitude = sqrtf((s_snapshot.accel_x_g * s_snapshot.accel_x_g) +
                                  (s_snapshot.accel_y_g * s_snapshot.accel_y_g) +
                                  (s_snapshot.accel_z_g * s_snapshot.accel_z_g));
    s_snapshot.vibration_peak = fabsf(magnitude - 1.0f);
    s_snapshot.imu_ready = true;
    s_snapshot.imu_error = ESP_OK;
    s_snapshot.consecutive_failures = 0;
    s_snapshot.sample_count++;
    s_snapshot.updated_at_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

// 传感器 owner task：固定 20ms 周期（vTaskDelayUntil 防漂移），IMU 每 20ms burst 读一帧
// （50Hz 主机读取）；光照与 MQ-5 共用 ADC1，用独立单调时间门控每 100ms 各采样一次
// （MQ-5 64 样本窗口约 6.4 秒覆盖）。IMU 失败连续达阈值后转入约 1 秒一次的恢复探测，
// I2C 事务在锁外执行，不阻塞光照/MQ 的 100ms 采样与 USB 快照读取。
static void mpu6050_task(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();
    int64_t next_mpu_sample_ms = 0;   // 下一次 IMU 采样时刻（20ms 节拍）
    int64_t next_mpu_retry_ms = 0;    // 下一次 IMU 恢复探测时刻（约 1 秒）
    int64_t next_light_sample_ms = 0; // 下一次光照 ADC 采样时刻（100ms 门控）
    int64_t next_mq_sample_ms = 0;    // 下一次 MQ-5 ADC 采样时刻（100ms 门控）
    while (true) {
        const int64_t now_ms = esp_timer_get_time() / 1000;

        // ---- IMU：就绪时按 20ms 门控 burst 读一帧；失败连续达阈值后转入慢速恢复 ----
        bool imu_ready = false;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS + 10)) == pdTRUE) {
            imu_ready = s_snapshot.imu_ready;
            xSemaphoreGive(s_mutex);
        }

        if (imu_ready) {
            if (now_ms >= next_mpu_sample_ms) {
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS + 10)) == pdTRUE) {
                    esp_err_t err = mpu6050_sample_locked();
                    if (err != ESP_OK) {
                        s_snapshot.imu_error = err;
                        s_snapshot.consecutive_failures++;
                        if (s_snapshot.consecutive_failures >= MPU6050_FAILURE_LIMIT) {
                            s_snapshot.imu_ready = false;
                            s_snapshot.baseline_ready = false;
                            s_baseline_ready = false;
                            next_mpu_retry_ms = now_ms + MPU6050_RECOVERY_PERIOD_MS;
                            ESP_LOGW(TAG, "MPU6050 连续通信失败，转入慢速恢复: %s",
                                     esp_err_to_name(err));
                        }
                    }
                    xSemaphoreGive(s_mutex);
                }
                next_mpu_sample_ms = now_ms + MPU6050_SAMPLE_PERIOD_MS;
            }
        } else if (now_ms >= next_mpu_retry_ms) {
            next_mpu_retry_ms = now_ms + MPU6050_RECOVERY_PERIOD_MS;
            // 慢速恢复：重新读取 WHO_AM_I 并写配置，不删除 device handle，也不私自复位
            // 共享 I2C0。I2C 配置重试在锁外执行（可能耗时数百 ms），不阻塞本周期光照/MQ
            // 采样与 USB 快照读取；成功后短锁提交快照并立即补采一帧。
            uint8_t recovered_who = 0;
            const esp_err_t recover_err = mpu6050_configure_i2c(false, &recovered_who);
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS + 10)) == pdTRUE) {
                if (recover_err == ESP_OK) {
                    s_snapshot.imu_who_am_i = recovered_who;
                    s_snapshot.imu_ready = true;
                    s_snapshot.imu_error = ESP_OK;
                    s_snapshot.consecutive_failures = 0;
                    s_baseline_ready = false;
                    s_snapshot.baseline_ready = false;
                    const esp_err_t sample_err = mpu6050_sample_locked();
                    if (sample_err != ESP_OK) {
                        s_snapshot.imu_error = sample_err;
                        s_snapshot.consecutive_failures++;
                        s_snapshot.imu_ready = false;
                    }
                    next_mpu_sample_ms = now_ms + MPU6050_SAMPLE_PERIOD_MS;
                } else {
                    s_snapshot.imu_error = recover_err;
                    s_snapshot.consecutive_failures++;
                }
                xSemaphoreGive(s_mutex);
            }
        }

        // ---- ADC：与 I2C 无关；光照与 MQ-5 各自 100ms 单调时间门控，每周期至多各采样一次 ----
        if (now_ms >= next_light_sample_ms) {
            light_sample_once();
            next_light_sample_ms = now_ms + SENSOR_ADC_PERIOD_MS;
        }
        if (now_ms >= next_mq_sample_ms) {
            mq5_sample_once();
            next_mq_sample_ms = now_ms + SENSOR_ADC_PERIOD_MS;
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SENSOR_TASK_PERIOD_MS));
    }
}

esp_err_t fridge_sensors_init(void)
{
    if (s_initialized) return ESP_OK;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_initialized) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.ready = true;
    s_snapshot.imu_error = ESP_ERR_INVALID_STATE;
    s_snapshot.mq5_state = FRIDGE_MQ5_STATE_DISABLED;  // CONFIG 关闭时保持 DISABLED

    // 光照与 MQ-5 共用同一个 ADC1 unit：unit 创建/通道配置失败只记录可诊断错误，
    // 保持 light_ready/mq5_ready=false，绝不导致 sensors 整体 init 失败。
    esp_err_t adc_err = light_adc_init();
    s_mq5_adc_ok = false;
    if (adc_err != ESP_OK) {
        s_snapshot.light_ready = false;
        s_snapshot.light_error = adc_err;
        s_snapshot.mq5_error = adc_err;
        ESP_LOGW(TAG, "光照 ADC 初始化失败，lightReady=false: %s（检查 GPIO19 接线与 3.3V 供电）",
                 esp_err_to_name(adc_err));
    } else {
        s_snapshot.light_ready = false;  // 首个有效样本到达前保持未就绪
        s_snapshot.light_error = ESP_OK;
        // MQ-5 在既有 unit 上增加 GPIO20=ADC1_CH4 通道；失败只禁用 MQ，不影响光照。
        esp_err_t mq5_err = mq5_adc_init();
        if (mq5_err != ESP_OK) {
            s_snapshot.mq5_error = mq5_err;
            ESP_LOGW(TAG, "MQ-5 ADC 通道初始化失败，mq5Ready=false: %s（检查 GPIO20 分压/接线）",
                     esp_err_to_name(mq5_err));
        } else {
            s_mq5_adc_ok = true;
            s_snapshot.mq5_error = ESP_OK;
            s_mq5_start_ms = esp_timer_get_time() / 1000;  // 预热起点（模块与整板同上电）
            mq5_nvs_load();  // 只读加载老化/基线；缺失不阻断
            s_snapshot.mq5_burn_in_confirmed = s_mq5_burn_in_confirmed;
            s_snapshot.mq5_baseline_ready = s_mq5_baseline_ready;
            s_snapshot.mq5_baseline_raw_12bit = s_mq5_baseline_raw12;
        }
    }

    // GT911 必须先建立共享 I2C0；这里绝不回退为新建总线，以免出现双 bus handle。
    // 总线不可用时只记录 imu_error，仍创建 owner task 让光照/MQ 继续运行；
    // 后台恢复探测会按约 1 秒一次持续尝试，但不会因 IMU 故障拖慢 ADC 采样。
    esp_err_t err = i2c_master_get_bus_handle(MPU6050_I2C_PORT, &s_bus);
    if (err != ESP_OK) {
        s_snapshot.imu_error = err;
        ESP_LOGE(TAG, "获取 I2C0 共享总线失败: %s（需先初始化 GT911）",
                 esp_err_to_name(err));
    } else {
        // 仅任务创建失败后的人工重试可能已有 device handle；每次 init 最多执行一次配置。
        if (s_device) {
            err = mpu6050_configure_locked(true);
        } else {
            err = mpu6050_attach_locked();
            if (err == ESP_OK) err = mpu6050_configure_locked(true);
        }
        if (!s_device) {
            s_snapshot.imu_error = err;
            ESP_LOGW(TAG,
                     "MPU6050 首次探测失败，将在后台按 1Hz 恢复: %s（检查 3.3V/GND、GPIO16/17、AD0）",
                     esp_err_to_name(err));
        } else if (err != ESP_OK) {
            s_snapshot.imu_ready = false;
            s_snapshot.imu_error = err;
            ESP_LOGW(TAG, "MPU6050 首次配置失败，将在后台按 1Hz 恢复: %s",
                     esp_err_to_name(err));
        }
    }

    if (xTaskCreate(mpu6050_task, "mpu6050", MPU6050_TASK_STACK_BYTES,
                    NULL, MPU6050_TASK_PRIORITY, NULL) != pdPASS) {
        // 任务未启动时不应遗留 MPU device；只移除自己的 handle，绝不删除共享 I2C0。
        esp_err_t remove_err = ESP_ERR_INVALID_STATE;
        if (s_device) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                remove_err = i2c_master_bus_rm_device(s_device);
                if (remove_err == ESP_OK) break;
                vTaskDelay(pdMS_TO_TICKS(MPU6050_RETRY_DELAY_MS));
            }
            if (remove_err == ESP_OK) {
                s_device = NULL;
                s_bus = NULL;
                s_snapshot.imu_address = 0;
                s_snapshot.imu_who_am_i = 0;
            } else {
                ESP_LOGW(TAG, "MPU6050 任务创建失败且 device 移除失败: %s",
                         esp_err_to_name(remove_err));
            }
        }
        s_snapshot.imu_ready = false;
        s_snapshot.baseline_ready = false;
        s_baseline_ready = false;
        s_snapshot.imu_error = ESP_ERR_NO_MEM;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    // 初始化完成后立即计算一次 MQ-5 状态（预热起点已定；首样本前为 WARMING_UP/ERROR）。
    mq5_update_state_locked();
    s_initialized = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t fridge_sensors_get_snapshot(fridge_sensor_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_mutex || !s_initialized) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MPU6050_IO_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

// ---- MQ-5 校准公共操作（USB 协议任务调用；均线程安全）----
// 三个函数统一在锁内完成“前置校验 -> NVS set/commit -> 内存状态更新”：commit 成功后才改 RAM，
// NVS 失败时 RAM 不变并原样返回错误码，绝不伪装成功；所有返回路径都会释放互斥锁。
// 此操作只由人工 USB 触发，允许短暂暂停采样换取 RAM/NVS 原子一致性。

// 确认首次老化（烧机）完成：显式写 NVS burn_in_ok=1 并 commit，再更新快照状态。
esp_err_t fridge_sensors_mq5_confirm_burn_in(void)
{
    if (!FRIDGE_MQ5_ENABLED) return ESP_ERR_INVALID_STATE;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MQ5_CALIBRATION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // 前置：MQ 启用且 ADC 已有有效样本、日常预热 60 秒完成、当前尚未确认老化。
    const bool can_confirm = s_mq5_adc_ok &&
                             (s_snapshot.mq5_sample_count > 0) &&
                             s_snapshot.mq5_daily_warmup_complete &&
                             !s_mq5_burn_in_confirmed;
    if (!can_confirm) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQ5_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, MQ5_NVS_KEY_BURN_IN, 1);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    // NVS commit 成功后才在锁内更新内存状态；失败时 RAM 不变并原样返回。
    s_mq5_burn_in_confirmed = true;
    mq5_update_state_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "MQ-5 老化确认已写入 NVS (burn_in_ok=1)");
    return ESP_OK;
}

// 标定基线：前置为 MQ 启用、采样运行、预热 60 秒完成、老化已确认、64 样本窗口已满；
// 锁内稳定取窗口整数均值写 NVS baseline_raw 并 commit，commit 成功后更新 RAM 与快照。
esp_err_t fridge_sensors_mq5_capture_baseline(void)
{
    if (!FRIDGE_MQ5_ENABLED) return ESP_ERR_INVALID_STATE;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MQ5_CALIBRATION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // 锁内校验前置条件并取窗口均值；窗口满 64 要求样本连续有效约 6.4 秒。
    const bool warmup_done = s_snapshot.mq5_daily_warmup_complete;
    const bool window_ok = s_mq5_adc_ok && (s_snapshot.mq5_sample_count > 0) &&
                           s_mq5_burn_in_confirmed &&
                           (s_mq5_window_count >= MQ5_WINDOW_SIZE);
    const uint16_t baseline = window_ok ? (uint16_t)(s_mq5_window_sum / MQ5_WINDOW_SIZE) : 0;
    if (!warmup_done || !window_ok) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQ5_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, MQ5_NVS_KEY_BASELINE, baseline);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    // NVS commit 成功后才在锁内更新内存状态；失败时 RAM 不变并原样返回。
    s_mq5_baseline_ready = true;
    s_mq5_baseline_raw12 = baseline;
    mq5_update_state_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "MQ-5 基线已标定并写入 NVS (baseline_raw=%u)", baseline);
    return ESP_OK;
}

// 复位校准：锁内擦除 NVS 的 burn_in_ok 与 baseline_raw 并 commit，成功后清空老化/基线/窗口，
// 并把公开样本计数归零，使 sample_count 从 0 重新累计到 64；
// 状态回到 WARMING_UP 或 NEEDS_BURN_IN。只擦本组件 namespace，不影响其他数据。
esp_err_t fridge_sensors_mq5_reset_calibration(void)
{
    if (!FRIDGE_MQ5_ENABLED) return ESP_ERR_INVALID_STATE;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MQ5_CALIBRATION_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQ5_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        (void)nvs_erase_key(handle, MQ5_NVS_KEY_BURN_IN);    // key 不存在视为已擦除
        (void)nvs_erase_key(handle, MQ5_NVS_KEY_BASELINE);
        err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return err;
    }

    // NVS 擦除 commit 成功后才在锁内清空内存状态与滚动窗口，并把公开样本计数归零。
    s_mq5_burn_in_confirmed = false;
    s_mq5_baseline_ready = false;
    s_mq5_baseline_raw12 = 0;
    mq5_window_clear_locked();
    s_snapshot.mq5_sample_count = 0;
    s_snapshot.mq5_delta_from_baseline = 0;
    mq5_update_state_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "MQ-5 校准已复位（擦除 burn_in_ok/baseline_raw，样本计数归零）");
    return ESP_OK;
}
