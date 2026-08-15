// 冰箱小精灵主控启动入口。
// 这里只做系统初始化编排：NVS、诊断、网络、USB JSON 协议、recovery 兜底和专用硬件测试模式。
// 硬件注意：MIPI 独立点屏与摄像头测试会占用专用外设，不能和正常运维链路同时启用。
// SPI/TR230S display 与 FRIDGE_SCREEN_TEST 已移除，P4 主线仅保留 MIPI-DSI。

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#if CONFIG_FRIDGE_RECOVERY_APP
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#elif CONFIG_FRIDGE_MIPI_SCREEN_TEST
#include "fridge_display_mipi_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#elif CONFIG_FRIDGE_P4_MINIMAL
// P4 服务模式：Wi-Fi(板载C6) + USB JSON + MIPI 屏/GT911 + ES8311/FY-SP703 + WakeNet/ASR + TF 卡。
// 摄像头为按需全屏预览（OV5647/MIPI-CSI）。旧 S3 业务组件不进入此构建图。
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "fridge_agent_runtime.h"
#include "fridge_agent_storage.h"
#include "fridge_ai_client.h"
#include "fridge_audio.h"
#include "fridge_camera.h"
#include "fridge_diagnostics.h"
#include "fridge_diagnostics_log.h"
#include "fridge_device_binding.h"
#include "fridge_display_mipi.h"
#include "fridge_glm_realtime.h"
#include "fridge_network.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_screenshot.h"
#include "fridge_ui_p4.h"
#include "fridge_voice_asr.h"
#include "fridge_skill_manager.h"
#include "fridge_tfcard.h"
#include "fridge_tool_broker.h"
#include "fridge_trace_service.h"
#include "fridge_usb_protocol.h"
#include "fridge_workflow_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_FRIDGE_P4_TOUCH_ENABLE
#include "fridge_touch.h"
#if CONFIG_FRIDGE_P4_GT911_ENABLE
#include "fridge_touch_gt911.h"
#include "fridge_sht4x.h"
#include "fridge_sensors.h"
#endif
#endif
#else
#include "fridge_ai_client.h"
#include "fridge_camera.h"
#include "fridge_diagnostics.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "fridge_usb_protocol.h"
#include "fridge_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_kitchen_tools.h"
#include "fridge_kitchen_ai.h"
#include "fridge_radar.h"
#include "fridge_sensors.h"
#include "fridge_speaker.h"
#include "fridge_state_machine.h"
#include "fridge_wake_word.h"
#endif

static const char *TAG = "fridge_main";

#if CONFIG_FRIDGE_P4_MINIMAL
// 仅统计 P4 外设可用的内部 DMA 堆；用于定位 SDMMC、JPEG 与显示 DMA 的连续内存不足。
static void p4_log_internal_dma_heap(const char *phase)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "internal_dma[%s]: free=%u largest=%u minimum=%u",
             phase ? phase : "unknown",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

// 分配失败钩子在失败调用方的上下文中执行；只输出固定格式日志，禁止再申请内存或加锁。
static void p4_heap_alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    ESP_EARLY_LOGE(TAG, "heap alloc failed: size=%u caps=0x%08lx function=%s",
                   (unsigned)size, (unsigned long)caps,
                   function_name ? function_name : "unknown");
}

// 绑定组件的网络任务只能推送脱敏快照；本回调只复制到 UI 的线程安全模型，
// 不在非 UI 任务直接访问 LVGL 对象。
static void p4_binding_status_changed(const fridge_device_binding_status_t *status,
                                      void *user_ctx)
{
    (void)user_ctx;
    if (!status) return;
    fridge_ui_p4_binding_model_t model = {
        .provisioned = status->provisioned,
        .bound = status->bound,
        .qr_ready = status->qr_ready,
        .expires_in_seconds = status->expires_in_seconds,
    };
    strlcpy(model.qr_payload, status->qr_payload, sizeof(model.qr_payload));
    strlcpy(model.device_sn, status->device_sn, sizeof(model.device_sn));
    strlcpy(model.status, status->status, sizeof(model.status));
    strlcpy(model.detail, status->detail, sizeof(model.detail));
    (void)fridge_ui_p4_update_binding(&model);
}

