# ESP32-P4 CB V1.2 扩展板直连接线方案

> 文档版本：V0.3（2026-07-14）
> 目标主板：慧勤智远 ESP32-P4 CB V1.2  
> 屏幕：WKS70WSV114-WCT MIPI-DSI + GT911 触摸  
> 状态：原理设计已确认，未完成全部扩展模块真机联调；干簧管 1/2 的物理安装方向和深睡唤醒行为待真机验证。

## 1. 已确认前提

1. 使用 MIPI-DSI 屏幕与 GT911 触摸。`GPIO15/16/17/18` 保留给触摸，`GPIO22/23` 保留给 MIPI 屏复位/背光。
2. 旧 TR230S QSPI 屏已退役，板载 20-pin SPI 屏座必须保持空置。因此 `GPIO7~10`、`GPIO19~21` 可用于扩展板，但它们仍连接到空置屏座网络，以后不得再插 SPI 屏。
3. `GPIO12` 通过 `R38=10k` 上拉到 3.3V，板载 KEY0 与干簧管 1 并联使用：两者任一闭合都会将 GPIO12 拉低，用作 P4 深睡唤醒/开机请求。它们的来源在软件中不可区分。
4. 干簧管 2 使用 `GPIO13`；保留板载 `R43=2.2k` 和蓝 LED。干簧管闭合时 GPIO13 为低、蓝 LED 会亮。GPIO13 只可配置为输入，不能再作为 LED 输出。
5. `GPIO14` 保留板载绿 LED，暂不接扩展板，作为后续低有效数字输入或状态灯的余量。
6. 不使用 CAN、RS485，且已卸除 `R83/R84/R85/R86`。`R83/R84` 使 P6 上的 `GPIO33/34` 与 CAN 收发器隔离，可改作普通 UART；`R85/R86` 仅隔离未引出 P6 的 `GPIO30/31`，不能使 `GPIO32` 可用。`GPIO32` 仍连接 RS485 芯片 RE/DE，不分配。
7. 不增加 ADS1115、SC16IS750 或其他 I/O 扩展芯片。光照和 MQ-5 模拟量直接接 ESP32-P4 ADC。
8. FY-SP703 使用完整数字录音，音频走 I2S；其 `SCL/SDA` 没有公开地址和寄存器资料，默认不接。
9. 默认使用板载 ES8311 + NS4150B + Speaker 接口播放，不使用外置 MAX98357A。

## 2. 方案总览

```text
FY-SP703 (I2S 主机) -> P4 I2S1 RX -> 音频处理/ASR
P4 I2S0 TX -> ES8311 -> NS4150B -> 板载 Speaker

KEY0 + 干簧管1（常开） -> GPIO12 -> 深睡低电平唤醒/开机请求
干簧管2（常开） -> GPIO13 -> 运行后软件关机请求（闭合时蓝 LED 亮）

GT911 + MPU6050 + SHT4x
             |
        P4 I2C0: GPIO16(SCL) / GPIO17(SDA)

光照 AO -> GPIO19 ADC1_CH3
MQ-5 AO -> 分压 -> GPIO20 ADC1_CH4
MQ-5 DO -> 分压 -> GPIO10
雷达 TX/RX/OT2 -> GPIO34/GPIO33/GPIO21
```

这里的“复用”仅指多个 I2C 从设备共用同一对开漏总线。ADC、I2S、UART 不与 I2C 电气并联。

## 3. P6 按实物顺序双列表

正面看开发板、板上文字正向、P6 位于下方：靠板边为奇数脚，靠 MCU 为偶数脚；P6-1 为方形焊盘。背面视角镜像。

