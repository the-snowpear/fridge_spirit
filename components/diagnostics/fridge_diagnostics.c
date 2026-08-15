// 冰箱小精灵诊断组件。
// 负责读取 heap、PSRAM、Flash、固件版本和网络健康状态；仅做只读诊断，不触碰 GPIO。

#include "fridge_diagnostics.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "esp_timer.h"
#include "fridge_network.h"
#include "sdkconfig.h"

#define TAG "fridge_diag"
#define DIAGNOSTICS_NVS_NAMESPACE "diagnostics"
#define DIAGNOSTICS_NVS_KEY_BOOT_SEQUENCE "boot_seq"
#define DIAGNOSTICS_NVS_KEY_BROWNOUT "brownout"
#define DIAGNOSTICS_NVS_KEY_PANIC "panic"
#define DIAGNOSTICS_NVS_KEY_TASK_WDT "task_wdt"
#define DIAGNOSTICS_NVS_KEY_INTERRUPT_WDT "int_wdt"
#define DIAGNOSTICS_NVS_KEY_WATCHDOG_OTHER "wdt_other"
#define DIAGNOSTICS_NVS_KEY_PLANNED_RESTART "planned_restart"

static int64_t s_boot_time_us;

// 诊断累计状态保存在 NVS，避免 brownout/看门狗重启后计数丢失。
// NVS 初始化由 main 统一完成；本组件遇到 NVS 不可用时退化为本次启动的 RAM 状态。
typedef struct {
    bool initialized;
    bool nvs_available;
    esp_reset_reason_t reset_reason;
    uint32_t boot_sequence;
    uint32_t brownout_count;
    uint32_t panic_count;
    uint32_t task_wdt_count;
    uint32_t interrupt_wdt_count;
    uint32_t watchdog_other_count;
    char planned_restart_reason[64];
} diagnostics_state_t;

static diagnostics_state_t s_state;

const char *fridge_diagnostics_reset_reason_name(esp_reset_reason_t reason)
{
    // 名称使用稳定 ASCII 文本，便于 USB JSON 和 TF 日志直接消费。
    switch (reason) {
    case ESP_RST_POWERON:    return "power_on";
    case ESP_RST_EXT:        return "external";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep_sleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default:                 return "unknown";
    }
}

static uint32_t nvs_get_u32_or_zero(nvs_handle_t handle, const char *key)
{
    uint32_t value = 0;
    if (nvs_get_u32(handle, key, &value) != ESP_OK) {
        return 0;
    }
    return value;
}

static void nvs_set_u32_best_effort(nvs_handle_t handle, const char *key, uint32_t value)
{
    // 计数写入失败不能阻断固件启动；失败原因会留在串口日志中，避免静默丢失诊断信息。
    esp_err_t err = nvs_set_u32(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS set %s failed: %s", key, esp_err_to_name(err));
    }
}

static void increment_current_reset_counter(void)
{
    // 复位原因按互斥枚举归类；task-WDT/interrupt-WDT 单独统计，便于定位卡死类型。
    switch (s_state.reset_reason) {
    case ESP_RST_BROWNOUT:
        s_state.brownout_count++;
        break;
    case ESP_RST_PANIC:
        s_state.panic_count++;
        break;
    case ESP_RST_TASK_WDT:
        s_state.task_wdt_count++;
        break;
    case ESP_RST_INT_WDT:
        s_state.interrupt_wdt_count++;
        break;
    case ESP_RST_WDT:
        s_state.watchdog_other_count++;
        break;
    default:
        break;
    }
}

static bool load_planned_restart(nvs_handle_t handle)
{
    size_t length = sizeof(s_state.planned_restart_reason);
    esp_err_t err = nvs_get_str(handle,
                                DIAGNOSTICS_NVS_KEY_PLANNED_RESTART,
                                s_state.planned_restart_reason,
                                &length);
    if (err != ESP_OK) {
        s_state.planned_restart_reason[0] = '\0';
        return false;
    }
    s_state.planned_restart_reason[sizeof(s_state.planned_restart_reason) - 1] = '\0';
    return s_state.planned_restart_reason[0] != '\0';
}

