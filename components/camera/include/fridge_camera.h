#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_CAMERA_MAX_ERROR_LEN 128
#define FRIDGE_CAMERA_DATA_URL_PREFIX "data:image/jpeg;base64,"

// 摄像头状态：P4 使用 OV5647 MIPI-CSI；拍照和音频通过共享 I2C gate 串行化。
typedef struct {
    bool initialized;
    bool has_frame;
    int width;
    int height;
    size_t jpeg_bytes;
    uint32_t capture_ms;
    uint32_t frame_id;
    uint32_t free_heap_kb;
    uint32_t free_psram_kb;
    char pixel_format[16];
    char frame_size[16];
    char last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
} fridge_camera_status_t;

// 最近一张 JPEG 的只读视图：调用者不得释放 data，需在下一次抓拍或 clear 前使用完。
typedef struct {
    const uint8_t *data;
    size_t len;
    int width;
    int height;
    uint32_t capture_ms;
    uint32_t frame_id;
} fridge_camera_frame_view_t;

// 高清预览嵌入 UI 时的像素区域和触控热区。摄像头接管 MIPI 双缓冲后，
// LVGL 不再写显存；因此这些区域由摄像头任务直接绘制和判定。
typedef struct {
    uint16_t view_x;
    uint16_t view_y;
    uint16_t view_width;
    uint16_t view_height;
    uint16_t capture_x;
    uint16_t capture_y;
    uint16_t capture_width;
    uint16_t capture_height;
    uint16_t cancel_x;
    uint16_t cancel_y;
    uint16_t cancel_width;
    uint16_t cancel_height;
    bool draw_guides;
} fridge_camera_preview_layout_t;

// 页面内高清预览结束时由摄像头任务上报的用户动作。
typedef enum {
    FRIDGE_CAMERA_PREVIEW_ACTION_NONE = 0,
    FRIDGE_CAMERA_PREVIEW_ACTION_CAPTURE,
    FRIDGE_CAMERA_PREVIEW_ACTION_CANCEL,
} fridge_camera_preview_action_t;

// UI 实时预览帧：RGB565 数据由调用者持有，使用后必须调用 fridge_camera_free_preview_frame() 释放。
// 注意：该帧只服务屏幕取景，不用于 AI 识别；AI 识别仍使用更清晰的 JPEG 抓拍路径。
typedef struct {
    uint8_t *rgb565;
    size_t len;
    int width;
    int height;
    uint32_t capture_ms;
    uint32_t frame_id;
} fridge_camera_preview_frame_t;

// RGB565 诊断结果：只用于判断 DVP 同步和并口采样是否能形成完整帧，不返回大图。
typedef struct {
    bool ok;
    int width;
    int height;
    size_t bytes;
    uint32_t capture_ms;
    uint32_t checksum;
    uint8_t first_bytes[16];
    size_t first_len;
    char last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
} fridge_camera_diag_result_t;

// 摄像头安全探测结果：只使用 XCLK + SCCB 读取芯片 ID，不启动 DVP/DMA 抓图。
typedef struct {
    bool ok;
    bool xclk_enabled;
    bool sccb_ready;
    uint8_t sccb_address;
    uint8_t pid_high;
    uint8_t pid_low;
    uint16_t pid;
    uint16_t expected_pid;
    uint32_t duration_ms;
    char last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
} fridge_camera_probe_result_t;

// 初始化板载 OV5647。CSI 走专用 D-PHY，SCCB 与 ES8311 共用 GPIO28/29，禁止并发控制。
esp_err_t fridge_camera_init(void);

// 提前保留硬件 JPEG 编码器所需的内部 DMA 描述符。
// 该接口不访问 OV5647、CSI 或 GPIO28/29 共享 I2C，可在启动早期安全调用；
// 目的仅是避免 UI/Agent 等组件导致内部 DMA 堆碎片化后，抓拍无法初始化编码器。
esp_err_t fridge_camera_prepare_jpeg_encoder(void);