| 奇数脚 | 信号 | 状态/规划 | 偶数脚 | 信号 | 状态/规划 |
| ---: | --- | --- | ---: | --- | --- |
| 1 | SYS_5V | 电源 | 2 | SYS_5V | 电源 |
| 3 | ESP_3V3 | 电源 | 4 | ESP_3V3 | 电源 |
| 5 | GND | 扩展板公共地 | 6 | GND | 扩展板公共地 |
| 7 | GPIO6 | ES8311 LRCK，占用 | 8 | VBAT | 不使用 |
| 9 | GPIO4 | ES8311 BCLK，占用 | 10 | GPIO3 | ES8311 DAC 数据，占用 |
| 11 | GPIO12 | KEY0 + 干簧管1：深睡低电平唤醒/开机请求 | 12 | GPIO5 | ES8311 ADC 数据，占用 |
| 13 | GPIO53 | C6_WKUP，占用 | 14 | GPIO11 | NS4150B PA_CTRL，占用 |
| 15 | GPIO34 | 雷达 UART RX | 16 | GPIO54 | C6_EN，占用 |
| 17 | GPIO32 | 仍连 RS485 RE/DE，不使用 | 18 | GPIO46 | LCD_ID，占用 |
| 19 | GPIO45 | TF 卡电源使能，占用 | 20 | GPIO33 | 雷达 UART TX |
| 21 | GPIO8 | FY-SP703 I2S BCLK | 22 | GPIO36 | 启动模式脚，禁用 |
| 23 | GPIO10 | MQ-5 DO 数字输入 | 24 | GPIO7 | FY-SP703 I2S LRCK |
| 25 | GPIO16 | I2C SCL：GT911 + MPU6050 + SHT4x | 26 | GPIO9 | FY-SP703 I2S SDIO |
| 27 | GPIO18 | GT911 RST，占用 | 28 | GPIO15 | GT911 INT，占用 |
| 29 | GPIO20 | MQ-5 AO，ADC1_CH4 | 30 | GPIO17 | I2C SDA：GT911 + MPU6050 + SHT4x |
| 31 | GPIO22 | MIPI LCD_RST，占用 | 32 | GPIO19 | 光照 AO，ADC1_CH3 |
| 33 | GPIO23 | MIPI LCD_BL，占用 | 34 | GPIO21 | 雷达 OT2 |
| 35 | GPIO26 | OV5647 CSI_RST，保留 | 36 | GPIO28 | ES8311/OV5647 共享 I2C，占用 |
| 37 | GPIO14 | 板载绿 LED，暂留作余量 | 38 | GPIO27 | OV5647 CSI_PWDN，保留 |
| 39 | NC | 禁止连接 | 40 | GPIO13 | 干簧管2：低有效软件关机请求；保留蓝 LED/R43 |

## 4. P4 最终网络表

| P6 Pin | GPIO | 扩展板网络 | 方向 | 连接对象 |
| ---: | ---: | --- | --- | --- |
| 11 | 12 | `REED1_WAKE` | RTC 输入/深睡唤醒 | KEY0 + 干簧管1（并联，低有效） |
| 40 | 13 | `REED2_SOFT_OFF` | 输入 | 干簧管2（低有效；闭合时蓝 LED 亮） |
| 24 | 7 | `FY_I2S_LRCK` | 输入 | FY-SP703 Pin6 |
| 21 | 8 | `FY_I2S_BCLK` | 输入 | FY-SP703 Pin7 |
| 26 | 9 | `FY_I2S_DIN` | 输入 | FY-SP703 Pin8 |
| 23 | 10 | `MQ5_DO` | 输入 | MQ-5 DO，经分压 |
| 25 | 16 | `TOUCH_I2C_SCL` | 开漏双向 | GT911、MPU6050、SHT4x |
| 30 | 17 | `TOUCH_I2C_SDA` | 开漏双向 | GT911、MPU6050、SHT4x |
| 32 | 19 | `LIGHT_AO` | ADC 输入 | 光照传感器 IO/AO |
| 29 | 20 | `MQ5_AO` | ADC 输入 | MQ-5 AO，经分压 |
| 34 | 21 | `RADAR_OT2` | 输入 | HMMD 雷达 OT2 |
| 20 | 33 | `RADAR_UART_TX` | 输出 | HMMD 雷达 RX |
| 15 | 34 | `RADAR_UART_RX` | 输入 | HMMD 雷达 TX |

