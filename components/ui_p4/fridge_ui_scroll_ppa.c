// ESP32-P4 HOME-01 主滚动区 PPA 加速。
//
// LVGL 仍负责触摸、滚动坐标和惯性动画。静止时把完整主页面快照到有界 PSRAM；
// 手势期间暂停显示刷新定时器，仅用 PPA SRM 做 RGB565 1:1 裁剪搬运，再直接提交
// MIPI 双缓冲。热路径无动态分配、无异步 PPA 回调，并保留摄像头显存互斥。

#include "fridge_ui_scroll_ppa.h"

#include <stdint.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fridge_display_mipi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/display/lv_display_private.h"
#include "src/core/lv_refr_private.h"

#define PPA_LANDSCAPE_SCROLL_X 0
#define PPA_LANDSCAPE_SCROLL_Y 87
#define PPA_LANDSCAPE_SCROLL_W 704
#define PPA_LANDSCAPE_SCROLL_H 513
#define PPA_PORTRAIT_SCROLL_X 0
#define PPA_PORTRAIT_SCROLL_Y 87
#define PPA_PORTRAIT_SCROLL_W 600
#define PPA_PORTRAIT_SCROLL_H 585
#define PPA_LANDSCAPE_CANVAS_W 1024
#define PPA_LANDSCAPE_CANVAS_H 600
#define PPA_PORTRAIT_CANVAS_W 600
#define PPA_PORTRAIT_CANVAS_H 1024
// HOME-02 的参考稿完整内容为 1668px；CHAT foreground 可能保留多条真实消息，
// 不能沿用只覆盖 HOME-02 的 1700px 上限。4096px 仍是按需申请的 RGB565 PSRAM
// 缓存上限，视窗、DMA 和 MIPI 时序不变；超过上限时继续安全回退 LVGL 软件滚动。
#define PPA_PAGE_MAX_H 4096
#define PPA_ELASTIC_MARGIN 80
#define PPA_CACHE_ALIGN 64
#define PPA_DYNAMIC_OVERLAY_MAX_W 704U
#define PPA_DYNAMIC_OVERLAY_MAX_H 256U
#define PPA_DYNAMIC_OVERLAY_BPP 4U
#define PPA_FRAME_BYTES ((size_t)FRIDGE_MIPI_WIDTH * FRIDGE_MIPI_HEIGHT * sizeof(uint16_t))
#define PPA_PERF_INTERVALS 20U
#define PPA_ACTIVE_GAP_US 250000LL
#define PPA_CACHE_RETRY_US 2000000LL
// 字体、图标和业务角标可能在相邻 UI 周期内连续失效。合并 650ms 后再重建，
// 避免同一轮资源绑定反复执行大面积软件栅格化而触发 P4 HP System WDT。
#define PPA_CACHE_REBUILD_DEBOUNCE_US 650000LL
#define PPA_FINISH_GRACE_US 120000LL
#define PPA_DYNAMIC_OVERLAY_REFRESH_US 50000LL
#define PPA_WDT_TRACE_MAGIC 0x50504157U

typedef enum {
    PPA_WDT_STAGE_IDLE = 0,
    PPA_WDT_STAGE_SYNC_LIST_CLEAR,
    PPA_WDT_STAGE_HW_COPY,
    PPA_WDT_STAGE_DSI_PRESENT,
    PPA_WDT_STAGE_ACTIVE_IDLE,
    PPA_WDT_STAGE_LVGL_SYNC,
    PPA_WDT_STAGE_RESTORE,
    PPA_WDT_STAGE_CACHE_SNAPSHOT,
} ppa_wdt_stage_t;

typedef struct {
    uint32_t magic;
    uint32_t stage;
} ppa_wdt_trace_t;

typedef struct {
    lv_display_t *display;
    lv_obj_t *scroll;
    fridge_ui_scroll_ppa_profile_t profile;
    fridge_ui_scroll_ppa_present_cb_t present_cb;
    void *present_user_ctx;
    lv_indev_t *indev;
    lv_timer_t *refresh_timer;
    ppa_client_handle_t client;
    ppa_client_handle_t blend_client;
    uint8_t *cache;
    size_t cache_capacity;
    uint8_t *dynamic_overlay_cache;
    size_t dynamic_overlay_capacity;
    lv_draw_buf_t dynamic_overlay_draw_buf;
    lv_obj_t *dynamic_overlay;
    int32_t dynamic_overlay_x;
    int32_t dynamic_overlay_y;
    uint32_t dynamic_overlay_w;
    uint32_t dynamic_overlay_h;
    bool dynamic_overlay_valid;
    int64_t dynamic_overlay_last_capture_us;
    uint32_t viewport_height_override;
    uint32_t page_h;
    bool initialized;
    bool enabled;
    bool cache_valid;
    bool cache_dirty;
    bool rebuilding_cache;
    bool active;
    bool frame_pending;
    bool finish_pending;
    int64_t finish_deadline_us;
    bool saved_invalidation;
    bool saved_timer_paused;
    lv_scrollbar_mode_t saved_scrollbar_mode;
    int64_t cache_retry_after_us;
    int32_t last_presented_y;
    bool has_presented_y;
    int64_t gesture_start_us;
    uint64_t gesture_copy_total_us;
    uint64_t gesture_present_total_us;
    uint32_t gesture_frame_count;
    uint32_t gesture_copy_max_us;
    uint32_t gesture_present_max_us;
    int64_t perf_last_frame_us;
    uint64_t perf_interval_total_us;
    uint64_t perf_copy_total_us;
    uint64_t perf_present_total_us;
    uint32_t perf_interval_count;
    uint32_t perf_frame_count;
    uint32_t perf_copy_max_us;
    uint32_t perf_present_max_us;
} ppa_scroll_state_t;

static const char *TAG = "ui_scroll_ppa";
static ppa_scroll_state_t s_ppa;
// RTC noinit 在 HP WDT 复位后保留，用于定位最后卡在 PPA、DSI 还是 LVGL 同步。
static RTC_NOINIT_ATTR ppa_wdt_trace_t s_wdt_trace;

_Static_assert(((PPA_LANDSCAPE_SCROLL_W * sizeof(uint16_t) * PPA_ELASTIC_MARGIN) %
                PPA_CACHE_ALIGN) == 0,
               "landscape elastic snapshot offset must keep PPA cache alignment");
_Static_assert(((PPA_PORTRAIT_SCROLL_W * sizeof(uint16_t) * PPA_ELASTIC_MARGIN) %
                PPA_CACHE_ALIGN) == 0,
               "portrait elastic snapshot offset must keep PPA cache alignment");

