// 冰箱小精灵 TF 卡组件 —— ESP32-P4 实现。
// 硬件链路：板载 TF 卡座 -> SDMMC 4-bit（CLK=43 CMD=44 D0-D3=39-42，51K 外部上拉）
//           SDIO 引脚域电源 -> 片内 LDO4 输出 3.3V（VDD_SD_DPHY，参考厂商 18_sdcard 例程）
//           卡座 VDD -> GPIO45 控制的 AO3401 P-MOS（栅极低=供电；板上 10K 下拉默认导通）
// 文件系统：FAT32。默认挂载路径禁止自动格式化；64GB SDXC 若为 exFAT，必须由带确认口令的
// USB JSON 命令显式触发格式化，避免供电抖动或接触不良被误判成文件系统错误后清空整卡。

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "fridge_board_pins.h"
#include "fridge_tfcard.h"

static const char *TAG = "fridge_tfcard";

static sdmmc_card_t *s_card = NULL;              // SDMMC 驱动卡句柄；NULL 表示未挂载
static bool s_format_required = false;           // 卡可初始化但文件系统无法挂载时提示显式格式化
static bool s_formatted_this_boot = false;       // 本次启动是否执行过显式格式化
static bool s_unavailable_this_boot = false;     // hosted/LDO4 前置条件失败后锁存，重启前禁止再访问 SDMMC
static char s_last_error[FRIDGE_TFCARD_MAX_ERROR_LEN + 1] = "";

// P4 只有一个 SDMMC 控制器，esp_hosted 已初始化控制器并长期占用 Slot1。
// FAT VFS 仍会调用 host.init，因此这里复用已存在的控制器，只让挂载流程新增 Slot0；
// host.deinit_p 保持默认 sdmmc_host_deinit_slot，挂载失败或卸载时只释放 TF 的 Slot0。
static esp_err_t tfcard_shared_host_init(void)
{
    sdmmc_host_state_t state = {0};
    esp_err_t err = sdmmc_host_get_state(&state);
    if (err != ESP_OK) {
        return err;
    }
    if (state.host_initialized) {
        ESP_LOGI(TAG, "复用 esp_hosted 已初始化的 SDMMC 控制器（当前槽位数=%d）",
                 state.num_of_init_slots);
        return ESP_OK;
    }

    // 仅用于独立 TF 测试或 hosted 尚未创建控制器的场景；组合固件正常路径不会走到这里。
    ESP_LOGW(TAG, "SDMMC 控制器尚未初始化，TF 组件将创建控制器");
    return sdmmc_host_init();
}

// 记录最近错误，供状态查询回显；只保留一条最新记录。
static void tfcard_set_error(const char *msg)
{
    strlcpy(s_last_error, msg ? msg : "", sizeof(s_last_error));
}

// 拉低 GPIO45 显式打开卡座供电（P-MOS 低电平导通；板上默认下拉本身已导通，这里做确定性初始化）。
static esp_err_t tfcard_power_on_slot(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FRIDGE_PIN_TF_PWR_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(FRIDGE_PIN_TF_PWR_EN, 0);
    if (err == ESP_OK) {
        // 给卡座电源和卡内控制器留出稳定时间，避免刚上电就发送 CMD0。
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err;
}

// 使用板载 SDMMC Slot0 尝试挂载。allow_format 只允许由显式格式化入口传 true。
static esp_err_t tfcard_mount(bool allow_format)
{
    if (s_card) {
        return ESP_OK; // 已挂载，避免重复初始化
    }

    // 32K 簇适合 64GB FAT32，兼顾空间占用和文件系统管理开销。
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = allow_format,
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
    };

    // TF 必须使用 SDMMC Slot0；板载 C6 的 esp_hosted 使用 Slot1。两者共享同一个控制器，
    // 所以覆盖 init 以复用 hosted 的控制器；沿用默认二次初始化会因控制器已被 claim 而失败。
    // 频率默认 20MHz 起步，挂载稳定后如需提速再评估 40MHz。
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.init = tfcard_shared_host_init;
    host.slot = SDMMC_HOST_SLOT_0;

    // 槽位引脚：P4 通过 GPIO 矩阵指定 TF 卡座实际引脚；4-bit 总线；板上已有 51K 上拉，内部弱上拉一并打开。
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = FRIDGE_PIN_TF_CLK;
    slot.cmd = FRIDGE_PIN_TF_CMD;
    slot.d0 = FRIDGE_PIN_TF_D0;
    slot.d1 = FRIDGE_PIN_TF_D1;
    slot.d2 = FRIDGE_PIN_TF_D2;
    slot.d3 = FRIDGE_PIN_TF_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "挂载 TF 卡到 %s（自动格式化=%s）",
             FRIDGE_TFCARD_MOUNT_POINT, allow_format ? "允许，本次为用户显式操作" : "禁止");
    esp_err_t err = esp_vfs_fat_sdmmc_mount(FRIDGE_TFCARD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_format_required = (err == ESP_FAIL);
        tfcard_set_error(esp_err_to_name(err));
        ESP_LOGE(TAG, "TF 卡挂载失败: %s（不会自动清卡；无卡/接触不良/不支持的文件系统均可能）",
                 esp_err_to_name(err));
        return err;
    }

    s_format_required = false;
    tfcard_set_error("");
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "TF 卡挂载成功：%s，容量约 %llu MB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

