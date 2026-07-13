// 冰箱小精灵 WKS70WSV114-WCT MIPI-DSI 独立点屏测试。
// 测试只初始化 7 寸 EK79007 显示，不启动 Wi-Fi、音频、TF、摄像头或 GT911 触摸。

#include "fridge_display_mipi_test.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fridge_display_mipi.h"

static const char *TAG = "mipi_test";

typedef struct {
    const char *name;
    uint16_t color;
} solid_pattern_t;

// 标准 RGB565 垂直彩条：白、黄、青、绿、洋红、红、蓝、黑。
static void paint_color_bars(uint16_t *fb, uint16_t width, uint16_t height, void *ctx)
{
    (void)ctx;
    static const uint16_t colors[] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    const size_t color_count = sizeof(colors) / sizeof(colors[0]);

    for (uint16_t y = 0; y < height; y++) {
        uint16_t *row = fb + (size_t)y * width;
        for (uint16_t x = 0; x < width; x++) {
            size_t band = ((size_t)x * color_count) / width;
            row[x] = colors[band];
        }
    }
}

// 初始化失败后关闭背光并保留串口错误，避免任务返回后反复重启点屏链路。
static void stop_forever(const char *reason, esp_err_t err)
{
    fridge_display_mipi_set_backlight(false);
    ESP_LOGE(TAG, "%s: %s", reason, esp_err_to_name(err));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void fridge_display_mipi_test_run(void)
{
    static const solid_pattern_t solids[] = {
        {"red", 0xF800},
        {"green", 0x07E0},
        {"blue", 0x001F},
        {"white", 0xFFFF},
        {"black", 0x0000},
    };

    ESP_LOGW(TAG,
             "standalone MIPI test: WKS70WSV114-WCT 1024x600, EK79007, DSI 2-lane; GT911 is not started");
    esp_err_t ret = fridge_display_mipi_init();
    if (ret != ESP_OK) {
        stop_forever("MIPI display initialization failed", ret);
    }

    uint32_t cycles = 0;
    while (true) {
        for (size_t i = 0; i < sizeof(solids) / sizeof(solids[0]); i++) {
            ESP_LOGI(TAG, "pattern: solid %s", solids[i].name);
            ret = fridge_display_mipi_fill_screen(solids[i].color);
            if (ret != ESP_OK) {
                stop_forever("solid fill failed", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_FRIDGE_MIPI_SCREEN_TEST_HOLD_MS));
        }

        ESP_LOGI(TAG, "pattern: RGB565 color bars");
        ret = fridge_display_mipi_render(paint_color_bars, NULL);
        if (ret != ESP_OK) {
            stop_forever("color bars render failed", ret);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FRIDGE_MIPI_SCREEN_TEST_HOLD_MS));

        cycles++;
        ESP_LOGI(TAG, "MIPI screen test alive: cycles=%lu hold=%d ms",
                 (unsigned long)cycles, CONFIG_FRIDGE_MIPI_SCREEN_TEST_HOLD_MS);
    }
}
