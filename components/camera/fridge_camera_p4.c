// 冰箱小精灵摄像头组件 —— ESP32-P4 OV5647 MIPI-CSI 实现（高清预览与 AI 抓拍）。
// 参考：厂商 basic__routines/17_mipicamera（example/17_mipicamera_reference）与
//       espressif/esp_video 0.5.1 + esp_cam_sensor 0.5.3。
//
// 视频路径：
//   实时预览：OV5647 RAW10 1280x960 → ISP RGB565 → 居中裁剪 1280x720
//             → 固定比例缩放 1024x576 → MIPI DPI 双缓冲（30fps）
//   AI 抓拍： OV5647 RAW10 1920x1080 → ISP RGB565 → P4 硬件 JPEG
//             使用 Espressif 官方最高分辨率模式，并按 RGB565 实测亮度等待 AE/AGC 收敛。
//
// 硬件约束：
//   - CSI 数据/时钟走专用 D-PHY；RST=GPIO26，PWDN=GPIO27
//   - SCCB 必须复用 ES8311 的 I2C1（GPIO28/29），init_sccb=false + 共享 bus handle
//   - 预览期间持有 FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA，禁止与录音并发
//   - 预览期间独占 MIPI 帧缓冲，LVGL 必须暂停 present
//
#include "fridge_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_sensor_types.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fridge_audio.h"
#include "fridge_board_pins.h"
#include "fridge_codec_es8311.h"
#include "fridge_display_mipi.h"
#include "fridge_glm_realtime.h"
#include "fridge_touch_gt911.h"
#include "fridge_wake_word.h"
#include "linux/videodev2.h"
#include "ov5647_hd_modes.h"
#include "sdkconfig.h"

static const char *TAG = "fridge_camera";

#define CAM_SCCB_FREQ_HZ          100000
#define CAM_VIDEO_PORT            0
#define CAM_BUFFER_COUNT          2
// 预览任务在非 ISR 上下文运行，栈明确放入 32MB PSRAM，避免挤占 CSI/JPEG 必需的内部 DMA 堆。
#define CAM_PREVIEW_TASK_STACK    8192
#define CAM_PREVIEW_TASK_PRIO     5
#define CAM_I2C_GATE_TIMEOUT_MS   3000
#define CAM_AUDIO_RELEASE_TIMEOUT_MS 5000
#define CAM_DQBUF_MAX_TIMEOUTS    50
#define CAM_PREVIEW_TARGET_FPS    30
#define CAM_PHOTO_MIN_FRAMES      10
#define CAM_PHOTO_MAX_FRAMES      60
#define CAM_PHOTO_READY_STREAK    2
#define CAM_PHOTO_MIN_NONBLACK    192
#define CAM_PHOTO_MIN_LUMA        8
#define CAM_JPEG_QUALITY          90

// 1280x960 合并帧居中裁成 16:9，再等比例缩到 1024x576；上下各留 12 像素黑边。
#define CAM_PPA_IN_BLOCK_W        1280
#define CAM_PPA_IN_BLOCK_H        720
#define CAM_PPA_IN_OFFSET_Y       120
#define CAM_PPA_OUT_BLOCK_W       1024
#define CAM_PPA_OUT_BLOCK_H       576
#define CAM_PPA_OUT_OFFSET_Y      12
#define CAM_GUIDE_COLOR           0x8410
#define CAM_GUIDE_CORNER_COLOR    0xF649
#define CAM_GUIDE_CORNER_SIZE     40
#define CAM_GUIDE_CORNER_WIDTH    3

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_video_inited;
static bool s_preview_running;
static bool s_capture_running;
static volatile bool s_preview_stop;
static bool s_i2c_gate_held;
static int s_video_fd = -1;
static TaskHandle_t s_preview_task;
static void *s_cam_bufs[CAM_BUFFER_COUNT];
static size_t s_cam_buf_size[CAM_BUFFER_COUNT];
static uint32_t s_frame_id;
static char s_last_error[FRIDGE_CAMERA_MAX_ERROR_LEN + 1];
static int s_stream_width;
static int s_stream_height;
static uint8_t *s_jpeg_frame;
static size_t s_jpeg_frame_len;
static int s_jpeg_width;
static int s_jpeg_height;
static uint32_t s_capture_ms;
// 高清 JPEG 编码前同步缩放一份屏幕回看图。该缓冲位于 PSRAM，UI 通过
// fridge_camera_take_captured_preview() 接管所有权，避免 LVGL 直接解码 JPEG。
static uint8_t *s_captured_preview_rgb565;
static size_t s_captured_preview_len;
static int s_captured_preview_width;
static int s_captured_preview_height;
// JPEG 驱动的描述符必须来自内部 DMA 堆。预览的 V4L2 映射会使该堆碎片化，
// 所以在进入拍照页、申请相机帧缓冲前一次性创建并在本次运行内复用编码器。
static jpeg_encoder_handle_t s_jpeg_encoder;
static bool s_voice_policy_paused;
static fridge_camera_preview_layout_t s_preview_layout;
static bool s_preview_layout_active;
static bool s_preview_touch_down;
static uint16_t s_preview_touch_x;
static uint16_t s_preview_touch_y;
static fridge_camera_preview_action_t s_preview_action;

static void set_last_error(const char *msg)
{
    if (!msg) {
        s_last_error[0] = '\0';
        return;
    }
    strlcpy(s_last_error, msg, sizeof(s_last_error));
}

// 在内部 DMA 内存尚未被 CSI 帧缓冲切碎时申请 JPEG 驱动资源。
// 截屏在摄像头预览/抓拍期间会自动退回 BMP，因此不会与该持久句柄并发使用。
static esp_err_t prewarm_jpeg_encoder(void)
{
    if (s_jpeg_encoder) {
        return ESP_OK;
    }

    jpeg_encode_engine_cfg_t config = {
        .intr_priority = 0,
        .timeout_ms = 2000,
    };
    esp_err_t ret = jpeg_new_encoder_engine(&config, &s_jpeg_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG prewarm failed: %s, internal DMA largest=%u",
                 esp_err_to_name(ret),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        s_jpeg_encoder = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "JPEG encoder prewarmed before CSI frame buffers");
    return ESP_OK;
}

esp_err_t fridge_camera_prepare_jpeg_encoder(void)
{
    // 只申请 JPEG 外设的内部 DMA 描述符，不初始化 OV5647，因而不触碰共享 SCCB 总线。
    return prewarm_jpeg_encoder();
}

// 配置 KEY0=GPIO12 为输入上拉，用于退出预览（低电平按下）。
static void key0_init_input(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << FRIDGE_PIN_KEY0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&conf);
}