## 5. 模块逐针接线表

表中的电源网络定义如下：`EXT_5V` 为扩展板独立 5V 电源入口，`EXT_3V3` 为扩展板本地 3.3V 稳压输出，`EXT_GND` 为扩展板地平面并与 P6 GND 共地。MQ-5 的大电流回路直接返回 `EXT_5V/EXT_GND` 电源入口，不经过 P6 细针。

### 5.1 光照传感器（三线）

| 模块 Pin | 模块丝印 | 扩展板网络 | 开发板端 |
| ---: | --- | --- | --- |
| 1 | IO | `LIGHT_AO` | P6-32 / GPIO19 / ADC1_CH3 |
| 2 | VDD | `EXT_3V3` | 扩展板 3.3V 稳压输出 |
| 3 | GND | `EXT_GND` | P6-5 或 P6-6 GND |

该表按当前用户提供的 `IO/VDD/GND` 引脚名称记录。只有确认 IO 为 3.3V 模拟输出时，才可接 GPIO19 ADC。

### 5.2 干簧管（两路）

两个干簧管均使用常开（NO）无源触点；每只干簧管的一端接 GPIO、另一端接扩展板地。不得向 GPIO 输入外部电压。

| 干簧管 | 触点 | 扩展板网络 | 开发板端 | 用途 |
| --- | --- | --- | --- | --- |
| 干簧管1 | A | `REED1_WAKE` | P6-11 / GPIO12 | 与 KEY0 并联的深睡唤醒/开机请求 |
| 干簧管1 | B | `EXT_GND` | P6-5 或 P6-6 GND | 闭合时将 GPIO12 拉低 |
| 干簧管2 | A | `REED2_SOFT_OFF` | P6-40 / GPIO13 | 仅在 P4 已运行后请求软件关机 |
| 干簧管2 | B | `EXT_GND` | P6-5 或 P6-6 GND | 闭合时将 GPIO13 拉低 |

`GPIO12` 的外部 `R38=10k` 已提供深睡期间所需上拉；P4 以低电平作为外部唤醒条件。进入深睡前必须确认 GPIO12 已恢复高电平，否则干簧管或 KEY0 持续闭合会导致立即唤醒循环。GPIO13 在运行期间配置为输入上拉并做 50ms 以上去抖；保留 `R43` 的代价是干簧管2闭合时蓝 LED 亮，GPIO13 绝不可配置为输出。

### 5.3 HMMD 毫米波雷达（五线）

| 雷达 Pin | 模块丝印 | 扩展板网络 | 开发板端 |
| ---: | --- | --- | --- |
| 1 | 3V3 | `EXT_3V3` | 扩展板 3.3V 稳压输出 |
| 2 | GND | `EXT_GND` | P6-5 或 P6-6 GND |
| 3 | TX | `RADAR_UART_RX` | P6-15 / GPIO34 |
| 4 | RX | `RADAR_UART_TX` | P6-20 / GPIO33 |
| 5 | OT2 | `RADAR_OT2` | P6-34 / GPIO21 |

雷达 TX/RX 必须交叉连接。`GPIO33/34` 已通过拆除 `R83/R84` 与 CAN 收发器隔离；不得接到板载 CANH/CANL 端子。

### 5.4 FY-SP703 降噪麦克风（26 Pin）

