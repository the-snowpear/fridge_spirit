// 冰箱小精灵摄像头组件 —— ESP32-P4 桩实现。
// 背景：P4 板（慧勤智远 ESP32-P4 CB V1.2）只有 MIPI-CSI 摄像头接口，原 OV3660/DVP 方案
// 与 esp32-camera 组件都不适用（esp32-camera 不支持 esp32p4 目标）。
// 本文件提供与 fridge_camera.h 完全一致的 API 桩：所有操作返回 ESP_ERR_NOT_SUPPORTED，
// 让 usb_protocol / ui 等调用方无需修改即可在 P4 上编译运行。
// 后续 OV5647 + esp_video (MIPI-CSI) 方案落地时替换本实现；届时必须与 ES8311 分时复用
// GPIO28/29 共享 I2C（见《用户手册》§2.10），通过 codec 组件导出的总线锁串行化访问。

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_camera.h"

static const char *TAG = "fridge_camera";

// 统一填写"未移植"错误信息，便于 Web 面板和串口调试时一眼看出原因。
#define P4_STUB_ERROR_MSG "camera not ported to ESP32-P4 yet (OV5647/MIPI-CSI planned)"

static void stub_warn(const char *func)
{
    ESP_LOGW(TAG, "%s: %s", func, P4_STUB_ERROR_MSG);
}

esp_err_t fridge_camera_init(void)
{
    stub_warn(__func__);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_camera_probe(fridge_camera_probe_result_t *out)
{
    stub_warn(__func__);
    if (out) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->last_error, P4_STUB_ERROR_MSG, sizeof(out->last_error));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_camera_capture(void)
{
    stub_warn(__func__);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_camera_capture_preview_rgb565(fridge_camera_preview_frame_t *out)
{
    stub_warn(__func__);
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void fridge_camera_free_preview_frame(fridge_camera_preview_frame_t *frame)
{
    // 桩实现不会分配预览帧内存，这里只做指针清理，保证调用方释放路径安全。
    if (frame) {
        memset(frame, 0, sizeof(*frame));
    }
}

esp_err_t fridge_camera_capture_ai_fullres(void)
{
    stub_warn(__func__);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_camera_capture_hardware_jpeg_diag(void)
{
    stub_warn(__func__);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_camera_capture_rgb565_diag(fridge_camera_diag_result_t *out)
{
    stub_warn(__func__);
    if (out) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->last_error, P4_STUB_ERROR_MSG, sizeof(out->last_error));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void fridge_camera_get_status(fridge_camera_status_t *out)
{
    // 状态查询保持可用：返回"未初始化 + 未移植"而不是错误，避免 Web 面板状态页刷错误弹窗。
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = false;
    strlcpy(out->last_error, P4_STUB_ERROR_MSG, sizeof(out->last_error));
}

esp_err_t fridge_camera_get_frame(fridge_camera_frame_view_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_camera_take_captured_preview(fridge_camera_preview_frame_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fridge_camera_clear_frame(void)
{
    return ESP_OK;
}

esp_err_t fridge_camera_reset(void)
{
    return ESP_OK;
}
