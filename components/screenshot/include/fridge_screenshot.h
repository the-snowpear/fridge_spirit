// 冰箱小精灵截屏公共接口。
// 截取当前 LVGL 显示帧（RGB565 双缓冲中正在被 DPI 扫描的那块）并保存到 TF 卡。
// 摄像头空闲时用 P4 片内硬件 JPEG 编码（~100-300 KB）；
// 摄像头忙（占用硬件编码器）时降级为 BMP 16-bit BI_BITFIELDS（~1.2 MB，零依赖）。
// 文件写入 FRIDGE_TFCARD_EXPORT_DIR，文件名 shot_<unix_sec>.jpg / .bmp。

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动早期预留 2KiB 内部 DMA 暂存区，截图写 TF 时用于承接 PSRAM 数据。
// 必须在 UI/FreeType/WakeNet 大量分配内部堆之前调用；可重复调用。
esp_err_t fridge_screenshot_init(void);

// 截取当前显示帧并保存到 TF 卡。
// out_path：返回相对于 /sdcard 的路径，如 .fridge_spirit/exports/shot_1234567890.jpg
// out_format：返回 "jpeg" 或 "bmp"（可传 NULL 忽略）
esp_err_t fridge_screenshot_capture(char *out_path, size_t out_path_size,
                                    char *out_format, size_t out_format_size);

#ifdef __cplusplus
}
#endif
