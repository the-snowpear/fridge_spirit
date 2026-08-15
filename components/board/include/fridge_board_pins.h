// 冰箱小精灵板级引脚统一定义。
// 本文件是所有硬件驱动的唯一 GPIO 来源，按芯片目标分支：
//   - ESP32-P4：慧勤智远 ESP32-P4 CB V1.2 小系统板（依据《用户手册 V1.4》§1.5 引脚分配表与原理图 P6 排针网表）。
//   - ESP32-S3：旧 ESP32-S3-DevKitC-1 N8R8 排线方案（doc/ESP32S3_DevKitC1_排线方案.md），仅为保留可编译的存档分支。
// 硬件安全注意：
//   - 修改任何引脚前必须复核开发板手册与原理图，禁止占用 strapping 脚和板载外设专用脚。
//   - 屏幕 VCC 为 5V，但所有信号线均为 3.3V 逻辑，禁止 5V 进入 GPIO。

#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4

#if CONFIG_FRIDGE_FY_SP703_ENABLE && !CONFIG_FRIDGE_P4_MIPI_UI_ENABLE
#error "FY-SP703 uses GPIO7/8/9; enable FRIDGE_P4_MIPI_UI_ENABLE before enabling FY-SP703"
#endif

// ================= 慧勤智远 ESP32-P4 CB V1.2 =================
// 板上已占用（禁止挪用）：GPIO0/1=32.768k 晶振；GPIO35=BOOT(strapping)；GPIO36=启动模式(10K上拉)；
// GPIO24/25=USB-JTAG；GPIO37/38=UART0(CH343P)；GPIO47-52=C6 SDIO；GPIO53=C6_WKUP；GPIO54=C6_EN；
// GPIO30-32=RS485；GPIO33/34=CAN；GPIO12=KEY0；GPIO13/14=LED。

// ---- WKS70WSV114-WCT MIPI-DSI 屏（板载 26-pin DSI 座；SPI/TR230S 驱动已移除）----
// DSI 时钟/数据使用 P4 专用 D-PHY；普通 GPIO 仅控制复位和背光。
// LCD_RST 复用 GPIO22，板上已分压到 1.8V；禁止绕过板载复位网络外接信号。
#define FRIDGE_PIN_LCD_RST     22  // 板载 LCD_RST（MIPI-DSI 复位）
#define FRIDGE_PIN_MIPI_LCD_BL 23  // 板载 LCD_BL，高电平开启背光

// ---- FY-SP703 降噪麦克风（扩展板；仅 MIPI 正常配置启用）----
// 模组为 I2S 主机，P4 的 GPIO7/8/9 均只作输入，严禁由 P4 反向输出时钟。
// 这三根线与旧 TR230S QSPI 排障接线冲突，Kconfig 已限制必须启用 MIPI UI。
#define FRIDGE_PIN_FY_I2S_LRCK 7
#define FRIDGE_PIN_FY_I2S_BCLK 8
#define FRIDGE_PIN_FY_I2S_DIN  9

// ---- 扩展板共享 I2C0（GT911 + SHT4x + MPU6050，避开 GPIO28/29 编解码器总线）----
// 总线由 GT911 驱动唯一创建；其余设备只能取得既有 bus handle 后添加自己的 device handle。
// 所有器件与上拉必须使用 3.3V。MPU6050 模块自带 2.2k 上拉，接入后需实测并联等效阻值。
#define FRIDGE_PIN_TOUCH_SDA 17  // 屏 FPC pin16 TP-SDA（板上网络 CTP-SDA）
#define FRIDGE_PIN_TOUCH_SCL 16  // 屏 FPC pin17 TP-SCL（板上网络 CTP-SCL）
#define FRIDGE_PIN_TOUCH_INT 15  // 屏 FPC pin15 TP-INT
#define FRIDGE_PIN_TOUCH_RST 18  // 屏 FPC pin18 TP-RST（P4 版接上，驱动内做复位脉冲）
#define FRIDGE_PIN_SENSOR_I2C_SDA FRIDGE_PIN_TOUCH_SDA
#define FRIDGE_PIN_SENSOR_I2C_SCL FRIDGE_PIN_TOUCH_SCL

// ---- 扩展板光照传感器（光敏模块，GPIO19 模拟输入）----
// 硬件约束（doc/ESP32-P4扩展板IO口规划.md §5.1，P6-32）：
//   - 模块 VCC 只允许接 3.3V（EXT_3V3），GND 与主控共地；AO 输出不得超过 3.3V，严禁 5V 进入 GPIO。
//   - GPIO19 是 ESP32-P4 的 ADC1_CH3（IDF soc/esp32p4/adc_channel.h 确认，P6-32 排针）。
//   - 本脚只作 ADC 输入：严禁配置为 GPIO 输出，严禁使能内部上/下拉，避免对光敏模块反灌电。
#define FRIDGE_PIN_LIGHT_AO 19

