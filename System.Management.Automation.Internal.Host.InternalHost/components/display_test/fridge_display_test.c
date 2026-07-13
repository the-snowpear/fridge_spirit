// 冰箱小精灵 TR230S QSPI-4SDA 独立点屏测试。
// 该入口复用正式显示驱动，避免测试路径和正常启动路径使用不同的总线时序。
// 硬件注意：屏幕 VCC=5V、GPIO 逻辑=3.3V、所有 GND 共地，板载 20-pin 屏座必须保持空置。

#include "fridge_display_test.h"

#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fridge_display.h"

#define LCD_TEST_ROWS 20
#define LCD_TEST_PIXELS (FRIDGE_DISPLAY_WIDTH * LCD_TEST_ROWS)

#ifndef CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS
#define CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS 3000
#endif

#ifndef CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES
#define CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES 10
#endif

#ifdef CONFIG_FRIDGE_SCREEN_TEST_VERBOSE_LOG
#define LCD_TEST_VERBOSE_LOG_ENABLED 1
#else
#define LCD_TEST_VERBOSE_LOG_ENABLED 0
#endif

static const char *TAG = "display_test";

// 显示驱动内部再按 20 行 DMA 分块；这里使用同样高度的 RGB565 缓冲逐段刷满全屏。
static esp_err_t fill_screen(uint16_t color, uint16_t *line_buf)
{
    for (size_t i = 0; i < LCD_TEST_PIXELS; i++) {
        line_buf[i] = color;
    }

    for (uint16_t y = 0; y < FRIDGE_DISPLAY_HEIGHT; y += LCD_TEST_ROWS) {
        uint16_t y2 = y + LCD_TEST_ROWS - 1;
        if (y2 >= FRIDGE_DISPLAY_HEIGHT) {
            y2 = FRIDGE_DISPLAY_HEIGHT - 1;
        }
        esp_err_t ret = fridge_display_flush_area(0,
                                                  y,
                                                  FRIDGE_DISPLAY_WIDTH - 1,
                                                  y2,
                                                  line_buf);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

// 硬件故障后停止继续发送 SPI 数据，保留串口现场供断电排线检查。
static void stop_forever(const char *reason, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", reason, esp_err_to_name(err));
    ESP_LOGE(TAG, "SPI stopped. Power off before rewiring; keep LCD VCC=5V and GPIO logic=3.3V.");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void fridge_display_test_run(void)
{
    ESP_LOGW(TAG, "Standalone TR230S QSPI-4SDA test is enabled; FT6336U is not started in this profile.");
    ESP_LOGW(TAG, "Before power-on: verify FPC pin 1, common GND, 5V VCC and 3.3V-only GPIO signals.");

    esp_err_t ret = fridge_display_init();
    if (ret != ESP_OK) {
        stop_forever("TR230S QSPI-4SDA initialization failed", ret);
    }

    uint16_t *line_buf = heap_caps_malloc(LCD_TEST_PIXELS * sizeof(uint16_t),
                                          MALLOC_CAP_DEFAULT);
    if (line_buf == NULL) {
        stop_forever("screen test buffer allocation failed", ESP_ERR_NO_MEM);
    }

    const uint16_t colors[] = {0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0000};
    const char *names[] = {"white", "red", "green", "blue", "black"};
    uint32_t cycle_count = 0;

    while (true) {
        for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
            if (LCD_TEST_VERBOSE_LOG_ENABLED) {
                ESP_LOGI(TAG, "fill screen: %s", names[i]);
            }
            ret = fill_screen(colors[i], line_buf);
            if (ret != ESP_OK) {
                stop_forever("screen color fill failed", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS));
        }

        cycle_count++;
        if ((cycle_count % CONFIG_FRIDGE_SCREEN_TEST_REPORT_EVERY_CYCLES) == 0) {
            ESP_LOGI(TAG, "screen test alive: cycles=%lu hold=%d ms",
                     (unsigned long)cycle_count,
                     CONFIG_FRIDGE_SCREEN_TEST_COLOR_HOLD_MS);
        }
    }
}
