#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRIDGE_I2S_SHARED_OWNER_NONE = 0,
    FRIDGE_I2S_SHARED_OWNER_MIC,
    FRIDGE_I2S_SHARED_OWNER_SPEAKER,
} fridge_i2s_shared_owner_t;

// 申请 GPIO40/41 共享 I2S 时钟线。
// 硬件约束：INMP441 和 MAX98357A 共用 BCLK/WS，不能让 RX/TX 两个通道同时重新绑定或 enable。
esp_err_t fridge_i2s_shared_acquire(fridge_i2s_shared_owner_t owner, TickType_t timeout_ticks);

// 释放共享 I2S 时钟线；只有当前 owner 匹配时才会释放，避免误释放其它音频路径。
void fridge_i2s_shared_release(fridge_i2s_shared_owner_t owner);

fridge_i2s_shared_owner_t fridge_i2s_shared_get_owner(void);

#ifdef __cplusplus
}
#endif
