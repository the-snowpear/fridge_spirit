// 冰箱小精灵厨房计时工具组件。
// 负责定时器、秒表和闹钟的本地状态管理；计时使用 esp_timer 单调时钟，闹钟使用已校准系统时间。
// 硬件注意：本组件不配置 GPIO/I2S；到点提示统一复用 P4 板载 ES8311 本地提示音，
// 不得拉入旧 MAX98357A 的 GPIO39/40/41 I2S 链路，以免与 TF 卡 SDMMC 总线冲突。

#include "fridge_kitchen_tools.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "fridge_audio.h"
#include "fridge_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define KITCHEN_TOOLS_TASK_STACK 4096
#define KITCHEN_TOOLS_TASK_PRIORITY 3
#define KITCHEN_TOOLS_POLL_MS 500
#define KITCHEN_TOOLS_EPOCH_READY_THRESHOLD 1735689600LL
#define KITCHEN_TOOLS_ALARM_PATH "/cache/kitchen_alarms.json"
#define KITCHEN_TOOLS_ALARM_TMP_PATH "/cache/kitchen_alarms.tmp"
#define KITCHEN_TOOLS_ALARM_FILE_MAX 4096

static const char *TAG = "fridge_kitchen_tools";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_initialized;
// 多倒计时槽位：id!=0 表示占用；DONE 保留给 UI「已完成」列表，可被 clear_done 清空。
static fridge_kitchen_timer_snapshot_t s_timers[FRIDGE_KITCHEN_TIMER_MAX];
static uint8_t s_next_timer_id = 1;
static fridge_kitchen_stopwatch_snapshot_t s_stopwatch;
static fridge_kitchen_alarm_t s_alarms[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
static size_t s_alarm_count;
static char s_last_alert[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
static char s_last_error[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool time_ready(void)
{
    return time(NULL) >= KITCHEN_TOOLS_EPOCH_READY_THRESHOLD;
}

static size_t utf8_valid_prefix_len(const char *text)
{
    if (!text) {
        return 0;
    }
    const unsigned char *bytes = (const unsigned char *)text;
    size_t len = strlen(text);
    size_t pos = 0;
    while (pos < len) {
        unsigned char ch = bytes[pos];
        size_t need = 1;
        if (ch < 0x80) {
            need = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            need = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            need = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            need = 4;
        } else {
            break;
        }
        if (pos + need > len) {
            break;
        }
        for (size_t i = 1; i < need; i++) {
            if ((bytes[pos + i] & 0xC0) != 0x80) {
                return pos;
            }
        }
        pos += need;
    }
    return pos;
}

static int64_t local_day_key(const struct tm *tm_now)
{
    if (!tm_now) {
        return 0;
    }
    return ((int64_t)(tm_now->tm_year + 1900) * 10000) + ((int64_t)(tm_now->tm_mon + 1) * 100) + tm_now->tm_mday;
}

static void copy_label(char *out, size_t out_size, const char *label, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *source = (label && label[0]) ? label : fallback;
    strlcpy(out, source ? source : "", out_size);
    // 标签按字节存储，截断中文时需要回退到完整 UTF-8 边界，避免持久化 JSON 或 USB 输出出现乱码。
    out[utf8_valid_prefix_len(out)] = '\0';
}

// 返回规则对应的星期位掩码；tm_wday 的 bit0 是周日，bit6 是周六。
static uint8_t repeat_mask(fridge_kitchen_alarm_repeat_t repeat, uint8_t custom_mask)
{
    switch (repeat) {
    case FRIDGE_KITCHEN_ALARM_REPEAT_WEEKDAYS:
        return FRIDGE_KITCHEN_ALARM_WEEKDAY_WORKDAYS;
    case FRIDGE_KITCHEN_ALARM_REPEAT_SUNDAY:
        return FRIDGE_KITCHEN_ALARM_WEEKDAY_SUNDAY;
    case FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM:
        return custom_mask & FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
    case FRIDGE_KITCHEN_ALARM_REPEAT_DAILY:
    default:
        return FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
    }
}

// 旧版 JSON 没有这些字段。统一在这里补齐默认值，保证旧闹钟升级后仍是每天的固定铃声提醒。
static void alarm_config_defaults(fridge_kitchen_alarm_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->kind = FRIDGE_KITCHEN_ALARM_KIND_FIXED;
    config->repeat = FRIDGE_KITCHEN_ALARM_REPEAT_DAILY;
    config->weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
    config->sound = FRIDGE_KITCHEN_ALARM_SOUND_CHIME;
    config->source = FRIDGE_KITCHEN_ALARM_SOURCE_MANUAL;
}

static bool alarm_config_valid(const fridge_kitchen_alarm_config_t *config)
{
    if (!config || config->hour > 23 || config->minute > 59 ||
        config->kind > FRIDGE_KITCHEN_ALARM_KIND_CYCLE ||
        config->repeat > FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM ||
        config->sound > FRIDGE_KITCHEN_ALARM_SOUND_VOICE ||
        config->source > FRIDGE_KITCHEN_ALARM_SOURCE_AGENT) {
        return false;
    }
    return config->repeat != FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM ||
           repeat_mask(config->repeat, config->weekday_mask) != 0;
}

static void alarm_apply_config(fridge_kitchen_alarm_t *alarm,
                               const fridge_kitchen_alarm_config_t *config)
{
    if (!alarm || !config) return;
    alarm->kind = config->kind;
    alarm->repeat = config->repeat;
    alarm->weekday_mask = repeat_mask(config->repeat, config->weekday_mask);
    alarm->sound = config->sound;
    alarm->source = config->source;
    alarm->hour = config->hour;
    alarm->minute = config->minute;
    copy_label(alarm->label, sizeof(alarm->label), config->label, "厨房提醒");
    copy_label(alarm->speech_text, sizeof(alarm->speech_text), config->speech_text, "");
}

static bool json_key_allowed(const char *key)
{
    static const char *allowed[] = {
        "tool",
        "action",
        "duration_seconds",
        "hour",
        "minute",
        "label",
        "kind",
        "repeat",
        "weekday_mask",
        "sound",
        "speech_text",
        "enabled",
        "id",
    };
    if (!key) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(key, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *kitchen_err_text(esp_err_t err, const char *tool, const char *action)
{
    if (err == ESP_ERR_INVALID_STATE && tool && strcmp(tool, "alarm") == 0 && action && strcmp(action, "set") == 0) {
        return "时间未同步，暂不能设置闹钟";
    }
    if (err == ESP_ERR_INVALID_STATE && tool && strcmp(tool, "timer") == 0) {
        return "当前定时器状态不支持这个操作";
    }
    if (err == ESP_ERR_INVALID_STATE && tool && strcmp(tool, "stopwatch") == 0) {
        return "当前秒表状态不支持这个操作";
    }
    if (err == ESP_ERR_NOT_FOUND && tool && strcmp(tool, "alarm") == 0) {
        return "没有找到这个闹钟";
    }
    if (err == ESP_ERR_NO_MEM && tool && strcmp(tool, "alarm") == 0) {
        return "闹钟数量已满，最多 5 个";
    }
    if (err == ESP_ERR_NO_MEM && tool && strcmp(tool, "timer") == 0) {
        return "定时器数量已满，最多 5 个";
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return "厨房工具指令参数无效";
    }
    return esp_err_to_name(err);
}

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static uint32_t timer_slot_remaining_locked(const fridge_kitchen_timer_snapshot_t *slot, int64_t now)
{
    if (!slot) {
        return 0;
    }
    if (slot->state == FRIDGE_KITCHEN_TIMER_RUNNING) {
        if (now >= slot->target_at_ms) {
            return 0;
        }
        int64_t left_ms = slot->target_at_ms - now;
        return (uint32_t)((left_ms + 999) / 1000);
    }
    return slot->remaining_seconds;
}

static fridge_kitchen_timer_snapshot_t *timer_find_locked(uint8_t id)
{
    if (id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        if (s_timers[i].id == id) {
            return &s_timers[i];
        }
    }
    return NULL;
}

static fridge_kitchen_timer_snapshot_t *timer_alloc_slot_locked(void)
{
    for (size_t i = 0; i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        if (s_timers[i].id == 0) {
            return &s_timers[i];
        }
    }
    // 槽位满时优先回收最早的 DONE，避免已完成记录占满无法新建。
    size_t victim = FRIDGE_KITCHEN_TIMER_MAX;
    for (size_t i = 0; i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        if (s_timers[i].state == FRIDGE_KITCHEN_TIMER_DONE) {
            victim = i;
            break;
        }
    }
    if (victim >= FRIDGE_KITCHEN_TIMER_MAX) {
        return NULL;
    }
    memset(&s_timers[victim], 0, sizeof(s_timers[victim]));
    return &s_timers[victim];
}

// 主定时器选取：RINGING 优先（到时层用），再取 remaining 最小的 RUNNING/PAUSED。
static fridge_kitchen_timer_snapshot_t *timer_primary_locked(int64_t now)
{
    fridge_kitchen_timer_snapshot_t *ringing = NULL;
    fridge_kitchen_timer_snapshot_t *best = NULL;
    uint32_t best_remain = UINT32_MAX;
    for (size_t i = 0; i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        fridge_kitchen_timer_snapshot_t *slot = &s_timers[i];
        if (slot->id == 0) {
            continue;
        }
        if (slot->state == FRIDGE_KITCHEN_TIMER_RINGING) {
            if (!ringing || slot->target_at_ms < ringing->target_at_ms) {
                ringing = slot;
            }
            continue;
        }
        if (slot->state != FRIDGE_KITCHEN_TIMER_RUNNING && slot->state != FRIDGE_KITCHEN_TIMER_PAUSED) {
            continue;
        }
        uint32_t remain = timer_slot_remaining_locked(slot, now);
        if (!best || remain < best_remain) {
            best = slot;
            best_remain = remain;
        }
    }
    return ringing ? ringing : best;
}

static fridge_kitchen_alarm_sound_t timer_sound_to_alarm(fridge_kitchen_timer_sound_t sound)
{
    return sound == FRIDGE_KITCHEN_TIMER_SOUND_CHIME ? FRIDGE_KITCHEN_ALARM_SOUND_CHIME
                                                    : FRIDGE_KITCHEN_ALARM_SOUND_VOICE;
}

// 计算秒表当前毫秒数。运行态只读单调时钟，不依赖任务轮询频率，避免 UI 刷新抖动累积误差。
static uint64_t stopwatch_elapsed_ms_locked(int64_t now)
{
    if (s_stopwatch.state == FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        uint64_t base = s_stopwatch.elapsed_ms;
        if (now > s_stopwatch.started_at_ms) {
            base += (uint64_t)(now - s_stopwatch.started_at_ms);
        }
        return base;
    }
    return s_stopwatch.elapsed_ms;
}

// 兼容旧快照字段；业务计时始终以毫秒字段为准，秒字段只供旧 USB/Web 接口显示。
static void stopwatch_sync_legacy_seconds(fridge_kitchen_stopwatch_snapshot_t *stopwatch)
{
    if (!stopwatch) return;
    uint64_t seconds = stopwatch->elapsed_ms / 1000U;
    stopwatch->elapsed_seconds = seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

static void fill_snapshot_locked(fridge_kitchen_tools_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->time_ready = time_ready();
    int64_t now = now_ms();

    // 复制全部有效槽位，并实时折算 remaining。
    out->timer_count = 0;
    for (size_t i = 0; i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        if (s_timers[i].id == 0) {
            continue;
        }
        fridge_kitchen_timer_snapshot_t *dst = &out->timers[out->timer_count++];
        *dst = s_timers[i];
        dst->remaining_seconds = timer_slot_remaining_locked(&s_timers[i], now);
    }

    // 兼容字段：对外仍暴露单一主定时器。
    fridge_kitchen_timer_snapshot_t *primary = timer_primary_locked(now);
    if (primary) {
        out->timer = *primary;
        out->timer.remaining_seconds = timer_slot_remaining_locked(primary, now);
    } else {
        memset(&out->timer, 0, sizeof(out->timer));
        out->timer.state = FRIDGE_KITCHEN_TIMER_IDLE;
    }

    out->stopwatch = s_stopwatch;
    out->stopwatch.elapsed_ms = stopwatch_elapsed_ms_locked(now);
    stopwatch_sync_legacy_seconds(&out->stopwatch);
    // 计次记录只读复制，调用方不得直接修改组件内部状态。
    if (s_stopwatch.lap_count > 0) {
        memcpy(out->stopwatch.laps_ms, s_stopwatch.laps_ms,
               s_stopwatch.lap_count * sizeof(s_stopwatch.laps_ms[0]));
    }
    out->alarm_count = s_alarm_count;
    for (size_t i = 0; i < s_alarm_count && i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        out->alarms[i] = s_alarms[i];
    }
    strlcpy(out->last_alert, s_last_alert, sizeof(out->last_alert));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
}

static esp_err_t save_alarms_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create alarm root failed");
    cJSON_AddNumberToObject(root, "schema_version", 2);
    cJSON *items = cJSON_AddArrayToObject(root, "alarms");
    if (!items) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_alarm_count && i < FRIDGE_KITCHEN_TOOL_MAX_ALARMS; i++) {
        const fridge_kitchen_alarm_t *alarm = &s_alarms[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(item, "id", alarm->id);
        cJSON_AddBoolToObject(item, "enabled", alarm->enabled);
        cJSON_AddNumberToObject(item, "kind", alarm->kind);
        cJSON_AddNumberToObject(item, "repeat", alarm->repeat);
        cJSON_AddNumberToObject(item, "weekday_mask", alarm->weekday_mask);
        cJSON_AddNumberToObject(item, "sound", alarm->sound);
        cJSON_AddNumberToObject(item, "source", alarm->source);
        cJSON_AddNumberToObject(item, "hour", alarm->hour);
        cJSON_AddNumberToObject(item, "minute", alarm->minute);
        cJSON_AddNumberToObject(item, "last_trigger_day", (double)alarm->last_trigger_day);
        cJSON_AddStringToObject(item, "label", alarm->label);
        cJSON_AddStringToObject(item, "speech_text", alarm->speech_text);
        cJSON_AddItemToArray(items, item);
    }

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print alarm json failed");

    FILE *file = fopen(KITCHEN_TOOLS_ALARM_TMP_PATH, "wb");
    if (!file) {
        cJSON_free(printed);
        return ESP_FAIL;
    }
    size_t len = strlen(printed);
    size_t written = fwrite(printed, 1, len, file);
    int close_ret = fclose(file);
    cJSON_free(printed);
    if (written != len || close_ret != 0) {
        remove(KITCHEN_TOOLS_ALARM_TMP_PATH);
        return ESP_FAIL;
    }
    if (rename(KITCHEN_TOOLS_ALARM_TMP_PATH, KITCHEN_TOOLS_ALARM_PATH) != 0) {
        remove(KITCHEN_TOOLS_ALARM_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool alarm_id_exists_locked(uint8_t id)
{
    if (id == 0) {
        return true;
    }
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id == id) {
            return true;
        }
    }
    return false;
}

static void load_alarms_locked(void)
{
    FILE *file = fopen(KITCHEN_TOOLS_ALARM_PATH, "rb");
    if (!file) {
        return;
    }
    char *buffer = calloc(1, KITCHEN_TOOLS_ALARM_FILE_MAX);
    if (!buffer) {
        fclose(file);
        return;
    }
    size_t bytes = fread(buffer, 1, KITCHEN_TOOLS_ALARM_FILE_MAX - 1U, file);
    fclose(file);
    buffer[bytes] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (!root) {
        ESP_LOGW(TAG, "alarm file invalid, ignore");
        return;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "alarms");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return;
    }

    s_alarm_count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        if (s_alarm_count >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) {
            break;
        }
        const cJSON *hour = cJSON_GetObjectItemCaseSensitive(item, "hour");
        const cJSON *minute = cJSON_GetObjectItemCaseSensitive(item, "minute");
        if (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || hour->valueint < 0 || hour->valueint > 23 ||
            minute->valueint < 0 || minute->valueint > 59) {
            continue;
        }
        fridge_kitchen_alarm_t *alarm = &s_alarms[s_alarm_count];
        memset(alarm, 0, sizeof(*alarm));
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "label");
        const cJSON *last_day = cJSON_GetObjectItemCaseSensitive(item, "last_trigger_day");
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
        const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(item, "repeat");
        const cJSON *weekday_mask = cJSON_GetObjectItemCaseSensitive(item, "weekday_mask");
        const cJSON *sound = cJSON_GetObjectItemCaseSensitive(item, "sound");
        const cJSON *source = cJSON_GetObjectItemCaseSensitive(item, "source");
        const cJSON *speech_text = cJSON_GetObjectItemCaseSensitive(item, "speech_text");
        uint8_t loaded_id = 0;
        if (cJSON_IsNumber(id) && id->valueint > 0 && id->valueint < 250) {
            loaded_id = (uint8_t)id->valueint;
        }
        if (loaded_id == 0 || alarm_id_exists_locked(loaded_id)) {
            loaded_id = (uint8_t)(s_alarm_count + 1);
            while (alarm_id_exists_locked(loaded_id) && loaded_id < 250) {
                loaded_id++;
            }
        }
        if (loaded_id == 0 || alarm_id_exists_locked(loaded_id)) {
            strlcpy(s_last_error, "跳过了无效闹钟编号", sizeof(s_last_error));
            continue;
        }
        alarm->id = loaded_id;
        alarm->enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
        alarm->kind = cJSON_IsNumber(kind) && kind->valueint >= FRIDGE_KITCHEN_ALARM_KIND_FIXED &&
                      kind->valueint <= FRIDGE_KITCHEN_ALARM_KIND_CYCLE ?
                      (fridge_kitchen_alarm_kind_t)kind->valueint : FRIDGE_KITCHEN_ALARM_KIND_FIXED;
        alarm->repeat = cJSON_IsNumber(repeat) && repeat->valueint >= FRIDGE_KITCHEN_ALARM_REPEAT_DAILY &&
                        repeat->valueint <= FRIDGE_KITCHEN_ALARM_REPEAT_CUSTOM ?
                        (fridge_kitchen_alarm_repeat_t)repeat->valueint : FRIDGE_KITCHEN_ALARM_REPEAT_DAILY;
        uint8_t loaded_mask = cJSON_IsNumber(weekday_mask) && weekday_mask->valueint >= 0 &&
                              weekday_mask->valueint <= FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL ?
                              (uint8_t)weekday_mask->valueint : FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
        alarm->weekday_mask = repeat_mask(alarm->repeat, loaded_mask);
        if (alarm->weekday_mask == 0) {
            alarm->repeat = FRIDGE_KITCHEN_ALARM_REPEAT_DAILY;
            alarm->weekday_mask = FRIDGE_KITCHEN_ALARM_WEEKDAY_ALL;
        }
        alarm->sound = cJSON_IsNumber(sound) && sound->valueint >= FRIDGE_KITCHEN_ALARM_SOUND_CHIME &&
                       sound->valueint <= FRIDGE_KITCHEN_ALARM_SOUND_VOICE ?
                       (fridge_kitchen_alarm_sound_t)sound->valueint : FRIDGE_KITCHEN_ALARM_SOUND_CHIME;
        alarm->source = cJSON_IsNumber(source) && source->valueint >= FRIDGE_KITCHEN_ALARM_SOURCE_MANUAL &&
                        source->valueint <= FRIDGE_KITCHEN_ALARM_SOURCE_AGENT ?
                        (fridge_kitchen_alarm_source_t)source->valueint : FRIDGE_KITCHEN_ALARM_SOURCE_MANUAL;
        alarm->hour = (uint8_t)hour->valueint;
        alarm->minute = (uint8_t)minute->valueint;
        alarm->last_trigger_day = cJSON_IsNumber(last_day) ? (int64_t)last_day->valuedouble : 0;
        if (cJSON_IsString(label) && label->valuestring) {
            copy_label(alarm->label, sizeof(alarm->label), label->valuestring, "厨房提醒");
        } else {
            copy_label(alarm->label, sizeof(alarm->label), NULL, "厨房提醒");
        }
        if (cJSON_IsString(speech_text) && speech_text->valuestring) {
            copy_label(alarm->speech_text, sizeof(alarm->speech_text), speech_text->valuestring, "");
        }
        s_alarm_count++;
    }
    cJSON_Delete(root);
}

typedef struct {
    fridge_kitchen_alarm_sound_t sound;
    char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
} alert_job_t;

static void alert_task(void *arg)
{
    alert_job_t *job = (alert_job_t *)arg;
    if (!job) {
        vTaskDelete(NULL);
        return;
    }
    esp_err_t err = ESP_OK;
    char error[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1] = {0};
    // P4 当前只使用板载 ES8311。旧“语音”选项保留为已存闹钟配置的兼容值，
    // 但必须安全降级为本地提示音，禁止触发 S3/MAX98357A 的 TF 卡总线引脚。
    if (job->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE) {
        ESP_LOGW(TAG, "voice alert falls back to ES8311 chime on ESP32-P4");
    }
    err = fridge_audio_speaker_beep_test();
    if (err != ESP_OK) {
        strlcpy(error, esp_err_to_name(err), sizeof(error));
    }
    if (err != ESP_OK) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            strlcpy(s_last_error, error[0] ? error : esp_err_to_name(err), sizeof(s_last_error));
            xSemaphoreGive(s_lock);
        } else {
            ESP_LOGW(TAG, "alert playback failed and last_error lock timed out");
        }
        ESP_LOGW(TAG, "alert playback failed: %s", error[0] ? error : esp_err_to_name(err));
    }
    free(job);
    vTaskDelete(NULL);
}

static void trigger_alert_locked(const char *message, fridge_kitchen_alarm_sound_t sound)
{
    strlcpy(s_last_alert, message ? message : "厨房提醒到了", sizeof(s_last_alert));
    alert_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        strlcpy(s_last_error, "提醒播报内存不足", sizeof(s_last_error));
        return;
    }
    job->sound = sound;
    strlcpy(job->message, s_last_alert, sizeof(job->message));
    BaseType_t ok = xTaskCreate(alert_task, "kitchen_alert", KITCHEN_TOOLS_TASK_STACK, job, KITCHEN_TOOLS_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        free(job);
        strlcpy(s_last_error, "提醒播报任务创建失败", sizeof(s_last_error));
    }
}