static bool point_in_rect(uint16_t x, uint16_t y, uint16_t rect_x, uint16_t rect_y,
                          uint16_t rect_width, uint16_t rect_height)
{
    return rect_width && rect_height && x >= rect_x && y >= rect_y &&
           x < (uint32_t)rect_x + rect_width && y < (uint32_t)rect_y + rect_height;
}

static bool preview_layout_is_valid(const fridge_camera_preview_layout_t *layout)
{
    return layout && layout->view_width >= 72 && layout->view_height >= 72 &&
           layout->capture_width && layout->capture_height &&
           layout->cancel_width && layout->cancel_height &&
           (uint32_t)layout->view_x + layout->view_width <= FRIDGE_MIPI_WIDTH &&
           (uint32_t)layout->view_y + layout->view_height <= FRIDGE_MIPI_HEIGHT &&
           (uint32_t)layout->capture_x + layout->capture_width <= FRIDGE_MIPI_WIDTH &&
           (uint32_t)layout->capture_y + layout->capture_height <= FRIDGE_MIPI_HEIGHT &&
           (uint32_t)layout->cancel_x + layout->cancel_width <= FRIDGE_MIPI_WIDTH &&
           (uint32_t)layout->cancel_y + layout->cancel_height <= FRIDGE_MIPI_HEIGHT;
}

// 全屏预览保持“任意触摸退出”的旧行为；嵌入页面时则等待真实抬手后只识别拍摄、取消和页头返回。
static fridge_camera_preview_action_t preview_poll_action(void)
{
    if (gpio_get_level((gpio_num_t)FRIDGE_PIN_KEY0) == 0) {
        return FRIDGE_CAMERA_PREVIEW_ACTION_CANCEL;
    }
    if (!fridge_touch_gt911_is_ready()) {
        return FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    }

    fridge_touch_point_t point = {0};
    if (fridge_touch_gt911_read(&point) != ESP_OK) {
        return FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    }
    if (!s_preview_layout_active) {
        return point.pressed ? FRIDGE_CAMERA_PREVIEW_ACTION_CANCEL :
                               FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    }
    if (point.pressed) {
        if (!s_preview_touch_down) {
            s_preview_touch_down = true;
            s_preview_touch_x = point.x;
            s_preview_touch_y = point.y;
        }
        return FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    }
    if (!s_preview_touch_down) {
        return FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    }

    s_preview_touch_down = false;
    if (point_in_rect(s_preview_touch_x, s_preview_touch_y,
                      s_preview_layout.capture_x, s_preview_layout.capture_y,
                      s_preview_layout.capture_width, s_preview_layout.capture_height)) {
        return FRIDGE_CAMERA_PREVIEW_ACTION_CAPTURE;
    }
    if (point_in_rect(s_preview_touch_x, s_preview_touch_y,
                      s_preview_layout.cancel_x, s_preview_layout.cancel_y,
                      s_preview_layout.cancel_width, s_preview_layout.cancel_height) ||
        s_preview_touch_y < s_preview_layout.view_y) {
        return FRIDGE_CAMERA_PREVIEW_ACTION_CANCEL;
    }
    return FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
}

// 摄像头与 ES8311 共用 GPIO28/29。点击预览代表用户主动切换到摄像头，
// 因此在后台任务中有序结束 Realtime、暂停唤醒词，再申请 CAMERA gate。
// 该函数可能等待数秒，严禁从 LVGL 回调直接调用。
static esp_err_t pause_audio_for_preview(void)
{
    if (fridge_glm_realtime_is_active()) {
        esp_err_t err = fridge_glm_realtime_stop();
        if (err != ESP_OK) {
            set_last_error("realtime stop request failed");
            return err;
        }
        int waited_ms = 0;
        while (fridge_glm_realtime_is_active() && waited_ms < CAM_AUDIO_RELEASE_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited_ms += 50;
        }
        if (fridge_glm_realtime_is_active()) {
            set_last_error("realtime audio stop timeout");
            return ESP_ERR_TIMEOUT;
        }
    }

    // Realtime 清理会解除它自己的 dialog guard，所以必须在会话完全停止后再建立摄像头 guard。
    fridge_wake_word_notify_dialog_started();
    s_voice_policy_paused = true;
    esp_err_t err = fridge_wake_word_stop();
    if (err != ESP_OK) {
        set_last_error("wake audio stop failed");
        return err;
    }

    // 普通手动录音同样长期持有 AUDIO gate；显式进入预览时停止并保留已录 PCM。
    fridge_audio_status_t audio = {0};
    if (fridge_audio_get_status(&audio) == ESP_OK && audio.state == FRIDGE_AUDIO_STATE_RECORDING) {
        err = fridge_audio_stop_recording();
        if (err != ESP_OK) {
            set_last_error("recording stop failed");
            return err;
        }
    }
    return ESP_OK;
}

static void resume_voice_policy_after_preview(void)
{
    if (s_voice_policy_paused) {
        s_voice_policy_paused = false;
        fridge_wake_word_notify_dialog_finished();
    }
}

// 调用者必须已经完成共享 I2C 初始化并持有 CAMERA gate，避免嵌套获取同一二值信号量。
static esp_err_t video_init_with_camera_gate_held(void)
{
    if (s_video_inited) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = fridge_codec_shared_i2c_get_bus();
    if (!bus) {
        set_last_error("shared I2C bus unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    // 使用共享 I2C 句柄，禁止 esp_video 再按错误默认脚初始化 SCCB。
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bus,
            .freq = CAM_SCCB_FREQ_HZ,
        },
        .reset_pin = FRIDGE_PIN_CSI_RST,
        .pwdn_pin = FRIDGE_PIN_CSI_PWDN,
    };
    esp_video_init_config_t cam_config = {
        .csi = &csi_config,
    };

    esp_err_t ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        set_last_error(esp_err_to_name(ret));
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_video_inited = true;
    ESP_LOGI(TAG, "esp_video CSI ready (SCCB via shared I2C1, RST=%d PWDN=%d)",
             FRIDGE_PIN_CSI_RST, FRIDGE_PIN_CSI_PWDN);
    return ESP_OK;
}