static uint32_t viewport_x(void)
{
    return s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
           PPA_PORTRAIT_SCROLL_X : PPA_LANDSCAPE_SCROLL_X;
}

static uint32_t viewport_y(void)
{
    return s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
           PPA_PORTRAIT_SCROLL_Y : PPA_LANDSCAPE_SCROLL_Y;
}

static uint32_t viewport_w(void)
{
    return s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
           PPA_PORTRAIT_SCROLL_W : PPA_LANDSCAPE_SCROLL_W;
}

static uint32_t viewport_h(void)
{
    if (s_ppa.viewport_height_override) return s_ppa.viewport_height_override;
    return s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
           PPA_PORTRAIT_SCROLL_H : PPA_LANDSCAPE_SCROLL_H;
}

static uint32_t canvas_w(void)
{
    return s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
           PPA_PORTRAIT_CANVAS_W : PPA_LANDSCAPE_CANVAS_W;
}

static uint32_t canvas_h(void)
{
    return s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
           PPA_PORTRAIT_CANVAS_H : PPA_LANDSCAPE_CANVAS_H;
}

static void wdt_trace_set(ppa_wdt_stage_t stage)
{
    s_wdt_trace.magic = PPA_WDT_TRACE_MAGIC;
    s_wdt_trace.stage = (uint32_t)stage;
}

// PPA 会直接翻转 DIRECT 双缓冲；接管前必须先完成 LVGL 尚未执行的区域同步。
// 若直接丢弃 sync_areas，时间等右侧小区域可能只更新一块缓冲，后续滚动翻页时会新旧数字跳变。
static esp_err_t synchronize_pending_direct_areas(void)
{
    if (lv_ll_is_empty(&s_ppa.display->sync_areas)) return ESP_OK;
    if (s_ppa.display->render_mode != LV_DISPLAY_RENDER_MODE_DIRECT ||
        !lv_display_is_double_buffered(s_ppa.display)) {
        return ESP_ERR_INVALID_STATE;
    }

    lv_draw_buf_t *off_screen = lv_display_get_buf_active(s_ppa.display);
    lv_draw_buf_t *on_screen = off_screen == s_ppa.display->buf_1 ?
                               s_ppa.display->buf_2 : s_ppa.display->buf_1;
    if (!off_screen || !on_screen || off_screen == on_screen) {
        return ESP_ERR_INVALID_STATE;
    }

    const int32_t max_x = lv_display_get_horizontal_resolution(s_ppa.display) - 1;
    const int32_t max_y = lv_display_get_vertical_resolution(s_ppa.display) - 1;
    for (lv_area_t *area = lv_ll_get_head(&s_ppa.display->sync_areas);
         area != NULL;
         area = lv_ll_get_next(&s_ppa.display->sync_areas, area)) {
        lv_area_t clipped = *area;
        if (clipped.x1 < 0) clipped.x1 = 0;
        if (clipped.y1 < 0) clipped.y1 = 0;
        if (clipped.x2 > max_x) clipped.x2 = max_x;
        if (clipped.y2 > max_y) clipped.y2 = max_y;
        if (clipped.x1 <= clipped.x2 && clipped.y1 <= clipped.y2) {
            lv_draw_buf_copy(off_screen, &clipped, on_screen, &clipped);
        }
    }
    lv_ll_clear(&s_ppa.display->sync_areas);
    return ESP_OK;
}