static void kitchen_tools_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int64_t mono_now = now_ms();
            // 多倒计时并行到点：每槽独立进入 RINGING 并触发一次提醒。
            for (size_t ti = 0; ti < FRIDGE_KITCHEN_TIMER_MAX; ti++) {
                fridge_kitchen_timer_snapshot_t *slot = &s_timers[ti];
                if (slot->id == 0 || slot->state != FRIDGE_KITCHEN_TIMER_RUNNING) {
                    continue;
                }
                if (mono_now < slot->target_at_ms) {
                    continue;
                }
                slot->state = FRIDGE_KITCHEN_TIMER_RINGING;
                slot->remaining_seconds = 0;
                char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
                snprintf(message, sizeof(message), "%s定时器到了", slot->label[0] ? slot->label : "厨房");
                trigger_alert_locked(message, timer_sound_to_alarm(slot->sound));
            }

            if (time_ready()) {
                time_t wall_now = time(NULL);
                struct tm tm_now = {0};
                localtime_r(&wall_now, &tm_now);
                int64_t day = local_day_key(&tm_now);
                for (size_t i = 0; i < s_alarm_count; i++) {
                    fridge_kitchen_alarm_t *alarm = &s_alarms[i];
                    uint8_t weekday = (uint8_t)(1U << tm_now.tm_wday);
                    if (!alarm->enabled || alarm->ringing || alarm->last_trigger_day == day ||
                        (alarm->weekday_mask & weekday) == 0U) {
                        continue;
                    }
                    if (tm_now.tm_hour == alarm->hour && tm_now.tm_min == alarm->minute) {
                        alarm->ringing = true;
                        alarm->last_trigger_day = day;
                        char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1];
                        snprintf(message, sizeof(message), "%s",
                                 alarm->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE && alarm->speech_text[0] ?
                                     alarm->speech_text :
                                     (alarm->label[0] ? alarm->label : "厨房提醒"));
                        if (alarm->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE && !alarm->speech_text[0]) {
                            strlcat(message, "闹钟到了", sizeof(message));
                        }
                        trigger_alert_locked(message, alarm->sound);
                        (void)save_alarms_locked();
                    }
                }
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(KITCHEN_TOOLS_POLL_MS));
    }
}