static esp_err_t video_open_and_set_rgb565(const esp_cam_sensor_format_t *sensor_format, int *out_fd)
{
    char name[16];
    snprintf(name, sizeof(name), "/dev/video%d", CAM_VIDEO_PORT);
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        set_last_error("open /dev/video0 failed");
        ESP_LOGE(TAG, "open %s failed errno=%d", name, errno);
        return ESP_FAIL;
    }

    // 传感器必须在 STREAMOFF 状态切换模式；该 ioctl 会同步重配 CSI 与 ISP 时钟。
    if (ioctl(fd, VIDIOC_S_SENSOR_FMT, sensor_format) != 0) {
        set_last_error("VIDIOC_S_SENSOR_FMT failed");
        ESP_LOGE(TAG, "set sensor mode %s failed errno=%d", sensor_format->name, errno);
        close(fd);
        return ESP_FAIL;
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format fmt = {0};
    fmt.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        set_last_error("VIDIOC_G_FMT failed");
        close(fd);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sensor default format %ux%u pixelformat=0x%08" PRIx32,
             (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height,
             (uint32_t)fmt.fmt.pix.pixelformat);

    // 枚举并选择 RGB565（ISP 输出），保持当前传感器模式的原生宽高。
    bool set_ok = false;
    for (int index = 0; index < 16; index++) {
        struct v4l2_fmtdesc desc = {
            .index = (uint32_t)index,
            .type = type,
        };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
            break;
        }
        if (desc.pixelformat != V4L2_PIX_FMT_RGB565) {
            continue;
        }
        struct v4l2_format set_fmt = {
            .type = type,
            .fmt.pix.width = fmt.fmt.pix.width,
            .fmt.pix.height = fmt.fmt.pix.height,
            .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
        };
        if (ioctl(fd, VIDIOC_S_FMT, &set_fmt) == 0) {
            set_ok = true;
            s_stream_width = (int)set_fmt.fmt.pix.width;
            s_stream_height = (int)set_fmt.fmt.pix.height;
            ESP_LOGI(TAG, "capture RGB565 %dx%d", s_stream_width, s_stream_height);
            break;
        }
        if (errno != ESRCH) {
            ESP_LOGW(TAG, "VIDIOC_S_FMT RGB565 failed errno=%d", errno);
        }
    }

    if (!set_ok) {
        set_last_error("RGB565 format not available");
        close(fd);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_fd = fd;
    return ESP_OK;
}

static esp_err_t video_reqbufs_mmap(int fd)
{
    struct v4l2_requestbuffers req = {
        .count = CAM_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        set_last_error("VIDIOC_REQBUFS failed");
        ESP_LOGE(TAG,
                 "VIDIOC_REQBUFS failed errno=%d: internal_dma free=%u largest=%u, psram free=%u largest=%u",
                 errno,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_FAIL;
    }

    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            set_last_error("VIDIOC_QUERYBUF failed");
            return ESP_FAIL;
        }
        void *ptr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        // esp_video 的 sys/mman 可能不定义 MAP_FAILED；失败时常见返回 (void*)-1 或 NULL。
        if (ptr == NULL || ptr == (void *)-1) {
            set_last_error("mmap camera buffer failed");
            return ESP_ERR_NO_MEM;
        }
        s_cam_bufs[i] = ptr;
        s_cam_buf_size[i] = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            set_last_error("VIDIOC_QBUF failed");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void video_unmap_bufs(void)
{
    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        if (s_cam_bufs[i] && s_cam_buf_size[i]) {
            munmap(s_cam_bufs[i], s_cam_buf_size[i]);
        }
        s_cam_bufs[i] = NULL;
        s_cam_buf_size[i] = 0;
    }
}

// 1280x720 → 1024x576 恰好是 5:4 缩小。每 5 个输入像素取前 4 个，
// 以固定步长展开循环，避免 PPA 通用缩放约 46ms/帧的开销。
static void draw_camera_guides(uint16_t *fb)
{
    if (!s_preview_layout_active || !s_preview_layout.draw_guides) {
        return;
    }

    const int x = s_preview_layout.view_x;
    const int y = s_preview_layout.view_y;
    const int width = s_preview_layout.view_width;
    const int height = s_preview_layout.view_height;
    // 实时预览保留九宫格，帮助用户对准食材；拍后回看由 LVGL 单独绘制，不含九宫格。
    for (int index = 1; index <= 2; index++) {
        int line_x = x + width * index / 3;
        int line_y = y + height * index / 3;
        for (int row = 0; row < height; row++) {
            fb[(size_t)(y + row) * FRIDGE_MIPI_WIDTH + line_x] = CAM_GUIDE_COLOR;
        }
        for (int col = 0; col < width; col++) {
            fb[(size_t)line_y * FRIDGE_MIPI_WIDTH + x + col] = CAM_GUIDE_COLOR;
        }
    }

    const int corner_x[] = {x + 16, x + width - 16 - CAM_GUIDE_CORNER_SIZE,
                            x + 16, x + width - 16 - CAM_GUIDE_CORNER_SIZE};
    const int corner_y[] = {y + 16, y + 16,
                            y + height - 16 - CAM_GUIDE_CORNER_SIZE,
                            y + height - 16 - CAM_GUIDE_CORNER_SIZE};
    const bool draw_top[] = {true, true, false, false};
    const bool draw_left[] = {true, false, true, false};
    for (size_t corner = 0; corner < 4; corner++) {
        for (int thickness = 0; thickness < CAM_GUIDE_CORNER_WIDTH; thickness++) {
            for (int offset = 0; offset < CAM_GUIDE_CORNER_SIZE; offset++) {
                int horizontal_y = draw_top[corner] ? corner_y[corner] + thickness :
                                                       corner_y[corner] + CAM_GUIDE_CORNER_SIZE - 1 - thickness;
                int vertical_x = draw_left[corner] ? corner_x[corner] + thickness :
                                                     corner_x[corner] + CAM_GUIDE_CORNER_SIZE - 1 - thickness;
                fb[(size_t)horizontal_y * FRIDGE_MIPI_WIDTH + corner_x[corner] + offset] =
                    CAM_GUIDE_CORNER_COLOR;
                fb[(size_t)(corner_y[corner] + offset) * FRIDGE_MIPI_WIDTH + vertical_x] =
                    CAM_GUIDE_CORNER_COLOR;
            }
        }
    }
}

