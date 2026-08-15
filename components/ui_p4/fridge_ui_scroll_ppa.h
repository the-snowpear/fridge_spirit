// 主滚动区 PPA 滚动加速器。
// 竖屏 HOME/PLAN/CHAT/MORE 工具页共用 portrait_scroll；定时器和秒表页
// 额外把动态主卡以 ARGB 覆盖到快照上，避免滚动期间冻结计时数字。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRIDGE_UI_SCROLL_PPA_LANDSCAPE = 0,
    FRIDGE_UI_SCROLL_PPA_PORTRAIT,
} fridge_ui_scroll_ppa_profile_t;

// 竖屏逻辑缓冲不能直接交给 MIPI 扫描，PPA 滚动帧完成后由 UI 编排层旋转并提交。
typedef esp_err_t (*fridge_ui_scroll_ppa_present_cb_t)(const void *logical_frame,
                                                       void *user_ctx);

// 初始化并绑定横屏主滚动区。只能在 LVGL/UI 任务中调用。
esp_err_t fridge_ui_scroll_ppa_init(lv_display_t *display, lv_obj_t *main_scroll);

// 横竖屏切换后重新绑定当前可见的滚动容器。横屏直接提交 MIPI 双缓冲；
// 竖屏通过 present_cb 把 600x1024 逻辑帧旋转到 1024x600 物理缓冲。
esp_err_t fridge_ui_scroll_ppa_bind(lv_obj_t *scroll,
                                    fridge_ui_scroll_ppa_profile_t profile,
                                    fridge_ui_scroll_ppa_present_cb_t present_cb,
                                    void *user_ctx);

// 启用或关闭快照加速。竖屏支持 HOME、PLAN、CHAT 和 MORE 工具页；
// 定时器/秒表的动态数字通过覆盖层更新，闹钟等静态页直接使用滚动快照。
// 调用方必须位于 LVGL/UI 任务，避免与滚动事件并发修改状态。
void fridge_ui_scroll_ppa_set_enabled(bool enabled);

// 覆盖当前横屏滚动视口高度；传 0 恢复该方向的完整主滚动区。
// 购物页的固定底栏覆盖主滚动区底部时，只搬运可见正文，避免每帧复制被遮住的像素。
// 只能在 LVGL/UI 任务中调用，调用后自动使当前快照失效。
void fridge_ui_scroll_ppa_set_viewport_height(uint32_t height);

// 注册当前页面需要实时覆盖的 LVGL 对象。对象快照使用 ARGB8888，
// 仅在 PPA 手势帧中更新；对象必须属于绑定的主滚动区，且只能由 LVGL 任务调用。
void fridge_ui_scroll_ppa_set_dynamic_overlay(lv_obj_t *object);

// 页面字体、图标或业务内容变化后使缓存失效；下一次空闲循环会自动重建。
void fridge_ui_scroll_ppa_invalidate_cache(void);

// 在一次 lv_timer_handler() 之后调用：合并本轮滚动位置并提交至 MIPI 双缓冲。
void fridge_ui_scroll_ppa_process(void);

// 请求结束正在进行的加速手势；用于摄像头接管显存前完成双缓冲奇偶同步。
void fridge_ui_scroll_ppa_request_finish(void);

// 返回手势期间是否正在绕过 LVGL 软件重绘。
bool fridge_ui_scroll_ppa_is_active(void);

#ifdef __cplusplus
}
#endif
