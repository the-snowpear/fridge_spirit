// 冰箱小精灵 TF 卡组件 —— 非 P4 目标桩实现。
// 旧 ESP32-S3 板没有 SDMMC 主机外设也未接 TF 卡；提供同 API 桩保证存档分支可编译，
// 所有操作返回 ESP_ERR_NOT_SUPPORTED。

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_tfcard.h"

static const char *TAG = "fridge_tfcard";

esp_err_t fridge_tfcard_init(void)
{
    ESP_LOGW(TAG, "TF card is not available on this target (stub)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_tfcard_format_fat32(void)
{
    ESP_LOGW(TAG, "TF card format is not available on this target (stub)");
    return ESP_ERR_NOT_SUPPORTED;
}

void fridge_tfcard_get_status(fridge_tfcard_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->last_error, "TF card not supported on this target", sizeof(out->last_error));
}

esp_err_t fridge_tfcard_selftest(char *detail, size_t detail_size)
{
    if (detail && detail_size) {
        snprintf(detail, detail_size, "当前目标不支持 TF 卡");
    }
    return ESP_ERR_NOT_SUPPORTED;
}