static esp_err_t scale_preview_to_lcd(const void *cam_rgb565, void *lcd_fb)
{
    if (!cam_rgb565 || !lcd_fb ||
            s_stream_width < CAM_PPA_IN_BLOCK_W ||
            s_stream_height < CAM_PPA_IN_OFFSET_Y + CAM_PPA_IN_BLOCK_H) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t *dst_fb = (uint16_t *)lcd_fb;
    const uint16_t *src_fb = (const uint16_t *)cam_rgb565;

    if (s_preview_layout_active) {
        // UI 取景区保持 16:9；每个目标像素直接映射到 1280x720 中心裁剪区，
        // 只改写左侧取景矩形，已由 LVGL 绘制的顶栏、状态舱和底栏保持原样。
        const int out_x = s_preview_layout.view_x;
        const int out_y = s_preview_layout.view_y;
        const int out_width = s_preview_layout.view_width;
        const int out_height = s_preview_layout.view_height;
        const uint16_t *src_base = src_fb + (size_t)CAM_PPA_IN_OFFSET_Y * s_stream_width;
        for (int y = 0; y < out_height; y++) {
            int src_y = y * CAM_PPA_IN_BLOCK_H / out_height;
            uint16_t *dst = dst_fb + (size_t)(out_y + y) * FRIDGE_MIPI_WIDTH + out_x;
            const uint16_t *src = src_base + (size_t)src_y * s_stream_width;
            for (int x = 0; x < out_width; x++) {
                dst[x] = src[x * CAM_PPA_IN_BLOCK_W / out_width];
            }
        }
        draw_camera_guides(dst_fb);
        return ESP_OK;
    }

    // 缩放区覆盖完整 1024x576 主画面，只清上下各 12 行黑边；
    // 避免每帧额外写 1.2MB PSRAM，给 30fps 缩放留下带宽。
    const size_t band_bytes = (size_t)FRIDGE_MIPI_WIDTH * CAM_PPA_OUT_OFFSET_Y * sizeof(uint16_t);
    memset(dst_fb, 0, band_bytes);
    memset((uint8_t *)dst_fb +
               (size_t)FRIDGE_MIPI_WIDTH * (CAM_PPA_OUT_OFFSET_Y + CAM_PPA_OUT_BLOCK_H) * sizeof(uint16_t),
           0, band_bytes);

    const uint16_t *src_base = src_fb + (size_t)CAM_PPA_IN_OFFSET_Y * s_stream_width;
    uint16_t *dst_base = dst_fb + (size_t)CAM_PPA_OUT_OFFSET_Y * FRIDGE_MIPI_WIDTH;
    for (int out_y = 0, in_y = 0; out_y < CAM_PPA_OUT_BLOCK_H; out_y += 4, in_y += 5) {
        for (int row = 0; row < 4; row++) {
            const uint16_t *src = src_base + (size_t)(in_y + row) * s_stream_width;
            uint16_t *dst = dst_base + (size_t)(out_y + row) * FRIDGE_MIPI_WIDTH;
            for (int out_x = 0, in_x = 0; out_x < CAM_PPA_OUT_BLOCK_W; out_x += 4, in_x += 5) {
                dst[out_x] = src[in_x];
                dst[out_x + 1] = src[in_x + 1];
                dst[out_x + 2] = src[in_x + 2];
                dst[out_x + 3] = src[in_x + 3];
            }
        }
    }
    return ESP_OK;
}

// 将抓拍 RGB565 原图居中裁为取景区比例，再按最近一次嵌入式取景区域缩放为静态回看图。
// 只在抓拍任务持有 V4L2 帧时调用；输出使用普通 PSRAM，不交给 CSI/JPEG DMA。
static esp_err_t make_captured_preview(const void *cam_rgb565,
                                       uint8_t **out_pixels, size_t *out_len,
                                       int *out_width, int *out_height)
{
    if (!cam_rgb565 || !out_pixels || !out_len || !out_width || !out_height ||
            !s_preview_layout_active || s_preview_layout.view_width == 0 ||
            s_preview_layout.view_height == 0 || s_stream_width <= 0 || s_stream_height <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    const int width = s_preview_layout.view_width;
    const int height = s_preview_layout.view_height;
    const size_t bytes = (size_t)width * height * sizeof(uint16_t);
    uint16_t *pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        return ESP_ERR_NO_MEM;
    }

    const uint16_t *src = (const uint16_t *)cam_rgb565;
    // 实时取景源是 4:3，而页面回看框是 16:9；先居中裁剪可避免回看图被纵向压扁。
    int crop_x = 0;
    int crop_y = 0;
    int crop_width = s_stream_width;
    int crop_height = s_stream_height;
    if ((int64_t)crop_width * height > (int64_t)crop_height * width) {
        crop_width = crop_height * width / height;
        crop_x = (s_stream_width - crop_width) / 2;
    } else if ((int64_t)crop_width * height < (int64_t)crop_height * width) {
        crop_height = crop_width * height / width;
        crop_y = (s_stream_height - crop_height) / 2;
    }
    uint32_t source_nonblack = 0;
    uint32_t source_luma = 0;
    for (int sample_y = 0; sample_y < 16; sample_y++) {
        const uint16_t *sample_row = src + (size_t)(sample_y * s_stream_height / 16) * s_stream_width;
        for (int sample_x = 0; sample_x < 16; sample_x++) {
            uint16_t pixel = sample_row[sample_x * s_stream_width / 16];
            source_nonblack += pixel != 0;
            source_luma += ((pixel >> 11) & 0x1f) + ((pixel >> 5) & 0x3f) + (pixel & 0x1f);
        }
    }
    for (int y = 0; y < height; y++) {
        const uint16_t *src_row = src + (size_t)(crop_y + y * crop_height / height) * s_stream_width + crop_x;
        uint16_t *dst_row = pixels + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            dst_row[x] = src_row[x * crop_width / width];
        }
    }
    ESP_LOGI(TAG,
             "photo review RGB565 samples: source_nonblack=%lu/256 luma=%lu first=%04x center=%04x output=%04x",
             (unsigned long)source_nonblack, (unsigned long)source_luma,
             src[0], src[(size_t)(s_stream_height / 2) * s_stream_width + s_stream_width / 2],
             pixels[(size_t)(height / 2) * width + width / 2]);

    *out_pixels = (uint8_t *)pixels;
    *out_len = bytes;
    *out_width = width;
    *out_height = height;
    return ESP_OK;
}

typedef struct {
    uint16_t nonblack;
    uint16_t average_luma;
    uint16_t first;
    uint16_t center;
} camera_rgb565_sample_t;

// 从 DMA 完成的一帧中均匀抽样 16x16 个点。亮度仅用于判断 OV5647 在模式复位后
// 是否已经完成自动曝光，不修改原图，也不把暗场景人为拉亮后交给 AI。
static camera_rgb565_sample_t sample_rgb565_frame(const void *frame, int width, int height)
{
    camera_rgb565_sample_t sample = {0};
    if (!frame || width <= 0 || height <= 0) {
        return sample;
    }

    const uint16_t *pixels = (const uint16_t *)frame;
    uint32_t luma_sum = 0;
    sample.first = pixels[0];
    sample.center = pixels[(size_t)(height / 2) * width + width / 2];
    for (int sample_y = 0; sample_y < 16; sample_y++) {
        const uint16_t *row = pixels + (size_t)(sample_y * height / 16) * width;
        for (int sample_x = 0; sample_x < 16; sample_x++) {
            uint16_t pixel = row[sample_x * width / 16];
            uint32_t red = ((pixel >> 11) & 0x1fU) * 255U / 31U;
            uint32_t green = ((pixel >> 5) & 0x3fU) * 255U / 63U;
            uint32_t blue = (pixel & 0x1fU) * 255U / 31U;
            sample.nonblack += pixel != 0;
            luma_sum += (77U * red + 150U * green + 29U * blue) >> 8;
        }
    }
    sample.average_luma = (uint16_t)(luma_sum / 256U);
    return sample;
}

