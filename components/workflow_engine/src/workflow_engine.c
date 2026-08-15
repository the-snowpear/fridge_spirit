// 冰箱小精灵最小 Workflow Engine 实现。
//
// 当前实现只执行固定内置工作流和单次 Tool Broker 调用，不支持任意脚本、循环或递归。
// 检查点写入 TF 卡 /sdcard/agent/runtime/active_run.json，便于掉电后查看最后一个确定性流程停在哪一步。

#include "fridge_workflow_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_agent_storage.h"
#include "fridge_tfcard.h"
#include "fridge_tool_broker.h"
#include "fridge_tool_registry.h"
#include "fridge_tool_types.h"
#include "fridge_trace_service.h"

static const char *TAG = "fridge_workflow_engine";

typedef struct {
    fridge_tool_definition_t definition;
    fridge_tool_call_t call;
    fridge_tool_result_t result;
} workflow_work_t;

static void *workflow_alloc(size_t bytes)
{
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(1, bytes);
    return ptr;
}

static fridge_workflow_status_t s_status = {
    .ready = false,
    .run_sequence = 0,
    .last_workflow_id = "",
    .last_run_id = "",
    .last_state = "idle",
    .last_error = "not initialized",
};

typedef struct {
    const char *workflow_id;
    const char *tool_name;
    const char *description;
    bool always_confirm;
} workflow_template_t;

static const workflow_template_t k_templates[] = {
    {"inventory.edit_confirm", "inventory.update", "确认后更新库存条目", true},
    {"task.create_timer", "task.create_timer", "创建本地厨房计时器记录", false},
    {"memory.preference_confirm", "memory.add", "确认后写入结构化偏好记忆", true},
    {"memory_md.append_confirm", "memory_md.append", "确认后追加 MEMORY.md", true},
    {"device.self_check", "device.state.get", "读取设备语义状态", false},
};

const char *fridge_workflow_state_name(fridge_workflow_state_t state)
{
    switch (state) {
    case FRIDGE_WORKFLOW_STATE_IDLE:
        return "idle";
    case FRIDGE_WORKFLOW_STATE_RUNNING:
        return "running";
    case FRIDGE_WORKFLOW_STATE_WAITING_CONFIRMATION:
        return "waiting_confirmation";
    case FRIDGE_WORKFLOW_STATE_COMPLETED:
        return "completed";
    case FRIDGE_WORKFLOW_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void set_status(const char *workflow_id, const char *run_id, fridge_workflow_state_t state, const char *error)
{
    strlcpy(s_status.last_workflow_id, workflow_id ? workflow_id : "", sizeof(s_status.last_workflow_id));
    strlcpy(s_status.last_run_id, run_id ? run_id : "", sizeof(s_status.last_run_id));
    strlcpy(s_status.last_state, fridge_workflow_state_name(state), sizeof(s_status.last_state));
    strlcpy(s_status.last_error, error ? error : "", sizeof(s_status.last_error));
}

static const workflow_template_t *find_template(const char *workflow_id)
{
    if (!workflow_id || !workflow_id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_templates) / sizeof(k_templates[0]); i++) {
        if (strcmp(k_templates[i].workflow_id, workflow_id) == 0) {
            return &k_templates[i];
        }
    }
    return NULL;
}

static void json_escape_to(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p && out + 1 < dst_size; p++) {
        if ((*p == '"' || *p == '\\') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = (char)*p;
        } else if (*p == '\n' && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else if (*p == '\r' && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = 'r';
        } else if (*p >= 0x20) {
            dst[out++] = (char)*p;
        } else {
            dst[out++] = '?';
        }
    }
    dst[out] = '\0';
}

static void checkpoint_write(const char *run_id,
                             const char *workflow_id,
                             const char *step,
                             fridge_workflow_state_t state,
                             const char *tool,
                             const char *summary)
{
    char escaped_summary[256] = {0};
    json_escape_to(summary, escaped_summary, sizeof(escaped_summary));
    char json[768];
    const int len = snprintf(json,
                             sizeof(json),
                             "{\n"
                             "  \"schema_version\": 1,\n"
                             "  \"run_id\": \"%s\",\n"
                             "  \"workflow_id\": \"%s\",\n"
                             "  \"step\": \"%s\",\n"
                             "  \"state\": \"%s\",\n"
                             "  \"tool\": \"%s\",\n"
                             "  \"summary\": \"%s\",\n"
                             "  \"at_ms\": %llu\n"
                             "}\n",
                             run_id ? run_id : "",
                             workflow_id ? workflow_id : "",
                             step ? step : "",
                             fridge_workflow_state_name(state),
                             tool ? tool : "",
                             escaped_summary,
                             (unsigned long long)(esp_timer_get_time() / 1000));
    if (len <= 0 || len >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "workflow checkpoint too large");
        return;
    }
    esp_err_t err = fridge_agent_storage_json_write(FRIDGE_WORKFLOW_ACTIVE_RUN_PATH, json, (size_t)len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write workflow checkpoint failed: %s", esp_err_to_name(err));
    }
}