// PPA 每帧只覆盖左侧主滚动视口，顶栏、导航栏和状态舱沿用目标帧缓冲原内容。
// 新手势接管前把当前可见整帧种到 LVGL 离屏缓冲一次；随后两块缓冲交替提交时，
// 固定区都会保持同一最新版本，不会把 TF 图标加载前的 Flash 占位帧重新翻上屏。
static esp_err_t seed_ppa_write_buffer_from_visible(void)
{
    if (s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT) {
        // 竖屏两块逻辑 DIRECT 缓冲已由 synchronize_pending_direct_areas() 同步；
        // 固定顶栏、导航和状态舱直接保留在当前 LVGL active 缓冲中。
        return ESP_OK;
    }
    void *write_fb = NULL;
    void *visible_fb = NULL;
    esp_err_t ret = fridge_display_mipi_get_ui_frame_buffers(&write_fb, &visible_fb);
    if (ret != ESP_OK || !write_fb || !visible_fb || write_fb == visible_fb) {
        return ret == ESP_OK ? ESP_ERR_INVALID_STATE : ret;
    }

    lv_draw_buf_t *active = lv_display_get_buf_active(s_ppa.display);
    if (!active || active->data != write_fb) {
        // 缓冲奇偶不一致时禁止覆盖可能正在扫描的帧，保留 LVGL 软件滚动路径。
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(write_fb, visible_fb, PPA_FRAME_BYTES);
    return ESP_OK;
}

static void perf_reset(void)
{
    s_ppa.perf_interval_total_us = 0;
    s_ppa.perf_copy_total_us = 0;
    s_ppa.perf_present_total_us = 0;
    s_ppa.perf_interval_count = 0;
    s_ppa.perf_frame_count = 0;
    s_ppa.perf_copy_max_us = 0;
    s_ppa.perf_present_max_us = 0;
}

static void perf_record(int64_t done_us, uint32_t copy_us, uint32_t present_us, int32_t scroll_y)
{
    s_ppa.gesture_copy_total_us += copy_us;
    s_ppa.gesture_present_total_us += present_us;
    s_ppa.gesture_frame_count++;
    if (copy_us > s_ppa.gesture_copy_max_us) s_ppa.gesture_copy_max_us = copy_us;
    if (present_us > s_ppa.gesture_present_max_us) s_ppa.gesture_present_max_us = present_us;
    if (s_ppa.perf_last_frame_us > 0) {
        int64_t interval_us = done_us - s_ppa.perf_last_frame_us;
        if (interval_us > 0 && interval_us <= PPA_ACTIVE_GAP_US) {
            s_ppa.perf_interval_total_us += (uint64_t)interval_us;
            s_ppa.perf_interval_count++;
        } else {
            perf_reset();
        }
    }
    s_ppa.perf_last_frame_us = done_us;
    s_ppa.perf_copy_total_us += copy_us;
    s_ppa.perf_present_total_us += present_us;
    s_ppa.perf_frame_count++;
    if (copy_us > s_ppa.perf_copy_max_us) s_ppa.perf_copy_max_us = copy_us;
    if (present_us > s_ppa.perf_present_max_us) s_ppa.perf_present_max_us = present_us;

    if (s_ppa.perf_interval_count >= PPA_PERF_INTERVALS && s_ppa.perf_frame_count > 0) {
        uint32_t fps_x10 = (uint32_t)((uint64_t)s_ppa.perf_interval_count * 10000000ULL /
                                      s_ppa.perf_interval_total_us);
        uint32_t copy_avg = (uint32_t)(s_ppa.perf_copy_total_us / s_ppa.perf_frame_count);
        uint32_t present_avg = (uint32_t)(s_ppa.perf_present_total_us / s_ppa.perf_frame_count);
        ESP_LOGI(TAG,
                 "PPA scroll=%lu.%lu fps copy=%lu.%lums(max %lu.%lu) present=%lu.%lums(max %lu.%lu) y=%ld",
                 (unsigned long)(fps_x10 / 10U), (unsigned long)(fps_x10 % 10U),
                 (unsigned long)(copy_avg / 1000U), (unsigned long)((copy_avg % 1000U) / 100U),
                 (unsigned long)(s_ppa.perf_copy_max_us / 1000U),
                 (unsigned long)((s_ppa.perf_copy_max_us % 1000U) / 100U),
                 (unsigned long)(present_avg / 1000U),
                 (unsigned long)((present_avg % 1000U) / 100U),
                 (unsigned long)(s_ppa.perf_present_max_us / 1000U),
                 (unsigned long)((s_ppa.perf_present_max_us % 1000U) / 100U),
                 (long)scroll_y);
        perf_reset();
    }
}

// PPA 快照在真实页面上下各保留 80px 背景，显示 LVGL 原生弹性滚动位移。
// 超过缓冲范围时只钳制视觉位移，不改变 LVGL 的阻尼和回弹状态机。
static int32_t clamp_cached_scroll_y(int32_t y)
{
    const uint32_t view_h = viewport_h();
    int32_t max_y = s_ppa.page_h > view_h ?
                    (int32_t)(s_ppa.page_h - view_h) : 0;
    if (y < -PPA_ELASTIC_MARGIN) return -PPA_ELASTIC_MARGIN;
    if (y > max_y + PPA_ELASTIC_MARGIN) return max_y + PPA_ELASTIC_MARGIN;
    return y;
}

// PPA 输出直接落到完整 1024x600 RGB565 帧缓冲的左下区域，其他区域保持原样。
static esp_err_t copy_cached_view(void *write_fb, int32_t scroll_y, uint32_t *copy_us)
{
    if (!write_fb || !s_ppa.cache_valid || !s_ppa.client) return ESP_ERR_INVALID_STATE;
    const uint32_t view_w = viewport_w();
    const uint32_t view_h = viewport_h();
    int32_t y = clamp_cached_scroll_y(scroll_y);
    uint32_t cache_h = s_ppa.page_h + 2U * PPA_ELASTIC_MARGIN;
    // 动态页面切换或内容高度变化时，旧快照可能比当前滚动位置短；
    // 先做边界校验，禁止把越界裁剪参数交给 PPA 硬件。
    int64_t block_y = (int64_t)y + PPA_ELASTIC_MARGIN;
    if (s_ppa.page_h < view_h || block_y < 0 ||
        (uint64_t)block_y + view_h > cache_h) {
        return ESP_ERR_INVALID_SIZE;
    }
    ppa_srm_oper_config_t config = {
        .in = {
            .buffer = s_ppa.cache,
            .pic_w = view_w,
            .pic_h = cache_h,
            .block_w = view_w,
            .block_h = view_h,
            .block_offset_x = 0,
            .block_offset_y = (uint32_t)(y + PPA_ELASTIC_MARGIN),
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = write_fb,
            .buffer_size = PPA_FRAME_BYTES,
            .pic_w = canvas_w(),
            .pic_h = canvas_h(),
            .block_offset_x = viewport_x(),
            .block_offset_y = viewport_y(),
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa.client, &config);
    if (copy_us) *copy_us = (uint32_t)(esp_timer_get_time() - started_us);
    return ret;
}

// 把秒表主卡的实时内容覆盖到静态页面快照上。
// 快照使用 ARGB8888 保留字体抗锯齿透明度；背景和输出都是当前安全 RGB565 帧缓冲，
// PPA 只处理主卡与视窗的交集，主卡滚出屏幕时不会访问负坐标或越界区域。
static esp_err_t blend_dynamic_overlay(void *write_fb, uint32_t *blend_us)
{
    if (!s_ppa.dynamic_overlay_valid || !s_ppa.blend_client || !write_fb) return ESP_OK;

    lv_area_t overlay = {
        .x1 = s_ppa.dynamic_overlay_x,
        .y1 = s_ppa.dynamic_overlay_y,
        .x2 = s_ppa.dynamic_overlay_x + (int32_t)s_ppa.dynamic_overlay_w - 1,
        .y2 = s_ppa.dynamic_overlay_y + (int32_t)s_ppa.dynamic_overlay_h - 1,
    };
    lv_area_t viewport = {
        .x1 = (int32_t)viewport_x(),
        .y1 = (int32_t)viewport_y(),
        .x2 = (int32_t)(viewport_x() + viewport_w() - 1U),
        .y2 = (int32_t)(viewport_y() + viewport_h() - 1U),
    };
    lv_area_t clipped = overlay;
    if (clipped.x1 < viewport.x1) clipped.x1 = viewport.x1;
    if (clipped.y1 < viewport.y1) clipped.y1 = viewport.y1;
    if (clipped.x2 > viewport.x2) clipped.x2 = viewport.x2;
    if (clipped.y2 > viewport.y2) clipped.y2 = viewport.y2;
    if (clipped.x1 > clipped.x2 || clipped.y1 > clipped.y2) return ESP_OK;

    uint32_t block_w = (uint32_t)lv_area_get_width(&clipped);
    uint32_t block_h = (uint32_t)lv_area_get_height(&clipped);
    uint32_t src_x = (uint32_t)(clipped.x1 - overlay.x1);
    uint32_t src_y = (uint32_t)(clipped.y1 - overlay.y1);
    uint32_t dst_x = (uint32_t)clipped.x1;
    uint32_t dst_y = (uint32_t)clipped.y1;
    ppa_blend_oper_config_t config = {
        .in_bg = {
            .buffer = write_fb,
            .pic_w = canvas_w(),
            .pic_h = canvas_h(),
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = dst_x,
            .block_offset_y = dst_y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = s_ppa.dynamic_overlay_cache,
            .pic_w = s_ppa.dynamic_overlay_w,
            .pic_h = s_ppa.dynamic_overlay_h,
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = src_x,
            .block_offset_y = src_y,
            .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888,
        },
        .out = {
            .buffer = write_fb,
            .buffer_size = PPA_FRAME_BYTES,
            .pic_w = canvas_w(),
            .pic_h = canvas_h(),
            .block_offset_x = dst_x,
            .block_offset_y = dst_y,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .bg_ck_en = false,
        .fg_ck_en = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = ppa_do_blend(s_ppa.blend_client, &config);
    if (blend_us) *blend_us = (uint32_t)(esp_timer_get_time() - started_us);
    return ret;
}

// 在 PPA 帧提交前重新栅格化秒表主卡，保证快照中的毫秒数字、状态和按钮状态是最新的。
static esp_err_t capture_dynamic_overlay(void)
{
    if (!s_ppa.dynamic_overlay || !s_ppa.enabled) {
        s_ppa.dynamic_overlay_valid = false;
        s_ppa.dynamic_overlay_last_capture_us = 0;
        return ESP_OK;
    }
    // 主卡上的毫秒数字由单调时钟持续计算，但滚动期间不必每个 PPA 帧都重新
    // 走一次 LVGL 快照。快照本身是软件栅格化热点；复用最近一帧可保持计时
    // 真实、滚动画面连续，同时把 PPA 留给真正的整区搬运和合成。
    int64_t now_us = esp_timer_get_time();
    if (s_ppa.dynamic_overlay_valid && s_ppa.dynamic_overlay_last_capture_us > 0 &&
        now_us - s_ppa.dynamic_overlay_last_capture_us < PPA_DYNAMIC_OVERLAY_REFRESH_US) {
        return ESP_OK;
    }
    lv_obj_update_layout(s_ppa.dynamic_overlay);
    int32_t ext_size = lv_obj_get_ext_draw_size(s_ppa.dynamic_overlay);
    int32_t width = lv_obj_get_width(s_ppa.dynamic_overlay) + ext_size * 2;
    int32_t height = lv_obj_get_height(s_ppa.dynamic_overlay) + ext_size * 2;
    if (width <= 0 || height <= 0 ||
        (uint32_t)width > PPA_DYNAMIC_OVERLAY_MAX_W ||
        (uint32_t)height > PPA_DYNAMIC_OVERLAY_MAX_H) {
        s_ppa.dynamic_overlay_valid = false;
        s_ppa.dynamic_overlay_last_capture_us = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    size_t required = (size_t)width * (size_t)height * PPA_DYNAMIC_OVERLAY_BPP;
    if (required > s_ppa.dynamic_overlay_capacity || !s_ppa.dynamic_overlay_cache) {
        uint8_t *next = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, required,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) {
            s_ppa.dynamic_overlay_valid = false;
            s_ppa.dynamic_overlay_last_capture_us = 0;
            return ESP_ERR_NO_MEM;
        }
        if (s_ppa.dynamic_overlay_cache) heap_caps_free(s_ppa.dynamic_overlay_cache);
        s_ppa.dynamic_overlay_cache = next;
        s_ppa.dynamic_overlay_capacity = required;
    }
    if (lv_draw_buf_init(&s_ppa.dynamic_overlay_draw_buf, (uint32_t)width,
                         (uint32_t)height, LV_COLOR_FORMAT_ARGB8888,
                         (uint32_t)width * PPA_DYNAMIC_OVERLAY_BPP,
                         s_ppa.dynamic_overlay_cache,
                         (uint32_t)s_ppa.dynamic_overlay_capacity) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(s_ppa.dynamic_overlay, LV_COLOR_FORMAT_ARGB8888,
                                     &s_ppa.dynamic_overlay_draw_buf) != LV_RESULT_OK) {
        s_ppa.dynamic_overlay_valid = false;
        s_ppa.dynamic_overlay_last_capture_us = 0;
        return ESP_FAIL;
    }
    lv_area_t coords;
    lv_obj_get_coords(s_ppa.dynamic_overlay, &coords);
    s_ppa.dynamic_overlay_x = coords.x1 - ext_size;
    s_ppa.dynamic_overlay_y = coords.y1 - ext_size;
    s_ppa.dynamic_overlay_w = s_ppa.dynamic_overlay_draw_buf.header.w;
    s_ppa.dynamic_overlay_h = s_ppa.dynamic_overlay_draw_buf.header.h;
    s_ppa.dynamic_overlay_valid = true;
    s_ppa.dynamic_overlay_last_capture_us = now_us;
    return ESP_OK;
}

static esp_err_t get_work_frame(void **out_frame)
{
    if (!out_frame) return ESP_ERR_INVALID_ARG;
    if (s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT) {
        lv_draw_buf_t *active = lv_display_get_buf_active(s_ppa.display);
        if (!active || !active->data) return ESP_ERR_INVALID_STATE;
        *out_frame = active->data;
        return ESP_OK;
    }
    void *other_fb = NULL;
    return fridge_display_mipi_get_ui_frame_buffers(out_frame, &other_fb);
}

static esp_err_t present_work_frame(const void *frame)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    if (s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT) {
        return s_ppa.present_cb ?
               s_ppa.present_cb(frame, s_ppa.present_user_ctx) :
               ESP_ERR_INVALID_STATE;
    }
    return fridge_display_mipi_present_frame((void *)frame);
}

static esp_err_t copy_and_present(int32_t scroll_y, bool record_perf)
{
    void *write_fb = NULL;
    esp_err_t ret = get_work_frame(&write_fb);
    if (ret != ESP_OK) return ret;

    uint32_t copy_us = 0;
    uint32_t blend_us = 0;
    esp_err_t overlay_ret = capture_dynamic_overlay();
    if (overlay_ret != ESP_OK) {
        return overlay_ret;
    }
    wdt_trace_set(PPA_WDT_STAGE_HW_COPY);
    ret = copy_cached_view(write_fb, scroll_y, &copy_us);
    if (ret != ESP_OK) return ret;
    ret = blend_dynamic_overlay(write_fb, &blend_us);
    if (ret != ESP_OK) return ret;
    // PPA 搬运是同步硬件操作；让同优先级的 USB/系统任务有机会运行，
    // 避免快速连续拖动时长时间占住 UI 任务调度窗口。
    taskYIELD();
    int64_t present_started_us = esp_timer_get_time();
    wdt_trace_set(PPA_WDT_STAGE_DSI_PRESENT);
    ret = present_work_frame(write_fb);
    uint32_t present_us = (uint32_t)(esp_timer_get_time() - present_started_us);
    wdt_trace_set(PPA_WDT_STAGE_ACTIVE_IDLE);
    if (ret == ESP_OK && record_perf) {
        perf_record(esp_timer_get_time(), copy_us + blend_us, present_us, scroll_y);
    }
    return ret;
}

// 手工翻页结束时让驱动的下一可写缓冲重新等于 LVGL buf_act，避免恢复软件绘制后写到扫描帧。
static esp_err_t synchronize_lvgl_buffer(int32_t scroll_y)
{
    wdt_trace_set(PPA_WDT_STAGE_LVGL_SYNC);
    lv_draw_buf_t *active = lv_display_get_buf_active(s_ppa.display);
    if (!active || !active->data) return ESP_ERR_INVALID_STATE;
    if (s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT) {
        lv_draw_buf_t *buffers[2] = {s_ppa.display->buf_1, s_ppa.display->buf_2};
        for (size_t i = 0; i < 2; ++i) {
            if (!buffers[i] || !buffers[i]->data) return ESP_ERR_INVALID_STATE;
            esp_err_t ret = copy_cached_view(buffers[i]->data, scroll_y, NULL);
            if (ret != ESP_OK) return ret;
        }
        return present_work_frame(active->data);
    }
    void *write_fb = NULL;
    void *other_fb = NULL;
    esp_err_t ret = fridge_display_mipi_get_ui_frame_buffers(&write_fb, &other_fb);
    if (ret != ESP_OK) return ret;

    // PPA 收尾时必须同时满足：两块帧缓冲都是最终滚动位置，且下一写入缓冲
    // 回到 LVGL active。若当前 write_fb 已等于 active，旧实现直接返回，active
    // 中仍可能保留上一滚动位置；下一次毫秒数字局部刷新就会让计次列表跳一下。
    // 此时连续提交两次可刷新两块缓冲并保持原奇偶；否则提交一次即可完成对齐。
    uint32_t present_count = write_fb == active->data ? 2U : 1U;
    for (uint32_t i = 0; i < present_count; ++i) {
        ret = copy_and_present(scroll_y, false);
        if (ret != ESP_OK) return ret;
    }
    ret = fridge_display_mipi_get_ui_frame_buffers(&write_fb, &other_fb);
    return ret == ESP_OK && write_fb == active->data ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void restore_lvgl_refresh(bool full_invalidate)
{
    wdt_trace_set(PPA_WDT_STAGE_RESTORE);
    lv_obj_set_scrollbar_mode(s_ppa.scroll, s_ppa.saved_scrollbar_mode);
    lv_display_enable_invalidation(s_ppa.display, s_ppa.saved_invalidation);
    if (!s_ppa.saved_timer_paused) lv_timer_resume(s_ppa.refresh_timer);
    s_ppa.active = false;
    s_ppa.frame_pending = false;
    s_ppa.finish_pending = false;
    s_ppa.finish_deadline_us = 0;
    s_ppa.indev = NULL;
    if (!s_ppa.saved_invalidation) return;
    if (full_invalidate) {
        lv_obj_invalidate(s_ppa.scroll);
    } else {
        // 两块帧缓冲已经包含相同的最终页面视窗，无需再走约 120ms 的整区软件重绘。
        // 只重绘右缘滚动条条带，恢复 AUTO 样式且不破坏 PPA 带来的流畅度。
        lv_area_t area;
        lv_obj_get_coords(s_ppa.scroll, &area);
        area.x1 = area.x2 - 7;
        lv_obj_invalidate_area(s_ppa.scroll, &area);
    }
    wdt_trace_set(PPA_WDT_STAGE_IDLE);
}

static void log_gesture_summary(int32_t scroll_y)
{
    if (s_ppa.gesture_frame_count == 0 || s_ppa.gesture_start_us == 0) return;
    int64_t elapsed_us = esp_timer_get_time() - s_ppa.gesture_start_us;
    uint32_t fps_x10 = elapsed_us > 0 ?
                       (uint32_t)((uint64_t)s_ppa.gesture_frame_count * 10000000ULL /
                                  (uint64_t)elapsed_us) : 0;
    uint32_t copy_avg = (uint32_t)(s_ppa.gesture_copy_total_us /
                                   s_ppa.gesture_frame_count);
    uint32_t present_avg = (uint32_t)(s_ppa.gesture_present_total_us /
                                      s_ppa.gesture_frame_count);
    ESP_LOGI(TAG,
             "gesture end frames=%lu fps=%lu.%lu copy=%lu.%lums(max %lu.%lu) present=%lu.%lums(max %lu.%lu) y=%ld",
             (unsigned long)s_ppa.gesture_frame_count,
             (unsigned long)(fps_x10 / 10U), (unsigned long)(fps_x10 % 10U),
             (unsigned long)(copy_avg / 1000U), (unsigned long)((copy_avg % 1000U) / 100U),
             (unsigned long)(s_ppa.gesture_copy_max_us / 1000U),
             (unsigned long)((s_ppa.gesture_copy_max_us % 1000U) / 100U),
             (unsigned long)(present_avg / 1000U),
             (unsigned long)((present_avg % 1000U) / 100U),
             (unsigned long)(s_ppa.gesture_present_max_us / 1000U),
             (unsigned long)((s_ppa.gesture_present_max_us % 1000U) / 100U),
             (long)scroll_y);
}

static void recover_from_accel_error(esp_err_t reason)
{
    ESP_LOGW(TAG, "PPA scroll fallback to LVGL: %s", esp_err_to_name(reason));
    s_ppa.cache_valid = false;
    s_ppa.cache_dirty = true;
    s_ppa.cache_retry_after_us = esp_timer_get_time() + PPA_CACHE_RETRY_US;

    // 即使 PPA 失败，也先用当前安全缓冲补一次翻页，尽量恢复 LVGL/驱动奇偶关系。
    if (s_ppa.profile == FRIDGE_UI_SCROLL_PPA_LANDSCAPE) {
        lv_draw_buf_t *active = lv_display_get_buf_active(s_ppa.display);
        void *write_fb = NULL;
        void *other_fb = NULL;
        if (active && active->data &&
            fridge_display_mipi_get_ui_frame_buffers(&write_fb, &other_fb) == ESP_OK &&
            write_fb != active->data) {
            (void)fridge_display_mipi_present_frame(write_fb);
        }
    }
    restore_lvgl_refresh(true);
}

static esp_err_t ensure_cache_capacity(size_t required)
{
    if (required <= s_ppa.cache_capacity && s_ppa.cache) return ESP_OK;
    uint8_t *next = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, required,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) return ESP_ERR_NO_MEM;
    if (s_ppa.cache) heap_caps_free(s_ppa.cache);
    s_ppa.cache = next;
    s_ppa.cache_capacity = required;
    return ESP_OK;
}

// 缓存构建只发生在 UI 空闲期；临时展开滚动容器，快照完成后恢复原几何和滚动位置。
static esp_err_t rebuild_cache(void)
{
    int32_t saved_y = lv_obj_get_scroll_y(s_ppa.scroll);
    int32_t original_view_height = lv_obj_get_height(s_ppa.scroll);
    lv_scrollbar_mode_t saved_mode = lv_obj_get_scrollbar_mode(s_ppa.scroll);
    bool invalidation = lv_display_is_invalidation_enabled(s_ppa.display);
    bool timer_paused = lv_timer_get_paused(s_ppa.refresh_timer);
    // 程序化滚动只服务于分段快照，不能被 scroll_event_cb 当成用户手势再次接管。
    s_ppa.rebuilding_cache = true;
    lv_display_enable_invalidation(s_ppa.display, false);
    if (!timer_paused) lv_timer_pause(s_ppa.refresh_timer);

    lv_obj_set_scrollbar_mode(s_ppa.scroll, LV_SCROLLBAR_MODE_OFF);
    // 固定底栏覆盖主滚动区时只快照其上方可见正文；真实容器高度会在末尾恢复，
    // 因此不会改变 LVGL 的触摸范围、惯性或最大滚动位置。
    const int32_t view_height = (int32_t)viewport_h();
    lv_obj_set_height(s_ppa.scroll, view_height);
    lv_obj_scroll_to_y(s_ppa.scroll, 0, LV_ANIM_OFF);
    lv_obj_update_layout(s_ppa.scroll);
    int32_t page_h = view_height + lv_obj_get_scroll_bottom(s_ppa.scroll);
    bool cache_not_needed = false;
    size_t required = 0;
    esp_err_t ret = ESP_OK;
    int32_t scroll_w = lv_obj_get_width(s_ppa.scroll);
    const uint32_t expected_w = viewport_w();
    const uint32_t expected_h = viewport_h();
    if (view_height != (int32_t)expected_h || scroll_w != (int32_t)expected_w ||
        page_h > PPA_PAGE_MAX_H) {
        ESP_LOGW(TAG, "cache geometry deferred: scroll=%ldx%ld page_h=%ld limit=%d",
                 (long)scroll_w, (long)view_height, (long)page_h, PPA_PAGE_MAX_H);
        ret = ESP_ERR_INVALID_SIZE;
        goto restore;
    }

    if (page_h < (int32_t)expected_h) {
        // 页面内容没有超过视窗时无需申请快照或每 2 秒重试；计次列表变长后，
        // refresh_stopwatch_page() 会通过 invalidate_cache() 重新触发一次构建。
        s_ppa.page_h = (uint32_t)page_h;
        s_ppa.cache_valid = false;
        s_ppa.cache_dirty = false;
        cache_not_needed = true;
        ret = ESP_OK;
        goto restore;
    }

    const size_t row_bytes = (size_t)expected_w * sizeof(uint16_t);
    const size_t padding_bytes = row_bytes * PPA_ELASTIC_MARGIN;
    const size_t page_bytes = row_bytes * (size_t)page_h;
    const uint32_t cache_h = (uint32_t)page_h + 2U * PPA_ELASTIC_MARGIN;
    required = row_bytes * cache_h;
    ret = ensure_cache_capacity(required);
    if (ret != ESP_OK) goto restore;

    // 快照上下缓冲使用滚动容器自身的 RGB565 背景色，弹性拉开时不会露出旧帧。
    uint16_t background = lv_color_to_u16(
        lv_obj_get_style_bg_color(s_ppa.scroll, LV_PART_MAIN));
    uint16_t *top = (uint16_t *)s_ppa.cache;
    uint16_t *bottom = (uint16_t *)(s_ppa.cache + padding_bytes + page_bytes);
    size_t padding_pixels = (size_t)expected_w * PPA_ELASTIC_MARGIN;
    for (size_t i = 0; i < padding_pixels; ++i) {
        top[i] = background;
        bottom[i] = background;
    }

    // 更多页和长计次页可明显超过一个视窗。一次性软件栅格化整页会长时间占住 UI 核；
    // 按当前 PPA 可见视窗分段写入连续缓存，并在段间让出一个 tick，保留 PPA 加速同时
    // 避免 HP System WDT。每段仍由 LVGL 在唯一 UI 任务内生成，不引入并发访问对象树。
    int32_t snapshot_y = 0;
    while (snapshot_y < page_h) {
        uint32_t chunk_h = (uint32_t)(page_h - snapshot_y);
        if (chunk_h > (uint32_t)view_height) chunk_h = (uint32_t)view_height;
        lv_obj_set_height(s_ppa.scroll, (int32_t)chunk_h);
        lv_obj_scroll_to_y(s_ppa.scroll, snapshot_y, LV_ANIM_OFF);
        lv_obj_update_layout(s_ppa.scroll);

        lv_draw_buf_t draw_buf;
        uint8_t *chunk_target = s_ppa.cache + padding_bytes +
                                (size_t)snapshot_y * row_bytes;
        size_t chunk_bytes = row_bytes * chunk_h;
        wdt_trace_set(PPA_WDT_STAGE_CACHE_SNAPSHOT);
        if (lv_draw_buf_init(&draw_buf, expected_w, chunk_h,
                             LV_COLOR_FORMAT_RGB565,
                             expected_w * sizeof(uint16_t),
                             chunk_target, (uint32_t)chunk_bytes) != LV_RESULT_OK ||
            lv_snapshot_take_to_draw_buf(s_ppa.scroll, LV_COLOR_FORMAT_RGB565,
                                         &draw_buf) != LV_RESULT_OK) {
            ret = ESP_FAIL;
            goto restore;
        }
        wdt_trace_set(PPA_WDT_STAGE_ACTIVE_IDLE);
        snapshot_y += (int32_t)chunk_h;
        if (snapshot_y < page_h) vTaskDelay(1);
    }
    s_ppa.page_h = (uint32_t)page_h;
    s_ppa.cache_valid = true;
    s_ppa.cache_dirty = false;

restore:
    lv_obj_set_height(s_ppa.scroll, original_view_height);
    lv_obj_scroll_to_y(s_ppa.scroll, saved_y, LV_ANIM_OFF);
    lv_obj_set_scrollbar_mode(s_ppa.scroll, saved_mode);
    lv_obj_update_layout(s_ppa.scroll);
    s_ppa.rebuilding_cache = false;
    lv_display_enable_invalidation(s_ppa.display, invalidation);
    if (!timer_paused) lv_timer_resume(s_ppa.refresh_timer);
    if (invalidation) lv_obj_invalidate(s_ppa.scroll);

    if (ret == ESP_OK && !cache_not_needed) {
        // 上电即做一次不翻页的真实 PPA 探针，验证驱动、PSRAM 和对齐；画面内容保持不变。
        void *write_fb = NULL;
        uint32_t probe_us = 0;
        if (get_work_frame(&write_fb) == ESP_OK) {
            ret = copy_cached_view(write_fb, saved_y, &probe_us);
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "cache ready %ux%lu RGB565 + %upx elastic (%luKB), PPA probe=%lu.%lums",
                     expected_w, (unsigned long)s_ppa.page_h,
                     PPA_ELASTIC_MARGIN,
                     (unsigned long)(required / 1024U),
                     (unsigned long)(probe_us / 1000U),
                     (unsigned long)((probe_us % 1000U) / 100U));
        } else {
            s_ppa.cache_valid = false;
            s_ppa.cache_dirty = true;
        }
    }
    return ret;
}

static void scroll_event_cb(lv_event_t *event)
{
    if (lv_event_get_target(event) != s_ppa.scroll) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (s_ppa.rebuilding_cache) return;
    if (code == LV_EVENT_SCROLL_BEGIN) {
        if (s_ppa.active) {
            // 到顶/到底时 LVGL 可能在手指仍按住期间反复发出 BEGIN/END。
            // 保持同一次 PPA 会话，取消尚未执行的收尾，避免重复翻页和恢复 LVGL。
            s_ppa.finish_pending = false;
            s_ppa.finish_deadline_us = 0;
            s_ppa.frame_pending = true;
            lv_indev_t *active = lv_indev_active();
            if (active) s_ppa.indev = active;
            return;
        }
        if (!s_ppa.enabled) return;
        if (!s_ppa.cache_valid || s_ppa.cache_dirty ||
            fridge_display_mipi_camera_owns_framebuffers()) {
            return;
        }
        s_ppa.saved_invalidation = lv_display_is_invalidation_enabled(s_ppa.display);
        s_ppa.saved_timer_paused = lv_timer_get_paused(s_ppa.refresh_timer);
        s_ppa.saved_scrollbar_mode = lv_obj_get_scrollbar_mode(s_ppa.scroll);
        if (s_ppa.saved_invalidation) {
            // SCROLL_BEGIN 到达时 LVGL 已经把首个滚动位置加入 invalid 区。
            // 若不清掉，恢复刷新后仍会补一次 704x520 软件重绘，连续手势会积累卡顿。
            // 当前工程固定 LVGL 9.2.2；lv_inv_area(NULL) 是该版本内部清空 invalid 队列的入口。
            lv_inv_area(s_ppa.display, NULL);
        }
        // DIRECT 双缓冲还有一条独立的 sync_areas 链表。先按 LVGL 原生方向完成复制，
        // 再清队列，既避免连续 PPA 手势积累旧区域，也不能丢失时间等右侧动态更新。
        wdt_trace_set(PPA_WDT_STAGE_SYNC_LIST_CLEAR);
        esp_err_t sync_ret = synchronize_pending_direct_areas();
        wdt_trace_set(PPA_WDT_STAGE_ACTIVE_IDLE);
        if (sync_ret != ESP_OK) {
            ESP_LOGW(TAG, "pending DIRECT buffer sync failed; keep LVGL path: %s",
                     esp_err_to_name(sync_ret));
            return;
        }
        esp_err_t seed_ret = seed_ppa_write_buffer_from_visible();
        if (seed_ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA fixed-region seed failed; keep LVGL path: %s",
                     esp_err_to_name(seed_ret));
            return;
        }
        lv_display_enable_invalidation(s_ppa.display, false);
        if (!s_ppa.saved_timer_paused) lv_timer_pause(s_ppa.refresh_timer);
        lv_obj_set_scrollbar_mode(s_ppa.scroll, LV_SCROLLBAR_MODE_OFF);
        s_ppa.active = true;
        s_ppa.frame_pending = true;
        s_ppa.finish_pending = false;
        s_ppa.finish_deadline_us = 0;
        s_ppa.indev = lv_indev_active();
        s_ppa.has_presented_y = false;
        s_ppa.gesture_start_us = esp_timer_get_time();
        s_ppa.gesture_copy_total_us = 0;
        s_ppa.gesture_present_total_us = 0;
        s_ppa.gesture_frame_count = 0;
        s_ppa.gesture_copy_max_us = 0;
        s_ppa.gesture_present_max_us = 0;
        s_ppa.perf_last_frame_us = 0;
        perf_reset();
    } else if (code == LV_EVENT_SCROLL && s_ppa.active) {
        // 同一 UI 周期中的多次惯性更新只提交最后一个位置，避免 PPA 排队和输入延迟。
        s_ppa.frame_pending = true;
        s_ppa.finish_pending = false;
        s_ppa.finish_deadline_us = 0;
    } else if (code == LV_EVENT_SCROLL_END && s_ppa.active) {
        s_ppa.frame_pending = true;
        s_ppa.finish_pending = true;
        s_ppa.finish_deadline_us = esp_timer_get_time() + PPA_FINISH_GRACE_US;
    }
}

esp_err_t fridge_ui_scroll_ppa_init(lv_display_t *display, lv_obj_t *main_scroll)
{
    if (!display || !main_scroll) return ESP_ERR_INVALID_ARG;
    if (s_ppa.initialized) return ESP_OK;

    const esp_reset_reason_t reset_reason = esp_reset_reason();
    if (s_wdt_trace.magic == PPA_WDT_TRACE_MAGIC &&
        s_wdt_trace.stage != PPA_WDT_STAGE_IDLE &&
        (reset_reason == ESP_RST_INT_WDT || reset_reason == ESP_RST_TASK_WDT ||
         reset_reason == ESP_RST_WDT)) {
        ESP_LOGE(TAG, "previous WDT occurred at PPA stage=%lu",
                 (unsigned long)s_wdt_trace.stage);
    }
    wdt_trace_set(PPA_WDT_STAGE_IDLE);

    ppa_client_config_t client_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        // PPA 每帧需要搬运约 700KB 的滚动视窗；P4 的 128B burst 能减少 PSRAM
        // 事务次数。DPI 仍持续扫描另一块双缓冲，若后续出现花屏再回退到 64B。
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&client_config, &s_ppa.client);
    if (ret != ESP_OK) return ret;
    ppa_client_config_t blend_client_config = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        // 动态秒表数字只覆盖小块 ARGB8888，使用最大 burst 降低 Blend 开销。
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ret = ppa_register_client(&blend_client_config, &s_ppa.blend_client);
    if (ret != ESP_OK) return ret;

    s_ppa.display = display;
    s_ppa.refresh_timer = lv_display_get_refr_timer(display);
    s_ppa.initialized = true;
    ret = fridge_ui_scroll_ppa_bind(main_scroll, FRIDGE_UI_SCROLL_PPA_LANDSCAPE,
                                    NULL, NULL);
    if (ret != ESP_OK) return ret;
    s_ppa.enabled = true;
    ESP_LOGI(TAG, "PPA HOME scroll accelerator initialized (no 30fps cap)");
    return ESP_OK;
}