// 预览任务是下列资源的唯一所有者；清理包含阻塞驱动调用，严禁放进 critical section。
static void preview_cleanup_resources(void)
{
    if (s_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(s_video_fd, VIDIOC_STREAMOFF, &type);
        video_unmap_bufs();
        close(s_video_fd);
        s_video_fd = -1;
    }
    (void)fridge_display_mipi_release_from_camera();
    if (s_i2c_gate_held) {
        (void)fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA);
        s_i2c_gate_held = false;
    }
    resume_voice_policy_after_preview();
}

// 预览任务：全部 esp_video/帧缓冲初始化放在本任务内完成。
// 禁止在 LVGL 事件回调里同步跑视频初始化/STREAMON，否则会卡死 UI 线程。
static void preview_task(void *arg)
{
    (void)arg;
    void *fb0 = NULL;
    void *fb1 = NULL;
    int fps_count = 0;
    int64_t scale_total_us = 0;
    int64_t present_total_us = 0;
    int dqbuf_timeouts = 0;
    int64_t fps_start = esp_timer_get_time();
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "HD preview task bootstrapping...");

    // 先释放共享 I2C 的音频所有者；全部在摄像头后台任务完成，UI 线程保持响应。
    ret = pause_audio_for_preview();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "preview abort while releasing audio: %s", s_last_error);
        goto exit_task;
    }

    // 1) 先在未持有 CAMERA gate 时完成共享总线初始化。
    // 若在持有 CAMERA gate 后调用 shared_i2c_init，会以 INIT owner 重取同一二值信号量并自死锁。
    ret = fridge_codec_shared_i2c_init();
    if (ret != ESP_OK) {
        set_last_error("shared I2C init failed");
        goto exit_task;
    }

    // 2) 共享 I2C gate：录音中会失败，提示上层稍后再试。
    ret = fridge_codec_shared_i2c_gate_lock(
        FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA, pdMS_TO_TICKS(CAM_I2C_GATE_TIMEOUT_MS));
    if (ret != ESP_OK) {
        set_last_error("I2C gate busy (audio active?)");
        ESP_LOGW(TAG, "preview abort: %s", s_last_error);
        goto exit_task;
    }
    s_i2c_gate_held = true;

    // 3) 持有 CAMERA gate 后初始化 esp_video，期间禁止 ES8311 控制事务并发访问 GPIO28/29。
    ret = video_init_with_camera_gate_held();
    if (ret != ESP_OK) {
        goto exit_task;
    }

    // JPEG 抓拍需要内部 DMA 描述符，但实时取景本身不依赖 JPEG。优先在申请 CSI
    // 缓冲前预热；若当前堆已碎片化，仍必须继续启动取景，避免拍照页被编码器资源阻断。
    ret = prewarm_jpeg_encoder();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "JPEG encoder unavailable; continue HD preview: %s", esp_err_to_name(ret));
    }

    int fd = -1;
    ret = video_open_and_set_rgb565(&s_ov5647_preview_format, &fd);
    if (ret != ESP_OK) {
        goto exit_task;
    }
    s_video_fd = fd;

    ret = video_reqbufs_mmap(s_video_fd);
    if (ret != ESP_OK) {
        goto exit_task;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        set_last_error("VIDIOC_STREAMON failed");
        ret = ESP_FAIL;
        goto exit_task;
    }

    // 只有视频流准备完成后才接管显示双缓冲；初始化失败时 LVGL 仍可继续刷新错误提示。
    ret = fridge_display_mipi_acquire_for_camera();
    if (ret != ESP_OK) {
        set_last_error("acquire MIPI FB failed");
        goto exit_task;
    }
    if (fridge_display_mipi_get_camera_frame_buffers(&fb0, &fb1) != ESP_OK || !fb0 || !fb1) {
        set_last_error("MIPI framebuffers unavailable");
        ESP_LOGE(TAG, "%s", s_last_error);
        goto exit_task;
    }

    // 页面内预览先把当前可见的 Shell 复制到可写缓冲，后续仅更新取景矩形；
    // 独立全屏预览则保留深色启动帧，避免长时间黑屏。
    if (s_preview_layout_active) {
        memcpy(fb0, fb1, (size_t)FRIDGE_MIPI_WIDTH * FRIDGE_MIPI_HEIGHT * sizeof(uint16_t));
    } else {
        uint16_t *splash = (uint16_t *)fb0;
        const size_t n = (size_t)FRIDGE_MIPI_WIDTH * FRIDGE_MIPI_HEIGHT;
        for (size_t i = 0; i < n; i++) {
            splash[i] = 0x1082;  // 深灰
        }
        ret = fridge_display_mipi_present_camera_frame(fb0);
        if (ret != ESP_OK) {
            set_last_error("camera splash present failed");
            goto exit_task;
        }
    }

    set_last_error("");
    ESP_LOGI(TAG, "HD preview loop start %dx%d crop 1280x720 → 1024x576 @ %dfps",
             s_stream_width, s_stream_height, CAM_PREVIEW_TARGET_FPS);

    // 进入预览前等待触点抬起，避免“点拍照入口”的那次按下被当成页面内快门。
    {
        int wait_ms = 0;
        while (wait_ms < 1500) {
            fridge_touch_point_t point = {0};
            bool pressed = fridge_touch_gt911_is_ready() &&
                           fridge_touch_gt911_read(&point) == ESP_OK && point.pressed;
            if (!pressed) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            wait_ms += 20;
        }
        // 再额外消抖 100ms，防止抬起抖动。
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // fb0 是驱动当前安全可写缓冲；页面内预览先写入它，避免覆盖仍在扫描的 Shell。
    void *draw_fb = s_preview_layout_active ? fb1 : fb0;
    while (!s_preview_stop) {
        fridge_camera_preview_action_t action = preview_poll_action();
        if (action != FRIDGE_CAMERA_PREVIEW_ACTION_NONE) {
            portENTER_CRITICAL(&s_lock);
            s_preview_action = action;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "preview action=%d requested by touch/KEY0", (int)action);
            break;
        }

        struct v4l2_buffer buf = {
            .type = type,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            dqbuf_timeouts++;
            if (dqbuf_timeouts == 1 || dqbuf_timeouts == CAM_DQBUF_MAX_TIMEOUTS) {
                ESP_LOGW(TAG, "VIDIOC_DQBUF timeout/failure count=%d errno=%d",
                         dqbuf_timeouts, errno);
            }
            if (dqbuf_timeouts >= CAM_DQBUF_MAX_TIMEOUTS) {
                set_last_error("camera frame timeout");
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            // esp_video 的 DQBUF 已改为 100ms 有界等待；失败后让出 CPU，再检查停止/触摸请求。
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        dqbuf_timeouts = 0;
        if (buf.index >= CAM_BUFFER_COUNT || !s_cam_bufs[buf.index]) {
            ESP_LOGW(TAG, "invalid camera buffer index %u", (unsigned)buf.index);
            (void)ioctl(s_video_fd, VIDIOC_QBUF, &buf);
            continue;
        }

        draw_fb = (draw_fb == fb0) ? fb1 : fb0;
        int64_t scale_started = esp_timer_get_time();
        esp_err_t pret = scale_preview_to_lcd(s_cam_bufs[buf.index], draw_fb);
        scale_total_us += esp_timer_get_time() - scale_started;
        if (pret == ESP_OK) {
            int64_t present_started = esp_timer_get_time();
            esp_err_t dret = fridge_display_mipi_present_camera_frame(draw_fb);
            present_total_us += esp_timer_get_time() - present_started;
            if (dret != ESP_OK) {
                ESP_LOGW(TAG, "present_frame failed: %s", esp_err_to_name(dret));
                set_last_error("camera frame present failed");
                ret = dret;
            } else {
                s_frame_id++;
                fps_count++;
                if (fps_count >= 60) {
                    int64_t now = esp_timer_get_time();
                    float fps = 1000000.0f * 60.0f / (float)(now - fps_start);
                    ESP_LOGI(TAG, "preview fps≈%.1f frame_id=%lu scale=%.1fms present=%.1fms",
                             fps, (unsigned long)s_frame_id,
                             (double)scale_total_us / 60000.0,
                             (double)present_total_us / 60000.0);
                    fps_start = now;
                    fps_count = 0;
                    scale_total_us = 0;
                    present_total_us = 0;
                }
            }
        } else {
            ESP_LOGW(TAG, "preview scale failed: %s", esp_err_to_name(pret));
        }

        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF failed errno=%d", errno);
            set_last_error("VIDIOC_QBUF failed");
            ret = ESP_FAIL;
        }
        if (ret != ESP_OK) {
            break;
        }
        // 固定比例缩放会连续访问 PSRAM；每 4 帧阻塞 1 tick，让 CPU0 Idle 有机会喂狗，
        // 又避免每帧额外损失 10ms。DQBUF/翻页本身仍提供常规任务切换点。
        if ((s_frame_id & 0x3U) == 0) {
            vTaskDelay(1);
        }
    }

exit_task:
    preview_cleanup_resources();
    portENTER_CRITICAL(&s_lock);
    s_preview_running = false;
    s_preview_stop = false;
    s_preview_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "HD preview task exit (last_err=%s)", s_last_error[0] ? s_last_error : "none");
    // 预览任务由 xTaskCreatePinnedToCoreWithCaps() 创建，必须配套释放外部 RAM 栈。
    vTaskDeleteWithCaps(NULL);
}