static void build_result_json(fridge_workflow_run_result_t *out,
                              const char *tool,
                              const fridge_tool_result_t *tool_result,
                              uint32_t latency_ms)
{
    // ToolResult 最多 4KB，而 Workflow 返回缓冲只有 2KB；这里显式截断，避免 snprintf 隐式截断被当作构建错误。
    const char *tool_data = tool_result && tool_result->data_json[0] ? tool_result->data_json : "{}";
    const size_t tool_data_len = strlen(tool_data);
    int n = snprintf(out->result_json,
                     sizeof(out->result_json),
                     "{\"workflowId\":\"%s\",\"runId\":\"%s\",\"state\":\"%s\","
                     "\"tool\":\"%s\",\"toolStatus\":\"%s\",\"toolCode\":\"%s\","
                     "\"latencyMs\":%u,\"toolData\":",
                     out->workflow_id,
                     out->run_id,
                     fridge_workflow_state_name(out->state),
                     tool ? tool : "",
                     tool_result ? fridge_tool_result_status_name(tool_result->status) : "none",
                     tool_result ? tool_result->code : "",
                     (unsigned)latency_ms);
    if (n < 0 || (size_t)n >= sizeof(out->result_json)) {
        strlcpy(out->result_json, "{\"toolData\":{\"truncated\":true}}", sizeof(out->result_json));
        return;
    }

    const size_t used = (size_t)n;
    if (used + tool_data_len + 2U < sizeof(out->result_json)) {
        strlcat(out->result_json, tool_data, sizeof(out->result_json));
    } else {
        char truncated[96];
        snprintf(truncated,
                 sizeof(truncated),
                 "{\"truncated\":true,\"bytes\":%u}",
                 (unsigned)tool_data_len);
        strlcat(out->result_json, truncated, sizeof(out->result_json));
    }
    strlcat(out->result_json, "}", sizeof(out->result_json));
}

esp_err_t fridge_workflow_engine_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.ready = true;
    strlcpy(s_status.last_state, "idle", sizeof(s_status.last_state));
    ESP_LOGI(TAG, "minimal Workflow Engine ready");
    return ESP_OK;
}

void fridge_workflow_engine_get_status(fridge_workflow_status_t *out)
{
    if (!out) return;
    *out = s_status;
}