static void p4_mqtt_autostart_task(void *arg)
{
    (void)arg;
    // 未完成二维码绑定时 MQTT 配置为空；绑定任务/USB 配置成功后会主动启动 MQTT。
    // 此处直接退出，避免在空配置上周期性重试并浪费 P4 宝贵的内部任务栈。
    fridge_mqtt_status_t mqtt = {0};
    if (fridge_mqtt_get_status(&mqtt) == ESP_OK &&
        (!mqtt.enabled || !mqtt.configured)) {
        ESP_LOGI(TAG, "P4 MQTT autostart waiting for device provisioning/binding");
        vTaskDelete(NULL);
    }

    // Wi-Fi 与 DHCP 在后台完成。只做有限频率重试，避免网络未就绪时刷日志或抢占 UI。
    for (uint32_t attempt = 0; attempt < 180U; attempt++) {
        fridge_network_status_t network = {0};
        if (fridge_network_get_status(&network) == ESP_OK && network.connected) {
            esp_err_t err = fridge_mqtt_start();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "P4 MQTT v2 started after Wi-Fi connected");
                vTaskDelete(NULL);
            }
            if (err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "P4 MQTT autostart failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "P4 MQTT autostart stopped after timeout; binding worker can retry after provisioning");
    vTaskDelete(NULL);
}
#endif

#if !CONFIG_FRIDGE_MIPI_SCREEN_TEST
static void nvs_init_safe(void)
{
    // 初始化 NVS，用于保存 Wi-Fi、API 配置和恢复模式需要的网络参数。
    // 注意：NVS 写入发生在 Flash 中，后续要避免高频写入；本项目只在用户更新配置时写入。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}
#endif

#if !CONFIG_FRIDGE_RECOVERY_APP && !CONFIG_FRIDGE_MIPI_SCREEN_TEST && \
    !CONFIG_FRIDGE_P4_MINIMAL
static void mqtt_autostart_task(void *arg)
{
    (void)arg;
    // Wi-Fi 是后台连接的，MQTT 必须等拿到 IP 后再启动；否则只会在启动早期返回 ESP_ERR_INVALID_STATE。
    for (int i = 0; i < 120; i++) {
        fridge_network_status_t net = {0};
        if (fridge_network_get_status(&net) == ESP_OK && net.connected) {
            esp_err_t err = fridge_mqtt_start();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "MQTT autostart after Wi-Fi connected");
            } else {
                ESP_LOGW(TAG, "MQTT autostart failed after Wi-Fi connected: %s", esp_err_to_name(err));
            }
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "MQTT autostart timed out waiting for Wi-Fi");
    vTaskDelete(NULL);
}

static void start_saved_network_and_mqtt_early(void)
{
    esp_err_t saved_ret = fridge_network_connect_saved_async();
    if (saved_ret == ESP_OK) {
        ESP_LOGI(TAG, "saved Wi-Fi background connect started");
    } else {
        ESP_LOGW(TAG, "start saved Wi-Fi background connect failed: %s", esp_err_to_name(saved_ret));
    }

    esp_err_t mqtt_ret = fridge_mqtt_start();
    if (mqtt_ret == ESP_OK) {
        ESP_LOGI(TAG, "saved MQTT background connect started");
        return;
    }

    // 首次上电时 Wi-Fi 往往还没拿到 IP。这里在语音/唤醒词等大内存组件启动前短暂等待，
    // 让 MQTT 任务优先占住连续内部 SRAM，保证离线恢复后能立刻接收云端快照并上传本地脏数据。
    ESP_LOGI(TAG, "MQTT not started yet: %s; waiting briefly before audio stack", esp_err_to_name(mqtt_ret));
    for (int i = 0; i < 20; i++) {
        fridge_network_status_t net = {0};
        if (fridge_network_get_status(&net) == ESP_OK && net.connected) {
            mqtt_ret = fridge_mqtt_start();
            if (mqtt_ret == ESP_OK) {
                ESP_LOGI(TAG, "MQTT started before audio stack after Wi-Fi connected");
                return;
            }
            ESP_LOGW(TAG, "MQTT early start after Wi-Fi connected failed: %s", esp_err_to_name(mqtt_ret));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    BaseType_t ok = xTaskCreate(mqtt_autostart_task, "mqtt_autostart", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "create MQTT autostart task failed");
    }
}
#endif

#if CONFIG_FRIDGE_RECOVERY_APP
static void recovery_print_json_status(const char *request_id, const char *command)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);

    // recovery 响应保持一行 JSON，方便 Web Serial 或脚本在日志噪声中按 request_id 匹配。
    printf("{\"type\":\"response\",\"request_id\":\"%s\",\"command\":\"%s\",\"ok\":true,"
           "\"payload\":{\"mode\":\"recovery\",\"firmware\":\"%s\",\"runningPartition\":\"%s\","
           "\"targetPartition\":\"ota_0\",\"wifiConnected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
           "\"note\":\"local recovery shell; future OTA restore writes main firmware to ota_0\"}}\n",
           request_id,
           command,
           app_desc ? app_desc->version : "dev",
           running ? running->label : "unknown",
           net.connected ? "true" : "false",
           net.ssid,
           net.ip);
    fflush(stdout);
}

static void recovery_print_error(const char *request_id, const char *command, const char *error)
{
    printf("{\"type\":\"response\",\"request_id\":\"%s\",\"command\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n",
           request_id,
           command,
           error);
    fflush(stdout);
}