esp_err_t fridge_camera_init(void)
{
    // 公共初始化也遵循“先建总线 → 再持 CAMERA gate → 初始化视频”的固定顺序，避免嵌套锁。
    ESP_RETURN_ON_ERROR(fridge_codec_shared_i2c_init(), TAG, "shared I2C init failed");
    ESP_RETURN_ON_ERROR(fridge_codec_shared_i2c_gate_lock(
                            FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA,
                            pdMS_TO_TICKS(CAM_I2C_GATE_TIMEOUT_MS)),
                        TAG, "camera I2C gate busy");
    esp_err_t ret = video_init_with_camera_gate_held();
    esp_err_t unlock_ret = fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA);
    return ret != ESP_OK ? ret : unlock_ret;
}

esp_err_t fridge_camera_probe(fridge_camera_probe_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->last_error, "probe not implemented; use start_hd_preview", sizeof(out->last_error));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

// 使用官方 1920x1080 最高分辨率模式抓拍，并直接交给片上 JPEG 硬件编码。
// 整个过程持有 CAMERA gate，保证 GPIO28/29 上不会并发访问 ES8311。
static esp_err_t capture_reliable_jpeg(void)
{
    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = ESP_OK;
    int fd = -1;
    bool stream_on = false;
    bool gate_held = false;
    jpeg_encoder_handle_t encoder = NULL;
    uint8_t *new_jpeg = NULL;
    size_t new_jpeg_capacity = 0;
    uint32_t new_jpeg_len = 0;
    uint8_t *new_preview = NULL;
    size_t new_preview_len = 0;
    int new_preview_width = 0;
    int new_preview_height = 0;

    portENTER_CRITICAL(&s_lock);
    if (s_preview_running || s_preview_task || s_capture_running) {
        portEXIT_CRITICAL(&s_lock);
        set_last_error("camera busy");
        return ESP_ERR_INVALID_STATE;
    }
    s_capture_running = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "AI photo capture start: official 1920x1080 RGB565 -> hardware JPEG Q%d",
             CAM_JPEG_QUALITY);

    // 高清抓拍可能紧接在预览任务退出之后；预览清理会恢复语音策略，因此这里必须重新建立
    // 摄像头语音门禁并同步停止 WakeNet/Realtime/普通录音，防止 1080p REQBUFS 与音频重启抢资源。
    ret = pause_audio_for_preview();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "capture abort while releasing audio: %s", s_last_error);
        goto cleanup;
    }

    ret = fridge_codec_shared_i2c_init();
    if (ret != ESP_OK) {
        set_last_error("shared I2C init failed");
        goto cleanup;
    }
    ret = fridge_codec_shared_i2c_gate_lock(
        FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA, pdMS_TO_TICKS(CAM_I2C_GATE_TIMEOUT_MS));
    if (ret != ESP_OK) {
        set_last_error("I2C gate busy (audio active?)");
        goto cleanup;
    }
    gate_held = true;

    ret = video_init_with_camera_gate_held();
    if (ret != ESP_OK) {
        goto cleanup;
    }
    // 正常 UI 路径已在预览启动时预热；保留直接抓拍入口的同等保障。
    ret = prewarm_jpeg_encoder();
    if (ret != ESP_OK) {
        set_last_error("JPEG encoder init failed");
        goto cleanup;
    }
    encoder = s_jpeg_encoder;
    ret = video_open_and_set_rgb565(&s_ov5647_photo_format, &fd);
    if (ret != ESP_OK) {
        goto cleanup;
    }
    s_video_fd = fd;
    ret = video_reqbufs_mmap(fd);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        set_last_error("VIDIOC_STREAMON failed");
        ret = ESP_FAIL;
        goto cleanup;
    }
    stream_on = true;

    // VIDIOC_S_SENSOR_FMT 会复位 OV5647 的曝光寄存器。固定丢 3 帧在室内并不可靠，
    // 因此按真实 RGB565 亮度等待连续两帧有效，最多约 2 秒；未收敛时返回错误，
    // 禁止把一张已知近黑的 JPEG/回看图伪装成拍照成功。
    int ready_streak = 0;
    for (int frame = 1; frame <= CAM_PHOTO_MAX_FRAMES; frame++) {
        struct v4l2_buffer buf = {
            .type = type,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0 ||
                buf.index >= CAM_BUFFER_COUNT || !s_cam_bufs[buf.index]) {
            set_last_error("photo frame timeout");
            ret = ESP_ERR_TIMEOUT;
            goto cleanup;
        }

        camera_rgb565_sample_t sample = sample_rgb565_frame(
            s_cam_bufs[buf.index], s_stream_width, s_stream_height);
        bool exposure_ready = frame >= CAM_PHOTO_MIN_FRAMES &&
                              sample.nonblack >= CAM_PHOTO_MIN_NONBLACK &&
                              sample.average_luma >= CAM_PHOTO_MIN_LUMA;
        ready_streak = exposure_ready ? ready_streak + 1 : 0;
        if (frame == 1 || (frame % 10) == 0 || ready_streak > 0) {
            ESP_LOGI(TAG,
                     "photo exposure frame=%d nonblack=%u/256 luma=%u streak=%d first=%04x center=%04x",
                     frame, sample.nonblack, sample.average_luma, ready_streak,
                     sample.first, sample.center);
        }

        bool use_frame = ready_streak >= CAM_PHOTO_READY_STREAK;
        if (frame == CAM_PHOTO_MAX_FRAMES && !use_frame) {
            set_last_error("photo exposure did not converge");
            ret = ESP_ERR_INVALID_RESPONSE;
        } else if (use_frame) {
            // 最坏情况下给到与 RGB565 原图相同的容量，避免高纹理画面溢出。
            jpeg_encode_memory_alloc_cfg_t alloc_cfg = {
                .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
            };
            size_t requested = (size_t)s_stream_width * s_stream_height * sizeof(uint16_t);
            new_jpeg = jpeg_alloc_encoder_mem(requested, &alloc_cfg, &new_jpeg_capacity);
            if (!new_jpeg) {
                set_last_error("JPEG output alloc failed");
                ret = ESP_ERR_NO_MEM;
            } else {
                jpeg_encode_cfg_t encode_cfg = {
                    .height = (uint32_t)s_stream_height,
                    .width = (uint32_t)s_stream_width,
                    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                    .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
                    .image_quality = CAM_JPEG_QUALITY,
                };
                size_t raw_size = (size_t)s_stream_width * s_stream_height * sizeof(uint16_t);
                // JPEG 硬件编码器可能复用输入缓冲的缓存行；必须在编码前复制/缩放
                // 回看图，否则编码完成后 LVGL 看到的 RGB565 可能已变成全黑。
                ret = make_captured_preview(s_cam_bufs[buf.index],
                                            &new_preview, &new_preview_len,
                                            &new_preview_width, &new_preview_height);
                if (ret != ESP_OK) {
                    set_last_error("captured preview alloc failed");
                } else {
                    ret = jpeg_encoder_process(encoder, &encode_cfg,
                                               s_cam_bufs[buf.index], raw_size,
                                               new_jpeg, new_jpeg_capacity, &new_jpeg_len);
                    if (ret != ESP_OK) {
                        set_last_error("JPEG hardware encode failed");
                    }
                }
            }
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0 && ret == ESP_OK) {
            set_last_error("VIDIOC_QBUF failed");
            ret = ESP_FAIL;
        }
        if (ret != ESP_OK) {
            goto cleanup;
        }
        if (use_frame) {
            break;
        }
    }

    free(s_jpeg_frame);
    s_jpeg_frame = new_jpeg;
    s_jpeg_frame_len = new_jpeg_len;
    s_jpeg_width = s_stream_width;
    s_jpeg_height = s_stream_height;
    free(s_captured_preview_rgb565);
    s_captured_preview_rgb565 = new_preview;
    s_captured_preview_len = new_preview_len;
    s_captured_preview_width = new_preview_width;
    s_captured_preview_height = new_preview_height;
    s_capture_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    s_frame_id++;
    new_jpeg = NULL;
    new_preview = NULL;
    set_last_error("");
    ESP_LOGI(TAG, "AI photo ready: %dx%d JPEG=%u bytes elapsed=%ums free_psram=%uKB",
             s_jpeg_width, s_jpeg_height, (unsigned)s_jpeg_frame_len,
             (unsigned)s_capture_ms,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

cleanup:
    free(new_jpeg);
    free(new_preview);
    if (fd >= 0) {
        if (stream_on) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl(fd, VIDIOC_STREAMOFF, &type);
        }
        video_unmap_bufs();
        close(fd);
        s_video_fd = -1;
    }
    if (gate_held) {
        (void)fridge_codec_shared_i2c_gate_unlock(FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA);
    }
    resume_voice_policy_after_preview();
    portENTER_CRITICAL(&s_lock);
    s_capture_running = false;
    portEXIT_CRITICAL(&s_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AI photo capture failed: %s (%s)", s_last_error, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t fridge_camera_capture(void)
{
    return capture_reliable_jpeg();
}

esp_err_t fridge_camera_capture_preview_rgb565(fridge_camera_preview_frame_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    set_last_error("use fridge_camera_start_hd_preview for P4 live view");
    return ESP_ERR_NOT_SUPPORTED;
}

void fridge_camera_free_preview_frame(fridge_camera_preview_frame_t *frame)
{
    if (frame) {
        free(frame->rgb565);
        memset(frame, 0, sizeof(*frame));
    }
}

esp_err_t fridge_camera_capture_ai_fullres(void)
{
    return capture_reliable_jpeg();
}

esp_err_t fridge_camera_capture_hardware_jpeg_diag(void)
{
    return capture_reliable_jpeg();
}

esp_err_t fridge_camera_capture_rgb565_diag(fridge_camera_diag_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->last_error, "rgb565 diag not implemented", sizeof(out->last_error));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void fridge_camera_get_status(fridge_camera_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_video_inited;
    out->has_frame = s_jpeg_frame != NULL;
    out->width = s_jpeg_frame ? s_jpeg_width : s_stream_width;
    out->height = s_jpeg_frame ? s_jpeg_height : s_stream_height;
    out->jpeg_bytes = s_jpeg_frame_len;
    out->capture_ms = s_capture_ms;
    out->frame_id = s_frame_id;
    out->free_heap_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);
    out->free_psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    strlcpy(out->pixel_format, s_jpeg_frame ? "JPEG" : "RGB565", sizeof(out->pixel_format));
    strlcpy(out->frame_size, s_jpeg_frame ? "1920x1080" : (s_preview_running ? "720p_preview" : "idle"),
            sizeof(out->frame_size));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
}

