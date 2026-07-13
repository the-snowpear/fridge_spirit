// 冰箱小精灵 MIPI-DSI 屏幕（WKS70WSV114-WCT / EK79007AD3）驱动实现。
// 移植自厂商已在本板真机点亮的 09_mipilcd 例程，仅保留 EK79007（7"1024x600）路径。
//
// 初始化链路：DSI PHY 上电(LDO_VO3/2.5V) → DSI 总线(2 lane/700Mbps) → DBI 命令通道
//   → EK79007 复位+寄存器初始化(0x80~0xB2) → MADCTL/COLMOD/DISPON → DPI 视频通道(50MHz)
//   → 取双帧缓冲 → 背光点亮。
//
// 硬件注意：
//   - 屏 VCC/背光走 26-pin DSI 座，逻辑电平 3.3V；RST=GPIO22、BL=GPIO23 是普通 GPIO。
//   - DSI PHY 电源固定用片内 LDO 通道 3（LDO_VO3→VDD_MIPI_DPHY），与 PSRAM(2)/TF+C6(4) 不冲突。
//   - 双帧缓冲各 ~1.2MB，DPI 驱动自动从 PSRAM 分配；请确认 32MB PSRAM 已在 200MHz 通过 memtest。
//   - EK79007 无需 SLPOUT(0x11)，厂商序列直接 9 条配置寄存器 + COLMOD + DISPON。

#include "fridge_display_mipi.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fridge_board_pins.h"

static const char *TAG = "fridge_mipi";

// ---- EK79007 / DSI 固定参数（厂商 09_mipilcd 已验证值）----
#define MIPI_DSI_LANE_NUM            2      // 2 路数据线
#define MIPI_DSI_LANE_BITRATE_MBPS   700    // 每通道比特率
#define MIPI_DSI_PHY_LDO_CHAN        3      // LDO_VO3 → VDD_MIPI_DPHY
#define MIPI_DSI_PHY_LDO_VOLTAGE_MV  2500   // MIPI D-PHY 供电 2.5V
#define MIPI_DPI_CLOCK_MHZ           50     // DPI 像素时钟 50MHz
#define MIPI_VIDEO_REARM_DELAY_MS     40     // 覆盖两个以上帧周期，确保 P4 rev1.x 视频桥完成重新武装
#define MIPI_FIRST_FRAME_TIMEOUT_MS  2000   // DSI/DPI 首帧启动可能明显慢于稳定刷新
#define MIPI_FRAME_TIMEOUT_MS         500   // 稳定运行阶段仍保留有界等待，避免任务永久卡死

// EK79007 面板时序（PCLK 周期数 / 行数），横屏 1024x600。
#define EK79007_HBP  160
#define EK79007_HFP  160
#define EK79007_HSW  20
#define EK79007_VBP  23
#define EK79007_VFP  12
#define EK79007_VSW  10

#define MIPI_PIN_RST ((gpio_num_t)FRIDGE_PIN_LCD_RST)   // GPIO22，板载 LCD_RST（DSI 复位在板上被分压到 1.8V）
#define MIPI_PIN_BL  ((gpio_num_t)FRIDGE_PIN_MIPI_LCD_BL)  // GPIO23，背光使能，高有效

// 单条 EK79007 初始化命令。
typedef struct {
    uint8_t cmd;
    uint8_t data;
} ek79007_cmd_t;

// 厂商 EK79007 初始化寄存器序列（2 lane），逐条单参数写入。
static const ek79007_cmd_t s_ek79007_init[] = {
    {0x80, 0x8B},
    {0x81, 0x78},
    {0x82, 0x84},
    {0x83, 0x88},
    {0x84, 0xA8},
    {0x85, 0xE3},
    {0x86, 0x88},
    {0xB1, 0x04},
    {0xB2, 0x10},   // 2 lane MIPI
};

