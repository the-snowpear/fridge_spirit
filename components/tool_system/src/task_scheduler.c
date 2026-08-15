// 冰箱小精灵 TF 持久化计时调度实现。
//
// 在线且系统时间有效时使用 Unix 时间恢复倒计时；尚未 SNTP 对时时使用本次启动的单调时钟。
// 如果设备在无绝对时间时重启，计时器从完整剩余时长重新开始，宁可延迟也不提前误触发。

#include "fridge_task_scheduler.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_agent_storage.h"
#include "fridge_tool_broker.h"
#include "fridge_tool_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define TASKS_PATH "agent/data/tasks.json"
#define TASK_SCHEDULER_JSON_MAX (32U * 1024U)
#define TASK_SCHEDULER_MAX_TIMERS 32
#define TASK_SCHEDULER_STACK_BYTES 6144
#define TASK_SCHEDULER_TIME_READY_EPOCH 1700000000LL

typedef struct {
    char id[FRIDGE_TASK_EVENT_ID_LEN];
    char title[FRIDGE_TASK_EVENT_TITLE_LEN];
    int64_t deadline_us;
} scheduled_timer_t;

typedef struct {
    fridge_tool_call_t call;
    fridge_tool_result_t result;
} trigger_work_t;

static const char *TAG = "fridge_task_scheduler";
static scheduled_timer_t *s_timers;
static TaskHandle_t s_task;
static fridge_task_due_callback_t s_callback;
static void *s_callback_ctx;
static fridge_task_scheduler_status_t s_status;

static void *scheduler_alloc(size_t bytes)
{
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(1, bytes);
    return ptr;
}

static uint32_t scheduler_id_hash(const char *text)
{
    uint32_t hash = 2166136261U;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        hash = (hash ^ *p) * 16777619U;
    }
    return hash;
}

static int64_t wall_clock_ms(bool *ready)
{
    const time_t now = time(NULL);
    const bool valid = (int64_t)now >= TASK_SCHEDULER_TIME_READY_EPOCH;
    if (ready) *ready = valid;
    return valid ? (int64_t)now * 1000LL : 0;
}

static void set_error(const char *message)
{
    strlcpy(s_status.last_error, message ? message : "", sizeof(s_status.last_error));
}

static esp_err_t reload_timers(void)
{
    char *text = scheduler_alloc(TASK_SCHEDULER_JSON_MAX + 1);
    if (!text) return ESP_ERR_NO_MEM;
    size_t text_size = 0;
    esp_err_t err = fridge_agent_storage_json_read(TASKS_PATH,
                                                   text,
                                                   TASK_SCHEDULER_JSON_MAX + 1,
                                                   &text_size,
                                                   NULL);
    if (err != ESP_OK) {
        free(text);
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(text, text_size);
    free(text);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (!cJSON_IsArray(tasks)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(s_timers, 0, sizeof(*s_timers) * TASK_SCHEDULER_MAX_TIMERS);
    size_t count = 0;
    const int64_t now_us = esp_timer_get_time();
    bool clock_ready = false;
    const int64_t now_epoch_ms = wall_clock_ms(&clock_ready);
    cJSON *task = NULL;
    cJSON_ArrayForEach(task, tasks) {
        if (count >= TASK_SCHEDULER_MAX_TIMERS) break;
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(task, "kind");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(task, "status");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(task, "id");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(task, "durationSeconds");
        if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "timer") != 0 ||
            !cJSON_IsString(status) || strcmp(status->valuestring, "pending") != 0 ||
            !cJSON_IsString(id) || !cJSON_IsNumber(duration) ||
            duration->valuedouble < 1 || duration->valuedouble > 86400) {
            continue;
        }

        int64_t remaining_ms = (int64_t)duration->valuedouble * 1000LL;
        const cJSON *due_epoch = cJSON_GetObjectItemCaseSensitive(task, "dueAtEpochMs");
        const cJSON *created_mono = cJSON_GetObjectItemCaseSensitive(task, "createdMonotonicMs");
        if (clock_ready && cJSON_IsNumber(due_epoch) && due_epoch->valuedouble > 0) {
            remaining_ms = (int64_t)due_epoch->valuedouble - now_epoch_ms;
        } else if (cJSON_IsNumber(created_mono)) {
            const int64_t created_ms = (int64_t)created_mono->valuedouble;
            const int64_t now_mono_ms = now_us / 1000LL;
            if (created_ms >= 0 && created_ms <= now_mono_ms) {
                remaining_ms -= now_mono_ms - created_ms;
            }
        }
        if (remaining_ms < 0) remaining_ms = 0;

        strlcpy(s_timers[count].id, id->valuestring, sizeof(s_timers[count].id));
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(task, "title");
        strlcpy(s_timers[count].title,
                cJSON_IsString(title) ? title->valuestring : "计时器",
                sizeof(s_timers[count].title));
        s_timers[count].deadline_us = now_us + remaining_ms * 1000LL;
        count++;
    }
    cJSON_Delete(root);
    s_status.tracked_timers = (uint32_t)count;
    s_status.reload_count++;
    s_status.last_error[0] = '\0';
    return ESP_OK;
}

