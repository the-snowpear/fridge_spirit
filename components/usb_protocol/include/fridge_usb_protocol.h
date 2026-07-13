#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动 USB/串口 JSON Lines 协议任务。
// 注意：该组件只读写控制台串口，不控制 GPIO；Web Serial 波特率建议 115200。
esp_err_t fridge_usb_protocol_start(void);

// P4 最小移植启动序列完成后开放外设命令并发送 ready 事件。
// 在此之前串口任务只报告 booting，并拒绝可能与 Wi-Fi/音频/TF 初始化并发的命令。
void fridge_usb_protocol_set_hardware_ready(void);

#ifdef __cplusplus
}
#endif