esp_err_t fridge_ui_scroll_ppa_bind(lv_obj_t *scroll,
                                    fridge_ui_scroll_ppa_profile_t profile,
                                    fridge_ui_scroll_ppa_present_cb_t present_cb,
                                    void *user_ctx)
{
    if (!s_ppa.initialized || !scroll || !lv_obj_is_valid(scroll)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile != FRIDGE_UI_SCROLL_PPA_LANDSCAPE &&
        profile != FRIDGE_UI_SCROLL_PPA_PORTRAIT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT && !present_cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ppa.active) return ESP_ERR_INVALID_STATE;

    if (s_ppa.scroll && s_ppa.scroll != scroll && lv_obj_is_valid(s_ppa.scroll)) {
        lv_obj_remove_event_cb(s_ppa.scroll, scroll_event_cb);
    }
    if (s_ppa.scroll != scroll) {
        lv_obj_add_event_cb(scroll, scroll_event_cb, LV_EVENT_ALL, NULL);
    }
    s_ppa.scroll = scroll;
    s_ppa.profile = profile;
    s_ppa.present_cb = present_cb;
    s_ppa.present_user_ctx = user_ctx;
    s_ppa.dynamic_overlay = NULL;
    s_ppa.dynamic_overlay_valid = false;
    s_ppa.cache_valid = false;
    s_ppa.cache_dirty = true;
    s_ppa.cache_retry_after_us = esp_timer_get_time() + PPA_CACHE_REBUILD_DEBOUNCE_US;
    ESP_LOGI(TAG, "PPA scroll rebound profile=%s viewport=%lux%lu",
             profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ? "portrait" : "landscape",
             (unsigned long)viewport_w(), (unsigned long)viewport_h());
    return ESP_OK;
}