static esp_err_t trigger_timer(const scheduled_timer_t *timer)
{
    trigger_work_t *work = scheduler_alloc(sizeof(*work));
    if (!work) return ESP_ERR_NO_MEM;
    const uint32_t id_hash = scheduler_id_hash(timer->id);
    snprintf(work->call.call_id, sizeof(work->call.call_id), "due_%08" PRIx32, id_hash);
    strlcpy(work->call.run_id, "task_scheduler", sizeof(work->call.run_id));
    strlcpy(work->call.tool_name, "task.trigger", sizeof(work->call.tool_name));
    cJSON *arguments = cJSON_CreateObject();
    if (!arguments || !cJSON_AddStringToObject(arguments, "id", timer->id) ||
        !cJSON_PrintPreallocated(arguments,
                                work->call.arguments_json,
                                sizeof(work->call.arguments_json),
                                false)) {
        cJSON_Delete(arguments);
        free(work);
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(arguments);
    snprintf(work->call.idempotency_key,
             sizeof(work->call.idempotency_key),
             "trigger:%08" PRIx32,
             id_hash);
    work->call.user_confirmed = true;

    const esp_err_t err = fridge_tool_broker_call(&work->call, &work->result);
    if (err == ESP_OK && work->result.status == FRIDGE_TOOL_RESULT_OK) {
        fridge_task_due_event_t event = {0};
        strlcpy(event.id, timer->id, sizeof(event.id));
        strlcpy(event.title, timer->title, sizeof(event.title));
        strlcpy(event.kind, "timer", sizeof(event.kind));
        bool clock_ready = false;
        event.triggered_at_epoch_ms = wall_clock_ms(&clock_ready);
        s_status.triggered_this_boot++;
        strlcpy(s_status.last_triggered_id, timer->id, sizeof(s_status.last_triggered_id));
        if (s_callback) s_callback(&event, s_callback_ctx);
    }
    free(work);
    return err;
}

static void scheduler_task(void *arg)
{
    (void)arg;
    while (true) {
        esp_err_t reload_err = reload_timers();
        if (reload_err != ESP_OK) {
            set_error(esp_err_to_name(reload_err));
            ESP_LOGW(TAG, "reload timers failed: %s", esp_err_to_name(reload_err));
        }

        while (true) {
            const int64_t now_us = esp_timer_get_time();
            int64_t wait_us = 1000000LL;
            int due_index = -1;
            for (uint32_t i = 0; i < s_status.tracked_timers; i++) {
                const int64_t delta = s_timers[i].deadline_us - now_us;
                if (delta <= 0) {
                    due_index = (int)i;
                    break;
                }
                if (delta < wait_us) wait_us = delta;
            }

            if (due_index >= 0) {
                esp_err_t err = trigger_timer(&s_timers[due_index]);
                if (err != ESP_OK) {
                    set_error(esp_err_to_name(err));
                    ESP_LOGW(TAG, "trigger timer %s failed: %s",
                             s_timers[due_index].id, esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                break;
            }

            TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)((wait_us + 999) / 1000));
            if (wait_ticks < 1) wait_ticks = 1;
            if (ulTaskNotifyTake(pdTRUE, wait_ticks) > 0) break;
        }
    }
}

esp_err_t fridge_task_scheduler_init(void)
{
    if (s_task) return ESP_OK;
    memset(&s_status, 0, sizeof(s_status));
    s_timers = scheduler_alloc(sizeof(*s_timers) * TASK_SCHEDULER_MAX_TIMERS);
    if (!s_timers) return ESP_ERR_NO_MEM;
    // 调度器可能与其他 Flash 访问并发，常驻栈保留内部 RAM。
    if (xTaskCreate(scheduler_task,
                    "task_scheduler",
                    TASK_SCHEDULER_STACK_BYTES,
                    NULL,
                    4,
                    &s_task) != pdPASS) {
        free(s_timers);
        s_timers = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_status.ready = true;
    ESP_LOGI(TAG, "persistent timer scheduler ready");
    return ESP_OK;
}

void fridge_task_scheduler_notify_reload(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void fridge_task_scheduler_set_event_callback(fridge_task_due_callback_t callback, void *user_ctx)
{
    s_callback = callback;
    s_callback_ctx = user_ctx;
}

void fridge_task_scheduler_get_status(fridge_task_scheduler_status_t *out)
{
    if (out) *out = s_status;
}
