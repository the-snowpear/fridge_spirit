// 冰箱小精灵 Agent Trace Service 实现。
//
// Trace 写入 TF 卡 /sdcard/agent/logs/runs.jsonl。当前日志文件限制为 64KB，超限后将旧文件
// 轮转为 runs.prev.jsonl，再创建新的 runs.jsonl，避免比赛演示中长时间运行导致 TF 日志无限增长。

#include "fridge_trace_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "fridge_tfcard.h"

static const char *TAG = "fridge_trace_service";

static fridge_trace_status_t s_status = {
    .ready = false,
    .appended_runs = 0,
    .rotated_count = 0,
    .runs_bytes = 0,
    .last_error = "not initialized",
};

static void set_error(const char *prefix, esp_err_t err)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s", prefix, esp_err_to_name(err));
}

static esp_err_t refresh_size(void)
{
    fridge_tfcard_entry_t entry = {0};
    esp_err_t err = fridge_tfcard_stat(FRIDGE_TRACE_RUNS_PATH, &entry);
    if (err == ESP_OK && !entry.is_dir) {
        s_status.runs_bytes = entry.size;
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        s_status.runs_bytes = 0;
        return ESP_OK;
    }
    return err;
}

static esp_err_t rotate_if_needed(size_t incoming_bytes)
{
    (void)refresh_size();
    if (s_status.runs_bytes + incoming_bytes <= FRIDGE_TRACE_MAX_FILE_BYTES) {
        return ESP_OK;
    }

    fridge_tfcard_entry_t entry = {0};
    esp_err_t err = fridge_tfcard_stat(FRIDGE_TRACE_RUNS_PATH, &entry);
    if (err == ESP_ERR_NOT_FOUND) {
        s_status.runs_bytes = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (entry.is_dir) {
        return ESP_ERR_INVALID_STATE;
    }

    // 轮转时覆盖上一份 prev，Trace 是诊断数据，不做无限历史保留，避免 TF 卡空间失控。
    err = fridge_tfcard_move(FRIDGE_TRACE_RUNS_PATH, FRIDGE_TRACE_RUNS_PREV_PATH, true);
    if (err == ESP_OK) {
        s_status.rotated_count++;
        s_status.runs_bytes = 0;
        ESP_LOGI(TAG, "rotated Agent run trace log");
    }
    return err;
}

esp_err_t fridge_trace_service_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.ready = true;
    esp_err_t err = refresh_size();
    if (err != ESP_OK) {
        set_error("refresh trace size", err);
        // 日志文件缺失以外的错误不阻塞系统启动；后续 append 会再次返回明确错误。
        ESP_LOGW(TAG, "trace size refresh failed: %s", esp_err_to_name(err));
    } else {
        s_status.last_error[0] = '\0';
    }
    ESP_LOGI(TAG, "Trace Service ready, runs_bytes=%llu", (unsigned long long)s_status.runs_bytes);
    return ESP_OK;
}

esp_err_t fridge_trace_append_run(const char *trace_json)
{
    if (!trace_json || !trace_json[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t line_len = strlen(trace_json) + 1; // 追加 '\n'
    if (line_len > FRIDGE_TRACE_MAX_FILE_BYTES / 2) {
        set_error("trace line too large", ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = rotate_if_needed(line_len);
    if (err != ESP_OK) {
        set_error("rotate trace", err);
        return err;
    }

    size_t old_len = 0;
    char *old_text = calloc(1, FRIDGE_TRACE_MAX_FILE_BYTES + 1);
    if (!old_text) {
        set_error("alloc old trace", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    err = fridge_tfcard_text_read(FRIDGE_TRACE_RUNS_PATH, old_text, FRIDGE_TRACE_MAX_FILE_BYTES + 1, &old_len);
    if (err == ESP_ERR_NOT_FOUND) {
        old_len = 0;
        old_text[0] = '\0';
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        free(old_text);
        set_error("read trace", err);
        return err;
    }

    const size_t new_len = old_len + line_len;
    if (new_len > FRIDGE_TRACE_MAX_FILE_BYTES) {
        free(old_text);
        err = fridge_tfcard_move(FRIDGE_TRACE_RUNS_PATH, FRIDGE_TRACE_RUNS_PREV_PATH, true);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            set_error("rotate trace after read", err);
            return err;
        }
        old_len = 0;
        old_text = calloc(1, FRIDGE_TRACE_MAX_FILE_BYTES + 1);
        if (!old_text) {
            set_error("alloc rotated trace", ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
        s_status.rotated_count++;
    }

    char *new_text = calloc(1, old_len + line_len + 1);
    if (!new_text) {
        free(old_text);
        set_error("alloc new trace", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    if (old_len > 0) {
        memcpy(new_text, old_text, old_len);
    }
    memcpy(new_text + old_len, trace_json, strlen(trace_json));
    new_text[old_len + strlen(trace_json)] = '\n';

    err = fridge_tfcard_text_write(FRIDGE_TRACE_RUNS_PATH, new_text, old_len + line_len, true);
    free(old_text);
    free(new_text);
    if (err != ESP_OK) {
        set_error("write trace", err);
        return err;
    }

    s_status.appended_runs++;
    s_status.runs_bytes = old_len + line_len;
    s_status.last_error[0] = '\0';
    return ESP_OK;
}

esp_err_t fridge_trace_read_runs_tail(char *out, size_t out_size, size_t *out_len)
{
    if (!out || out_size == 0 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;
    out[0] = '\0';

    char *text = calloc(1, FRIDGE_TRACE_MAX_FILE_BYTES + 1);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    esp_err_t err = fridge_tfcard_text_read(FRIDGE_TRACE_RUNS_PATH, text, FRIDGE_TRACE_MAX_FILE_BYTES + 1, &n);
    if (err == ESP_ERR_NOT_FOUND) {
        free(text);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        free(text);
        return err;
    }

    const size_t copy_len = n < (out_size - 1) ? n : (out_size - 1);
    const size_t start = n > copy_len ? n - copy_len : 0;
    memcpy(out, text + start, copy_len);
    out[copy_len] = '\0';
    *out_len = copy_len;
    free(text);
    return ESP_OK;
}

void fridge_trace_get_status(fridge_trace_status_t *out)
{
    if (!out) {
        return;
    }
    (void)refresh_size();
    *out = s_status;
}