static esp_ldo_channel_handle_t s_phy_ldo;   // DSI PHY 供电 LDO 句柄
static esp_lcd_dsi_bus_handle_t s_dsi_bus;    // DSI 总线句柄
static esp_lcd_panel_io_handle_t s_dbi_io;    // DBI 命令通道句柄
static esp_lcd_panel_handle_t s_dpi_panel;    // DPI 视频面板句柄
static void *s_fb[2];                         // DPI 双帧缓冲（PSRAM）
static uint8_t s_fb_index;                    // 当前可写帧缓冲索引
static SemaphoreHandle_t s_refresh_sem;       // 帧刷新完成信号
static volatile uint32_t s_refresh_count;     // ISR 刷新计数，仅用于首帧/超时诊断
static bool s_first_frame;
static bool s_ready;

// 初始化中途失败时按依赖逆序释放资源，并保持背光关闭。
static void mipi_cleanup(void)
{
    gpio_set_level(MIPI_PIN_BL, 0);
    s_ready = false;
    s_fb[0] = NULL;
    s_fb[1] = NULL;
    s_fb_index = 0;
    s_first_frame = false;

    if (s_dpi_panel) {
        esp_lcd_panel_del(s_dpi_panel);
        s_dpi_panel = NULL;
    }
    if (s_dbi_io) {
        esp_lcd_panel_io_del(s_dbi_io);
        s_dbi_io = NULL;
    }
    if (s_dsi_bus) {
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
    }
    if (s_phy_ldo) {
        esp_ldo_release_channel(s_phy_ldo);
        s_phy_ldo = NULL;
    }
    if (s_refresh_sem) {
        vSemaphoreDelete(s_refresh_sem);
        s_refresh_sem = NULL;
    }
}

// DPI 内部帧缓冲刷新完成回调（ISR 上下文）：释放刷新完成信号。
static IRAM_ATTR bool mipi_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                           esp_lcd_dpi_panel_event_data_t *edata,
                                           void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_refresh_count++;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_refresh_sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

// 复位 EK79007：厂商时序 1→(5ms)→0→(10ms)→1→(120ms)，复位低有效。
static esp_err_t ek79007_hard_reset(void)
{
    gpio_config_t rst_conf = {
        .pin_bit_mask = 1ULL << MIPI_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_conf), TAG, "configure panel reset failed");

    ESP_RETURN_ON_ERROR(gpio_set_level(MIPI_PIN_RST, 1), TAG, "set panel reset high failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gpio_set_level(MIPI_PIN_RST, 0), TAG, "set panel reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(MIPI_PIN_RST, 1), TAG, "release panel reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

// 打开 DSI PHY 电源（LDO_VO3 输出 2.5V 给 VDD_MIPI_DPHY）。
static esp_err_t mipi_enable_phy_power(void)
{
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_VOLTAGE_MV,
    };
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "acquire MIPI PHY LDO chan%d failed: %s", MIPI_DSI_PHY_LDO_CHAN, esp_err_to_name(ret));
    }
    return ret;
}

// 通过 DBI 命令通道发送 EK79007 初始化寄存器与显示开启序列。
static esp_err_t ek79007_send_init_seq(void)
{
    for (size_t i = 0; i < sizeof(s_ek79007_init) / sizeof(s_ek79007_init[0]); i++) {
        uint8_t param = s_ek79007_init[i].data;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi_io, s_ek79007_init[i].cmd, &param, 1),
                            TAG, "EK79007 init cmd 0x%02x failed", s_ek79007_init[i].cmd);
    }

    // 扫描方向不翻转（横屏正装）：MADCTL=0x00。
    uint8_t madctl = 0x00;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi_io, LCD_CMD_MADCTL, &madctl, 1),
                        TAG, "MADCTL failed");
    // 像素格式 RGB565：COLMOD=0x55。
    uint8_t colmod = 0x55;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi_io, LCD_CMD_COLMOD, &colmod, 1),
                        TAG, "COLMOD failed");
    // 开显示。
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi_io, LCD_CMD_DISPON, NULL, 0),
                        TAG, "DISPON failed");
    return ESP_OK;
}