static void update_persistent_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DIAGNOSTICS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        // recovery/测试镜像可能没有成功初始化 NVS，保持 RAM 诊断可用即可。
        ESP_LOGW(TAG, "NVS open failed, counters are RAM-only: %s", esp_err_to_name(err));
        s_state.nvs_available = false;
        s_state.boot_sequence = 1;
        increment_current_reset_counter();
        return;
    }
    s_state.nvs_available = true;

    s_state.boot_sequence = nvs_get_u32_or_zero(handle, DIAGNOSTICS_NVS_KEY_BOOT_SEQUENCE);
    if (s_state.boot_sequence == UINT32_MAX) {
        s_state.boot_sequence = 1;
    } else {
        s_state.boot_sequence++;
    }
    s_state.brownout_count = nvs_get_u32_or_zero(handle, DIAGNOSTICS_NVS_KEY_BROWNOUT);
    s_state.panic_count = nvs_get_u32_or_zero(handle, DIAGNOSTICS_NVS_KEY_PANIC);
    s_state.task_wdt_count = nvs_get_u32_or_zero(handle, DIAGNOSTICS_NVS_KEY_TASK_WDT);
    s_state.interrupt_wdt_count = nvs_get_u32_or_zero(handle, DIAGNOSTICS_NVS_KEY_INTERRUPT_WDT);
    s_state.watchdog_other_count = nvs_get_u32_or_zero(handle, DIAGNOSTICS_NVS_KEY_WATCHDOG_OTHER);

    // 先读取上一次计划重启原因，再清除 pending 标记；这样 USB 看到的是“刚刚完成的计划重启”。
    bool had_planned_restart = load_planned_restart(handle);

    increment_current_reset_counter();

    nvs_set_u32_best_effort(handle, DIAGNOSTICS_NVS_KEY_BOOT_SEQUENCE, s_state.boot_sequence);
    nvs_set_u32_best_effort(handle, DIAGNOSTICS_NVS_KEY_BROWNOUT, s_state.brownout_count);
    nvs_set_u32_best_effort(handle, DIAGNOSTICS_NVS_KEY_PANIC, s_state.panic_count);
    nvs_set_u32_best_effort(handle, DIAGNOSTICS_NVS_KEY_TASK_WDT, s_state.task_wdt_count);
    nvs_set_u32_best_effort(handle, DIAGNOSTICS_NVS_KEY_INTERRUPT_WDT, s_state.interrupt_wdt_count);
    nvs_set_u32_best_effort(handle, DIAGNOSTICS_NVS_KEY_WATCHDOG_OTHER, s_state.watchdog_other_count);

    if (had_planned_restart) {
        // 只清除 pending 标志，不影响 RAM 中刚读取的 planned_restart_reason。
        err = nvs_erase_key(handle, DIAGNOSTICS_NVS_KEY_PLANNED_RESTART);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS erase planned restart failed: %s", esp_err_to_name(err));
        }
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS commit diagnostics failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static void ensure_initialized(void)
{
    if (!s_state.initialized) {
        fridge_diagnostics_init();
    }
}

static uint32_t total_watchdog_count(void)
{
    return s_state.task_wdt_count + s_state.interrupt_wdt_count + s_state.watchdog_other_count;
}

static void format_uptime(char *buffer, size_t buffer_size)
{
    int64_t elapsed_s = (esp_timer_get_time() - s_boot_time_us) / 1000000;
    int hours = (int)(elapsed_s / 3600);
    int minutes = (int)((elapsed_s % 3600) / 60);
    int seconds = (int)(elapsed_s % 60);
    snprintf(buffer, buffer_size, "%02d:%02d:%02d", hours, minutes, seconds);
}

static void set_task_snapshot(fridge_task_snapshot_t *task,
                              const char *name,
                              const char *priority,
                              const char *state,
                              const char *heartbeat)
{
    // 任务快照要复制字符串内容，不能保存局部状态结构中的指针，否则 USB 响应中会出现乱码。
    if (!task) {
        return;
    }
    strlcpy(task->name, name ? name : "", sizeof(task->name));
    strlcpy(task->priority, priority ? priority : "", sizeof(task->priority));
    strlcpy(task->state, state ? state : "", sizeof(task->state));
    strlcpy(task->heartbeat, heartbeat ? heartbeat : "", sizeof(task->heartbeat));
}

void fridge_diagnostics_init(void)
{
    // 诊断初始化应在 main 的 nvs_init_safe() 之后调用；重复调用不重复递增启动序号。
    if (s_state.initialized) {
        return;
    }
    s_boot_time_us = esp_timer_get_time();
    s_state.reset_reason = esp_reset_reason();
    s_state.planned_restart_reason[0] = '\0';
    update_persistent_state();
    s_state.initialized = true;
    ESP_LOGI(TAG,
             "boot #%lu reset=%s brownout=%lu panic=%lu task_wdt=%lu int_wdt=%lu",
             (unsigned long)s_state.boot_sequence,
             fridge_diagnostics_reset_reason_name(s_state.reset_reason),
             (unsigned long)s_state.brownout_count,
             (unsigned long)s_state.panic_count,
             (unsigned long)s_state.task_wdt_count,
             (unsigned long)s_state.interrupt_wdt_count);
}

