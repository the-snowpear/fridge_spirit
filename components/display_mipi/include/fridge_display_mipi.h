// 冰箱小精灵 MIPI-DSI 屏幕（WKS70WSV114-WCT / EK79007AD3）驱动公共接口。
// 面板：7.0 寸 1024x600、2 lane MIPI-DSI、驱动 IC EK79007、RGB565、只能横屏。
// 硬件注意：
//   - 屏幕从板上 26-pin DSI 座取电，DSI 数据/时钟走专用 D-PHY 通道，不占用普通 GPIO；
//     仅复位 RST=GPIO22、背光 BL=GPIO23 是普通 GPIO（与旧 QSPI 屏共用同一板载网络）。
//   - DSI PHY 供电由片内 LDO_VO3（通道 3）输出 2.5V，与 PSRAM(chan2)/TF+C6(chan4) 不冲突；
//     板上 DSI 复位网络已被分压到 1.8V，驱动只负责推挽复位时序，不要外接改压。
//   - 双帧缓冲各约 1024*600*2=1.2MB，分配在 32MB 片内 PSRAM，不挤占内部 DMA SRAM。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// EK79007 面板固定分辨率（横屏），供测试图案与后续 LVGL 复用。
#define FRIDGE_MIPI_WIDTH  1024
#define FRIDGE_MIPI_HEIGHT 600

// 初始化 DSI PHY 电源、DSI 总线、EK79007 面板与 DPI 数据通道，并点亮背光。
// 成功后内部 DPI 面板已完成 init，可直接调用整屏刷新。
esp_err_t fridge_display_mipi_init(void);

// 用单一 RGB565 颜色填充整屏（阻塞直到本帧刷新完成）。
// 用于单色全屏点亮测试；color 为主机端常见 little-endian RGB565。
esp_err_t fridge_display_mipi_fill_screen(uint16_t rgb565);

// 帧缓冲绘制回调：在 fb（行优先 RGB565，尺寸 width*height）上绘制任意整屏图案。
typedef void (*fridge_mipi_paint_cb_t)(uint16_t *fb, uint16_t width, uint16_t height, void *ctx);

// 用回调在“当前可写帧缓冲”上绘制整屏图案，然后整帧提交并翻页（阻塞直到刷新完成）。
// 采用零拷贝双缓冲：回调只填当前离屏缓冲，避免撕裂；供彩条等复杂图案使用。
// fill/render 与 LVGL/摄像头共用驱动互斥；刷新超时后需重启设备再初始化。
// 注意：本版只做整屏刷新，局部/LVGL 增量刷新待点亮验证后再单独实现。
esp_err_t fridge_display_mipi_render(fridge_mipi_paint_cb_t paint, void *ctx);

// 获取 MIPI DPI 驱动已经分配的双帧缓冲。
// LVGL P4 UI 直接复用这两块 PSRAM 帧缓冲，避免再申请两块 1024x600 RGB565 缓冲。
esp_err_t fridge_display_mipi_get_frame_buffers(void **fb0, void **fb1);

// UI/PPA 路径获取“当前安全可写缓冲”和另一块缓冲，顺序由 DPI 翻页状态决定。
// 调用者必须已经持有 begin_ui_render() 的渲染互斥；摄像头接管期间返回失败。
esp_err_t fridge_display_mipi_get_ui_frame_buffers(void **write_fb, void **other_fb);

// LVGL 整轮渲染互斥。begin 成功后必须调用 end；摄像头接管期间 begin 会失败，
// 从渲染开始就阻止 LVGL 写双缓冲，而不是等到 flush 阶段才丢弃提交。
esp_err_t fridge_display_mipi_begin_ui_render(void);
void fridge_display_mipi_end_ui_render(void);

// 提交 LVGL 渲染完成的整帧缓冲并等待 DPI 刷新边界。
// frame_buffer 必须是 fridge_display_mipi_get_frame_buffers() 返回的其中一块。
// 当摄像头预览持有帧缓冲所有权时，LVGL 调用会返回 ESP_ERR_INVALID_STATE，避免双写撕裂。
esp_err_t fridge_display_mipi_present_frame(void *frame_buffer);

// 摄像头预览专用整帧提交接口。调用前必须先 acquire，避免普通 LVGL 路径绕过所有权检查。
esp_err_t fridge_display_mipi_present_camera_frame(void *frame_buffer);

// 摄像头接管后获取“当前安全可写缓冲”和另一块缓冲，顺序由驱动的翻页状态决定。
esp_err_t fridge_display_mipi_get_camera_frame_buffers(void **write_fb, void **other_fb);

// 摄像头预览申请/释放 DPI 帧缓冲所有权。
// 持有期间只允许摄像头路径提交整帧；LVGL flush 应暂停。
// 预览结束后必须 release，并触发一次 LVGL 全屏重绘。
esp_err_t fridge_display_mipi_acquire_for_camera(void);
esp_err_t fridge_display_mipi_release_from_camera(void);
bool fridge_display_mipi_camera_owns_framebuffers(void);

// 打开/关闭背光（BL=GPIO23，高有效）。
// 关闭时只将 PWM 占空比降为 0，不会丢失此前设置的亮度百分比；重新打开后恢复该亮度。
void fridge_display_mipi_set_backlight(bool on);

// 设置/读取背光亮度百分比。有效范围固定为 10%～100%，调用方负责将用户偏好持久化到 NVS。
// 该接口只操作 GPIO23 的 20kHz LEDC PWM，不访问 Flash；显示未初始化时设置会返回 ESP_ERR_INVALID_STATE。
esp_err_t fridge_display_mipi_set_brightness(uint8_t percent);
uint8_t fridge_display_mipi_get_brightness(void);

// 返回当前背光输出是否打开。亮度偏好仍可保留为非零值，但屏幕待机时该状态为 false，
// 供 WakeNet 策略决定是否暂停监听，避免黑屏时继续占用板载音频资源。
bool fridge_display_mipi_is_backlight_enabled(void);

// 返回 MIPI-DSI 面板是否已初始化完成。
bool fridge_display_mipi_is_ready(void);

#ifdef __cplusplus
}
#endif