static bool recovery_get_json_string(const char *line, const char *key, char *out, size_t out_size)
{
    // recovery 只解析调试脚本发来的短 JSON Lines，避免引入完整 JSON 库扩大 ota_1 固件体积。
    if (!line || !key || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t used = 0;
    while (*p && *p != '"' && used + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[used++] = *p++;
    }
    out[used] = '\0';
    return used > 0;
}

static void recovery_handle_set_network(const char *line, const char *request_id, const char *command)
{
    fridge_wifi_config_t cfg = {0};
    recovery_get_json_string(line, "ssid", cfg.ssid, sizeof(cfg.ssid));
    recovery_get_json_string(line, "password", cfg.password, sizeof(cfg.password));
    if (cfg.ssid[0] == '\0') {
        recovery_print_error(request_id, command, "ssid is required");
        return;
    }

    // recovery 允许保存 Wi-Fi，便于主固件损坏时仍能靠本地兜底程序联网下载后续主固件。
    esp_err_t err = fridge_network_connect(&cfg, true);
    if (err != ESP_OK) {
        fridge_network_status_t status = {0};
        fridge_network_get_status(&status);
        recovery_print_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    recovery_print_json_status(request_id, command);
}

static void recovery_handle_boot_main(const char *request_id, const char *command)
{
    const esp_partition_t *main_app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    if (!main_app) {
        recovery_print_error(request_id, command, "ota_0 main partition not found");
        return;
    }

    // recovery 不覆盖自身，只把下次启动目标切回主固件 ota_0。
    // 如果 ota_0 镜像已损坏，bootloader 仍会回到可启动分区，避免擦写恢复固件导致失去本地兜底。
    esp_err_t err = esp_ota_set_boot_partition(main_app);
    if (err != ESP_OK) {
        recovery_print_error(request_id, command, esp_err_to_name(err));
        return;
    }
    recovery_print_json_status(request_id, command);
    vTaskDelay(pdMS_TO_TICKS(200));
    // 记录恢复模式主动切回主固件，重启后可与真实 panic/watchdog 区分。
    (void)fridge_diagnostics_set_planned_restart("recovery_boot_main");
    esp_restart();
}

static void recovery_handle_restore_main(const char *line, const char *request_id, const char *command)
{
    char url[256] = "";
    recovery_get_json_string(line, "url", url, sizeof(url));
    if (url[0] == '\0') {
        recovery_print_error(request_id, command, "url is required");
        return;
    }

    const esp_partition_t *main_app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    if (!main_app) {
        recovery_print_error(request_id, command, "ota_0 main partition not found");
        return;
    }

    // 从 recovery 覆盖 ota_0。这里依赖系统证书包校验 HTTPS 服务器证书；
    // 生产前还需要增加固件签名/哈希白名单，避免只凭 URL 下载未授权镜像。
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .bulk_flash_erase = true,
        .partition = {
            .staging = main_app,
            .final = main_app,
            .finalize_with_copy = false,
        },
    };

    puts("{\"type\":\"event\",\"event\":\"recovery_restore_started\",\"target\":\"ota_0\"}");
    fflush(stdout);
    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        recovery_print_error(request_id, command, esp_err_to_name(err));
        return;
    }

    esp_err_t boot_err = esp_ota_set_boot_partition(main_app);
    if (boot_err != ESP_OK) {
        recovery_print_error(request_id, command, esp_err_to_name(boot_err));
        return;
    }

    recovery_print_json_status(request_id, command);
    vTaskDelay(pdMS_TO_TICKS(300));
    // OTA 写入成功后的计划重启只记录原因，不把网络凭据写入 NVS 或日志。
    (void)fridge_diagnostics_set_planned_restart("recovery_restore_main");
    esp_restart();
}

static void recovery_shell_task(void *arg)
{
    (void)arg;
    char line[1024];

    ESP_LOGI(TAG, "recovery JSON Lines shell started");
    puts("{\"type\":\"event\",\"event\":\"recovery_ready\",\"mode\":\"recovery\",\"commands\":[\"get_status\",\"set_network\",\"restore_main\",\"boot_main\"]}");
    fflush(stdout);

    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char request_id[48] = "recovery";
        char command[32] = "";
        recovery_get_json_string(line, "request_id", request_id, sizeof(request_id));
        recovery_get_json_string(line, "command", command, sizeof(command));
        if (command[0] == '\0') {
            recovery_get_json_string(line, "cmd", command, sizeof(command));
        }

        if (strcmp(command, "get_status") == 0 || strcmp(command, "get_diagnostics") == 0) {
            recovery_print_json_status(request_id, command);
        } else if (strcmp(command, "set_network") == 0) {
            recovery_handle_set_network(line, request_id, command);
        } else if (strcmp(command, "boot_main") == 0) {
            recovery_handle_boot_main(request_id, command);
        } else if (strcmp(command, "restore_main") == 0) {
            recovery_handle_restore_main(line, request_id, command);
        } else {
            recovery_print_error(request_id, command[0] ? command : "unknown", "recovery command not available");
        }
    }
}
#elif CONFIG_FRIDGE_MIPI_SCREEN_TEST
static void mipi_display_test_task(void *arg)
{
    (void)arg;
    // 独立 MIPI 点屏任务：循环整屏图案，不初始化其它高功耗外设，便于观察 DSI 链路稳定性。
    fridge_display_mipi_test_run();
}
#elif CONFIG_FRIDGE_P4_MINIMAL
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
// OTA 回滚健康确认：只有首阶段初始化已走完且 USB 控制台可用时，才确认当前 ota_0 镜像。
// 外接屏未连接或单个外设失败不代表固件不可启动；USB 控制台仍可用于读取日志和继续排障。
static void p4_minimal_confirm_ota_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "cannot query running OTA partition; image remains unconfirmed");
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot query OTA image state: %s", esp_err_to_name(err));
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }

    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image confirmed valid after P4 minimal health gate");
    } else {
        ESP_LOGE(TAG, "failed to confirm OTA image: %s", esp_err_to_name(err));
    }
}
#endif

