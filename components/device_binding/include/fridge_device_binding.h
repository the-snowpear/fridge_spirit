// 冰箱小精灵 P4 设备二维码绑定接口。
// 本组件只负责设备身份预置、短期二维码会话和绑定凭据领取，不直接访问 LVGL。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_BINDING_API_BASE_MAX 160
#define FRIDGE_BINDING_DEVICE_SN_MAX 39
#define FRIDGE_BINDING_KEY_ID_MAX 39
#define FRIDGE_BINDING_SECRET_MAX 95
#define FRIDGE_BINDING_DEVICE_TOKEN_MAX 191
#define FRIDGE_BINDING_SESSION_ID_MAX 63
#define FRIDGE_BINDING_QR_PAYLOAD_MAX 255
#define FRIDGE_BINDING_TEXT_MAX 95

typedef struct {
    char api_base_url[FRIDGE_BINDING_API_BASE_MAX + 1];
    char device_sn[FRIDGE_BINDING_DEVICE_SN_MAX + 1];
    char bootstrap_key_id[FRIDGE_BINDING_KEY_ID_MAX + 1];
    char bootstrap_secret[FRIDGE_BINDING_SECRET_MAX + 1];
} fridge_device_provisioning_t;

typedef struct {
    bool initialized;
    bool provisioned;
    bool bound;
    bool qr_ready;
    bool worker_running;
    uint32_t expires_in_seconds;
    uint32_t binding_epoch;
    int last_http_status;
    int last_error;
    char api_base_url[FRIDGE_BINDING_API_BASE_MAX + 1];
    char device_sn[FRIDGE_BINDING_DEVICE_SN_MAX + 1];
    char session_id[FRIDGE_BINDING_SESSION_ID_MAX + 1];
    char qr_payload[FRIDGE_BINDING_QR_PAYLOAD_MAX + 1];
    char status[FRIDGE_BINDING_TEXT_MAX + 1];
    char detail[FRIDGE_BINDING_TEXT_MAX + 1];
} fridge_device_binding_status_t;

typedef void (*fridge_device_binding_status_cb_t)(
    const fridge_device_binding_status_t *status,
    void *user_ctx);

// 初始化绑定状态和后台任务。回调只收到脱敏状态，可安全转发给 UI。
esp_err_t fridge_device_binding_init(fridge_device_binding_status_cb_t callback,
                                     void *user_ctx);

// 通过受控 USB 管理通道一次写入设备身份；bootstrap secret 不会通过状态接口回显。
esp_err_t fridge_device_binding_set_provisioning(
    const fridge_device_provisioning_t *provisioning);

// 返回脱敏状态。
esp_err_t fridge_device_binding_get_status(fridge_device_binding_status_t *out);

// 使当前二维码会话失效并立即申请新会话；网络未连接时会在恢复后自动重试。
esp_err_t fridge_device_binding_refresh_session(void);

#ifdef __cplusplus
}
#endif