esp_err_t fridge_camera_get_frame(fridge_camera_frame_view_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_jpeg_frame || !s_jpeg_frame_len) {
        return ESP_ERR_NOT_FOUND;
    }
    out->data = s_jpeg_frame;
    out->len = s_jpeg_frame_len;
    out->width = s_jpeg_width;
    out->height = s_jpeg_height;
    out->capture_ms = s_capture_ms;
    out->frame_id = s_frame_id;
    return ESP_OK;
}

esp_err_t fridge_camera_take_captured_preview(fridge_camera_preview_frame_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_captured_preview_rgb565 || !s_captured_preview_len) {
        return ESP_ERR_NOT_FOUND;
    }
    out->rgb565 = s_captured_preview_rgb565;
    out->len = s_captured_preview_len;
    out->width = s_captured_preview_width;
    out->height = s_captured_preview_height;
    out->capture_ms = s_capture_ms;
    out->frame_id = s_frame_id;
    s_captured_preview_rgb565 = NULL;
    s_captured_preview_len = 0;
    s_captured_preview_width = 0;
    s_captured_preview_height = 0;
    return ESP_OK;
}

esp_err_t fridge_camera_clear_frame(void)
{
    free(s_jpeg_frame);
    s_jpeg_frame = NULL;
    s_jpeg_frame_len = 0;
    s_jpeg_width = 0;
    s_jpeg_height = 0;
    free(s_captured_preview_rgb565);
    s_captured_preview_rgb565 = NULL;
    s_captured_preview_len = 0;
    s_captured_preview_width = 0;
    s_captured_preview_height = 0;
    s_capture_ms = 0;
    return ESP_OK;
}

