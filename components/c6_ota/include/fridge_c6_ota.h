#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_C6_OTA_MAX_IMAGE_SIZE 0x180000U
#define FRIDGE_C6_OTA_MAX_CHUNK_SIZE 1024U
#define FRIDGE_C6_OTA_SHA256_HEX_LEN 64U
#define FRIDGE_C6_OTA_ERROR_LEN 128U

typedef enum {
    FRIDGE_C6_OTA_IDLE = 0,
    FRIDGE_C6_OTA_WRITING,
    FRIDGE_C6_OTA_WAIT_ACTIVATE,
    FRIDGE_C6_OTA_COMPLETE,
    FRIDGE_C6_OTA_FAILED,
} fridge_c6_ota_state_t;

typedef enum {
    // 出厂 Hosted 0.0.6 在 OTA end 后设置启动分区并自动重启，不支持 activate RPC。
    FRIDGE_C6_OTA_LEGACY_END_REBOOTS = 0,
    // Hosted 2.6+ 在 end 校验后等待单独 activate RPC。
    FRIDGE_C6_OTA_EXPLICIT_ACTIVATE,
} fridge_c6_ota_activation_t;

// C6 升级状态。session/offset 用于让串口脚本安全确认重发，不允许重复写同一块 Flash。
typedef struct {
    fridge_c6_ota_state_t state;
    uint32_t session;
    uint32_t expected_size;
    uint32_t received_size;
    uint32_t last_offset;
    uint32_t last_length;
    uint32_t last_crc32;
    uint32_t slave_major;
    uint32_t slave_minor;
    uint32_t slave_patch;
    bool requires_activation;
    bool last_chunk_duplicate;
    char expected_sha256[FRIDGE_C6_OTA_SHA256_HEX_LEN + 1];
    char actual_sha256[FRIDGE_C6_OTA_SHA256_HEX_LEN + 1];
    char last_error[FRIDGE_C6_OTA_ERROR_LEN];
} fridge_c6_ota_status_t;

// 开始把 APP-only network_adapter.bin 写入 C6 的非活动 OTA 分区。
esp_err_t fridge_c6_ota_begin(uint32_t image_size,
                              const char *sha256_hex,
                              fridge_c6_ota_activation_t activation,
                              uint32_t *out_session);

// 按 offset 顺序写入一块；完全相同的最后一块可重复确认，但不会再次写入。
esp_err_t fridge_c6_ota_write(uint32_t session,
                              uint32_t offset,
                              const uint8_t *data,
                              size_t data_len,
                              uint32_t expected_crc32,
                              uint32_t *out_next_offset,
                              bool *out_duplicate);

// 校验总长度/SHA-256并结束 OTA；旧于2.6的 Slave会自行激活，新版需再调用 activate。
esp_err_t fridge_c6_ota_end(uint32_t session);

// 激活已校验的新版 C6 镜像；旧版 Slave 不支持也不需要该命令。
esp_err_t fridge_c6_ota_activate(uint32_t session);

void fridge_c6_ota_get_status(fridge_c6_ota_status_t *status);
const char *fridge_c6_ota_state_name(fridge_c6_ota_state_t state);

#ifdef __cplusplus
}
#endif
