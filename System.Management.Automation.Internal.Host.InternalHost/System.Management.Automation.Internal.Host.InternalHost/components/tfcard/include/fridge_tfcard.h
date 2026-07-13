// 冰箱小精灵 TF 卡组件公共接口。
// 硬件（慧勤智远 ESP32-P4 CB V1.2）：板载 TF 卡座走 SDMMC Slot0 4-bit（CLK=43 CMD=44 D0-D3=39-42），
// 板载 C6 的 esp_hosted 使用 Slot1，两者必须分槽并行；
// SDIO 引脚域由 esp_hosted 唯一持有片内可调 LDO4 并供电 3.3V；TF 不得重复申请该通道；
// 卡座供电由 GPIO45 控制的 P-MOS 开关（低电平导通，默认下拉导通）。
// 文件系统：本阶段使用 FAT32。64GB SDXC 常见出厂格式为 exFAT，ESP-IDF 当前配置不能直接挂载；
// 默认初始化绝不自动格式化，必须由用户通过带确认口令的显式命令触发，避免接触不良时误清空整卡。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TFCARD_MOUNT_POINT "/sdcard"
#define FRIDGE_TFCARD_MAX_ERROR_LEN 96

// TF 卡状态快照：供 USB JSON 命令与诊断查询使用。
typedef struct {
    bool mounted;            // 是否已挂载到 /sdcard
    bool format_required;     // 已识别到卡但当前文件系统无法挂载，通常需要显式格式化
    bool formatted_this_boot; // 本次启动是否执行过显式格式化（提示用户卡已被清空）
    uint64_t capacity_mb;    // 卡容量（MB）
    uint64_t free_mb;        // 文件系统剩余空间（MB）
    char card_name[8];       // 卡片 CID 名称
    char last_error[FRIDGE_TFCARD_MAX_ERROR_LEN + 1];
} fridge_tfcard_status_t;

// 初始化并尝试挂载 TF 卡，不会格式化或清除任何数据。
// 返回 ESP_OK 表示挂载成功；无卡、exFAT 或文件系统损坏时返回错误，固件其余部分继续运行。
esp_err_t fridge_tfcard_init(void);

// hosted/LDO4 等前置条件未满足时记录“本次启动跳过”原因，供串口状态查询显示。
// 该接口不接触卡座供电或 SDMMC，避免把电源域故障误报成卡/文件系统故障。
void fridge_tfcard_mark_unavailable(const char *reason);

// 显式把整张卡重新分区并格式化为 FAT32，然后挂载到 /sdcard。
// 这是破坏性操作，调用者必须先完成用户确认；64GB 卡可能阻塞数十秒。
esp_err_t fridge_tfcard_format_fat32(void);

// 获取当前 TF 卡状态；未初始化时 mounted=false。
void fridge_tfcard_get_status(fridge_tfcard_status_t *out);

// 写入-读回-校验自检：在卡根目录写 fridge_selftest.txt 并读回比较。
// detail 返回人类可读的结果说明（UTF-8 中文），供串口/Web 显示。
esp_err_t fridge_tfcard_selftest(char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