esp_err_t fridge_workflow_engine_start(const char *workflow_id,
                                       const char *arguments_json,
                                       bool user_confirmed,
                                       fridge_workflow_run_result_t *out)
{
    if (!workflow_id || !workflow_id[0] || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_status.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const workflow_template_t *tmpl = find_template(workflow_id);
    if (!tmpl) {
        return ESP_ERR_NOT_FOUND;
    }

    // Tool 定义、调用和结果合计超过 6KB，统一放入 PSRAM，避免压垮调用方任务栈。
    workflow_work_t *work = workflow_alloc(sizeof(*work));
    if (!work) {
        return ESP_ERR_NO_MEM;
    }

    memset(out, 0, sizeof(*out));
    s_status.run_sequence++;
    strlcpy(out->workflow_id, workflow_id, sizeof(out->workflow_id));
    snprintf(out->run_id, sizeof(out->run_id), "wf_%06u", (unsigned)s_status.run_sequence);
    out->state = FRIDGE_WORKFLOW_STATE_RUNNING;
    set_status(workflow_id, out->run_id, out->state, "");

    const int64_t start_us = esp_timer_get_time();
    checkpoint_write(out->run_id, workflow_id, "start", out->state, tmpl->tool_name, tmpl->description);

    esp_err_t err = fridge_tool_registry_find(tmpl->tool_name, &work->definition);
    if (err != ESP_OK) {
        out->state = FRIDGE_WORKFLOW_STATE_FAILED;
        strlcpy(out->error, "workflow tool not registered", sizeof(out->error));
        set_status(workflow_id, out->run_id, out->state, out->error);
        free(work);
        return err;
    }

    if ((tmpl->always_confirm || work->definition.permission != FRIDGE_TOOL_PERMISSION_READ ||
         work->definition.requires_confirmation) &&
        !user_confirmed) {
        out->state = FRIDGE_WORKFLOW_STATE_WAITING_CONFIRMATION;
        out->ok = false;
        snprintf(out->reply,
                 sizeof(out->reply),
                 "工作流 %s 需要用户确认后才能执行工具 %s。请用 confirmed=true 重试。",
                 workflow_id,
                 tmpl->tool_name);
        strlcpy(out->error, "confirmation required", sizeof(out->error));
        work->result.status = FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION;
        strlcpy(work->result.code, "CONFIRM_REQUIRED", sizeof(work->result.code));
        strlcpy(work->result.summary, out->reply, sizeof(work->result.summary));
        checkpoint_write(out->run_id, workflow_id, "wait_confirmation", out->state, tmpl->tool_name, out->reply);
        build_result_json(out,
                          tmpl->tool_name,
                          &work->result,
                          (uint32_t)((esp_timer_get_time() - start_us) / 1000));
        set_status(workflow_id, out->run_id, out->state, out->error);
        free(work);
        return ESP_ERR_INVALID_STATE;
    }

    checkpoint_write(out->run_id, workflow_id, "before_tool", out->state, tmpl->tool_name, "准备通过 Tool Broker 执行工具");
    strlcpy(work->call.call_id, out->run_id, sizeof(work->call.call_id));
    strlcpy(work->call.run_id, out->run_id, sizeof(work->call.run_id));
    strlcpy(work->call.tool_name, tmpl->tool_name, sizeof(work->call.tool_name));
    strlcpy(work->call.arguments_json,
            (arguments_json && arguments_json[0]) ? arguments_json : "{}",
            sizeof(work->call.arguments_json));
    work->call.user_confirmed = user_confirmed;

    err = fridge_tool_broker_call(&work->call, &work->result);
    checkpoint_write(out->run_id, workflow_id, "after_tool", out->state, tmpl->tool_name, work->result.summary);

    if (err == ESP_OK && work->result.status == FRIDGE_TOOL_RESULT_OK) {
        out->state = FRIDGE_WORKFLOW_STATE_COMPLETED;
        out->ok = true;
        snprintf(out->reply, sizeof(out->reply), "工作流 %s 已完成：%s", workflow_id, work->result.summary);
        checkpoint_write(out->run_id, workflow_id, "complete", out->state, tmpl->tool_name, work->result.summary);
    } else if (work->result.status == FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION) {
        out->state = FRIDGE_WORKFLOW_STATE_WAITING_CONFIRMATION;
        out->ok = false;
        strlcpy(out->reply, work->result.summary, sizeof(out->reply));
        strlcpy(out->error, work->result.summary, sizeof(out->error));
        checkpoint_write(out->run_id, workflow_id, "wait_confirmation", out->state, tmpl->tool_name, work->result.summary);
    } else {
        out->state = FRIDGE_WORKFLOW_STATE_FAILED;
        out->ok = false;
        strlcpy(out->reply, work->result.summary[0] ? work->result.summary : "工作流执行失败", sizeof(out->reply));
        strlcpy(out->error,
                work->result.summary[0] ? work->result.summary : esp_err_to_name(err),
                sizeof(out->error));
        checkpoint_write(out->run_id, workflow_id, "fail", out->state, tmpl->tool_name, out->error);
    }

    const uint32_t latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    build_result_json(out, tmpl->tool_name, &work->result, latency_ms);
    (void)fridge_trace_append_run(out->result_json);
    set_status(workflow_id, out->run_id, out->state, out->error);
    free(work);
    return err;
}