esp_err_t fridge_camera_reset(void)
{
    esp_err_t ret = fridge_camera_stop_hd_preview();
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_capture_running) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    jpeg_encoder_handle_t encoder = s_jpeg_encoder;
    s_jpeg_encoder = NULL;
    portEXIT_CRITICAL(&s_lock);
    if (encoder) {
        // reset 是显式资源回收入口；抓拍任务运行时已在上方拒绝，避免释放仍在使用的句柄。
        return jpeg_del_encoder_engine(encoder);
    }
    return ESP_OK;
}

static esp_err_t start_hd_preview(const fridge_camera_preview_layout_t *layout)
{
    if (!fridge_display_mipi_is_ready()) {
        set_last_error("MIPI display not ready");
        return ESP_ERR_INVALID_STATE;
    }

    // 仅做轻量标记 + 拉起后台任务；重初始化全部在 preview_task 内完成，
    // 保证可从 LVGL 点击回调安全调用（不会阻塞 UI 线程数秒）。
    portENTER_CRITICAL(&s_lock);
    if (s_preview_running || s_preview_task || s_capture_running) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_preview_running = true;
    s_preview_stop = false;
    s_preview_action = FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    s_preview_touch_down = false;
    s_preview_layout_active = layout != NULL;
    if (layout) {
        s_preview_layout = *layout;
    } else {
        memset(&s_preview_layout, 0, sizeof(s_preview_layout));
    }
    set_last_error("starting");
    portEXIT_CRITICAL(&s_lock);

    key0_init_input();

    // CSI ISR 使用自己的内部中断栈；此任务只处理 V4L2、缩放和 MIPI present，
    // 可安全使用 PSRAM 任务栈。sdkconfig 已启用 FREERTOS_TASK_CREATE_ALLOW_EXT_MEM。
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(preview_task,
                                                    "cam_hd_preview",
                                                    CAM_PREVIEW_TASK_STACK,
                                                    NULL,
                                                    CAM_PREVIEW_TASK_PRIO,
                                                    &s_preview_task,
                                                    0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        set_last_error("preview task create failed");
        portENTER_CRITICAL(&s_lock);
        s_preview_running = false;
        s_preview_task = NULL;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "start_hd_preview failed: %s (stack=%u free_internal=%u largest_internal=%u free_psram=%u)",
                 s_last_error, (unsigned)CAM_PREVIEW_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "HD preview start requested (async bootstrap)");
    return ESP_OK;
}

esp_err_t fridge_camera_start_hd_preview(void)
{
    return start_hd_preview(NULL);
}

esp_err_t fridge_camera_start_hd_preview_with_layout(const fridge_camera_preview_layout_t *layout)
{
    if (!preview_layout_is_valid(layout)) {
        return ESP_ERR_INVALID_ARG;
    }
    return start_hd_preview(layout);
}

esp_err_t fridge_camera_request_hd_preview_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_preview_running && !s_preview_task) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    s_preview_stop = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t fridge_camera_stop_hd_preview(void)
{
    esp_err_t stop_ret = fridge_camera_request_hd_preview_stop();
    if (stop_ret != ESP_OK) {
        return stop_ret;
    }

    // 等待资源所有者任务自行清理（最长约 3s）；禁止从调用者任务并发 close/munmap。
    for (int i = 0; i < 60; i++) {
        if (!fridge_camera_hd_preview_running()) {
            ESP_LOGI(TAG, "HD preview stopped");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "HD preview stop timed out; owner task keeps resources for safe cleanup");
    return ESP_ERR_TIMEOUT;
}

bool fridge_camera_hd_preview_running(void)
{
    portENTER_CRITICAL(&s_lock);
    bool running = s_preview_running;
    portEXIT_CRITICAL(&s_lock);
    return running;
}

fridge_camera_preview_action_t fridge_camera_take_hd_preview_action(void)
{
    portENTER_CRITICAL(&s_lock);
    fridge_camera_preview_action_t action = s_preview_action;
    s_preview_action = FRIDGE_CAMERA_PREVIEW_ACTION_NONE;
    portEXIT_CRITICAL(&s_lock);
    return action;
}

bool fridge_camera_is_busy(void)
{
    // 硬件 JPEG 编码器是单例资源；拍照或预览任一进行中均视为忙。
    portENTER_CRITICAL(&s_lock);
    bool busy = s_capture_running || s_preview_running || (s_preview_task != NULL);
    portEXIT_CRITICAL(&s_lock);
    return busy;
}