#if CONFIG_FRIDGE_P4_TOUCH_ENABLE && !CONFIG_FRIDGE_P4_GT911_ENABLE
// 触摸轮询任务：20-pin FPC 转杜邦板引出 FT6336U 的 INT/SDA/SCL/RST 四根控制线。
static void p4_minimal_touch_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    while (true) {
        fridge_touch_point_t pt = {0};
        if (fridge_touch_read(&pt) == ESP_OK) {
            if (pt.pressed && !was_pressed) {
                ESP_LOGI(TAG, "触摸按下: x=%u y=%u points=%u", pt.x, pt.y, pt.points);
            } else if (!pt.pressed && was_pressed) {
                ESP_LOGI(TAG, "触摸抬起");
            }
            was_pressed = pt.pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif
#endif

#if CONFIG_FRIDGE_P4_MINIMAL
// Agent Runtime 与 UI 的桥仅在 main 中做字段映射：两端组件不直接相互依赖，避免
// ui_p4 → agent_runtime → tool_system 的链路再次形成循环。所有函数均为非阻塞投递/轮询。
static esp_err_t p4_agent_ui_submit(const char *text, void *user_ctx)
{
    (void)user_ctx;
    return fridge_agent_runtime_submit_text(text);
}

static esp_err_t p4_agent_ui_cancel(void *user_ctx)
{
    (void)user_ctx;
    return fridge_agent_runtime_cancel_active();
}

// 对话页确认后才调用：Runtime 会清除 TF 中的当前会话，并广播 SESSION_RESET 给 UI。
static esp_err_t p4_agent_ui_clear_session(void *user_ctx)
{
    (void)user_ctx;
    return fridge_agent_runtime_clear_session();
}

static esp_err_t p4_agent_ui_resolve(const char *run_id, const char *call_id,
                                     bool confirmed, void *user_ctx)
{
    (void)user_ctx;
    return fridge_agent_runtime_resolve_confirmation(run_id, call_id, confirmed);
}

static bool p4_agent_ui_poll(fridge_ui_p4_agent_event_t *out, void *user_ctx)
{
    (void)user_ctx;
    fridge_agent_runtime_ui_event_t event = {0};
    if (!out || !fridge_agent_runtime_poll_ui_event(&event)) return false;
    // Runtime 事件仍由 UI 消费；这里只向连续语音协调器复制一个非阻塞通知，不能抢走队列。
    (void)fridge_voice_asr_dialog_notify_agent_event(&event);
    memset(out, 0, sizeof(*out));
    switch (event.kind) {
    case FRIDGE_AGENT_RUNTIME_EVENT_SESSION_RESET:
        out->kind = FRIDGE_UI_P4_AGENT_EVENT_SESSION_RESET;
        break;
    case FRIDGE_AGENT_RUNTIME_EVENT_USER_MESSAGE:
        out->kind = FRIDGE_UI_P4_AGENT_EVENT_USER_MESSAGE;
        break;
    case FRIDGE_AGENT_RUNTIME_EVENT_ASSISTANT_MESSAGE:
        out->kind = FRIDGE_UI_P4_AGENT_EVENT_ASSISTANT_MESSAGE;
        break;
    case FRIDGE_AGENT_RUNTIME_EVENT_TOOL:
        out->kind = FRIDGE_UI_P4_AGENT_EVENT_TOOL;
        break;
    case FRIDGE_AGENT_RUNTIME_EVENT_TTS:
        out->kind = FRIDGE_UI_P4_AGENT_EVENT_TTS;
        break;
    case FRIDGE_AGENT_RUNTIME_EVENT_STATUS:
    default:
        out->kind = FRIDGE_UI_P4_AGENT_EVENT_STATUS;
        break;
    }
    strlcpy(out->run_id, event.run_id, sizeof(out->run_id));
    strlcpy(out->call_id, event.call_id, sizeof(out->call_id));
    strlcpy(out->time, event.time, sizeof(out->time));
    strlcpy(out->state, event.state, sizeof(out->state));
    strlcpy(out->text, event.text, sizeof(out->text));
    strlcpy(out->tool_name, event.tool_name, sizeof(out->tool_name));
    strlcpy(out->tool_label, event.tool_label, sizeof(out->tool_label));
    strlcpy(out->detail, event.detail, sizeof(out->detail));
    strlcpy(out->latency, event.latency, sizeof(out->latency));
    out->risk = event.risk;
    out->tool_status = event.tool_status;
    out->requires_confirmation = event.requires_confirmation;
    out->working = event.working;
    out->dialog_complete = event.dialog_complete;
    return true;
}
#endif

void app_main(void)
{
#if CONFIG_FRIDGE_RECOVERY_APP
    // 小型本地恢复模式：只启动 NVS、诊断、Wi-Fi 和串口 JSON Lines。
    // 注意：该固件应烧录到 ota_1 小分区，禁止在此模式中初始化屏幕、摄像头、语音或 AI 大组件。
    ESP_LOGW(TAG, "CONFIG_FRIDGE_RECOVERY_APP is enabled; local recovery mode");
    nvs_init_safe();
    fridge_diagnostics_init();
    ESP_ERROR_CHECK(fridge_network_init());

    esp_err_t saved_ret = fridge_network_connect_saved_async();
    if (saved_ret == ESP_OK) {
        ESP_LOGI(TAG, "recovery saved Wi-Fi background connect started");
    } else {
        ESP_LOGW(TAG, "recovery saved Wi-Fi not started: %s", esp_err_to_name(saved_ret));
    }

    BaseType_t ok = xTaskCreate(recovery_shell_task, "recovery_shell", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create recovery_shell task failed");
    }
#elif CONFIG_FRIDGE_MIPI_SCREEN_TEST
    // 26-pin MIPI-DSI 独立测试模式：只启动 EK79007 全屏单色/彩条循环。
    ESP_LOGW(TAG, "CONFIG_FRIDGE_MIPI_SCREEN_TEST is enabled");
    BaseType_t ok = xTaskCreate(mipi_display_test_task, "mipi_test", 6144, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create MIPI display test task failed");
    }
#elif CONFIG_FRIDGE_P4_MINIMAL
    // P4 最小移植模式（慧勤智远 ESP32-P4 CB V1.2）：
    // 初始化 NVS、诊断、USB JSON、网络、MIPI 屏/触摸、ES8311、WakeNet/云端 ASR 和 TF 卡。
    // 摄像头按需由 UI「摄像头预览」按钮启动（OV5647 MIPI-CSI 全屏流），不在上电阶段强制 init。
    // 旧 S3 传感器/雷达业务不参与链接；MPU6050 使用 P4 共享 I2C0 独立接入。
    // 其余未迁移的 USB 命令会立即返回错误。
    // 日志 tee 必须在任何 P4 初始化日志之前安装；此时 TF 尚未挂载，记录先暂存于 8KB RAM 环形缓冲。
    esp_err_t diag_log_install_ret = fridge_diagnostics_log_install();
    if (diag_log_install_ret != ESP_OK) {
        ESP_LOGW(TAG, "diagnostics log tee install failed: %s", esp_err_to_name(diag_log_install_ret));
    }
    esp_err_t heap_hook_ret = heap_caps_register_failed_alloc_callback(p4_heap_alloc_failed);
    if (heap_hook_ret != ESP_OK) {
        ESP_LOGW(TAG, "heap allocation failure hook register failed: %s",
                 esp_err_to_name(heap_hook_ret));
    }
    p4_log_internal_dma_heap("startup_begin");
    ESP_LOGW(TAG, "CONFIG_FRIDGE_P4_MINIMAL is enabled; P4 bring-up profile");
    nvs_init_safe();
    fridge_diagnostics_init();

    // USB JSON 控制台最先启动，保证 hosted 握手或任一外设初始化失败时仍能读取诊断。
    esp_err_t usb_ret = fridge_usb_protocol_start();
    if (usb_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB console task started; hardware commands remain gated during initialization");
    } else {
        ESP_LOGW(TAG, "USB provisioning console skipped: %s", esp_err_to_name(usb_ret));
    }

    // 网络随后启动：esp_hosted 需要复位并握手板载 C6，失败时不阻塞其余硬件验证。
    esp_err_t net_ret = fridge_network_init();
    if (net_ret != ESP_OK) {
        ESP_LOGE(TAG, "network init failed: %s (check C6/SDIO power management)", esp_err_to_name(net_ret));
    }

    // P4 Agent、AI 配置多端同步和语音会话共用同一套 NVS 配置。
    // 这里只加载配置，不发起网络请求；必须早于 Agent Runtime 与设备绑定 seed。
    esp_err_t ai_client_ret = fridge_ai_client_init();
    if (ai_client_ret != ESP_OK) {
        ESP_LOGW(TAG, "AI client config init failed: %s", esp_err_to_name(ai_client_ret));
    }

    if (net_ret == ESP_OK) {
        esp_err_t saved_ret = fridge_network_connect_saved_async();
        if (saved_ret == ESP_OK) {
            ESP_LOGI(TAG, "saved Wi-Fi background connect started");
        } else {
            ESP_LOGW(TAG, "saved Wi-Fi background connect not started: %s", esp_err_to_name(saved_ret));
        }
    }

#if CONFIG_FRIDGE_P4_MIPI_UI_ENABLE
    // 正常 P4 UI 使用 26-pin MIPI-DSI 屏；DSI PHY、RST=GPIO22 和 BL=GPIO23 参数沿用已验证点屏路径。
    // SPI/TR230S 驱动已移除，关闭本选项时不再有 QSPI 色带回退。
    esp_err_t disp_ret = fridge_display_mipi_init();
    if (disp_ret != ESP_OK) {
        ESP_LOGW(TAG, "MIPI display init failed: %s (check 26-pin FPC/5V power)", esp_err_to_name(disp_ret));
    }
#else
    esp_err_t disp_ret = ESP_ERR_NOT_SUPPORTED;
    ESP_LOGW(TAG, "P4 MIPI UI disabled and SPI/TR230S display removed; no screen backend");
#endif

#if CONFIG_FRIDGE_P4_TOUCH_ENABLE
#if CONFIG_FRIDGE_P4_GT911_ENABLE
    // MIPI 屏触摸：WKS70 GT911 使用 GPIO15/16/17/18；优先 0x14，对齐厂商 7 寸复位与软复位。
    esp_err_t touch_ret = fridge_touch_gt911_init();
#else
    // 旧 QSPI/FT6336U 触摸保留给排障配置。
    esp_err_t touch_ret = fridge_touch_init();
#endif
    if (touch_ret == ESP_OK) {
#if CONFIG_FRIDGE_P4_GT911_ENABLE
        ESP_LOGI(TAG, "GT911 touch ready addr=0x%02X; use touch_status/touch_scan over USB for diagnostics",
                 (unsigned)fridge_touch_gt911_get_address());
#else
        if (xTaskCreate(p4_minimal_touch_task, "p4_touch", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "create touch poll task failed");
        }
#endif
    } else {
        ESP_LOGW(TAG, "touch init failed: %s (WKS70: check 26-pin FPC CTP lines INT15/SCL16/SDA17/RST18)",
                 esp_err_to_name(touch_ret));
    }
#else
    ESP_LOGI(TAG, "P4 direct touch disabled by configuration");
#endif

#if CONFIG_FRIDGE_P4_GT911_ENABLE
    // SHT40 温湿度：复用 GT911 已建的 I2C0 共享总线（GPIO16/17），必须在 GT911 init 之后调用。
    esp_err_t sht4x_ret = fridge_sht4x_init();
    if (sht4x_ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT40 init failed: %s (check GPIO16/17 wiring, addr 0x44/0x45)",
                 esp_err_to_name(sht4x_ret));
    }

    // MPU6050 姿态传感器：复用同一 I2C0，模块使用 3.3V、AD0 接地（0x68），INT 首版悬空。
    // 驱动只添加 device handle；探测失败不会阻断启动，后台会按 1Hz 慢速恢复。
    esp_err_t sensors_ret = fridge_sensors_init();
    if (sensors_ret != ESP_OK) {
        ESP_LOGW(TAG, "MPU6050 monitor init failed: %s (check GT911 bus and GPIO16/17)",
                 esp_err_to_name(sensors_ret));
    }
#endif

    // 在 UI、TF 与 Agent Runtime 申请大量内存前，提前保留 JPEG 的内部 DMA 描述符。
    // 该操作不启动 OV5647、不申请 CSI 帧缓冲，也不访问与 ES8311 共用的 GPIO28/29。
    // 失败不能影响启动和实时取景；抓拍时仍会再次尝试并返回明确错误。
    esp_err_t jpeg_prewarm_ret = fridge_camera_prepare_jpeg_encoder();
    if (jpeg_prewarm_ret != ESP_OK) {
        ESP_LOGW(TAG, "early JPEG encoder prewarm failed: %s", esp_err_to_name(jpeg_prewarm_ret));
    }

    // 截图保存到 TF 时，SDMMC 仍需要内部 DMA 可寻址缓冲；PSRAM 只能承载整帧和文件源。
    // 在 UI/FreeType/WakeNet 分配前预留 2KiB DMA 暂存区，保持已验证的切页堆布局。
    esp_err_t screenshot_init_ret = fridge_screenshot_init();
    if (screenshot_init_ret != ESP_OK) {
        ESP_LOGW(TAG, "screenshot SDMMC DMA reserve failed: %s",
                 esp_err_to_name(screenshot_init_ret));
    }

    // 麦克风：ES8311 编解码器；Realtime 双工时才打开 PA_CTRL，空闲保持低电平防爆音。
    esp_err_t audio_ret = fridge_audio_init();
    if (audio_ret != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %s (check ES8311/shared I2C GPIO28-29)", esp_err_to_name(audio_ret));
    }

    // 尽早启动 LVGL UI：屏幕首帧优先于 Agent/Realtime 重型初始化，避免用户看到长时间黑屏。
#if CONFIG_FRIDGE_P4_UI_ENABLE
    if (disp_ret == ESP_OK) {
        esp_err_t ui_ret = fridge_ui_p4_init();
        if (ui_ret != ESP_OK) {
            ESP_LOGW(TAG, "P4 LVGL UI early start failed: %s", esp_err_to_name(ui_ret));
        } else {
            ESP_LOGI(TAG, "P4 LVGL UI task started early (before TF/agent/realtime)");
        }
    }
#endif

    // TF 与 C6 hosted 共用 LDO4 电源域。只有 network/hosted 成功持有并开启 LDO4 后才初始化 Slot0。
    // 否则继续访问 TF 会把电源域故障误报成无卡或文件系统损坏。
    if (net_ret == ESP_OK) {
        esp_err_t tf_ret = fridge_tfcard_init();
        if (tf_ret != ESP_OK) {
            ESP_LOGW(TAG, "tfcard init failed: %s", esp_err_to_name(tf_ret));
        } else {
            // TF 已挂载后启动低优先级日志任务，并优先处理上一次启动遗留的 Flash coredump。
            // coredump 只有 ELF 与摘要都原子写入受保护目录后才擦除，失败会留到下次继续重试。
            esp_err_t diag_log_start_ret = fridge_diagnostics_log_start();
            if (diag_log_start_ret != ESP_OK) {
                ESP_LOGW(TAG, "diagnostics log writer start failed: %s", esp_err_to_name(diag_log_start_ret));
            }
            fridge_diagnostic_snapshot_t diag_snapshot = {0};
            if (fridge_diagnostics_get_snapshot(&diag_snapshot) == ESP_OK) {
                esp_err_t coredump_ret = fridge_diagnostics_log_process_coredump(diag_snapshot.boot_sequence);
                if (coredump_ret != ESP_OK) {
                    ESP_LOGW(TAG, "coredump export pending: %s", esp_err_to_name(coredump_ret));
                }
            }
            // Agent 数据目录：只有 TF 卡成功挂载后才创建 /sdcard/agent，避免把无卡/电源域问题误写成业务失败。
            esp_err_t agent_store_ret = fridge_agent_storage_init();
            if (agent_store_ret != ESP_OK) {
                ESP_LOGW(TAG, "agent storage init failed: %s", esp_err_to_name(agent_store_ret));
            } else {
                // Trace Service 写入 /sdcard/agent/logs，必须在 Agent Storage 创建目录后初始化。
                esp_err_t trace_ret = fridge_trace_service_init();
                if (trace_ret != ESP_OK) {
                    ESP_LOGW(TAG, "trace service init failed: %s", esp_err_to_name(trace_ret));
                }
                // Tool Broker 依赖 Agent TF 数据目录，注册只读与业务写入工具；所有副作用都从这里统一过权限。
                esp_err_t tool_ret = fridge_tool_broker_init();
                if (tool_ret != ESP_OK) {
                    ESP_LOGW(TAG, "tool broker init failed: %s", esp_err_to_name(tool_ret));
                } else {
                    // UI Tool 由 ui_p4 自己注册。Broker 已完成 Registry 初始化后再调用，
                    // 保持 tool_system 不依赖 UI，避免 Agent Runtime 链路出现静态组件循环。
                    esp_err_t ui_tools_ret = fridge_ui_p4_register_tools();
                    if (ui_tools_ret != ESP_OK) {
                        ESP_LOGW(TAG, "P4 UI tool registration failed: %s", esp_err_to_name(ui_tools_ret));
                    }
                    // Skill 文档落在 TF 卡，便于后续不刷固件直接调整厨房场景提示和流程说明。
                    esp_err_t skill_ret = fridge_skill_manager_init();
                    if (skill_ret != ESP_OK) {
                        ESP_LOGW(TAG, "skill manager init failed: %s", esp_err_to_name(skill_ret));
                    }
                    // Workflow Engine 只执行确定性本地流程，副作用步骤仍回到 Tool Broker。
                    esp_err_t workflow_ret = fridge_workflow_engine_init();
                    if (workflow_ret != ESP_OK) {
                        ESP_LOGW(TAG, "workflow engine init failed: %s", esp_err_to_name(workflow_ret));
                    }
                    // 最小 Agent Runtime 依赖 Tool Broker，先启用本地确定性 Router → Tool 调用闭环。
                    esp_err_t agent_runtime_ret = fridge_agent_runtime_init();
                    if (agent_runtime_ret != ESP_OK) {
                        ESP_LOGW(TAG, "agent runtime init failed: %s", esp_err_to_name(agent_runtime_ret));
                    } else {
                        fridge_ui_p4_agent_actions_t agent_actions = {
                            .submit_text = p4_agent_ui_submit,
                            .cancel_active = p4_agent_ui_cancel,
                            .clear_session = p4_agent_ui_clear_session,
                            .resolve_confirmation = p4_agent_ui_resolve,
                            .poll_event = p4_agent_ui_poll,
                        };
                        esp_err_t agent_ui_ret = fridge_ui_p4_register_agent_actions(&agent_actions);
                        if (agent_ui_ret != ESP_OK) {
                            ESP_LOGW(TAG, "agent UI bridge registration failed: %s", esp_err_to_name(agent_ui_ret));
                        }
                    }
                    // GLM Realtime：依赖 network/audio/Tool Broker；失败不阻断 USB 调试控制台。
                    esp_err_t glm_rt_ret = fridge_glm_realtime_init();
                    if (glm_rt_ret != ESP_OK) {
                        ESP_LOGW(TAG, "glm realtime init failed: %s", esp_err_to_name(glm_rt_ret));
                    }
                }
            }
        }
    } else {
        fridge_tfcard_mark_unavailable("esp_hosted/LDO4 未就绪，已跳过 TF 初始化；请重启设备");
        ESP_LOGW(TAG, "TF card skipped because esp_hosted/LDO4 is not ready");
    }

    // 即使 Tool Broker/TF 未就绪，也尽量初始化 Realtime（文本+配置调试可用；工具在 start 时再检查）。
    // 若上面已 init，则此处幂等返回 ESP_OK。
    {
        esp_err_t glm_rt_ret = fridge_glm_realtime_init();
        if (glm_rt_ret != ESP_OK) {
            ESP_LOGW(TAG, "glm realtime late init failed: %s", esp_err_to_name(glm_rt_ret));
        }
    }

    // P4 云端连接必须在 NVS、网络和 Agent/Tool 基础设施之后启动。旧 S3 MQTT 数据模型
    // 不进入此路径；二维码绑定成功后只写入 P4 v2 配置并由 MQTT worker 执行同步。
    esp_err_t mqtt_ret = fridge_mqtt_protocol_init();
    if (mqtt_ret != ESP_OK) {
        ESP_LOGW(TAG, "P4 MQTT v2 init failed: %s", esp_err_to_name(mqtt_ret));
    } else {
        esp_err_t binding_ret =
            fridge_device_binding_init(p4_binding_status_changed, NULL);
        if (binding_ret != ESP_OK) {
            ESP_LOGW(TAG, "device binding worker init failed: %s",
                     esp_err_to_name(binding_ret));
        }
        // MQTT 启动路径包含 TLS/JSON 配置检查，任务栈保留 6 KiB 内部 SRAM，
        // 避免 4 KiB 边界在中断切换时触发 P4 栈保护；大消息/Outbox 仍放 PSRAM。
        if (xTaskCreate(p4_mqtt_autostart_task, "p4_mqtt_start", 6144,
                        NULL, 3, NULL) != pdPASS) {
            ESP_LOGW(TAG, "create P4 MQTT autostart task failed");
        }
    }

// UI 已在 audio 之后尽早启动；此处仅作幂等兜底（若早期路径因 disp 失败跳过）。
#if CONFIG_FRIDGE_P4_UI_ENABLE
    if (disp_ret == ESP_OK && !fridge_ui_p4_is_ready()) {
        esp_err_t ui_ret = fridge_ui_p4_init();
        if (ui_ret != ESP_OK) {
            ESP_LOGW(TAG, "P4 LVGL UI late start failed: %s", esp_err_to_name(ui_ret));
        }
    }
#endif

    // 初始化屏障：从此刻起串口发送 ready，并允许配网、录音和 TF 等硬件命令。
    p4_log_internal_dma_heap("startup_complete");
    if (usb_ret == ESP_OK) {
        fridge_usb_protocol_set_hardware_ready();
        ESP_LOGI(TAG, "P4 minimal bring-up sequence finished; USB hardware commands enabled");
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        p4_minimal_confirm_ota_image();
#endif
    } else {
        ESP_LOGI(TAG, "P4 minimal bring-up sequence finished; USB console unavailable");
    }
#else
    // 正常主控模式：初始化 NVS、诊断、Wi-Fi、AI 配置和 USB 配网控制台。
    // 注意：此路径会启动 Wi-Fi，真实硬件调试时需确认 5V/USB 供电足够稳定。
    nvs_init_safe();
    fridge_diagnostics_init();

    ESP_ERROR_CHECK(fridge_network_init());
    ESP_ERROR_CHECK(fridge_ai_client_init());
    ESP_ERROR_CHECK(fridge_mqtt_protocol_init());
    ESP_ERROR_CHECK(fridge_sensors_init());

#if CONFIG_FRIDGE_UI_ENABLE
    // 旧 S3 LVGL UI 路径仍可能存在于源码树，但 SPI/TR230S display 组件已移除。
    // 此分支无法再初始化物理屏幕，仅保留日志提示，避免误链已删除的 fridge_display API。
    ESP_LOGW(TAG, "legacy FRIDGE_UI_ENABLE path has no SPI display backend on P4; use CONFIG_FRIDGE_P4_MINIMAL + ui_p4");
#endif

    esp_err_t usb_ret = fridge_usb_protocol_start();
    if (usb_ret == ESP_OK) {
        ESP_LOGI(TAG, "USB provisioning console is ready");
    } else {
        ESP_LOGW(TAG, "USB provisioning console skipped: %s", esp_err_to_name(usb_ret));
    }

    // 厨房 AI 后台任务只做网络请求和 LittleFS 缓存写入，不控制 GPIO。
    // 它需要一小块连续内部 SRAM 作为 FreeRTOS 栈，因此放在音频/雷达/扬声器前初始化；
    // 实际 AI 请求和响应大缓冲仍由组件内部优先分配到 PSRAM，降低黑屏重启风险。
    esp_err_t kitchen_ai_ret = fridge_kitchen_ai_init();
    if (kitchen_ai_ret != ESP_OK) {
        ESP_LOGW(TAG, "kitchen AI skipped to keep demo firmware online: %s", esp_err_to_name(kitchen_ai_ret));
    }

    start_saved_network_and_mqtt_early();

    ESP_ERROR_CHECK(fridge_asr_init());
    ESP_ERROR_CHECK(fridge_radar_init());
    ESP_ERROR_CHECK(fridge_audio_init());
    ESP_ERROR_CHECK(fridge_state_machine_init());
    ESP_ERROR_CHECK(fridge_speaker_init());
    esp_err_t kitchen_ret = fridge_kitchen_tools_init();
    if (kitchen_ret != ESP_OK) {
        ESP_LOGW(TAG, "kitchen tools skipped to keep demo firmware online: %s", esp_err_to_name(kitchen_ret));
    }
    // 语音唤醒：默认不监听，也不在启动期分配 AFE/模型内存。
    // WakeNet 的初始化（含较大内部 SRAM 占用）推迟到用户首次打开“语音唤醒”开关、
    // 由策略层在亮屏空闲时惰性触发；NVS 默认 user_en=false，保证升级后启动内存基线不变。
    ESP_LOGI(TAG, "wake word available; lazily initialized on first enable, gated by screen-awake policy");
#endif
}
