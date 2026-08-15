// ver2 TF 页面资源加载器。
// 只接受固件白名单中的 A8 图标、home.json 和 more.json；资源不包含可执行代码或 LVGL 对象描述。

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "fridge_ui_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_UI_VER2_ROOT "/sdcard/fridge/assets/ui/ver2"
#define FRIDGE_UI_VER2_MANIFEST FRIDGE_UI_VER2_ROOT "/manifest.json"

typedef enum {
    FRIDGE_UI_ICON_HOME = 0,
    FRIDGE_UI_ICON_BOX,
    FRIDGE_UI_ICON_CHAT,
    FRIDGE_UI_ICON_PLAN,
    FRIDGE_UI_ICON_MORE,
    FRIDGE_UI_ICON_SEARCH,
    FRIDGE_UI_ICON_PLUS,
    FRIDGE_UI_ICON_CLOCK,
    FRIDGE_UI_ICON_WARN,
    FRIDGE_UI_ICON_LOCATION,
    FRIDGE_UI_ICON_FOOD,
    FRIDGE_UI_ICON_INFO,
    FRIDGE_UI_ICON_STAR,
    FRIDGE_UI_ICON_ZAP,
    FRIDGE_UI_ICON_CLOSE,
    FRIDGE_UI_ICON_CART,
    FRIDGE_UI_ICON_FWD,
    FRIDGE_UI_ICON_TEMP,
    FRIDGE_UI_ICON_DROP,
    FRIDGE_UI_ICON_WIFI,
    FRIDGE_UI_ICON_CLOUD,
    FRIDGE_UI_ICON_TF,
    FRIDGE_UI_ICON_CAMERA,
    FRIDGE_UI_ICON_MIC,
    FRIDGE_UI_ICON_TIMER,
    FRIDGE_UI_ICON_CHECK,
    FRIDGE_UI_ICON_BELL,
    FRIDGE_UI_ICON_STOPWATCH,
    FRIDGE_UI_ICON_BACK,
    FRIDGE_UI_ICON_RETRY,
    FRIDGE_UI_ICON_PLAY,
    FRIDGE_UI_ICON_PAUSE,
    FRIDGE_UI_ICON_LIST,
    FRIDGE_UI_ICON_LOCK,
    FRIDGE_UI_ICON_GRID,
    FRIDGE_UI_ICON_EDIT,
    FRIDGE_UI_ICON_MINUS,
    FRIDGE_UI_ICON_TRASH,
    FRIDGE_UI_ICON_UNDO,
    FRIDGE_UI_ICON_SHIELD,
    FRIDGE_UI_ICON_KEYBOARD,
    FRIDGE_UI_ICON_DOOR,
    FRIDGE_UI_ICON_FRIDGE,
    FRIDGE_UI_ICON_COUNT,
} fridge_ui_icon_id_t;

typedef struct {
    uint8_t *data;
    lv_image_dsc_t image;
} fridge_ui_a8_icon_t;

typedef struct {
    bool loaded;
    bool manifest_verified;
    char package_version[32];
    char last_error[128];
    fridge_ui_a8_icon_t icons[FRIDGE_UI_ICON_COUNT];
} fridge_ui_assets_t;

void fridge_ui_assets_init(fridge_ui_assets_t *assets);

// 必须由 UI/LVGL 任务调用：成功后图标常驻 PSRAM，内容 JSON DOM 已释放。
esp_err_t fridge_ui_assets_try_load(fridge_ui_assets_t *assets,
                                    home_business_model_t *home_model,
                                    more_business_model_t *more_model,
                                    plan_business_model_t *plan_model);

const lv_image_dsc_t *fridge_ui_assets_get_icon(const fridge_ui_assets_t *assets,
                                                fridge_ui_icon_id_t id);

fridge_ui_icon_id_t fridge_ui_assets_icon_id_from_name(const char *name);

#ifdef __cplusplus
}
#endif