void fridge_ui_scroll_ppa_set_enabled(bool enabled)
{
    if (!s_ppa.initialized) return;
    bool changed = s_ppa.enabled != enabled;
    s_ppa.enabled = enabled;
    if (!enabled) {
        // 页面切换发生在 LVGL 任务中；若当前手势尚未收尾，先让下一轮
        // process 完成恢复，再由页面切换末尾统一使旧快照失效。
        if (s_ppa.active) {
            s_ppa.frame_pending = true;
            s_ppa.finish_pending = true;
            s_ppa.finish_deadline_us = 0;
            s_ppa.indev = NULL;
        }
        return;
    }
    if (changed) {
        // 回到 HOME 后必须重新抓取当前页面，不能复用工具页旧快照。
        s_ppa.cache_valid = false;
        s_ppa.cache_dirty = true;
        s_ppa.cache_retry_after_us = esp_timer_get_time() + PPA_CACHE_REBUILD_DEBOUNCE_US;
    }
}

void fridge_ui_scroll_ppa_set_viewport_height(uint32_t height)
{
    if (!s_ppa.initialized) return;
    const uint32_t maximum = s_ppa.profile == FRIDGE_UI_SCROLL_PPA_PORTRAIT ?
                             PPA_PORTRAIT_SCROLL_H : PPA_LANDSCAPE_SCROLL_H;
    if (height > maximum) height = maximum;
    if (height == s_ppa.viewport_height_override) return;
    s_ppa.viewport_height_override = height;
    fridge_ui_scroll_ppa_invalidate_cache();
}

