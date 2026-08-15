#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_WIFI_MAX_SSID_LEN 32
#define FRIDGE_WIFI_MAX_PASSWORD_LEN 64
#define FRIDGE_WIFI_MAX_IP_LEN 16
#define FRIDGE_WIFI_MAX_AUTHMODE_LEN 24
#define FRIDGE_WIFI_MAX_ERROR_LEN 192
#define FRIDGE_WIFI_DEFAULT_NTP "pool.ntp.org"

// Wi-Fi 配置：由 USB 配网或后续官方 provisioning 复用。
// 注意：password 只用于连接和 NVS 保存，不允许输出到日志或状态响应。
typedef struct {
    char ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1];
    char password[FRIDGE_WIFI_MAX_PASSWORD_LEN + 1];
} fridge_wifi_config_t;

// 扫描结果：只描述 2.4GHz AP 的必要信息，避免前端依赖 ESP-IDF 私有结构。
typedef struct {
    char ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    bool secured;
    char authmode[FRIDGE_WIFI_MAX_AUTHMODE_LEN];
} fridge_wifi_ap_t;

// 网络状态：供 Web 面板展示，密码永远不回传。
typedef struct {
    bool initialized;
    bool connected;
    bool connecting;
    bool saved;
    bool internet_ready;
    char ssid[FRIDGE_WIFI_MAX_SSID_LEN + 1];
    char ip[FRIDGE_WIFI_MAX_IP_LEN];
    int8_t rssi;
    bool hosted_ready;
    bool c6_ota_guard;
    uint32_t hosted_retry_count;
    char hosted_host_version[24];
    char hosted_slave_version[24];
    char hosted_last_error[FRIDGE_WIFI_MAX_ERROR_LEN];
    char ntp_server[64];
    char last_error[FRIDGE_WIFI_MAX_ERROR_LEN];
} fridge_network_status_t;

// 初始化 Wi-Fi STA、事件循环、NVS 命名空间和 SNTP 基础状态。
// P4 由 esp_wifi_remote/esp_hosted 经板载 C6 联网，不是 P4 片内 Wi-Fi；必须正确配置 SDIO 电源管理。
// 硬件注意：Wi-Fi 发射有电流峰值，调试时建议使用稳定 USB/5V 供电，避免 brownout。
esp_err_t fridge_network_init(void);

// 扫描真实 Wi-Fi AP。该函数会阻塞等待扫描结束，最多返回 max_count 个结果。
// 只过滤空 SSID，并对同名 SSID 保留 RSSI 最强的一项。
esp_err_t fridge_network_scan(fridge_wifi_ap_t *aps, size_t max_count, size_t *out_count);

// 连接 Wi-Fi；save 为 true 时保存 SSID/密码到 NVS，重启后可自动连接。
esp_err_t fridge_network_connect(const fridge_wifi_config_t *config, bool save);

// 保存已经通过连接和 SNTP 校验的当前 Wi-Fi 凭据。
// 仅当设备仍关联 config->ssid、Wi-Fi 驱动中保存的 SSID/密码与 config 一致，且 internet_ready=true 时成功；
// 因此配网 UI 可先 connect(..., false) + sync_time()，验证成功后调用本接口，不会因失败测试覆盖旧凭据。
// 密码只与 Wi-Fi 驱动/NVS 比对，不会写入日志或网络状态。
esp_err_t fridge_network_save_config(const fridge_wifi_config_t *config);

// 使用 NVS 中保存的 SSID/密码自动连接；没有凭据时返回 ESP_ERR_NOT_FOUND。
esp_err_t fridge_network_connect_saved(void);

// 读取已验证并保存的 SSID。只返回 SSID，绝不向 UI、日志或调用方暴露密码。
esp_err_t fridge_network_get_saved_ssid(char *ssid, size_t ssid_size);

// 后台尝试连接 NVS 中保存的 Wi-Fi；不会阻塞 USB/Web Serial 调试面板启动。
esp_err_t fridge_network_connect_saved_async(void);

// 获取当前网络状态；不包含密码。
esp_err_t fridge_network_get_status(fridge_network_status_t *status);

// 执行 SNTP 校时；成功后 internet_ready 置 true，首版用它判断互联网可达。
esp_err_t fridge_network_sync_time(void);

// C6 固件升级期间禁止扫描、配网、SNTP 和自动重连，避免业务 RPC 与 OTA RPC 并发。
// 此 guard 一旦成功进入就保持到整板重启；不在当前启动周期恢复网络业务。
esp_err_t fridge_network_enter_c6_ota(void);
bool fridge_network_hosted_ready(void);

#ifdef __cplusplus
}
#endif
