// 冰箱小精灵截屏实现。
// 流程：持 UI 渲染互斥 → 取"正在显示"的帧缓冲 → 拷贝到 PSRAM 暂存并尽快放锁
//       → 摄像头空闲则硬件 JPEG，忙则 BMP16 降级 → 原子写入 TF 卡 exports 目录。
// 硬件约束：
//   - 屏幕帧缓冲为 1024x600 RGB565，位于 32MB 片内 PSRAM，本文件只读不写显存。
//   - 硬件 JPEG 编码器是单例；摄像头拍照/预览期间被占用，此时必须走 BMP 降级路径。
//   - 截屏读的是显存，不触碰摄像头 SCCB，因此无需 GPIO28/29 共享 I2C gate。

#include "fridge_screenshot.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fridge_display_mipi.h"
#include "fridge_tfcard.h"
#include "fridge_camera.h"
#include "fridge_ui_p4.h"

static const char *TAG = "screenshot";

// 面板固定分辨率（横屏 RGB565），与 display_mipi 帧缓冲一致。
#define SHOT_W FRIDGE_MIPI_WIDTH   // 1024
#define SHOT_H FRIDGE_MIPI_HEIGHT  // 600
#define SHOT_RGB565_BYTES ((size_t)SHOT_W * SHOT_H * 2)

// 硬件 JPEG 质量与子采样。
//   - 高度 600 % 16 != 0，不能用 YUV420（要求 16 对齐）；
//   - 600 % 8 == 0，改用 YUV422（要求 8 对齐），无需补行。
#define SHOT_JPEG_QUALITY 85
// IDF 5.5.1 的 JPEG 驱动需要连续内部 DMA 堆；内部 DMA 碎片化时其
// jpeg_new_encoder_engine() 异常路径可能仍进入 jpeg_encoder_process() 并触发崩溃。
// 低于该门槛直接走 BMP，保证截屏链路可用且不抢占音频/唤醒缓冲。
#define SHOT_JPEG_MIN_INTERNAL_DMA (48U * 1024U)
// FATFS 的文件内缓存位于 PSRAM，地址不一定满足 SDMMC cache-line 对齐要求。
// 启动早期保留 2KiB（4 个扇区）的内部 DMA 暂存区；该大小已通过切页与截图实机验证，
// 不能为了减少占用而缩小，否则会改变 FreeType/SDMMC 的启动堆布局并诱发切页看门狗。
#define SHOT_SDMMC_DMA_STAGE_BYTES 2048U

// exports 目录挂载后的绝对前缀。
#define SHOT_EXPORT_ABS_PREFIX FRIDGE_TFCARD_MOUNT_POINT "/" FRIDGE_TFCARD_EXPORT_DIR

static uint8_t *s_sdmmc_dma_stage;
static SemaphoreHandle_t s_capture_lock;