| 模块 Pin | 模块丝印 | 扩展板连接 | 开发板端/说明 |
| ---: | --- | --- | --- |
| 1 | GND | `EXT_GND` | P6-5 或 P6-6 GND |
| 2 | SPI-CSN0 | N/C，预留测试焊盘 | 缺少控制协议，不接 P4 |
| 3 | SPI-MISO | N/C，预留测试焊盘 | 不接 P4 |
| 4 | SPI-MOSI | N/C，预留测试焊盘 | 不接 P4 |
| 5 | SPI-CLK | N/C，预留测试焊盘 | 不接 P4 |
| 6 | I2S-LRCK | `FY_I2S_LRCK` | P6-24 / GPIO7 |
| 7 | I2S-SCLK | `FY_I2S_BCLK` | P6-21 / GPIO8 |
| 8 | I2S-SDIO | `FY_I2S_DIN` | P6-26 / GPIO9 |
| 9 | GPIO0 | N/C | 不接 P4 |
| 10 | MIC2- | 配套 MIC2 负端 | 不接 P4 |
| 11 | MIC2+ | 配套 MIC2 正端 | 不接 P4 |
| 12 | MIC1- | 配套 MIC1 负端 | 不接 P4 |
| 13 | MIC1+ | 配套 MIC1 正端 | 不接 P4 |
| 14 | 5V | N/C | 与 Pin15 二选一；本方案不用 5V 供电 |
| 15 | 3.3V | `EXT_3V3` | 扩展板 3.3V 稳压输出 |
| 16 | SCL | N/C，DNP | 未取得 I2C 地址/寄存器资料 |
| 17 | SDA | N/C，DNP | 未取得 I2C 地址/寄存器资料 |
| 18 | USB-DP | N/C | 不接 USB |
| 19 | USB-DM | N/C | 不接 USB |
| 20 | RESET | 10k 上拉至 `EXT_3V3` | 默认不占 P4 GPIO |
| 21 | GND | `EXT_GND` | 与 Pin1 同接地平面 |
| 22 | AGND | 模块附近模拟地 | 按厂家建议与 `EXT_GND` 单点/就近连接 |
| 23 | OUT+ | N/C，预留测试点 | 不接板载 ES8311 MIC 或功放输入 |
| 24 | OUT- | N/C，预留测试点 | 不接板载 ES8311 MIC 或功放输入 |
| 25 | AEC- | 初版 N/C；可选接 Speaker `-` | 仅低音量 AEC 验证时连接 |
| 26 | AEC+ | 初版 N/C；可选接 Speaker `+` | 仅低音量 AEC 验证时连接 |

FY-SP703 是 I2S 主机，P4 必须以独立 I2S 外设从机接收 Pin6/7/8。它不得与板载 ES8311 的 GPIO2~6 I2S 物理并线。

### 5.5 MPU6050 六轴姿态模块（六线）

| 模块 Pin | 模块丝印 | 扩展板网络 | 开发板端/说明 |
| ---: | --- | --- | --- |
| 1 | 3V3 | `EXT_3V3` | 扩展板 3.3V 稳压输出 |
| 2 | GND | `EXT_GND` | P6-5 或 P6-6 GND |
| 3 | SDA | `TOUCH_I2C_SDA` | P6-30 / GPIO17 |
| 4 | SCL | `TOUCH_I2C_SCL` | P6-25 / GPIO16 |
| 5 | INT | N/C | 首版使用轮询，不接中断 |
| 6 | AD0 | `EXT_GND` | I2C 地址固定为 0x68 |

### 5.6 SHT4x 温湿度传感器（四线）

| 模块 Pin | 模块丝印 | 扩展板网络 | 开发板端/说明 |
| ---: | --- | --- | --- |
| 1 | VIN | `EXT_3V3` | 扩展板 3.3V 稳压输出 |
| 2 | GND | `EXT_GND` | P6-5 或 P6-6 GND |
| 3 | SCL | `TOUCH_I2C_SCL` | P6-25 / GPIO16 |
| 4 | SDA | `TOUCH_I2C_SDA` | P6-30 / GPIO17 |

### 5.7 MQ-5 燃气传感器（四线）