// 摄像头安全探测接口；P4 当前请使用初始化日志确认 OV5647 PID。
esp_err_t fridge_camera_probe(fridge_camera_probe_result_t *out);

// 按实测亮度等待自动曝光收敛后，抓拍官方最高 1920x1080 RGB565，
// 并用 P4 硬件 JPEG 编码后保存到 PSRAM。
esp_err_t fridge_camera_capture(void);

// 抓取一张低分辨率 RGB565 预览帧，用于 LVGL 登记页实时取景。
esp_err_t fridge_camera_capture_preview_rgb565(fridge_camera_preview_frame_t *out);

// 释放 UI 预览帧数据。
void fridge_camera_free_preview_frame(fridge_camera_preview_frame_t *frame);

// 以 Espressif 官方 1920x1080 RAW10 模式抓拍并硬件编码 JPEG，供 AI 图片识别使用。
esp_err_t fridge_camera_capture_ai_fullres(void);

// 执行一次与 AI 主路径相同的 P4 硬件 JPEG 抓拍诊断。
esp_err_t fridge_camera_capture_hardware_jpeg_diag(void);

// 抓取一帧 RGB565 诊断帧，只返回统计信息，用于区分 JPEG 编码问题和 DVP 总线问题。
esp_err_t fridge_camera_capture_rgb565_diag(fridge_camera_diag_result_t *out);

// 获取最近帧状态。
void fridge_camera_get_status(fridge_camera_status_t *out);

// 获取最近 JPEG 帧只读视图。
esp_err_t fridge_camera_get_frame(fridge_camera_frame_view_t *out);

// 取走最近一次高清抓拍同步生成的屏幕回看图（RGB565）。
// 成功后所有权转移给调用者，使用完必须调用 fridge_camera_free_preview_frame()；
// JPEG 仍由 camera 组件持有，可继续用于后续上传识别。
esp_err_t fridge_camera_take_captured_preview(fridge_camera_preview_frame_t *out);

// 清除最近帧，释放 PSRAM/RAM；不会反初始化摄像头硬件。
esp_err_t fridge_camera_clear_frame(void);

// 反初始化摄像头驱动并释放帧缓存；用于切换 JPEG/RGB565 诊断模式或清理 LEDC/I2C 资源。
esp_err_t fridge_camera_reset(void);

// ---- P4 高清预览（1280x720@30 → PPA 1024x576 → MIPI-DSI）----
// 仅 ESP32-P4 实现；S3 路径返回 ESP_ERR_NOT_SUPPORTED。
// 预览期间占用 ES8311 共享 I2C gate（CAMERA owner），并独占 MIPI 帧缓冲；
// 音频录音/双工进行中时 start 会失败，禁止强行抢总线。
// 退出：调用 stop，或由预览任务检测触摸/KEY0 后自行停止。
esp_err_t fridge_camera_start_hd_preview(void);
// 按 UI 提供的区域启动高清预览。预览仍独占 MIPI 双缓冲，但只改写取景区，
// 其余页面像素保留；capture/cancel 热区在真实抬手后直接由摄像头任务处理。
esp_err_t fridge_camera_start_hd_preview_with_layout(const fridge_camera_preview_layout_t *layout);
// 非阻塞请求预览退出，适合 UI 页面切换；资源由预览任务自身安全释放。
esp_err_t fridge_camera_request_hd_preview_stop(void);
esp_err_t fridge_camera_stop_hd_preview(void);
bool fridge_camera_hd_preview_running(void);
// 读取并清除最近一次页面预览动作；仅 UI 在确认预览已经停止后调用。
fridge_camera_preview_action_t fridge_camera_take_hd_preview_action(void);

// 摄像头是否正在占用硬件 JPEG 编码器（拍照或高清预览进行中）。
// 硬件 JPEG 编码器是单例资源：截屏组件据此决定用 JPEG 编码，
// 还是在摄像头忙时降级为 BMP，避免与摄像头抢占同一编码引擎。
bool fridge_camera_is_busy(void);

#ifdef __cplusplus
}
#endif