esp_err_t fridge_screenshot_init(void)
{
    if (!s_capture_lock) {
        s_capture_lock = xSemaphoreCreateMutex();
        if (!s_capture_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_sdmmc_dma_stage) {
        s_sdmmc_dma_stage = heap_caps_malloc(
            SHOT_SDMMC_DMA_STAGE_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_sdmmc_dma_stage) {
            ESP_LOGE(TAG, "reserve SDMMC DMA stage failed (%u bytes)",
                     (unsigned)SHOT_SDMMC_DMA_STAGE_BYTES);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "reserved %u-byte internal DMA stage for screenshot TF writes",
                 (unsigned)SHOT_SDMMC_DMA_STAGE_BYTES);
    }
    return ESP_OK;
}

// 所有截图文件数据都先复制到启动早期预留的内部 DMA 暂存区；源可以位于 PSRAM。
static esp_err_t write_dma_staged(FILE *file, const void *data, size_t length)
{
    if (!file || (!data && length != 0) || !s_sdmmc_dma_stage) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *source = (const uint8_t *)data;
    while (length > 0) {
        const size_t chunk = length > SHOT_SDMMC_DMA_STAGE_BYTES ?
                                 SHOT_SDMMC_DMA_STAGE_BYTES : length;
        memcpy(s_sdmmc_dma_stage, source, chunk);
        if (fwrite(s_sdmmc_dma_stage, 1, chunk, file) != chunk) {
            return ESP_FAIL;
        }
        source += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

// 将 RGB565 暂存缓冲编码为 JPEG 并原子写入 TF 卡。
// 返回 ESP_OK 时 rel_path/format 已填好。
static esp_err_t encode_and_write_jpeg(const uint8_t *rgb565, uint32_t unix_sec,
                                       char *rel_path, size_t rel_path_size,
                                       char *format, size_t format_size);

// 将 RGB565 暂存缓冲写为 16-bit BI_BITFIELDS BMP 并原子写入 TF 卡。
static esp_err_t encode_and_write_bmp16(const uint8_t *rgb565, uint32_t unix_sec,
                                        char *rel_path, size_t rel_path_size,
                                        char *format, size_t format_size);

esp_err_t fridge_screenshot_capture(char *out_path, size_t out_path_size,
                                    char *out_format, size_t out_format_size)
{
    if (!out_path || out_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_path[0] = '\0';
    if (out_format && out_format_size) {
        out_format[0] = '\0';
    }

    // 1) 前置检查：屏幕就绪 + TF 卡已挂载。
    if (!fridge_display_mipi_is_ready()) {
        ESP_LOGW(TAG, "display not ready");
        return ESP_ERR_INVALID_STATE;
    }
    fridge_ui_p4_status_t ui = {0};
    if (fridge_ui_p4_get_status(&ui) != ESP_OK || !ui.ready || !ui.resource_ready ||
        !ui.framebuffers_synced || !ui.page_frame_ready) {
        ESP_LOGW(TAG, "UI frame not stable: ready=%d resource=%d synced=%d page_frame=%d page=%s",
                 ui.ready, ui.resource_ready, ui.framebuffers_synced, ui.page_frame_ready,
                 fridge_ui_p4_page_key(ui.page));
        return ESP_ERR_INVALID_STATE;
    }
    fridge_tfcard_status_t tf = {0};
    fridge_tfcard_get_status(&tf);
    if (!tf.mounted) {
        ESP_LOGW(TAG, "TF card not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(fridge_screenshot_init(), TAG, "screenshot DMA stage unavailable");

    // 2) 申请 PSRAM 暂存缓冲（先分配，失败则不进临界区）。
    uint8_t *snap = heap_caps_malloc(SHOT_RGB565_BYTES, MALLOC_CAP_SPIRAM);
    if (!snap) {
        ESP_LOGE(TAG, "snapshot buffer alloc failed (%u bytes)", (unsigned)SHOT_RGB565_BYTES);
        return ESP_ERR_NO_MEM;
    }

    // 3) 持渲染互斥并拷贝"正在显示"的帧，尽快放锁避免阻塞 LVGL。
    esp_err_t err = fridge_display_mipi_begin_ui_render();
    if (err != ESP_OK) {
        // 摄像头接管帧缓冲期间 begin 会失败，此时无法可靠截屏。
        ESP_LOGW(TAG, "begin_ui_render failed: %s", esp_err_to_name(err));
        free(snap);
        return err;
    }
    void *write_fb = NULL;
    void *other_fb = NULL;
    err = fridge_display_mipi_get_ui_frame_buffers(&write_fb, &other_fb);
    if (err == ESP_OK && other_fb) {
        // other_fb 是当前正在被 DPI 扫描（即屏幕上可见）的那块缓冲。
        memcpy(snap, other_fb, SHOT_RGB565_BYTES);
    } else {
        err = (err == ESP_OK) ? ESP_ERR_INVALID_STATE : err;
    }
    fridge_display_mipi_end_ui_render();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_ui_frame_buffers failed: %s", esp_err_to_name(err));
        free(snap);
        return err;
    }

    // 4) 生成时间戳文件名基。esp_timer 单调计数不代表真实时间，
    //    这里用启动后微秒数换算的秒值即可保证同一次运行内文件名唯一。
    uint32_t stamp = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    // 5) 摄像头忙则 BMP 降级，空闲则硬件 JPEG。
    const size_t largest_internal_dma =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (xSemaphoreTake(s_capture_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free(snap);
        return ESP_ERR_TIMEOUT;
    }
    if (fridge_camera_is_busy() || largest_internal_dma < SHOT_JPEG_MIN_INTERNAL_DMA) {
        if (!fridge_camera_is_busy()) {
            ESP_LOGW(TAG, "internal DMA largest=%u < %u; saving BMP fallback",
                     (unsigned)largest_internal_dma, (unsigned)SHOT_JPEG_MIN_INTERNAL_DMA);
        } else {
            ESP_LOGI(TAG, "camera busy; saving BMP fallback");
        }
        err = encode_and_write_bmp16(snap, stamp, out_path, out_path_size,
                                     out_format, out_format_size);
    } else {
        err = encode_and_write_jpeg(snap, stamp, out_path, out_path_size,
                                    out_format, out_format_size);
        if (err != ESP_OK) {
            // 硬件编码器意外失败时回退 BMP，保证截屏不空手而归。
            ESP_LOGW(TAG, "JPEG path failed (%s), fallback to BMP", esp_err_to_name(err));
            err = encode_and_write_bmp16(snap, stamp, out_path, out_path_size,
                                         out_format, out_format_size);
        }
    }

    xSemaphoreGive(s_capture_lock);
    free(snap);
    return err;
}

// 原子写文件：先写 .part，再 rename 到目标；成功后填相对路径。
static esp_err_t atomic_write(const char *abs_final, const char *abs_tmp,
                              const uint8_t *data1, size_t len1,
                              const uint8_t *data2, size_t len2)
{
    FILE *f = fopen(abs_tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed", abs_tmp);
        return ESP_FAIL;
    }
    (void)setvbuf(f, NULL, _IONBF, 0);
    esp_err_t err = ESP_OK;
    if (data1 && len1) err = write_dma_staged(f, data1, len1);
    if (err == ESP_OK && data2 && len2) err = write_dma_staged(f, data2, len2);
    if (err == ESP_OK && fflush(f) != 0) err = ESP_FAIL;
    if (fclose(f) != 0) err = ESP_FAIL;
    if (err != ESP_OK) {
        remove(abs_tmp);
        return err;
    }
    remove(abs_final);  // FAT rename 不覆盖，先删旧目标（同名不太可能，稳妥起见）
    if (rename(abs_tmp, abs_final) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", abs_tmp, abs_final);
        remove(abs_tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t encode_and_write_jpeg(const uint8_t *rgb565, uint32_t unix_sec,
                                       char *rel_path, size_t rel_path_size,
                                       char *format, size_t format_size)
{
    jpeg_encoder_handle_t encoder = NULL;
    uint8_t *jpg = NULL;
    size_t jpg_cap = 0;
    uint32_t jpg_len = 0;
    esp_err_t err;

    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 2000,
    };
    err = jpeg_new_encoder_engine(&engine_cfg, &encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        return err;
    }

    // 最坏情况给到与 RGB565 原图相同容量，避免高纹理画面溢出。
    jpeg_encode_memory_alloc_cfg_t alloc_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    jpg = jpeg_alloc_encoder_mem(SHOT_RGB565_BYTES, &alloc_cfg, &jpg_cap);
    if (!jpg) {
        ESP_LOGE(TAG, "jpeg output alloc failed");
        jpeg_del_encoder_engine(encoder);
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_cfg_t encode_cfg = {
        .height = SHOT_H,
        .width = SHOT_W,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,  // 高度 600 8 对齐，不能用 420
        .image_quality = SHOT_JPEG_QUALITY,
    };
    err = jpeg_encoder_process(encoder, &encode_cfg, rgb565, SHOT_RGB565_BYTES,
                               jpg, jpg_cap, &jpg_len);
    jpeg_del_encoder_engine(encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_encoder_process failed: %s", esp_err_to_name(err));
        free(jpg);
        return err;
    }

    char abs_final[sizeof(SHOT_EXPORT_ABS_PREFIX) + 40];
    char abs_tmp[sizeof(SHOT_EXPORT_ABS_PREFIX) + 48];
    snprintf(abs_final, sizeof(abs_final), "%s/shot_%lu.jpg",
             SHOT_EXPORT_ABS_PREFIX, (unsigned long)unix_sec);
    snprintf(abs_tmp, sizeof(abs_tmp), "%s/shot_%lu.jpg.part",
             SHOT_EXPORT_ABS_PREFIX, (unsigned long)unix_sec);

    err = atomic_write(abs_final, abs_tmp, jpg, jpg_len, NULL, 0);
    free(jpg);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(rel_path, rel_path_size, "%s/shot_%lu.jpg",
             FRIDGE_TFCARD_EXPORT_DIR, (unsigned long)unix_sec);
    if (format && format_size) {
        strlcpy(format, "jpeg", format_size);
    }
    ESP_LOGI(TAG, "screenshot JPEG saved: %s (%u bytes)", rel_path, (unsigned)jpg_len);
    return ESP_OK;
}

// 小端写入辅助。
static inline void put_u16le(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static esp_err_t encode_and_write_bmp16(const uint8_t *rgb565, uint32_t unix_sec,
                                        char *rel_path, size_t rel_path_size,
                                        char *format, size_t format_size)
{
    // BMP 有效头为 66B，但像素偏移补齐到 DMA 暂存区大小（必为 512B 整数倍）。
    // 这样 1024x600 RGB565 的每一行（2048B）也保持扇区对齐，SDMMC 可直接使用
    // 启动时预留的 DMA 缓冲，不再依赖已经碎片化的内部堆临时分配 bounce buffer。
    const uint32_t pixel_offset = SHOT_SDMMC_DMA_STAGE_BYTES;
    const uint32_t pixel_bytes = (uint32_t)SHOT_RGB565_BYTES;  // 每行已 2 字节对齐（1024*2）
    uint8_t hdr[SHOT_SDMMC_DMA_STAGE_BYTES] = {0};

    // -- BITMAPFILEHEADER --
    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32le(&hdr[2], pixel_offset + pixel_bytes);  // bfSize
    put_u32le(&hdr[10], pixel_offset);               // bfOffBits
    // -- BITMAPINFOHEADER --
    put_u32le(&hdr[14], 40);                         // biSize
    put_u32le(&hdr[18], SHOT_W);                     // biWidth
    put_u32le(&hdr[22], SHOT_H);                     // biHeight（正=自下而上）
    put_u16le(&hdr[26], 1);                          // biPlanes
    put_u16le(&hdr[28], 16);                         // biBitCount
    put_u32le(&hdr[30], 3);                          // biCompression = BI_BITFIELDS
    put_u32le(&hdr[34], pixel_bytes);                // biSizeImage
    // -- 颜色掩码（RGB565）--
    put_u32le(&hdr[54], 0xF800);                     // R
    put_u32le(&hdr[58], 0x07E0);                     // G
    put_u32le(&hdr[62], 0x001F);                     // B

    char abs_final[sizeof(SHOT_EXPORT_ABS_PREFIX) + 40];
    char abs_tmp[sizeof(SHOT_EXPORT_ABS_PREFIX) + 48];
    snprintf(abs_final, sizeof(abs_final), "%s/shot_%lu.bmp",
             SHOT_EXPORT_ABS_PREFIX, (unsigned long)unix_sec);
    snprintf(abs_tmp, sizeof(abs_tmp), "%s/shot_%lu.bmp.part",
             SHOT_EXPORT_ABS_PREFIX, (unsigned long)unix_sec);

    // BMP 像素自下而上存储：逐行倒序写入，源顶行写到文件末尾。
    FILE *f = fopen(abs_tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed", abs_tmp);
        return ESP_FAIL;
    }
    (void)setvbuf(f, NULL, _IONBF, 0);
    esp_err_t err = ESP_OK;
    err = write_dma_staged(f, hdr, sizeof(hdr));
    const size_t row_bytes = (size_t)SHOT_W * 2;
    for (int y = SHOT_H - 1; err == ESP_OK && y >= 0; y--) {
        const uint8_t *row = rgb565 + (size_t)y * row_bytes;
        err = write_dma_staged(f, row, row_bytes);
    }
    if (err == ESP_OK && fflush(f) != 0) err = ESP_FAIL;
    if (fclose(f) != 0) err = ESP_FAIL;
    if (err != ESP_OK) {
        remove(abs_tmp);
        ESP_LOGE(TAG, "BMP write failed");
        return err;
    }
    remove(abs_final);
    if (rename(abs_tmp, abs_final) != 0) {
        remove(abs_tmp);
        ESP_LOGE(TAG, "BMP rename failed");
        return ESP_FAIL;
    }

    snprintf(rel_path, rel_path_size, "%s/shot_%lu.bmp",
             FRIDGE_TFCARD_EXPORT_DIR, (unsigned long)unix_sec);
    if (format && format_size) {
        strlcpy(format, "bmp", format_size);
    }
    ESP_LOGI(TAG, "screenshot BMP saved: %s (%u bytes)",
             rel_path, (unsigned)(pixel_offset + pixel_bytes));
    return ESP_OK;
}