| 模块 Pin | 模块丝印 | 扩展板连接 | 开发板端/说明 |
| ---: | --- | --- | --- |
| 1 | VCC | `EXT_5V` | 扩展板独立 5V 高电流支路 |
| 2 | GND | `EXT_GND` | 高电流地直接回电源入口；与主板共地 |
| 3 | DO | `MQ5_DO` 分压节点 | `DO -- 22k -- GPIO10`，GPIO10 `-- 33k -- GND`；P6-23 / GPIO10 |
| 4 | AO | `MQ5_AO` 分压节点 | `AO -- 22k -- GPIO20`，GPIO20 `-- 33k -- GND`，GPIO20 对 GND 并 100nF；P6-29 / GPIO20 |

### 5.8 MAX98357A 扬声器功放（当前不装）

当前方案使用板载 ES8311 + NS4150B，MAX98357A 模块不装入扩展板，其所有排针均不接。若将来恢复该模块，必须重新规划 GPIO10，因为它当前已经用于 MQ-5 DO。

| MAX98357A Pin | 模块丝印 | 当前接线 |
| ---: | --- | --- |
| 1 | VDD | N/C，不装模块 |
| 2 | GND | N/C，不装模块 |
| 3 | SD_MODE | N/C，不装模块 |
| 4 | GAIN | N/C，不装模块 |
| 5 | DIN | N/C，不装模块 |
| 6 | BCLK | N/C，不装模块 |
| 7 | LRCLK | N/C，不装模块 |

## 6. 音频接线

### 6.1 FY-SP703 数字录音

FY-SP703 默认是 I2S 主输出，格式为 48kHz、32-bit、Philips I2S。因此 P4 使用独立 I2S 外设作为从机 RX，不能与板载 ES8311 的 I2S0 物理共线。

| FY-SP703 Pin | 信号 | 扩展板连接 | 说明 |
| ---: | --- | --- | --- |
| 1、21 | GND | GND | 至少接一处；推荐两处均接地平面 |
| 6 | I2S-LRCK | GPIO7 / P6-24 | FY 输出，P4 输入 |
| 7 | I2S-SCLK | GPIO8 / P6-21 | FY 输出，P4 输入 |
| 8 | I2S-SDIO | GPIO9 / P6-26 | FY 输出，P4 输入 |
| 14 | 5V | 不接 | 与 3.3V 二选一，不能同时供电 |
| 15 | 3.3V | 扩展板 3.3V | 额定工作电流约 85mA |
| 16、17 | SCL、SDA | 不接/DNP | 缺少厂家 I2C 协议，不能用于录音 |
| 20 | RESET | 10k 上拉至 3.3V | 默认不占 P4 GPIO |
| 22 | AGND | 模块附近地 | 按厂家建议连接 |
| 23、24 | OUT+、OUT- | 悬空/测试点 | 不接板载 MIC 或功放输入 |
| 25、26 | AEC-、AEC+ | 可选接 Speaker -/+ | 仅低音量验证后启用；两端都不得接 GND |

板载播放链路为 P4 -> ES8311 -> NS4150B -> Speaker。若启用 FY-SP703 回声消除参考，Speaker `+` 接 FY `AEC+`，Speaker `-` 接 FY `AEC-`。厂家资料未给出 AEC 最大输入幅度，扩展板应预留串阻/分压焊位，并从低音量开始验证。

## 7. 共享 I2C：GT911、MPU6050、SHT4x

`GPIO16(SCL)` 和 `GPIO17(SDA)` 是 MIPI 屏 GT911 已使用的 I2C0 总线，同时接 MPU6050 与 SHT4x。I2C 没有片选，靠 7-bit 地址选中从设备。

| 设备 | 地址 | 接线 |
| --- | --- | --- |
| GT911 | 0x14 或 0x5D | 屏幕原有连接；GPIO15 为 INT/地址选择，GPIO18 为 RST |
| MPU6050 | 0x68（AD0=GND）或 0x69 | VCC=3.3V、GND、SCL=GPIO16、SDA=GPIO17；INT 首版悬空并轮询 |
| SHT4x | 通常 0x44 或 0x45，需扫描确认 | VIN=3.3V、GND、SCL=GPIO16、SDA=GPIO17 |