// 重新武装 IDF 5.5.1 的 DSI 视频桥。
// ESP32-P4 rev1.x 实测首次 DW-GDMA 完成后可能只输出一帧；官方 Pattern Generator
// 会先关闭 DPI bridge，切回 NONE 时再打开，从而恢复连续 DMA。此过程在背光关闭时完成。
static esp_err_t mipi_rearm_video_bridge(void)
{
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_set_pattern(s_dpi_panel, MIPI_DSI_PATTERN_BAR_VERTICAL),
                        TAG, "enable DSI bridge rearm pattern failed");
    vTaskDelay(pdMS_TO_TICKS(MIPI_VIDEO_REARM_DELAY_MS));
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_set_pattern(s_dpi_panel, MIPI_DSI_PATTERN_NONE),
                        TAG, "disable DSI bridge rearm pattern failed");
    ESP_LOGI(TAG, "DSI video bridge rearmed (%d ms, backlight off)", MIPI_VIDEO_REARM_DELAY_MS);
    return ESP_OK;
}

// 把整帧内容提交给 DPI 面板并等待刷新完成（阻塞）。fb 必须是内部帧缓冲之一。
static esp_err_t mipi_flush_full_fb(void *fb)
{
    const uint32_t refresh_before = s_refresh_count;
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_dpi_panel, 0, 0, FRIDGE_MIPI_WIDTH, FRIDGE_MIPI_HEIGHT, fb);
    if (ret != ESP_OK) {
        return ret;
    }
    // 回调在 DPI 连续扫描时每帧都会触发。切换缓冲后再清掉已有 token，确保随后等待到的
    // 一定是本次切换之后的刷新边界；最坏多等一帧，但不会覆写仍在扫描的缓冲。
    while (xSemaphoreTake(s_refresh_sem, 0) == pdTRUE) {
    }
    // 厂商例程对首帧采用无界等待；这里保留保护上限，但首帧给予更长的 DSI/DPI 启动时间。
    const uint32_t timeout_ms = s_first_frame ? MIPI_FIRST_FRAME_TIMEOUT_MS : MIPI_FRAME_TIMEOUT_MS;
    if (xSemaphoreTake(s_refresh_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "wait refresh done timeout: first=%d timeout=%lu ms refresh_count=%lu->%lu",
                 s_first_frame,
                 (unsigned long)timeout_ms,
                 (unsigned long)refresh_before,
                 (unsigned long)s_refresh_count);
        // draw_bitmap 已可能切换 DPI 当前缓冲，继续复用本地索引会覆写活动帧；关闭背光并锁死刷新。
        gpio_set_level(MIPI_PIN_BL, 0);
        s_ready = false;
        return ESP_ERR_TIMEOUT;
    }
    s_first_frame = false;
    return ESP_OK;
}