void fridge_ui_scroll_ppa_set_dynamic_overlay(lv_obj_t *object)
{
    if (!s_ppa.initialized) return;
    if (object && object != s_ppa.scroll && !lv_obj_is_valid(object)) return;
    s_ppa.dynamic_overlay = object;
    s_ppa.dynamic_overlay_valid = false;
    s_ppa.dynamic_overlay_last_capture_us = 0;
}

void fridge_ui_scroll_ppa_invalidate_cache(void)
{
    if (!s_ppa.initialized) return;
    s_ppa.cache_valid = false;
    s_ppa.cache_dirty = true;
    s_ppa.cache_retry_after_us = esp_timer_get_time() + PPA_CACHE_REBUILD_DEBOUNCE_US;
    if (s_ppa.active) {
        s_ppa.finish_pending = true;
        s_ppa.finish_deadline_us = 0;
        s_ppa.indev = NULL;
    }
}

void fridge_ui_scroll_ppa_process(void)
{
    if (!s_ppa.initialized || fridge_display_mipi_camera_owns_framebuffers()) return;
    if (!s_ppa.active) {
        if (!s_ppa.enabled) return;
        if (s_ppa.cache_dirty && esp_timer_get_time() >= s_ppa.cache_retry_after_us) {
            esp_err_t ret = rebuild_cache();
            if (ret != ESP_OK) {
                s_ppa.cache_retry_after_us = esp_timer_get_time() + PPA_CACHE_RETRY_US;
                ESP_LOGW(TAG, "cache build deferred, LVGL fallback: %s", esp_err_to_name(ret));
            } else {
                s_ppa.cache_retry_after_us = 0;
            }
        }
        return;
    }
    if (!s_ppa.frame_pending && !s_ppa.finish_pending) return;

    int32_t scroll_y = lv_obj_get_scroll_y(s_ppa.scroll);
    int32_t clamped_y = clamp_cached_scroll_y(scroll_y);
    esp_err_t ret = ESP_OK;
    if (s_ppa.frame_pending &&
        (!s_ppa.has_presented_y || clamped_y != s_ppa.last_presented_y)) {
        ret = copy_and_present(clamped_y, true);
        if (ret != ESP_OK) {
            recover_from_accel_error(ret);
            return;
        }
        s_ppa.last_presented_y = clamped_y;
        s_ppa.has_presented_y = true;
    }
    s_ppa.frame_pending = false;
    if (s_ppa.finish_pending) {
        // 边界 SCROLL_END 可能发生在手指仍按下时；等真实释放且短暂静止后只收尾一次。
        if ((s_ppa.indev && lv_indev_get_state(s_ppa.indev) == LV_INDEV_STATE_PRESSED) ||
            esp_timer_get_time() < s_ppa.finish_deadline_us) {
            return;
        }
        ret = synchronize_lvgl_buffer(clamped_y);
        if (ret != ESP_OK) {
            recover_from_accel_error(ret);
            return;
        }
        log_gesture_summary(clamped_y);
        restore_lvgl_refresh(false);
    }
}

void fridge_ui_scroll_ppa_request_finish(void)
{
    if (!s_ppa.active) return;
    s_ppa.frame_pending = true;
    s_ppa.finish_pending = true;
    s_ppa.finish_deadline_us = 0;
    s_ppa.indev = NULL;
}

bool fridge_ui_scroll_ppa_is_active(void)
{
    return s_ppa.active;
}
