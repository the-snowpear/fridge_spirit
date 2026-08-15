// 冰箱小精灵本地计时调度器。
//
// 计时任务权威记录位于 TF 卡 tasks.json。调度器只在任务上下文中等待到期并通过 Tool Broker
// 执行 task.trigger；不在定时回调中做文件 I/O，也不直接控制扬声器或 UI。

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TASK_EVENT_ID_LEN 64
#define FRIDGE_TASK_EVENT_TITLE_LEN 128

typedef struct {
    char id[FRIDGE_TASK_EVENT_ID_LEN];
    char title[FRIDGE_TASK_EVENT_TITLE_LEN];
    char kind[16];
    int64_t triggered_at_epoch_ms;
} fridge_task_due_event_t;

typedef void (*fridge_task_due_callback_t)(const fridge_task_due_event_t *event, void *user_ctx);

typedef struct {
    bool ready;
    uint32_t tracked_timers;
    uint32_t triggered_this_boot;
    uint32_t reload_count;
    char last_triggered_id[FRIDGE_TASK_EVENT_ID_LEN];
    char last_error[128];
} fridge_task_scheduler_status_t;

esp_err_t fridge_task_scheduler_init(void);
void fridge_task_scheduler_notify_reload(void);
void fridge_task_scheduler_set_event_callback(fridge_task_due_callback_t callback, void *user_ctx);
void fridge_task_scheduler_get_status(fridge_task_scheduler_status_t *out);

#ifdef __cplusplus
}
#endif
