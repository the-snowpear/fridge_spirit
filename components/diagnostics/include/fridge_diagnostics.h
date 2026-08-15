#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

// 任务快照：首版只提供启动、USB 协议和网络三类关键任务，后续可扩展为真实 FreeRTOS 遍历。
typedef struct {
    char name[24];
    char priority[8];
    char state[24];
    char heartbeat[32];
} fridge_task_snapshot_t;

typedef struct {
    char model[32];
    char chip[32];
    char firmware[32];
    char uptime[24];
    char flash[32];
    char psram[32];
    uint32_t free_heap_kb;
    uint32_t min_heap_kb;
    uint32_t free_psram_kb;
    // 最近一次启动的复位原因和累计异常计数，供 USB/本地 UI 展示。
    char reset_reason[32];
    uint32_t reset_reason_code;
    uint32_t boot_sequence;
    uint32_t brownout_count;
    uint32_t panic_count;
    uint32_t task_wdt_count;
    uint32_t interrupt_wdt_count;
    uint32_t watchdog_count;
    char planned_restart_reason[64];
    const char *page;
    const char *power_note;
    const char *wifi_health;
    const char *mqtt_health;
    const char *usb_health;
    const char *ota_health;
    fridge_task_snapshot_t tasks[4];
    size_t task_count;
} fridge_device_status_t;

typedef struct {
    char psram[48];
    char flash_partition[80];
    char littlefs[48];
    char ota_slot[48];
    uint32_t brownout_count;
    uint32_t watchdog_count;
    char last_error[96];
    // 复位与看门狗细分信息；旧字段保留以兼容现有 USB 协议。
    char reset_reason[32];
    uint32_t reset_reason_code;
    uint32_t boot_sequence;
    uint32_t panic_count;
    uint32_t task_wdt_count;
    uint32_t interrupt_wdt_count;
    uint32_t watchdog_other_count;
    char planned_restart_reason[64];
} fridge_diagnostic_snapshot_t;

// 初始化诊断模块，记录启动时间。该模块只读系统状态，不改 GPIO 或外设输出。
void fridge_diagnostics_init(void);

// 读取设备状态，供 Web 面板 get_status 使用。
esp_err_t fridge_diagnostics_get_status(fridge_device_status_t *status);

// 读取诊断摘要，供 Web 面板 get_diagnostics 使用。
esp_err_t fridge_diagnostics_get_snapshot(fridge_diagnostic_snapshot_t *snapshot);

// 在调用 esp_restart() 前记录计划重启原因。原因写入 NVS，保证重启后仍可读取。
// 该接口只保存短文本，不应传入 API Key、密码或其它敏感数据。
esp_err_t fridge_diagnostics_set_planned_restart(const char *reason);

// 清除当前保存的计划重启原因；通常仅用于测试或恢复流程完成后的显式清理。
esp_err_t fridge_diagnostics_clear_planned_restart(void);

// 将复位枚举转换成稳定的 ASCII 名称，便于 JSON/日志记录。
const char *fridge_diagnostics_reset_reason_name(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif
