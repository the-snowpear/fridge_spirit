# ESP32-P4 CB V1.2 → TR230S QSPI-4SDA + FT6336U 接线方案

适用硬件：慧勤智远 ESP32-P4 CB V1.2、TS040HDS02CP-B1620A（TR230S + FT6336U）、20-pin FPC 转 20 路杜邦线转接板。

本方案复用 S3 已经实际显示白、红、绿、蓝、黑画面的 QSPI 协议。屏幕主电源为 5V，所有 GPIO 信号为 3.3V。严禁把屏幕 FPC 直接插入开发板板载 20-pin SPI 屏座；板座与屏幕 FPC 的电源脚排列不同，必须保持板载屏座空置。

## 逐 Pin 接线表

接线和改线前必须拔掉 USB 并完全断电。FPC Pin 1 必须根据转接板丝印或万用表通断确认，不能根据排线正反面猜测。

| FPC Pin | 屏幕信号 | 接开发板丝印 | GPIO | 说明 |
| ---: | --- | --- | ---: | --- |
| 1 | GND | `GND` | — | 屏幕主地 |
| 2 | VCC | `5V` | — | 屏幕主电源，只能接 5V |
| 3 | GND | `GND` | — | 屏幕主地 |
| 4 | CS# | `07` | 7 | QSPI 片选，低有效 |
| 5 | GND | `GND` | — | 屏幕主地 |
| 6 | SDO1 / DATA1 | `10` | 10 | QSPI D1 |
| 7 | SDO0 / DATA0 | `08` | 8 | QSPI D0 |
| 8 | SCL / SCLK | `09` | 9 | QSPI 时钟，Mode 0，首次 10MHz |
| 9 | SDO3 / DATA3 | `21` | 21 | QSPI D3 |
| 10 | SDO2 / DATA2 | `20` | 20 | QSPI D2 |
| 11 | RESET# | `22` | 22 | 屏幕复位，低有效 |
| 12 | WAIT# | `19` | 19 | TR230S 忙信号，高电平允许发送命令 |
| 13 | QSPI-INT / HOST | **不接** | — | 当前固件不用 |
| 14 | D/C# | **不接** | — | QSPI 使用 `0x02/0x12` 封包，不使用 D/C# |
| 15 | TP-INT | `15` | 15 | FT6336U 触摸中断 |
| 16 | TP-SDA | `17` | 17 | 触摸 I²C SDA；注意不是 GPIO16 |
| 17 | TP-SCL | `16` | 16 | 触摸 I²C SCL；注意不是 GPIO17 |
| 18 | TP-RST | `18` | 18 | 触摸复位，低有效 |
| 19 | TP-VCC | **不接、悬空** | — | 当前 Rev1.2 规格为 NC/共享屏幕供电，禁止接 5V |
| 20 | TP-GND | `GND` | — | 触摸地 |

## 从当前 SPI-4WIRE 接线改线

原有电源、CS、D0、时钟、复位、WAIT 和触摸线保持不变，只改以下三处：

1. 原 FPC Pin 6 已接 GPIO10，信号含义从 MISO 改为 QSPI D1，线不用移动。
2. 把 FPC Pin 10 接到 GPIO20，作为 QSPI D2。
3. 把 FPC Pin 9 接到 GPIO21，作为 QSPI D3；原 FPC Pin 14 D/C# 从 GPIO20 拔下并悬空。

## 上电前检查

- FPC Pin 1、3、5、20 对开发板 GND 必须导通。
- FPC Pin 2 只接 5V，不能误接 3.3V，也不能与任何 GPIO 短路。
- FPC Pin 14 必须悬空；GPIO20 现在连接 FPC Pin 10，GPIO21 连接 FPC Pin 9。
- TP-SDA、TP-SCL 如转接板没有自带上拉，各使用 4.7kΩ 上拉到 3.3V，禁止上拉到 5V。
- 建议杜邦线不超过 10cm，SCLK、D0-D3、CS 与 GND 尽量短并避免松动。
- 首次上电使用稳定 5V/2A 电源；出现发热、异味、反复重启或电压塌陷立即断电。

当前固件固定以 QSPI Mode 0、10MHz 发送静态色带，并保留 FT6336U 100kHz I²C 触摸测试。完整 LVGL UI 尚未迁移到 P4。