esp_err_t fridge_tfcard_init(void)
{
    if (s_unavailable_this_boot) {
        return ESP_ERR_INVALID_STATE;
    }
    // 组合固件中 esp_hosted 先启动，并以“可调 LDO”方式独占 LDO4 给两个 SDMMC PHY 供电。
    // 这里禁止再次 esp_ldo_acquire_channel()；IDF 6 不允许第二引用可调通道，重复申请会让 TF 必然失败。
    esp_err_t err = tfcard_power_on_slot();
    if (err != ESP_OK) {
        tfcard_set_error(esp_err_to_name(err));
        return err;
    }
    return tfcard_mount(false);
}

void fridge_tfcard_mark_unavailable(const char *reason)
{
    if (s_card) {
        return;
    }
    s_unavailable_this_boot = true;
    s_format_required = false;
    tfcard_set_error(reason && reason[0] ? reason : "TF 卡前置条件未满足，本次启动未初始化");
}

esp_err_t fridge_tfcard_format_fat32(void)
{
    if (s_unavailable_this_boot) {
        return ESP_ERR_INVALID_STATE;
    }
    // LDO4 由已经初始化的 esp_hosted 唯一持有；格式化路径同样不得重复申请该可调通道。
    esp_err_t err = tfcard_power_on_slot();
    if (err != ESP_OK) {
        tfcard_set_error(esp_err_to_name(err));
        return err;
    }

    if (!s_card) {
        // 先做无损挂载：已有 FAT32 时随后只格式化一次；exFAT/无文件系统时再由 mount(true) 创建 FAT32。
        err = tfcard_mount(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "显式格式化：原文件系统无法挂载，开始重建整卡 FAT32");
            err = tfcard_mount(true);
            if (err != ESP_OK) {
                tfcard_set_error(esp_err_to_name(err));
                return err;
            }
            s_formatted_this_boot = true;
            s_format_required = false;
            tfcard_set_error("");
            return ESP_OK;
        }
    }

    // 已挂载 FAT 文件系统时，调用专用 API 强制清空并重新建立 FAT32。
    esp_vfs_fat_mount_config_t format_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
    };
    ESP_LOGW(TAG, "开始显式格式化整张 TF 卡为 FAT32，所有原有数据都会被清除");
    err = esp_vfs_fat_sdcard_format_cfg(FRIDGE_TFCARD_MOUNT_POINT, s_card, &format_config);
    if (err != ESP_OK) {
        tfcard_set_error(esp_err_to_name(err));
        ESP_LOGE(TAG, "TF 卡显式格式化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_formatted_this_boot = true;
    s_format_required = false;
    tfcard_set_error("");
    ESP_LOGI(TAG, "TF 卡显式格式化并重新挂载成功");
    return ESP_OK;
}

void fridge_tfcard_get_status(fridge_tfcard_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mounted = (s_card != NULL);
    out->format_required = s_format_required;
    out->formatted_this_boot = s_formatted_this_boot;
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    if (!s_card) {
        return;
    }

    strlcpy(out->card_name, s_card->cid.name, sizeof(out->card_name));
    out->capacity_mb = ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024);

    // 剩余空间：esp_vfs_fat_info 会遍历 FAT，容量大时首次调用可能较慢，仅在状态查询时执行。
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(FRIDGE_TFCARD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        out->free_mb = free_bytes / (1024 * 1024);
    }
}

esp_err_t fridge_tfcard_selftest(char *detail, size_t detail_size)
{
    if (detail && detail_size) {
        detail[0] = '\0';
    }
    if (!s_card) {
        if (detail) {
            snprintf(detail, detail_size, "TF 卡未挂载：%s", s_last_error[0] ? s_last_error : "未初始化");
        }
        return ESP_ERR_INVALID_STATE;
    }

    // 写入-读回-校验：验证 SDMMC 数据链路与 FAT 写入在断电前的完整路径。
    const char *path = FRIDGE_TFCARD_MOUNT_POINT "/fridge_selftest.txt";
    char payload[96];
    snprintf(payload, sizeof(payload), "fridge_spirit tfcard selftest tick=%lu", (unsigned long)xTaskGetTickCount());

    FILE *f = fopen(path, "w");
    if (!f) {
        tfcard_set_error("selftest fopen(w) failed");
        if (detail) {
            snprintf(detail, detail_size, "写入打开失败：%s", path);
        }
        return ESP_FAIL;
    }
    fputs(payload, f);
    fclose(f);

    char readback[96] = "";
    f = fopen(path, "r");
    if (!f) {
        tfcard_set_error("selftest fopen(r) failed");
        if (detail) {
            snprintf(detail, detail_size, "读回打开失败：%s", path);
        }
        return ESP_FAIL;
    }
    size_t n = fread(readback, 1, sizeof(readback) - 1, f);
    readback[n] = '\0';
    fclose(f);

    if (strcmp(payload, readback) != 0) {
        tfcard_set_error("selftest verify mismatch");
        if (detail) {
            snprintf(detail, detail_size, "读回内容不一致：期望[%s] 实际[%s]", payload, readback);
        }
        return ESP_FAIL;
    }

    if (detail) {
        snprintf(detail, detail_size, "写入并读回校验通过：%s", path);
    }
    return ESP_OK;
}