// 保存计划重启原因。必须在 esp_restart() 前完成 NVS commit，避免把软件重启误判为卡死。
esp_err_t fridge_diagnostics_set_planned_restart(const char *reason)
{
    if (!reason) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t reason_length = strlen(reason);
    if (reason_length >= sizeof(s_state.planned_restart_reason)) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(DIAGNOSTICS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (reason_length == 0) {
        err = nvs_erase_key(handle, DIAGNOSTICS_NVS_KEY_PLANNED_RESTART);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(handle, DIAGNOSTICS_NVS_KEY_PLANNED_RESTART, reason);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        strlcpy(s_state.planned_restart_reason,
                reason,
                sizeof(s_state.planned_restart_reason));
    }
    return err;
}

// 显式清除计划重启标记，供恢复流程或测试脚本在不重启时复位状态。
esp_err_t fridge_diagnostics_clear_planned_restart(void)
{
    return fridge_diagnostics_set_planned_restart("");
}

esp_err_t fridge_diagnostics_get_status(fridge_device_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const esp_app_desc_t *app_desc = esp_app_get_description();

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);

#if CONFIG_IDF_TARGET_ESP32P4
    strlcpy(status->model, "冰箱小精灵 / ESP32-P4 CB V1.2", sizeof(status->model));
    snprintf(status->chip, sizeof(status->chip), "ESP32-P4 rev %d", chip_info.revision);
#else
    strlcpy(status->model, "冰箱小精灵 DevKit", sizeof(status->model));
    snprintf(status->chip, sizeof(status->chip), "ESP32-S3 rev %d", chip_info.revision);
#endif
    snprintf(status->firmware, sizeof(status->firmware), "%s", app_desc ? app_desc->version : "dev");
    ensure_initialized();
    format_uptime(status->uptime, sizeof(status->uptime));
    snprintf(status->flash, sizeof(status->flash), "%lu MB", (unsigned long)(flash_size / (1024 * 1024)));
    snprintf(status->psram, sizeof(status->psram), "%lu KB free", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    status->free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
    status->min_heap_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
    status->free_psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    strlcpy(status->reset_reason,
            fridge_diagnostics_reset_reason_name(s_state.reset_reason),
            sizeof(status->reset_reason));
    status->reset_reason_code = (uint32_t)s_state.reset_reason;
    status->boot_sequence = s_state.boot_sequence;
    status->brownout_count = s_state.brownout_count;
    status->panic_count = s_state.panic_count;
    status->task_wdt_count = s_state.task_wdt_count;
    status->interrupt_wdt_count = s_state.interrupt_wdt_count;
    status->watchdog_count = total_watchdog_count();
    strlcpy(status->planned_restart_reason,
            s_state.planned_restart_reason,
            sizeof(status->planned_restart_reason));
    status->page = net.connected ? "WIFI_CONNECTED / USB_CONSOLE" : (net.connecting ? "WIFI_CONNECTING / USB_CONSOLE" : "PROVISIONING / USB_CONSOLE");
    status->power_note = "Wi-Fi 发射有电流峰值；调试时请确认 USB/5V 供电稳定，避免 brownout。";
    status->wifi_health = net.connected ? "ok" : (net.connecting ? "warn" : "offline");
    status->mqtt_health = "offline";
    status->usb_health = "ok";
    status->ota_health = "warn";
    set_task_snapshot(&status->tasks[0], "main_task", "高", "running", "boot");
    set_task_snapshot(&status->tasks[1], "usb_protocol", "中", "running", "stdin");
    set_task_snapshot(&status->tasks[2], "wifi", "中", net.connected ? "connected" : (net.connecting ? "connecting" : "idle"), net.ip);
    status->task_count = 3;
    return ESP_OK;
}

esp_err_t fridge_diagnostics_get_snapshot(fridge_diagnostic_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    ensure_initialized();

    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);

    snprintf(snapshot->psram, sizeof(snapshot->psram), "%lu KB free", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    snprintf(snapshot->flash_partition,
             sizeof(snapshot->flash_partition),
             "%luMB: ota_0 主固件 + ota_1 recovery + assets/cache/model",
             (unsigned long)(flash_size / (1024 * 1024)));
    strlcpy(snapshot->littlefs, "planned: assets + cache", sizeof(snapshot->littlefs));
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(snapshot->ota_slot, sizeof(snapshot->ota_slot), "%s active; ota_1 recovery", running->label);
    } else {
        strlcpy(snapshot->ota_slot, "ota_0 main; ota_1 recovery", sizeof(snapshot->ota_slot));
    }
    strlcpy(snapshot->reset_reason,
            fridge_diagnostics_reset_reason_name(s_state.reset_reason),
            sizeof(snapshot->reset_reason));
    snapshot->reset_reason_code = (uint32_t)s_state.reset_reason;
    snapshot->boot_sequence = s_state.boot_sequence;
    snapshot->brownout_count = s_state.brownout_count;
    snapshot->panic_count = s_state.panic_count;
    snapshot->task_wdt_count = s_state.task_wdt_count;
    snapshot->interrupt_wdt_count = s_state.interrupt_wdt_count;
    snapshot->watchdog_other_count = s_state.watchdog_other_count;
    snapshot->watchdog_count = total_watchdog_count();
    strlcpy(snapshot->planned_restart_reason,
            s_state.planned_restart_reason,
            sizeof(snapshot->planned_restart_reason));
    strlcpy(snapshot->last_error, net.last_error[0] ? net.last_error : "无网络错误", sizeof(snapshot->last_error));
    return ESP_OK;
}