接线红线：

- 所有 SDA/SCL 上拉只能到 3.3V，禁止 5V 上拉。
- 扩展板不默认焊额外上拉；GT911、MPU6050、SHT4x 的板载上拉叠加后可能过低。应预留 4.7k DNP/跳帽并通过实测决定。
- 首次固定 100kHz，传感器分支线尽量短，靠地线走，远离 MQ-5 加热回路、Speaker 输出和 I2S 时钟。
- 软件必须只创建一个 I2C0 主总线句柄，以互斥锁或总线管理任务串行化 GT911、MPU6050、SHT4x 的访问。先完成 GT911 地址选择/复位，再加入传感器设备句柄。
- FY-SP703 的 I2C 不加入该总线，直到拿到厂家地址、寄存器和电平资料。

## 8. 直接 ADC 与数字输入

### 8.1 光照传感器

| 模块信号 | 扩展板连接 | 约束 |
| --- | --- | --- |
| VDD | 3.3V | 禁止默认接 5V |
| GND | GND | 与主板共地 |
| IO/AO | GPIO19 / ADC1_CH3 | 默认按模拟量；串 1k，ADC 端可并 100nF |

光照模块的 `IO` 类型尚待实物型号确认；本方案仅适用于它是 3.3V 模拟输出的情形。若实物为数字 DO，需重新分配输入脚，不得直接按 ADC 使用。

### 8.2 MQ-5 燃气传感器

MQ-5 模块由 5V 供电，AO/DO 可能达到 5V，绝不能直连 ESP32-P4 GPIO。

| 模块信号 | 扩展板连接 | 电路 |
| --- | --- | --- |
| VCC | 独立 5V 高电流支路 | 加热电流约 150-190mA |
| GND | 电源地回路 | 直接回扩展板电源入口，并与主板共地 |
| AO | GPIO20 / ADC1_CH4 | `AO -- 22k -- GPIO20`，GPIO20 `-- 33k -- GND`，GPIO20 对 GND 并 100nF；5V 映射约 3.0V |
| DO | GPIO10 / P6-23 | `DO -- 22k -- GPIO10`，GPIO10 `-- 33k -- GND`；5V 映射约 3.0V |

GPIO10 仅连接到空置 SPI 屏座的 MISO 网络；在 SPI 屏座保持空置时可安全作为输入。MQ-5 DO 通过 22k/33k 分压后最大约 3.0V，禁止直接接入 GPIO10。

MQ-5 是持续发热源，应置于扩展板边缘和通风位置，远离 SHT4x、FY-SP703、光照传感器和高阻 ADC 走线。首次老化/预热至少 48 小时。

### 8.3 干簧管数字输入与低功耗约束

| 网络 | GPIO | 有效电平 | 软件行为 |
| --- | ---: | --- | --- |
| `REED1_WAKE` | GPIO12 | 低有效 | KEY0 和干簧管1任一闭合后唤醒 P4；深睡使用 GPIO12 的低电平外部唤醒 |
| `REED2_SOFT_OFF` | GPIO13 | 低有效 | 仅在系统已运行时触发软件关机状态机，不作为深睡唤醒源 |

干簧管1的安装方向必须保证设备处于等待唤醒的深睡状态时触点断开；若门磁位置使触点一直闭合，GPIO12 会持续为低，P4 不能稳定留在深睡。两路触点均需软件去抖；长线应与 GND 成对走线，避免感应干扰造成误唤醒或误关机。

## 9. HMMD 毫米波雷达

