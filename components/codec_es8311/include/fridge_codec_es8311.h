#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// GPIO28/29 同时连接板载 ES8311 与 OV5647 SCCB。
// 两类设备必须通过同一个二值 gate 分时访问，并复用本组件创建的 I2C1 总线句柄；
// 禁止摄像头另建一条 GPIO28/29 I2C 总线，也禁止在录音期间访问摄像头 SCCB。
typedef enum {
    FRIDGE_CODEC_I2C_GATE_OWNER_NONE = 0,
    FRIDGE_CODEC_I2C_GATE_OWNER_INIT,
    FRIDGE_CODEC_I2C_GATE_OWNER_AUDIO,
    FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA,
} fridge_codec_i2c_gate_owner_t;

// 初始化 GPIO28/29 上的共享 I2C1 主总线，不初始化 ES8311 或 I2S。
// 未来 camera 组件可先调用本接口，再通过 get_bus() 取得同一个总线句柄。
esp_err_t fridge_codec_shared_i2c_init(void);

// 返回共享 I2C1 总线句柄；未成功初始化时返回 NULL。
i2c_master_bus_handle_t fridge_codec_shared_i2c_get_bus(void);

// 申请/释放 GPIO28/29 共享总线 gate。
// gate 是允许跨任务释放的二值信号量：短录音可能由调用任务启动、录音任务收尾。
// unlock 必须使用与 lock 相同的 owner，避免错误释放另一外设正在持有的总线。
esp_err_t fridge_codec_shared_i2c_gate_lock(fridge_codec_i2c_gate_owner_t owner, TickType_t timeout_ticks);
esp_err_t fridge_codec_shared_i2c_gate_unlock(fridge_codec_i2c_gate_owner_t owner);
fridge_codec_i2c_gate_owner_t fridge_codec_shared_i2c_gate_get_owner(void);

// 初始化板载 ES8311 录音链路：I2C1 SDA=GPIO28/SCL=GPIO29，
// I2S0 MCLK=GPIO2、BCLK=GPIO4、WS=GPIO6、DOUT=GPIO3、DIN=GPIO5，
// 采样格式固定为 16kHz / 16-bit / mono PCM，MCLK=384 * sample_rate。
// NS4150B 的 PA_CTRL=GPIO11 从初始化开始始终保持低；esp_codec_dev 不接管该引脚。
esp_err_t fridge_codec_es8311_init(void);

// 启动/停止 ES8311 ADC 输入。start 成功后会持续持有共享 I2C gate，
// 直到 stop 完成，确保录音期间摄像头不会访问同一条 SCCB/I2C 线路。
esp_err_t fridge_codec_es8311_start_input(TickType_t gate_timeout_ticks);
esp_err_t fridge_codec_es8311_stop_input(void);

// 直接读取 ES8311 输出的 int16 PCM；不执行旧 INMP441 的 32-bit >> 14 转换。
// RX 与 TX 可并行，stop 会等待正在进行的读写退出后再关闭 codec，禁止上层绕过本接口操作 I2S0。
esp_err_t fridge_codec_es8311_read(int16_t *samples,
                                   size_t max_samples,
                                   size_t *sample_count,
                                   TickType_t timeout_ticks);

// ---- Realtime 双工扩展（ADC 16k 上行 + DAC 16k 下行）----
// 成功后持有共享 I2C gate（AUDIO owner）；PA_CTRL 仅在实际写 PCM 时拉高，
// 停止/失败路径必须强制拉低，防止 NS4150B 爆音或持续导通。
esp_err_t fridge_codec_es8311_start_duplex(TickType_t gate_timeout_ticks);
esp_err_t fridge_codec_es8311_stop_duplex(void);
// timeout_ticks 同时约束 TX 锁等待与 I2S 写入；失败路径会立即关闭 PA。
esp_err_t fridge_codec_es8311_write(const int16_t *samples,
                                    size_t sample_count,
                                    TickType_t timeout_ticks);
// 立即关闭功放并使能电平回到低；不停止 ADC。
esp_err_t fridge_codec_es8311_pa_off(void);
bool fridge_codec_es8311_duplex_active(void);

// 云端 TTS 的单向 DAC 输出。该会话仍独占 GPIO28/29 的 AUDIO gate；摄像头 SCCB
// 正在访问共享 I2C 时必须等待或失败，禁止抢占。PCM 固定为 16kHz/16-bit/mono。
esp_err_t fridge_codec_es8311_start_output(TickType_t gate_timeout_ticks);
esp_err_t fridge_codec_es8311_stop_output(void);
bool fridge_codec_es8311_output_active(void);

#ifdef __cplusplus
}
#endif