esp_err_t fridge_kitchen_tools_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "create kitchen tools mutex failed");
    ESP_RETURN_ON_ERROR(fridge_storage_init(), TAG, "storage init before kitchen tools failed");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        memset(s_timers, 0, sizeof(s_timers));
        s_next_timer_id = 1;
        memset(&s_stopwatch, 0, sizeof(s_stopwatch));
        memset(s_alarms, 0, sizeof(s_alarms));
        s_alarm_count = 0;
        load_alarms_locked();
        s_initialized = true;
        xSemaphoreGive(s_lock);
    }

    // 主循环会在闹钟触发时持久化 last_trigger_day 到 Flash；任务栈必须在内部 SRAM，
    // 避免 Flash 访问关闭 cache 时访问 PSRAM 栈导致断言复位。
    BaseType_t ok = xTaskCreate(kitchen_tools_task,
                                "kitchen_tools",
                                KITCHEN_TOOLS_TASK_STACK,
                                NULL,
                                KITCHEN_TOOLS_TASK_PRIORITY,
                                &s_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG,
                 "create kitchen tools internal stack failed, stack=%lu, largest_internal=%u bytes",
                 (unsigned long)KITCHEN_TOOLS_TASK_STACK,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create kitchen tools task failed");
    ESP_LOGI(TAG, "kitchen tools initialized, alarms=%u", (unsigned)s_alarm_count);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_get_snapshot(fridge_kitchen_tools_snapshot_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "snapshot output is NULL");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before snapshot failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "snapshot lock timeout");
    fill_snapshot_locked(out);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_start_ex(uint32_t seconds,
                                              const char *label,
                                              const char *source,
                                              fridge_kitchen_timer_sound_t sound,
                                              uint8_t *out_id)
{
    ESP_RETURN_ON_FALSE(seconds > 0 && seconds <= 24 * 60 * 60, ESP_ERR_INVALID_ARG, TAG, "timer seconds invalid");
    ESP_RETURN_ON_FALSE(sound <= FRIDGE_KITCHEN_TIMER_SOUND_VOICE, ESP_ERR_INVALID_ARG, TAG, "timer sound invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer start failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");

    fridge_kitchen_timer_snapshot_t *slot = timer_alloc_slot_locked();
    if (!slot) {
        set_last_error("定时器数量已满，最多 5 个");
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    int64_t now = now_ms();
    memset(slot, 0, sizeof(*slot));
    // id 循环 1..255，跳过 0；冲突时再分配（槽位很少，几乎不会撞）。
    uint8_t id = s_next_timer_id;
    for (int guard = 0; guard < 256; guard++) {
        if (id == 0) {
            id = 1;
        }
        if (!timer_find_locked(id)) {
            break;
        }
        id++;
    }
    s_next_timer_id = (uint8_t)(id + 1);
    if (s_next_timer_id == 0) {
        s_next_timer_id = 1;
    }

    slot->id = id;
    slot->state = FRIDGE_KITCHEN_TIMER_RUNNING;
    slot->sound = sound;
    slot->duration_seconds = seconds;
    slot->remaining_seconds = seconds;
    slot->started_at_ms = now;
    slot->target_at_ms = now + ((int64_t)seconds * 1000);
    copy_label(slot->label, sizeof(slot->label), label, "厨房计时");
    copy_label(slot->source, sizeof(slot->source), source, "手动");
    set_last_error("");
    if (out_id) {
        *out_id = id;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_start(uint32_t seconds, const char *label)
{
    return fridge_kitchen_tools_timer_start_ex(seconds, label, "手动", FRIDGE_KITCHEN_TIMER_SOUND_VOICE, NULL);
}

esp_err_t fridge_kitchen_tools_timer_pause_id(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer pause failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_find_locked(id);
    if (!slot || slot->state != FRIDGE_KITCHEN_TIMER_RUNNING) {
        xSemaphoreGive(s_lock);
        return slot ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    slot->remaining_seconds = timer_slot_remaining_locked(slot, now_ms());
    slot->state = FRIDGE_KITCHEN_TIMER_PAUSED;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_pause(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer pause failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_primary_locked(now_ms());
    if (!slot || slot->state != FRIDGE_KITCHEN_TIMER_RUNNING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    slot->remaining_seconds = timer_slot_remaining_locked(slot, now_ms());
    slot->state = FRIDGE_KITCHEN_TIMER_PAUSED;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_resume_id(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer resume failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_find_locked(id);
    if (!slot || slot->state != FRIDGE_KITCHEN_TIMER_PAUSED || slot->remaining_seconds == 0) {
        xSemaphoreGive(s_lock);
        return slot ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    int64_t now = now_ms();
    slot->state = FRIDGE_KITCHEN_TIMER_RUNNING;
    slot->started_at_ms = now;
    slot->target_at_ms = now + ((int64_t)slot->remaining_seconds * 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_resume(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer resume failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_primary_locked(now_ms());
    if (!slot || slot->state != FRIDGE_KITCHEN_TIMER_PAUSED || slot->remaining_seconds == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int64_t now = now_ms();
    slot->state = FRIDGE_KITCHEN_TIMER_RUNNING;
    slot->started_at_ms = now;
    slot->target_at_ms = now + ((int64_t)slot->remaining_seconds * 1000);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_cancel_id(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer cancel failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_find_locked(id);
    if (!slot) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_cancel(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer cancel failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_primary_locked(now_ms());
    if (slot) {
        memset(slot, 0, sizeof(*slot));
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_add_seconds(uint8_t id, int32_t delta_seconds)
{
    ESP_RETURN_ON_FALSE(delta_seconds != 0, ESP_ERR_INVALID_ARG, TAG, "timer delta invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer add failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_find_locked(id);
    if (!slot) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    int64_t now = now_ms();
    uint32_t remain = timer_slot_remaining_locked(slot, now);
    // RINGING / DONE 延长时视为从 0 再加，并重新进入 RUNNING。
    if (slot->state == FRIDGE_KITCHEN_TIMER_RINGING || slot->state == FRIDGE_KITCHEN_TIMER_DONE) {
        remain = 0;
    } else if (slot->state != FRIDGE_KITCHEN_TIMER_RUNNING && slot->state != FRIDGE_KITCHEN_TIMER_PAUSED) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    int64_t next = (int64_t)remain + (int64_t)delta_seconds;
    if (next < 1) {
        next = 1;
    }
    if (next > 24 * 60 * 60) {
        next = 24 * 60 * 60;
    }
    slot->remaining_seconds = (uint32_t)next;
    slot->duration_seconds += (delta_seconds > 0) ? (uint32_t)delta_seconds : 0;
    if (slot->duration_seconds > 24 * 60 * 60) {
        slot->duration_seconds = 24 * 60 * 60;
    }
    slot->state = FRIDGE_KITCHEN_TIMER_RUNNING;
    slot->started_at_ms = now;
    slot->target_at_ms = now + next * 1000;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_timer_dismiss_id(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer dismiss failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_find_locked(id);
    if (!slot) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    // 结束到时：进入 DONE 供列表展示，而不是直接清空。
    if (slot->state == FRIDGE_KITCHEN_TIMER_RINGING || slot->state == FRIDGE_KITCHEN_TIMER_RUNNING ||
        slot->state == FRIDGE_KITCHEN_TIMER_PAUSED) {
        slot->state = FRIDGE_KITCHEN_TIMER_DONE;
        slot->remaining_seconds = 0;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (slot->state == FRIDGE_KITCHEN_TIMER_DONE) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t fridge_kitchen_tools_timer_dismiss(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before timer dismiss failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    fridge_kitchen_timer_snapshot_t *slot = timer_primary_locked(now_ms());
    if (!slot) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t id = slot->id;
    xSemaphoreGive(s_lock);
    return fridge_kitchen_tools_timer_dismiss_id(id);
}

esp_err_t fridge_kitchen_tools_timer_clear_done(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before clear done failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "timer lock timeout");
    for (size_t i = 0; i < FRIDGE_KITCHEN_TIMER_MAX; i++) {
        if (s_timers[i].id != 0 && s_timers[i].state == FRIDGE_KITCHEN_TIMER_DONE) {
            memset(&s_timers[i], 0, sizeof(s_timers[i]));
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_stopwatch_start(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch start failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    int64_t now = now_ms();
    if (s_stopwatch.state != FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        s_stopwatch.started_at_ms = now;
        s_stopwatch.state = FRIDGE_KITCHEN_STOPWATCH_RUNNING;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_stopwatch_pause(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch pause failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    if (s_stopwatch.state != FRIDGE_KITCHEN_STOPWATCH_RUNNING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stopwatch.elapsed_ms = stopwatch_elapsed_ms_locked(now_ms());
    stopwatch_sync_legacy_seconds(&s_stopwatch);
    s_stopwatch.state = FRIDGE_KITCHEN_STOPWATCH_PAUSED;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_stopwatch_lap(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch lap failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    uint64_t elapsed_ms = stopwatch_elapsed_ms_locked(now_ms());
    if (elapsed_ms == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    // 新记录放在数组头部，保持与浏览器参考页“最近计次在最上方”的顺序一致。
    size_t move_count = s_stopwatch.lap_count;
    if (move_count >= FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS) {
        move_count = FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS - 1U;
        s_stopwatch.lap_count = FRIDGE_KITCHEN_STOPWATCH_MAX_LAPS;
    } else {
        s_stopwatch.lap_count++;
    }
    if (move_count > 0) {
        memmove(&s_stopwatch.laps_ms[1], &s_stopwatch.laps_ms[0],
                move_count * sizeof(s_stopwatch.laps_ms[0]));
    }
    s_stopwatch.laps_ms[0] = elapsed_ms;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_stopwatch_reset(void)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before stopwatch reset failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "stopwatch lock timeout");
    memset(&s_stopwatch, 0, sizeof(s_stopwatch));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static uint8_t next_alarm_id_locked(void)
{
    for (uint8_t id = 1; id < 250; id++) {
        bool used = false;
        for (size_t i = 0; i < s_alarm_count; i++) {
            if (s_alarms[i].id == id) {
                used = true;
                break;
            }
        }
        if (!used) {
            return id;
        }
    }
    return 0;
}

esp_err_t fridge_kitchen_tools_alarm_set(uint8_t hour, uint8_t minute, const char *label, uint8_t *out_id)
{
    fridge_kitchen_alarm_config_t config;
    alarm_config_defaults(&config);
    config.hour = hour;
    config.minute = minute;
    copy_label(config.label, sizeof(config.label), label, "厨房提醒");
    return fridge_kitchen_tools_alarm_create(&config, out_id);
}

esp_err_t fridge_kitchen_tools_alarm_create(const fridge_kitchen_alarm_config_t *config, uint8_t *out_id)
{
    ESP_RETURN_ON_FALSE(alarm_config_valid(config), ESP_ERR_INVALID_ARG, TAG, "alarm config invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm create failed");
    ESP_RETURN_ON_FALSE(time_ready(), ESP_ERR_INVALID_STATE, TAG, "wall clock is not ready");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    if (s_alarm_count >= FRIDGE_KITCHEN_TOOL_MAX_ALARMS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    uint8_t id = next_alarm_id_locked();
    if (id == 0) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    fridge_kitchen_alarm_t *alarm = &s_alarms[s_alarm_count++];
    memset(alarm, 0, sizeof(*alarm));
    alarm->id = id;
    alarm->enabled = true;
    alarm_apply_config(alarm, config);
    esp_err_t err = save_alarms_locked();
    if (err != ESP_OK) {
        s_alarm_count--;
        memset(alarm, 0, sizeof(*alarm));
    } else if (out_id) {
        *out_id = id;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t fridge_kitchen_tools_alarm_update(uint8_t id, const fridge_kitchen_alarm_config_t *config)
{
    ESP_RETURN_ON_FALSE(id != 0 && alarm_config_valid(config), ESP_ERR_INVALID_ARG, TAG,
                        "alarm update config invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm update failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id != id) continue;
        fridge_kitchen_alarm_t previous = s_alarms[i];
        alarm_apply_config(&s_alarms[i], config);
        // 改变时刻或规则后允许在当天按新规则触发，不能沿用旧时间的已触发标记。
        s_alarms[i].last_trigger_day = 0;
        s_alarms[i].ringing = false;
        esp_err_t err = save_alarms_locked();
        if (err != ESP_OK) s_alarms[i] = previous;
        xSemaphoreGive(s_lock);
        return err;
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_kitchen_tools_alarm_set_enabled(uint8_t id, bool enabled)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm enable failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id != id) continue;
        bool previous = s_alarms[i].enabled;
        bool previous_ringing = s_alarms[i].ringing;
        s_alarms[i].enabled = enabled;
        if (!enabled) s_alarms[i].ringing = false;
        esp_err_t err = save_alarms_locked();
        if (err != ESP_OK) {
            s_alarms[i].enabled = previous;
            s_alarms[i].ringing = previous_ringing;
        }
        xSemaphoreGive(s_lock);
        return err;
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_kitchen_tools_alarm_preview(const fridge_kitchen_alarm_config_t *config)
{
    ESP_RETURN_ON_FALSE(alarm_config_valid(config), ESP_ERR_INVALID_ARG, TAG, "alarm preview config invalid");
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm preview failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1] = {0};
    if (config->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE && config->speech_text[0]) {
        copy_label(message, sizeof(message), config->speech_text, "厨房提醒到了");
    } else {
        copy_label(message, sizeof(message), config->label, "厨房提醒");
        if (config->sound == FRIDGE_KITCHEN_ALARM_SOUND_VOICE) {
            strlcat(message, "闹钟到了", sizeof(message));
        }
    }
    trigger_alert_locked(message, config->sound);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_kitchen_tools_alarm_cancel(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm cancel failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id == id) {
            fridge_kitchen_alarm_t backup[FRIDGE_KITCHEN_TOOL_MAX_ALARMS];
            size_t backup_count = s_alarm_count;
            memcpy(backup, s_alarms, sizeof(backup));
            if (i + 1 < s_alarm_count) {
                memmove(&s_alarms[i], &s_alarms[i + 1], (s_alarm_count - i - 1) * sizeof(s_alarms[0]));
            }
            s_alarm_count--;
            esp_err_t err = save_alarms_locked();
            if (err != ESP_OK) {
                memcpy(s_alarms, backup, sizeof(backup));
                s_alarm_count = backup_count;
            }
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_kitchen_tools_alarm_dismiss(uint8_t id)
{
    ESP_RETURN_ON_ERROR(fridge_kitchen_tools_init(), TAG, "init before alarm dismiss failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "alarm lock timeout");
    for (size_t i = 0; i < s_alarm_count; i++) {
        if (s_alarms[i].id == id) {
            bool old_ringing = s_alarms[i].ringing;
            s_alarms[i].ringing = false;
            esp_err_t err = save_alarms_locked();
            if (err != ESP_OK) {
                s_alarms[i].ringing = old_ringing;
            }
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool json_bool(const cJSON *root, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void ai_result(fridge_kitchen_tools_ai_result_t *out,
                      bool executed,
                      bool needs_confirmation,
                      const char *tool,
                      const char *action,
                      const char *message)
{
    if (!out) {
        return;
    }
    out->executed = executed;
    out->needs_confirmation = needs_confirmation;
    strlcpy(out->tool, tool ? tool : "", sizeof(out->tool));
    strlcpy(out->action, action ? action : "", sizeof(out->action));
    strlcpy(out->message, message ? message : "", sizeof(out->message));
}

esp_err_t fridge_kitchen_tools_execute_ai_json(const char *json, fridge_kitchen_tools_ai_result_t *out)
{
    ESP_RETURN_ON_FALSE(json && out, ESP_ERR_INVALID_ARG, TAG, "invalid AI json args");
    memset(out, 0, sizeof(*out));
    const char *start = strchr(json, '{');
    const char *end = strrchr(json, '}');
    if (!start || !end || end <= start) {
        ai_result(out, false, true, "", "", "没有找到可执行的厨房工具指令");
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = (size_t)(end - start + 1);
    char *slice = calloc(1, len + 1);
    ESP_RETURN_ON_FALSE(slice, ESP_ERR_NO_MEM, TAG, "allocate AI json slice failed");
    memcpy(slice, start, len);
    cJSON *root = cJSON_Parse(slice);
    free(slice);
    if (!root) {
        ai_result(out, false, true, "", "", "厨房工具指令不是有效 JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *tool = json_string(root, "tool");
    const char *action = json_string(root, "action");
    const char *label = json_string(root, "label");
    const cJSON *field = NULL;
    cJSON_ArrayForEach(field, root) {
        if (!json_key_allowed(field->string)) {
            cJSON_Delete(root);
            ai_result(out, false, true, "", "", "厨房工具指令包含不支持的字段");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!tool || !action) {
        cJSON_Delete(root);
        ai_result(out, false, true, "", "", "厨房工具指令缺少 tool 或 action");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    char message[FRIDGE_KITCHEN_TOOL_MAX_MESSAGE_LEN + 1] = {0};
    if (strcmp(tool, "timer") == 0) {
        if (strcmp(action, "start") == 0) {
            int seconds = json_int(root, "duration_seconds", 0);
            if (seconds <= 0) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "定时器缺少有效时长");
                return ESP_ERR_INVALID_ARG;
            }
            err = fridge_kitchen_tools_timer_start((uint32_t)seconds, label);
            snprintf(message, sizeof(message), "已启动%s定时器，%d分钟%02d秒",
                     label && label[0] ? label : "厨房",
                     seconds / 60,
                     seconds % 60);
        } else if (strcmp(action, "pause") == 0) {
            err = fridge_kitchen_tools_timer_pause();
            strlcpy(message, "定时器已暂停", sizeof(message));
        } else if (strcmp(action, "resume") == 0) {
            err = fridge_kitchen_tools_timer_resume();
            strlcpy(message, "定时器已继续", sizeof(message));
        } else if (strcmp(action, "cancel") == 0 || strcmp(action, "dismiss") == 0) {
            err = fridge_kitchen_tools_timer_cancel();
            strlcpy(message, "定时器已取消", sizeof(message));
        }
    } else if (strcmp(tool, "stopwatch") == 0) {
        if (strcmp(action, "start") == 0 || strcmp(action, "resume") == 0) {
            err = fridge_kitchen_tools_stopwatch_start();
            strlcpy(message, "秒表已开始", sizeof(message));
        } else if (strcmp(action, "pause") == 0) {
            err = fridge_kitchen_tools_stopwatch_pause();
            strlcpy(message, "秒表已暂停", sizeof(message));
        } else if (strcmp(action, "reset") == 0 || strcmp(action, "cancel") == 0) {
            err = fridge_kitchen_tools_stopwatch_reset();
            strlcpy(message, "秒表已重置", sizeof(message));
        }
    } else if (strcmp(tool, "alarm") == 0) {
        if (strcmp(action, "set") == 0 || strcmp(action, "create") == 0 ||
            strcmp(action, "update") == 0 || strcmp(action, "preview") == 0) {
            int hour = json_int(root, "hour", -1);
            int minute = json_int(root, "minute", -1);
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "闹钟缺少有效时间");
                return ESP_ERR_INVALID_ARG;
            }
            fridge_kitchen_alarm_config_t config;
            alarm_config_defaults(&config);
            config.hour = (uint8_t)hour;
            config.minute = (uint8_t)minute;
            config.kind = (fridge_kitchen_alarm_kind_t)json_int(root, "kind", config.kind);
            config.repeat = (fridge_kitchen_alarm_repeat_t)json_int(root, "repeat", config.repeat);
            config.weekday_mask = (uint8_t)json_int(root, "weekday_mask", config.weekday_mask);
            config.sound = (fridge_kitchen_alarm_sound_t)json_int(root, "sound", config.sound);
            config.source = FRIDGE_KITCHEN_ALARM_SOURCE_AGENT;
            copy_label(config.label, sizeof(config.label), label, "厨房提醒");
            copy_label(config.speech_text, sizeof(config.speech_text),
                       json_string(root, "speech_text"), "");
            if (!alarm_config_valid(&config)) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "闹钟周期或提醒声音参数无效");
                return ESP_ERR_INVALID_ARG;
            }
            if (strcmp(action, "update") == 0) {
                int id = json_int(root, "id", -1);
                if (id <= 0 || id >= 250) {
                    cJSON_Delete(root);
                    ai_result(out, false, true, tool, action, "请指定要更新的闹钟");
                    return ESP_ERR_INVALID_ARG;
                }
                err = fridge_kitchen_tools_alarm_update((uint8_t)id, &config);
                snprintf(message, sizeof(message), "已更新%02d:%02d的%s提醒", hour, minute,
                         config.label);
            } else if (strcmp(action, "preview") == 0) {
                err = fridge_kitchen_tools_alarm_preview(&config);
                strlcpy(message, "正在预览提醒声音", sizeof(message));
            } else {
                uint8_t id = 0;
                err = fridge_kitchen_tools_alarm_create(&config, &id);
                snprintf(message, sizeof(message), "已设置%02d:%02d的%s提醒", hour, minute,
                         config.label);
            }
        } else if (strcmp(action, "enable") == 0 || strcmp(action, "disable") == 0) {
            int id = json_int(root, "id", -1);
            if (id <= 0 || id >= 250) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "请指定要开关的闹钟");
                return ESP_ERR_INVALID_ARG;
            }
            bool enabled = strcmp(action, "enable") == 0 ? true :
                           strcmp(action, "disable") == 0 ? false :
                           json_bool(root, "enabled", true);
            err = fridge_kitchen_tools_alarm_set_enabled((uint8_t)id, enabled);
            strlcpy(message, enabled ? "闹钟已启用" : "闹钟已关闭", sizeof(message));
        } else if (strcmp(action, "cancel") == 0 || strcmp(action, "dismiss") == 0) {
            int id = json_int(root, "id", -1);
            if (id < 0) {
                cJSON_Delete(root);
                ai_result(out, false, true, tool, action, "请确认要取消或停止哪个闹钟");
                return ESP_OK;
            }
            err = strcmp(action, "dismiss") == 0 ? fridge_kitchen_tools_alarm_dismiss((uint8_t)id)
                                                  : fridge_kitchen_tools_alarm_cancel((uint8_t)id);
            strlcpy(message, strcmp(action, "dismiss") == 0 ? "闹钟已停止提醒" : "闹钟已取消", sizeof(message));
        }
    } else {
        strlcpy(message, "不支持这个厨房工具", sizeof(message));
    }

    cJSON_Delete(root);
    if (err != ESP_OK) {
        if (message[0] == '\0') {
            strlcpy(message, kitchen_err_text(err, tool, action), sizeof(message));
        }
        ai_result(out, false, true, tool, action, message);
        return err;
    }
    if (message[0] == '\0') {
        ai_result(out, false, true, tool, action, "不支持这个厨房工具动作");
        return ESP_ERR_INVALID_ARG;
    }
    ai_result(out, true, false, tool, action, message);
    return ESP_OK;
}

const char *fridge_kitchen_timer_state_text(fridge_kitchen_timer_state_t state)
{
    switch (state) {
    case FRIDGE_KITCHEN_TIMER_RUNNING:
        return "running";
    case FRIDGE_KITCHEN_TIMER_PAUSED:
        return "paused";
    case FRIDGE_KITCHEN_TIMER_RINGING:
        return "ringing";
    case FRIDGE_KITCHEN_TIMER_DONE:
        return "done";
    case FRIDGE_KITCHEN_TIMER_IDLE:
    default:
        return "idle";
    }
}

const char *fridge_kitchen_stopwatch_state_text(fridge_kitchen_stopwatch_state_t state)
{
    switch (state) {
    case FRIDGE_KITCHEN_STOPWATCH_RUNNING:
        return "running";
    case FRIDGE_KITCHEN_STOPWATCH_PAUSED:
        return "paused";
    case FRIDGE_KITCHEN_STOPWATCH_IDLE:
    default:
        return "idle";
    }
}