// ---- MQ-5 可燃气体/烟雾传感器（四针模块，首阶段只用 AO，DO 本阶段悬空）----
// 硬件约束（P6-29，GPIO20）：
//   - GPIO20 是 ESP32-P4 的 ADC1_CH4（IDF soc/esp32p4/adc_channel.h 确认）。
//   - 模块 AO 经 22kΩ 串联电阻接入 GPIO20，节点处 33kΩ 对地分压并并 100nF 滤波；
//     分压后最高约 3.0V（0.6×5V），严禁 5V 直接进入 GPIO。
//   - 模块由 P6 SYS_5V 供电（5V 只到模块 VCC，与主控共地）；整板供电实测在后续工单完成。
//   - DO 本阶段不接：不定义、不配置任何 DO GPIO。
//   - 本脚只作 ADC 输入：严禁配置为 GPIO 输出，严禁使能内部上/下拉。
#define FRIDGE_PIN_MQ5_AO 20
#define FRIDGE_MQ5_DIVIDER_TOP_OHM 22000U     // AO 串联电阻 22kΩ
#define FRIDGE_MQ5_DIVIDER_BOTTOM_OHM 33000U  // 节点对地电阻 33kΩ

// ---- ES8311 音频编解码器（板载，手册 §1.5 GPIO2-6 / §2.10）----
#define FRIDGE_PIN_I2S_MCLK    2   // ES8311 MCLK
#define FRIDGE_PIN_I2S_DSDIN   3   // P4 -> ES8311 DAC 数据（播放，本阶段未启用）
#define FRIDGE_PIN_I2S_SCLK    4   // I2S 位时钟
#define FRIDGE_PIN_I2S_ASDOUT  5   // ES8311 ADC -> P4 数据（录音）
#define FRIDGE_PIN_I2S_LRCK    6   // I2S 帧时钟
#define FRIDGE_PIN_PA_CTRL     11  // NS4150B 功放使能，高有效；不放音时必须保持低电平防爆音

// ---- ES8311 / MIPI-CSI 共享控制 I2C（手册明确要求分时复用，禁止摄像头与编解码器同时占用）----
#define FRIDGE_PIN_CODEC_I2C_SDA 28
#define FRIDGE_PIN_CODEC_I2C_SCL 29

// ---- OV5647 MIPI-CSI 控制脚（15-pin CSI 座；数据/时钟走专用 D-PHY，不占用普通 GPIO）----
// 复位/掉电为普通 GPIO；SCCB 必须复用上面的 GPIO28/29 共享总线，禁止另建 I2C。
#define FRIDGE_PIN_CSI_RST  26
#define FRIDGE_PIN_CSI_PWDN 27

// ---- TF 卡（SDMMC Slot0；Slot1 留给 C6 hosted；手册 §1.5 GPIO39-45；LDO4=3.3V）----
#define FRIDGE_PIN_TF_CLK    43
#define FRIDGE_PIN_TF_CMD    44
#define FRIDGE_PIN_TF_D0     39
#define FRIDGE_PIN_TF_D1     40
#define FRIDGE_PIN_TF_D2     41
#define FRIDGE_PIN_TF_D3     42
#define FRIDGE_PIN_TF_PWR_EN 45  // AO3401 P-MOS 栅极：低=供电（默认 10K 下拉导通），高=断电
#define FRIDGE_TF_PWR_LDO_CHAN 4       // LDO_VO4 -> VDD_SD_DPHY，与 esp_hosted C6 SDIO 共用同一 LDO 通道
#define FRIDGE_TF_PWR_LDO_VOLTAGE_MV 3300

// ---- 板载指示灯 / 按键（可选调试用）----
#define FRIDGE_PIN_LED_GREEN 14
#define FRIDGE_PIN_LED_BLUE  13
#define FRIDGE_PIN_KEY0      12

#elif CONFIG_IDF_TARGET_ESP32S3

// ================= 旧 ESP32-S3-DevKitC-1 N8R8（存档分支，仅保编译）=================
// 详见 doc/ESP32S3_DevKitC1_排线方案.md 与 doc/屏幕点亮经验总结.md；禁用 GPIO35/36/37。

// ---- TR230S QSPI-4SDA 屏（仅保持旧目标可编译；实物接线以 s3-legacy 分支为准）----
#define FRIDGE_PIN_LCD_CS    10
#define FRIDGE_PIN_LCD_D0    11
#define FRIDGE_PIN_LCD_SCLK  12
#define FRIDGE_PIN_LCD_D1    13
#define FRIDGE_PIN_LCD_D2    14
#define FRIDGE_PIN_LCD_D3    9
#define FRIDGE_PIN_LCD_RST   7
#define FRIDGE_PIN_LCD_WAIT  6

// ---- FT6336U 触摸（挂在 sensors 组件的 I2C0：SDA=4 SCL=5）----
#define FRIDGE_PIN_TOUCH_SDA 4
#define FRIDGE_PIN_TOUCH_SCL 5
#define FRIDGE_PIN_TOUCH_INT 15
#define FRIDGE_PIN_TOUCH_RST (-1)  // S3 排线未接触摸复位，依赖上电复位

// ---- INMP441 / MAX98357A（S3 外接 I2S，与 P4 的 ES8311 宏名不复用）----
#define FRIDGE_PIN_S3_I2S_BCLK 40
#define FRIDGE_PIN_S3_I2S_WS   41
#define FRIDGE_PIN_S3_MIC_SD   42
#define FRIDGE_PIN_S3_SPK_DOUT 39

#else
#error "fridge_board_pins.h: unsupported IDF target; only esp32p4 (current) and esp32s3 (legacy) are defined"
#endif