| 雷达 Pin | 信号 | 扩展板连接 | 说明 |
| ---: | --- | --- | --- |
| 1 | 3V3 | 3.3V | 仅使用 3.3V 逻辑 |
| 2 | GND | GND | 共地 |
| 3 | TX | GPIO34 / P6-15 | P4 UART RX |
| 4 | RX | GPIO33 / P6-20 | P4 UART TX |
| 5 | OT2 | GPIO21 / P6-34 | 0-3.3V 独立数字状态输入；有人高、无人低 |

`GPIO33/34` 必须经已拆除的 `R83/R84` 与 CAN 收发器隔离后才可使用。雷达接在 P6 GPIO，不接 CANH/CANL 端子。雷达仅接扩展板 3.3V，禁止接 5V；UART 从 115200、8N1 开始验证。OT2 保持接 GPIO21，不占用 LED 引脚或 C6 I/O。

## 10. 电源与布局

1. 扩展板使用稳定的 5V/2A 以上输入；MQ-5 高电流支路不应穿过 P6 细针和主板 1.5A 保险丝。
2. 扩展板应有独立 3.3V 稳压器，供 FY-SP703、雷达、MPU6050、SHT4x 和光照模块使用；额定输出至少 500mA。
3. 主板、扩展板和所有模块必须共地。外部 5V 与 USB 5V 不得直接反灌。
4. FY-SP703 与 I2S 走线短且靠完整地平面；MQ-5 加热回路、Speaker BTL 输出和 ADC/I2C 走线隔离。
5. Speaker `+/-` 是桥接功放输出，任一端都不得接 GND。

## 11. 固件改动与验证顺序

当前 P4 触摸驱动在 `components/touch/fridge_touch_p4.c` 内部独占创建 I2C0。接入 MPU6050/SHT4x 前，必须重构为共享 I2C0 总线：GT911、MPU6050、SHT4x 使用同一个 `i2c_master_bus_handle_t`，所有事务经同一 mutex/总线管理器串行化。不能让多个组件分别在 I2C0 上创建驱动。

首次验证顺序：

1. 不接模块时检查扩展板 5V、3.3V、GND 无短路，确认 P6 Pin1 方向。
2. 只接 MIPI 屏，确认 GT911 触摸仍可用。
3. 接干簧管1，确认 KEY0 与干簧管均可将 GPIO12 拉低；在 GPIO12 已释放为高电平时进入深睡，分别验证 KEY0 和干簧管唤醒，确认无立即唤醒循环。
4. 接干簧管2，确认 GPIO13 闭合/断开稳定、蓝 LED 的低有效副作用符合预期，且 GPIO13 没有被配置为输出。
5. 接 MPU6050 与 SHT4x，100kHz 扫描并读出设备地址；连续触摸与传感器读取，确认无 I2C 超时。
6. 接光照模块，确认 GPIO19 ADC 值随遮挡变化。
7. 接 MQ-5 前复测 AO/DO 分压后最大电压不超过 3.3V；接入后监测温升和电源压降。
8. 接雷达，验证 UART 收发、OT2 的“有人高/无人低”状态和 3.3V 电平。
9. 接 FY-SP703，先验证 48kHz/32-bit I2S PCM 非零，再验证 ASR；板载 Speaker/AEC 只在低音量下单独验证。
10. 全部并发时检查 brownout、watchdog、I2C timeout、I2S timeout、ADC 异常和音频底噪。

## 12. 资料依据

- `doc/慧勤智远 ESP32-P4 CB V1.2小系统板/1. 用户手册/ESP32-P4 CB V1.2_用户手册_V1.4.pdf`
- `doc/降噪麦克风/FY-SP703 V2.0麦克风模组规格书.pdf`
- `doc/毫米波雷达模块/1-使用手册/HMMD-mmWave-Sensor_使用手册.pdf`
- `doc/memory.md`
- `components/board/include/fridge_board_pins.h`
- `components/touch/fridge_touch_p4.c`
- ESP-IDF 5.5.1 `components/soc/esp32p4/include/soc/adc_channel.h`
