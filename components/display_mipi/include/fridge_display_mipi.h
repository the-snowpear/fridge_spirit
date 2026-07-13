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
// 驱动采用单任务所有权，fill/render 不允许跨任务并发调用；刷新超时后需重启设备再初始化。
// 注意：本版只做整屏刷新，局部/LVGL 增量刷新待点亮验证后再单独实现。
esp_err_t fridge_display_mipi_render(fridge_mipi_paint_cb_t paint, void *ctx);

// 打开/关闭背光（BL=GPIO23，高有效）。
void fridge_display_mipi_set_backlight(bool on);

// 返回 MIPI-DSI 面板是否已初始化完成。
bool fridge_display_mipi_is_ready(void);

#ifdef __cplusplus
}
#endif
