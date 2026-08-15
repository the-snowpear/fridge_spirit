# SHT40 温湿度传感器接入文档

## 硬件接线

| 信号 | ESP32-P4 GPIO | 说明 |
|------|--------------|------|
| VIN  | 3.3V         | 模块供电 |
| GND  | GND          | 共地 |
| SCL  | GPIO16       | 共享 I2C0，与 GT911 触摸同线 |
| SDA  | GPIO17       | 共享 I2C0，与 GT911 触摸同线 |

I2C 地址：0x44（默认），备用 0x45（ADDR 引脚接 VDD 时）。

## SHT40 vs SHT30 协议差异

文件夹 `doc/温湿度传感器/` 内为 SHT30 官方文档，**实际芯片为 SHT40**，命令集不同：

| 项目 | SHT30 | SHT40 |
|------|-------|-------|
| 高精度测量命令 | 0x2C06（带 clock stretching）/ 0x2400 | **0xFD**（单字节，无 clock stretching）|
| 响应字节数 | 6 | 6 |
| 测量等待时间 | ~15ms | **≥8.3ms（取 10ms）** |
| CRC poly/init | 0x31 / 0xFF | 0x31 / 0xFF（相同）|

## 协议细节

### 测量流程

1. 发送单字节命令 `0xFD`（高精度，无加热）
2. 等待 ≥10ms（`vTaskDelay`，不使用 clock stretching）
3. 读取 6 字节：`T_MSB T_LSB T_CRC RH_MSB RH_LSB RH_CRC`

### CRC-8 校验

- Polynomial: `0x31`，Init: `0xFF`
- 对每组 2 字节（MSB+LSB）独立计算，与第 3 字节比对

### 换算公式

```
T(°C)  = -45 + 175 × raw_T  / 65535
RH(%)  = -6  + 125 × raw_RH / 65535   （clamp 到 0~100）
```

## 共享 I2C0 红线

- **整条 I2C0 总线只能有一个 bus handle**（`doc/ESP32-P4扩展板IO口规划.md` §7）
- GT911 触摸驱动（`components/touch/fridge_touch_gt911_p4.c`）在 `fridge_touch_gt911_init()` 中用 `i2c_new_master_bus` 建立 I2C0
- SHT40 驱动通过 `i2c_master_get_bus_handle(I2C_NUM_0, &bus)` 复用该总线，**不重新创建**
- 因此 `fridge_sht4x_init()` **必须在 `fridge_touch_gt911_init()` 之后调用**
- 新版 `i2c_master.h` 驱动对同一 bus 的设备事务是总线级串行的，天然满足互斥要求

## 代码接入点

| 层 | 文件 | 改动 |
|----|------|------|
| 驱动 | `components/sht4x/` | 新建组件（CMakeLists.txt / include/fridge_sht4x.h / fridge_sht4x.c）|
| 启动 | `main/main.c` | GT911 init 后调用 `fridge_sht4x_init()`，失败仅 LOGW |
| 构建闭包 | `main/CMakeLists.txt` | else() 闭包加 `sht4x` |
| UI 显示 | `components/ui_p4/fridge_ui_p4.c` | `collect_device_status()` 替换占位符为真实读数 |
| AI 工具 | `components/tool_system/src/builtin_tools.c` | `device.state.get` JSON 追加 `tempC`/`humidityRh`/`envValid` |
| USB 调试 | `components/usb_protocol/fridge_usb_protocol_minimal.c` | `get_status` 响应替换 `temperatureC:null` 为真实值 |

## 轮询策略

- 周期：5s（`SHT4X_POLL_INTERVAL_MS`）
- 任务栈：3072 字节（无大缓冲，普通栈足够）
- 失败时 `valid=false`，UI/AI/USB 均显示占位符，不阻断系统

## 验证步骤

1. `tools\idf.ps1 build` — 编译通过
2. `tools\idf.ps1 -p COM15 flash` + `monitor` — 启动日志出现 `SHT40 就绪 addr=0x44`
3. UI 屏幕温湿度从 `--.-°C / --%` 变为真实值
4. USB 发 `get_status`，JSON 含 `temperatureC`/`humidityRh`/`envValid:true`
5. GLM Agent 调用 `device.state.get`，返回含 `tempC`/`humidityRh`
6. 手捂传感器，数值上升，确认读数活的
