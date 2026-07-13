// 冰箱小精灵 I2S 共享总线仲裁。
// INMP441 麦克风和 MAX98357A 扬声器共用 GPIO40/41 作为 BCLK/WS，
// 这里用一个轻量互斥锁保证同一时间只有一个方向占用共享时钟线。
#include "fridge_i2s_shared.h"

#include <stdbool.h>
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "fridge_i2s_shared";

static SemaphoreHandle_t s_bus_token;
static portMUX_TYPE s_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static fridge_i2s_shared_owner_t s_owner = FRIDGE_I2S_SHARED_OWNER_NONE;

static esp_err_t ensure_lock(void)
{
    if (s_bus_token) {
        return ESP_OK;
    }

    SemaphoreHandle_t new_token = xSemaphoreCreateBinary();
    if (!new_token) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(new_token);

    portENTER_CRITICAL(&s_owner_mux);
    if (!s_bus_token) {
        s_bus_token = new_token;
        new_token = NULL;
    }
    portEXIT_CRITICAL(&s_owner_mux);

    if (new_token) {
        vSemaphoreDelete(new_token);
    }
    return ESP_OK;
}

esp_err_t fridge_i2s_shared_acquire(fridge_i2s_shared_owner_t owner, TickType_t timeout_ticks)
{
    if (owner == FRIDGE_I2S_SHARED_OWNER_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_bus_token, timeout_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "I2S shared bus busy, requested owner=%d current=%d", owner, fridge_i2s_shared_get_owner());
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_owner_mux);
    s_owner = owner;
    portEXIT_CRITICAL(&s_owner_mux);
    return ESP_OK;
}

void fridge_i2s_shared_release(fridge_i2s_shared_owner_t owner)
{
    if (!s_bus_token || owner == FRIDGE_I2S_SHARED_OWNER_NONE) {
        return;
    }

    bool should_release = false;
    portENTER_CRITICAL(&s_owner_mux);
    if (s_owner == owner) {
        s_owner = FRIDGE_I2S_SHARED_OWNER_NONE;
        should_release = true;
    }
    portEXIT_CRITICAL(&s_owner_mux);

    if (should_release) {
        xSemaphoreGive(s_bus_token);
    }
}

fridge_i2s_shared_owner_t fridge_i2s_shared_get_owner(void)
{
    fridge_i2s_shared_owner_t owner;
    portENTER_CRITICAL(&s_owner_mux);
    owner = s_owner;
    portEXIT_CRITICAL(&s_owner_mux);
    return owner;
}