esp_err_t fridge_display_mipi_init(void)
{
    esp_err_t ret = ESP_OK;

    if (s_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "init EK79007 MIPI-DSI: %dx%d, %d lane @ %d Mbps, DPI %d MHz, RST=%d BL=%d",
             FRIDGE_MIPI_WIDTH, FRIDGE_MIPI_HEIGHT, MIPI_DSI_LANE_NUM,
             MIPI_DSI_LANE_BITRATE_MBPS, MIPI_DPI_CLOCK_MHZ, MIPI_PIN_RST, MIPI_PIN_BL);

    s_refresh_sem = xSemaphoreCreateBinary();
    if (!s_refresh_sem) {
        return ESP_ERR_NO_MEM;
    }
    s_refresh_count = 0;
    s_first_frame = true;

    // 背光引脚先拉低，等整屏刷成黑色后再打开，避免上电初始化时闪花屏。
    gpio_config_t bl_conf = {
        .pin_bit_mask = 1ULL << MIPI_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&bl_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "configure backlight failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = gpio_set_level(MIPI_PIN_BL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "disable backlight failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // 1) DSI PHY 供电。
    ret = mipi_enable_phy_power();
    if (ret != ESP_OK) {
        goto fail;
    }

    // 2) DSI 总线。
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ret = esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new DSI bus failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // 3) DBI 命令通道（EK79007 命令/参数均为 8bit）。
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new DBI io failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // 4) 硬复位 + 发送 EK79007 初始化序列。
    ret = ek79007_hard_reset();
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = ek79007_send_init_seq();
    if (ret != ESP_OK) {
        goto fail;
    }

    // 5) DPI 视频通道：RGB565、双帧缓冲、厂商时序。
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = MIPI_DPI_CLOCK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = FRIDGE_MIPI_WIDTH,
            .v_size = FRIDGE_MIPI_HEIGHT,
            .hsync_back_porch = EK79007_HBP,
            .hsync_pulse_width = EK79007_HSW,
            .hsync_front_porch = EK79007_HFP,
            .vsync_back_porch = EK79007_VBP,
            .vsync_pulse_width = EK79007_VSW,
            .vsync_front_porch = EK79007_VFP,
        },
    };
    ret = esp_lcd_new_panel_dpi(s_dsi_bus, &dpi_cfg, &s_dpi_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new DPI panel failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = esp_lcd_panel_init(s_dpi_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DPI panel init failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // 6) 注册刷新完成回调，取双帧缓冲。
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_refresh_done = mipi_refresh_done_cb,
    };
    ret = esp_lcd_dpi_panel_register_event_callbacks(s_dpi_panel, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register DPI callback failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = esp_lcd_dpi_panel_get_frame_buffer(s_dpi_panel, 2, &s_fb[0], &s_fb[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get frame buffer failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    // IDF 5.5.1 + P4 rev1.x 的首帧 DMA 可能不会自动续传；在背光关闭期间切换一次
    // 官方内建图案，重新打开 DPI bridge 后再提交首个可见帧。
    ret = mipi_rearm_video_bridge();
    if (ret != ESP_OK) {
        goto fail;
    }
    // 与厂商 7 寸例程保持一致：DPI 初始化后先在当前 fb0 完成一帧黑屏，再切换到 fb1。
    // 此时背光仍关闭，不会暴露首帧写入；P4 rev1.x 上也避免 DMA 刚启动就立即换 link-list。
    s_fb_index = 0;
    s_ready = true;

    // 7) 先刷黑再开背光。
    esp_err_t clr = fridge_display_mipi_fill_screen(0x0000);
    if (clr != ESP_OK) {
        ESP_LOGE(TAG, "initial clear failed: %s", esp_err_to_name(clr));
        ret = clr;
        goto fail;
    }
    ret = gpio_set_level(MIPI_PIN_BL, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable backlight failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ESP_LOGI(TAG, "EK79007 MIPI-DSI ready (fb0=%p fb1=%p)", s_fb[0], s_fb[1]);
    return ESP_OK;

fail:
    mipi_cleanup();
    return ret;
}

esp_err_t fridge_display_mipi_fill_screen(uint16_t rgb565)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    // 向当前可写帧缓冲写入单色，再整帧提交并翻页。
    uint16_t *fb = (uint16_t *)s_fb[s_fb_index];
    const size_t count = (size_t)FRIDGE_MIPI_WIDTH * FRIDGE_MIPI_HEIGHT;
    for (size_t i = 0; i < count; i++) {
        fb[i] = rgb565;
    }
    esp_err_t ret = mipi_flush_full_fb(fb);
    if (ret == ESP_OK) {
        s_fb_index ^= 1;
    }
    return ret;
}

esp_err_t fridge_display_mipi_render(fridge_mipi_paint_cb_t paint, void *ctx)
{
    if (!s_ready || !paint) {
        return ESP_ERR_INVALID_STATE;
    }
    // 在当前离屏帧缓冲上绘制整屏图案，再整帧提交并翻页（零拷贝双缓冲）。
    uint16_t *fb = (uint16_t *)s_fb[s_fb_index];
    paint(fb, FRIDGE_MIPI_WIDTH, FRIDGE_MIPI_HEIGHT, ctx);
    esp_err_t ret = mipi_flush_full_fb(fb);
    if (ret == ESP_OK) {
        s_fb_index ^= 1;
    }
    return ret;
}

void fridge_display_mipi_set_backlight(bool on)
{
    if (s_ready) {
        gpio_set_level(MIPI_PIN_BL, on ? 1 : 0);
    }
}

bool fridge_display_mipi_is_ready(void)
{
    return s_ready;
}
