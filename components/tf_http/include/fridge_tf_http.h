// 冰箱小精灵 TF 卡 Wi-Fi HTTP 大文件传输接口。
// HTTP 端点不接受任意路径；只能访问先由 COM15 创建并授权的单个 TF 传输会话。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TF_HTTP_PORT 8080
#define FRIDGE_TF_HTTP_TOKEN_HEX_LEN 32
#define FRIDGE_TF_HTTP_URL_LEN 128
#define FRIDGE_TF_HTTP_ERROR_LEN 128

typedef struct {
    bool server_running;
    bool authorized;
    uint32_t session_id;
    uint32_t expires_in_seconds;
    uint16_t port;
    char url[FRIDGE_TF_HTTP_URL_LEN];
    char token[FRIDGE_TF_HTTP_TOKEN_HEX_LEN + 1];
    char last_error[FRIDGE_TF_HTTP_ERROR_LEN + 1];
} fridge_tf_http_status_t;

// 为当前唯一 TF 传输会话生成一次性 Bearer token，并按需启动 HTTP server。
// 前置条件：C6 Hosted/Wi-Fi 已连接并取得 IPv4，TF transfer session 已通过串口 begin。
esp_err_t fridge_tf_http_enable(fridge_tf_http_status_t *out);
esp_err_t fridge_tf_http_get_status(fridge_tf_http_status_t *out);
esp_err_t fridge_tf_http_disable(void);

#ifdef __cplusplus
}
#endif
