// 冰箱小精灵 Agent TF 卡存储初始化。
//
// 这里建立 /sdcard/agent 的固定目录结构和默认配置文件，后续 Agent Runtime、Tool、Skill、Workflow
// 都通过这些路径加载可热更新内容，避免把提示词、技能文档、运行日志等大文本塞进 Flash。

#include "fridge_agent_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_tfcard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fridge_agent_storage";
static SemaphoreHandle_t s_transaction_lock;

// Agent 会话和业务 JSON 可能包含 2 KiB 回复及工具卡；缓冲优先分配到 PSRAM，
// 不能再沿用 64 KiB 而导致完整回复在 TF 写入阶段被拒绝。
#define AGENT_STORAGE_JSON_MAX_BYTES (192U * 1024U)
#define AGENT_MEMORY_CONTEXT_ITEMS_MAX 16

static fridge_agent_storage_status_t s_status = {
    .ready = false,
    .created_defaults = false,
    .created_dirs = 0,
    .created_files = 0,
    .root = FRIDGE_AGENT_STORAGE_ROOT,
    .last_error = "not initialized",
};

static const char *const k_agent_dirs[] = {
    "agent",
    "agent/config",
    "agent/data",
    "agent/skills",
    "agent/skills/system",
    "agent/skills/kitchen",
    "agent/skills/user",
    "agent/skills/drafts",
    "agent/workflows",
    "agent/workflows/kitchen",
    "agent/workflows/task",
    "agent/workflows/memory",
    "agent/runtime",
    "agent/cache",
    "agent/cache/images",
    "agent/cache/audio",
    "agent/cache/model",
    "agent/cache/mcp",
    "agent/logs",
    "agent/docs",
};

static const char k_default_agent_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"device_role\": \"fridge_spirit_p4_agent\",\n"
    "  \"agent_name\": \"冰箱小精灵\",\n"
    "  \"default_permission_mode\": \"confirm\",\n"
    "  \"storage_policy\": \"tfcard_first\",\n"
    "  \"trace_enabled\": true,\n"
    "  \"max_agent_steps\": 6,\n"
    "  \"max_tool_retry\": 2,\n"
    "  \"notes\": \"Agent配置、技能、工作流、记忆、日志优先保存在TF卡；API Key和Wi-Fi凭据仍保存在NVS。\"\n"
    "}\n";

static const char k_default_persona_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"name\": \"小冰\",\n"
    "  \"language\": \"zh-CN\",\n"
    "  \"tone\": {\"warmth\": 0.75, \"humor\": 0.2, \"directness\": 0.8},\n"
    "  \"response\": {\"screen_length\": \"short\", \"voice_length\": \"very_short\", \"use_emoji\": false},\n"
    "  \"behavior\": {\"ask_before_assuming\": true, \"offer_alternatives\": true},\n"
    "  \"product_identity\": \"你是冰箱小精灵，运行在家庭厨房终端上的多模态Agent。你负责管理食材、保质期、购物清单、菜谱和提醒。信息不足时应询问，不能猜测库存或伪造执行结果。\"\n"
    "}\n";

static const char k_default_providers_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"active_provider\": \"device_nvs_default\",\n"
    "  \"providers\": [\n"
    "    {\n"
    "      \"id\": \"device_nvs_default\",\n"
    "      \"name\": \"设备NVS中当前AI配置\",\n"
    "      \"api_key_ref\": \"nvs://fridge_ai/active_profile\",\n"
    "      \"api_format\": \"openai_compatible\",\n"
    "      \"note\": \"比赛阶段复用现有ai_client配置；TF卡只保存可展示元数据，不保存API Key明文。\"\n"
    "    }\n"
    "  ]\n"
    "}\n";

static const char k_default_mcp_servers_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"servers\": [],\n"
    "  \"notes\": \"MCP比赛版只实现initialize、notifications/initialized、tools/list和tools/call；优先只读工具，认证密钥不写入本文件明文。\"\n"
    "}\n";

static const char k_default_inventory_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"items\": [],\n"
    "  \"notes\": \"Agent比赛版库存权威数据；写入必须经过Tool Broker和必要的用户确认。\"\n"
    "}\n";

static const char k_default_tasks_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"tasks\": []\n"
    "}\n";

static const char k_default_memory_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"items\": []\n"
    "}\n";

static const char k_default_sessions_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"sessions\": []\n"
    "}\n";

// P4 Agent MQTT v2 的其余结构化数据域。
// 大对象和历史内容保存在 TF 卡；这里只建立轻量默认文件，不占用 Flash/NVS。
static const char k_default_items_json[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"items\": []\n"
    "}\n";

// 旧默认模板混入了系统规则和开发备注，只用于精确识别并迁移未被用户修改的文件。
static const char k_legacy_default_memory_md[] =
    "# Household Agent Memory\n\n"
    "本文件保存家庭共享记忆、厨房偏好、菜谱反馈和设备备注。\n\n"
    "## Always Remember\n\n"
    "- 食材识别结果必须经过用户确认后才能写入库存。\n"
    "- AI输出不能直接修改库存、任务或记忆，所有写操作必须经过Tool Broker。\n\n"
    "## Household Notes\n\n"
    "- 可在这里记录冰箱分区、常用食材摆放位置和家庭厨房习惯。\n\n"
    "## Cooking Experience\n\n"
    "- 可在这里记录做菜反馈，例如某道菜是否受欢迎、需要少油少盐等。\n\n"
    "## Device Notes\n\n"
    "- ESP32-P4板载TF卡是Agent数据、技能、工作流和日志的优先存储位置。\n"
    "- API Key和Wi-Fi凭据不写入TF卡明文，继续保存在设备NVS。\n\n"
    "## Current Context\n\n"
    "- 当前正在接入冰箱小精灵比赛版Agent Runtime。\n\n"
    "## Miscellaneous\n\n"
    "- 暂时无法分类的长期备注写在这里。\n";

// MEMORY.md 只保存人工维护的家庭笔记，不参与默认模型上下文，也不承载系统规则。
static const char k_default_memory_md[] =
    "# 家庭记忆笔记\n\n"
    "这里保存需要人工维护的家庭共享笔记。普通用户偏好和长期事实请通过结构化记忆保存。\n\n"
    "## 家庭笔记\n\n"
    "- 暂无。\n";

static const char k_docs_readme[] =
    "# 冰箱小精灵 Agent 技术文档入口\n\n"
    "本目录给后续AI和开发者快速理解Agent接入边界使用。\n\n"
    "## 核心原则\n\n"
    "1. 能放在TF卡的Agent内容尽量放在TF卡，节省Flash。\n"
    "2. 模型只提出回复、澄清问题或Tool Call，不能直接执行副作用。\n"
    "3. 库存、任务、记忆、技能、工作流和日志均以 `/sdcard/agent/` 为权威目录。\n"
    "4. API Key和Wi-Fi凭据仍在NVS，不写入TF卡明文。\n\n"
    "## 文档索引\n\n"
    "- `AGENT_ARCH.md`：Agent Runtime总体架构。\n"
    "- `TOOLS.md`：Tool Registry与Tool Broker约定。\n"
    "- `SKILLS.md`：Skill目录与manifest/SKILL.md格式。\n"
    "- `WORKFLOWS.md`：Workflow JSON语法。\n"
    "- `MCP.md`：比赛版MCP接入边界。\n";

static const char k_docs_arch[] =
    "# Agent Runtime 架构\n\n"
    "Agent Runtime运行在ESP32-P4上，负责Router、Run状态机、Context Composer、Tool Broker、Workflow和Trace。\n\n"
    "## 当前落地顺序\n\n"
    "1. TF卡目录和默认文档。\n"
    "2. Tool Registry / Tool Broker。\n"
    "3. 最小文本Agent Loop。\n"
    "4. Skills / Workflow。\n"
    "5. MEMORY.md和结构化家庭记忆。\n"
    "6. MCP和Trace演示。\n";

static const char k_docs_tools[] =
    "# Tool System 约定\n\n"
    "Tool是Agent能调用的最小原子能力，命名采用 `namespace.action`。\n\n"
    "## 权限\n\n"
    "- READ：只读，默认可执行。\n"
    "- WRITE：会改变本地数据，比赛阶段默认需要确认。\n"
    "- DANGEROUS：批量删除、恢复出厂、修改密钥等，必须明确确认。\n\n"
    "## 首批工具\n\n"
    "- `inventory.list` / `inventory.add` / `inventory.update` / `inventory.delete` / `inventory.list_expiring`\n"
    "- `task.create_timer` / `task.create_reminder` / `task.list` / `task.cancel`\n"
    "- `memory.search` / `memory.add` / `memory_md.read` / `memory_md.append`\n"
    "- `device.state.get` / `device.camera.capture` / `device.audio.speak`\n";

static const char k_docs_skills[] =
    "# Skill 约定\n\n"
    "每个Skill位于 `/sdcard/agent/skills/<category>/<skill>/`，至少包含 `manifest.json` 和 `SKILL.md`。\n\n"
    "## manifest.json字段\n\n"
    "- `skill_id`：唯一ID，例如 `kitchen.expiring_meal`。\n"
    "- `triggers.keywords`：关键词触发。\n"
    "- `required_tools`：必须工具。\n"
    "- `execution_mode`：`prompt_only`、`agent_guided` 或 `workflow_backed`。\n\n"
    "## SKILL.md建议章节\n\n"
    "Goal / Use When / Do Not Use When / Required Information / Procedure / Failure Handling / Completion Criteria\n";

static const char k_docs_workflows[] =
    "# Workflow 约定\n\n"
    "Workflow是本地确定性流程，不执行任意脚本。比赛版JSON步骤类型：`tool`、`if`、`switch`、`wait`、`retry`、`return`、`fail`。\n\n"
    "## 检查点\n\n"
    "Workflow开始、副作用工具前后、等待用户确认、完成时写入 `/sdcard/agent/runtime/active_run.json`。\n";

static const char k_docs_mcp[] =
    "# MCP 接入边界\n\n"
    "ESP32-P4只实现轻量MCP Client，通过Streamable HTTP连接外部MCP Server。\n\n"
    "## 比赛版范围\n\n"
    "- `initialize`\n"
    "- `notifications/initialized`\n"
    "- `tools/list`\n"
    "- `tools/call`\n\n"
    "不实现Resources、Prompts、Sampling、OAuth、多Server并发和长期服务端主动通知。\n";

static void set_error(const char *prefix, esp_err_t err)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s", prefix, esp_err_to_name(err));
    s_status.ready = false;
}

static bool json_has_supported_schema(const char *text, size_t text_size, esp_err_t *out_error)
{
    if (out_error) *out_error = ESP_ERR_INVALID_RESPONSE;
    if (!text || text_size == 0) return false;

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, text_size + 1, &parse_end, true);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(schema) || schema->valuedouble < 1) {
        cJSON_Delete(root);
        return false;
    }
    if (schema->valueint > 1) {
        if (out_error) *out_error = ESP_ERR_INVALID_VERSION;
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);
    if (out_error) *out_error = ESP_OK;
    return true;
}

static void *agent_storage_alloc(size_t bytes)
{
    void *ptr = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(1, bytes);
    return ptr;
}

static esp_err_t read_valid_json_file(const char *path, char *buffer, size_t buffer_size, size_t *out_size)
{
    esp_err_t err = fridge_tfcard_text_read(path, buffer, buffer_size, out_size);
    if (err != ESP_OK) return err;
    esp_err_t validation = ESP_OK;
    return json_has_supported_schema(buffer, *out_size, &validation) ? ESP_OK : validation;
}

static esp_err_t restore_json_candidate(const char *path, const char *suffix,
                                        char *buffer, size_t buffer_size, size_t *out_size)
{
    char candidate[FRIDGE_AGENT_STORAGE_MAX_PATH_LEN + 8];
    if (snprintf(candidate, sizeof(candidate), "%s%s", path, suffix) >= (int)sizeof(candidate)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = read_valid_json_file(candidate, buffer, buffer_size, out_size);
    if (err != ESP_OK) return err;

    err = fridge_tfcard_move(candidate, path, true);
    if (err == ESP_OK) {
        s_status.recovered_files++;
        ESP_LOGW(TAG, "recovered Agent JSON from /sdcard/%s", candidate);
    }
    return err;
}

static esp_err_t json_write_unlocked(const char *path, const char *json,
                                     size_t json_size);

static esp_err_t json_read_unlocked(const char *path, char *buffer,
                                    size_t buffer_size, size_t *out_size,
                                    const char *default_json)
{
    if (!path || !buffer || buffer_size == 0 || !out_size) return ESP_ERR_INVALID_ARG;

    esp_err_t err = read_valid_json_file(path, buffer, buffer_size, out_size);
    if (err == ESP_OK || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_VERSION) return err;

    // 正式文件不可用时先恢复上一版已提交数据；只有没有有效 .bak 时才提升未完成的 .part。
    esp_err_t backup_err = restore_json_candidate(path, ".bak", buffer, buffer_size, out_size);
    if (backup_err == ESP_OK || backup_err == ESP_ERR_INVALID_VERSION) return backup_err;
    esp_err_t part_err = restore_json_candidate(path, ".part", buffer, buffer_size, out_size);
    if (part_err == ESP_OK || part_err == ESP_ERR_INVALID_VERSION) return part_err;

    if (!default_json) return err;
    const size_t default_size = strlen(default_json);
    if (default_size + 1 > buffer_size) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(json_write_unlocked(path, default_json, default_size),
                        TAG, "创建 Agent 默认 JSON 失败");
    memcpy(buffer, default_json, default_size + 1);
    *out_size = default_size;
    return ESP_OK;
}

static esp_err_t json_write_unlocked(const char *path, const char *json,
                                     size_t json_size)
{
    if (!path || !json || json_size == 0 || json_size > AGENT_STORAGE_JSON_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t validation = ESP_OK;
    if (!json_has_supported_schema(json, json_size, &validation)) return validation;

    ESP_RETURN_ON_ERROR(fridge_tfcard_text_write_recoverable(path, json, json_size, true),
                        TAG, "写入可恢复 Agent JSON 失败");

    // 从盘读回并重新解析，避免把短写或损坏文件当成已提交成功。
    char *verify = agent_storage_alloc(AGENT_STORAGE_JSON_MAX_BYTES + 1);
    if (!verify) return ESP_ERR_NO_MEM;
    size_t verify_size = 0;
    esp_err_t err = read_valid_json_file(path, verify, AGENT_STORAGE_JSON_MAX_BYTES + 1, &verify_size);
    if (err == ESP_OK && (verify_size != json_size || memcmp(verify, json, json_size) != 0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    free(verify);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Agent JSON disk verification failed for /sdcard/%s: %s", path, esp_err_to_name(err));
        char backup[FRIDGE_AGENT_STORAGE_MAX_PATH_LEN + 8];
        if (snprintf(backup, sizeof(backup), "%s.bak", path) < (int)sizeof(backup)) {
            // 写后复验失败时优先恢复上一版完整文件，不能把疑似损坏的新版本继续暴露给业务层。
            esp_err_t restore_err = fridge_tfcard_move(backup, path, true);
            if (restore_err != ESP_OK) {
                ESP_LOGE(TAG, "restore previous Agent JSON failed for /sdcard/%s: %s",
                         path, esp_err_to_name(restore_err));
            }
        }
    }
    return err;
}

esp_err_t fridge_agent_storage_transaction_begin(uint32_t timeout_ms)
{
    if (!s_transaction_lock) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = timeout_ms == UINT32_MAX ?
                           portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_transaction_lock, ticks) == pdTRUE ?
               ESP_OK : ESP_ERR_TIMEOUT;
}

void fridge_agent_storage_transaction_end(void)
{
    if (s_transaction_lock) xSemaphoreGiveRecursive(s_transaction_lock);
}

esp_err_t fridge_agent_storage_json_read(const char *path, char *buffer,
                                        size_t buffer_size, size_t *out_size,
                                        const char *default_json)
{
    ESP_RETURN_ON_ERROR(fridge_agent_storage_transaction_begin(UINT32_MAX),
                        TAG, "获取 Agent Storage 读取锁失败");
    esp_err_t err = json_read_unlocked(path, buffer, buffer_size,
                                       out_size, default_json);
    fridge_agent_storage_transaction_end();
    return err;
}

esp_err_t fridge_agent_storage_json_write(const char *path, const char *json,
                                         size_t json_size)
{
    ESP_RETURN_ON_ERROR(fridge_agent_storage_transaction_begin(UINT32_MAX),
                        TAG, "获取 Agent Storage 写入锁失败");
    esp_err_t err = json_write_unlocked(path, json, json_size);
    fridge_agent_storage_transaction_end();
    return err;
}

esp_err_t fridge_agent_storage_text_write(const char *path, const char *text, size_t text_size)
{
    if (!path || !text) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(fridge_agent_storage_transaction_begin(UINT32_MAX),
                        TAG, "获取 Agent Storage 文本写入锁失败");
    esp_err_t err =
        fridge_tfcard_text_write_recoverable(path, text, text_size, true);
    fridge_agent_storage_transaction_end();
    return err;
}

esp_err_t fridge_agent_storage_build_memory_context(char *buffer, size_t buffer_size,
                                                    size_t *out_size)
{
    if (!buffer || buffer_size < 64U || !out_size) return ESP_ERR_INVALID_ARG;
    *out_size = 0;
    buffer[0] = '\0';

    char *memory_json = agent_storage_alloc(AGENT_STORAGE_JSON_MAX_BYTES + 1U);
    if (!memory_json) {
        free(memory_json);
        return ESP_ERR_NO_MEM;
    }

    static const char empty_memory[] = "{\"schema_version\":1,\"items\":[]}";
    size_t memory_json_size = 0;
    esp_err_t err = fridge_agent_storage_transaction_begin(UINT32_MAX);
    if (err != ESP_OK) {
        free(memory_json);
        return err;
    }
    err = json_read_unlocked("agent/data/memory.json", memory_json,
                             AGENT_STORAGE_JSON_MAX_BYTES + 1U,
                             &memory_json_size, empty_memory);
    fridge_agent_storage_transaction_end();
    if (err != ESP_OK) {
        free(memory_json);
        return err;
    }

    cJSON *source = cJSON_ParseWithLength(memory_json, memory_json_size);
    const cJSON *source_items = source ? cJSON_GetObjectItemCaseSensitive(source, "items") : NULL;
    cJSON *context = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!source || !cJSON_IsArray(source_items) || !context || !items) {
        cJSON_Delete(source);
        cJSON_Delete(context);
        cJSON_Delete(items);
        free(memory_json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON_AddNumberToObject(context, "schema_version", 1);
    cJSON_AddStringToObject(context, "kind", "user_memory");
    cJSON_AddItemToObject(context, "items", items);
    int total = cJSON_GetArraySize(source_items);
    int start = total > AGENT_MEMORY_CONTEXT_ITEMS_MAX ?
                    total - AGENT_MEMORY_CONTEXT_ITEMS_MAX : 0;
    for (int i = start; i < total; i++) {
        cJSON *copy = cJSON_Duplicate(cJSON_GetArrayItem(source_items, i), true);
        if (copy) cJSON_AddItemToArray(items, copy);
    }
    cJSON_AddBoolToObject(context, "truncated", start > 0);

    // 调用方可提供更小的缓冲；优先丢弃最旧结构化项，始终保证返回完整合法 JSON。
    char *printed = cJSON_PrintUnformatted(context);
    bool buffer_truncated = false;
    while (printed && strlen(printed) + 1U > buffer_size &&
           cJSON_GetArraySize(items) > 0) {
        cJSON_DeleteItemFromArray(items, 0);
        buffer_truncated = true;
        cJSON_free(printed);
        printed = cJSON_PrintUnformatted(context);
    }
    if (printed && buffer_truncated) {
        cJSON *truncated = cJSON_CreateBool(true);
        if (!truncated) {
            cJSON_free(printed);
            printed = NULL;
        } else {
            cJSON_ReplaceItemInObjectCaseSensitive(context, "truncated", truncated);
            cJSON_free(printed);
            printed = cJSON_PrintUnformatted(context);
        }
    }

    if (!printed) {
        err = ESP_ERR_NO_MEM;
    } else if (strlen(printed) + 1U > buffer_size) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        *out_size = strlen(printed);
        memcpy(buffer, printed, *out_size + 1U);
        err = ESP_OK;
    }
    cJSON_free(printed);
    cJSON_Delete(context);
    cJSON_Delete(source);
    free(memory_json);
    return err;
}

static esp_err_t ensure_dir(const char *path)
{
    fridge_tfcard_entry_t entry = {0};
    esp_err_t stat_ret = fridge_tfcard_stat(path, &entry);
    if (stat_ret == ESP_OK) {
        if (!entry.is_dir) {
            ESP_LOGW(TAG, "path exists but is not a directory: %s", path);
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    esp_err_t err = fridge_tfcard_mkdir(path);
    if (err == ESP_OK) {
        s_status.created_dirs++;
        ESP_LOGI(TAG, "created Agent directory: /sdcard/%s", path);
        return ESP_OK;
    }

    // mkdir 后复查一次，避免同一启动阶段重复初始化时把“已存在”误判为失败。
    stat_ret = fridge_tfcard_stat(path, &entry);
    if (stat_ret == ESP_OK && entry.is_dir) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t ensure_text_file(const char *path, const char *text)
{
    fridge_tfcard_entry_t entry = {0};
    esp_err_t stat_ret = fridge_tfcard_stat(path, &entry);
    if (stat_ret == ESP_OK && !entry.is_dir) {
        return ESP_OK;
    }
    if (stat_ret == ESP_OK && entry.is_dir) {
        ESP_LOGW(TAG, "path exists but is a directory: %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    // 正式文件缺失时优先恢复上一版；普通文档不做 JSON 解析，但仍避免直接覆盖历史数据。
    char backup[FRIDGE_AGENT_STORAGE_MAX_PATH_LEN + 8];
    if (snprintf(backup, sizeof(backup), "%s.bak", path) < (int)sizeof(backup)) {
        fridge_tfcard_entry_t backup_entry = {0};
        if (fridge_tfcard_stat(backup, &backup_entry) == ESP_OK && !backup_entry.is_dir) {
            esp_err_t restore_err = fridge_tfcard_move(backup, path, true);
            if (restore_err == ESP_OK) {
                s_status.recovered_files++;
                return ESP_OK;
            }
        }
    }

    esp_err_t err = fridge_tfcard_text_write_recoverable(path, text, strlen(text), false);
    if (err == ESP_OK) {
        s_status.created_defaults = true;
        s_status.created_files++;
        ESP_LOGI(TAG, "created Agent default file: /sdcard/%s", path);
    }
    return err;
}

static esp_err_t migrate_legacy_default_memory_md(void)
{
    char existing[2048] = {0};
    size_t size = 0;
    esp_err_t err = fridge_tfcard_text_read("agent/MEMORY.md", existing,
                                            sizeof(existing), &size);
    if (err != ESP_OK) return err;

    const size_t legacy_size = strlen(k_legacy_default_memory_md);
    if (size != legacy_size || memcmp(existing, k_legacy_default_memory_md, legacy_size) != 0) {
        return ESP_OK;
    }

    // 仅迁移逐字等于旧内置模板的文件，任何用户编辑过的 MEMORY.md 都原样保留。
    ESP_LOGI(TAG, "migrating untouched legacy MEMORY.md template");
    return fridge_agent_storage_text_write("agent/MEMORY.md", k_default_memory_md,
                                           strlen(k_default_memory_md));
}

static esp_err_t ensure_json_file(const char *path, const char *default_json)
{
    char *buffer = agent_storage_alloc(AGENT_STORAGE_JSON_MAX_BYTES + 1);
    if (!buffer) return ESP_ERR_NO_MEM;

    fridge_tfcard_entry_t before = {0};
    const bool existed = fridge_tfcard_stat(path, &before) == ESP_OK && !before.is_dir;
    const uint32_t recovered_before = s_status.recovered_files;
    size_t size = 0;
    esp_err_t err = fridge_agent_storage_json_read(path,
                                                   buffer,
                                                   AGENT_STORAGE_JSON_MAX_BYTES + 1,
                                                   &size,
                                                   default_json);
    free(buffer);
    if (err == ESP_OK && !existed && s_status.recovered_files == recovered_before) {
        s_status.created_defaults = true;
        s_status.created_files++;
    }
    return err;
}

static esp_err_t remove_deprecated_agent_config_fields(void)
{
    char buffer[2048] = {0};
    size_t size = 0;
    esp_err_t err = fridge_agent_storage_json_read("agent/config/agent.json", buffer,
                                                    sizeof(buffer), &size,
                                                    k_default_agent_json);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_ParseWithLength(buffer, size);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    if (!cJSON_HasObjectItem(root, "session_ttl_hours")) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    // 新 Runtime 只按 24 条 user/assistant 消息裁剪，不再使用时间 TTL。
    cJSON_DeleteItemFromObjectCaseSensitive(root, "session_ttl_hours");
    char *updated = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!updated) return ESP_ERR_NO_MEM;
    err = fridge_agent_storage_json_write("agent/config/agent.json",
                                          updated, strlen(updated));
    cJSON_free(updated);
    return err;
}

esp_err_t fridge_agent_storage_init(void)
{
    if (!s_transaction_lock) {
        s_transaction_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_transaction_lock) return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.ready = false;
    strlcpy(s_status.root, FRIDGE_AGENT_STORAGE_ROOT, sizeof(s_status.root));
    strlcpy(s_status.last_error, "initializing", sizeof(s_status.last_error));

    for (size_t i = 0; i < sizeof(k_agent_dirs) / sizeof(k_agent_dirs[0]); i++) {
        esp_err_t err = ensure_dir(k_agent_dirs[i]);
        if (err != ESP_OK) {
            set_error(k_agent_dirs[i], err);
            return err;
        }
    }

    const struct {
        const char *path;
        const char *text;
    } json_files[] = {
        {"agent/config/agent.json", k_default_agent_json},
        {"agent/config/persona.json", k_default_persona_json},
        {"agent/config/providers.json", k_default_providers_json},
        {"agent/config/mcp_servers.json", k_default_mcp_servers_json},
        {"agent/data/inventory.json", k_default_inventory_json},
        {"agent/data/tasks.json", k_default_tasks_json},
        {"agent/data/memory.json", k_default_memory_json},
        {"agent/data/sessions.json", k_default_sessions_json},
        {"agent/data/stopwatch.json", k_default_items_json},
        {"agent/data/shopping.json", k_default_items_json},
        {"agent/data/recipes.json", k_default_items_json},
        {"agent/data/nutrition.json", k_default_items_json},
        {"agent/config/device_settings.json", k_default_items_json},
        {"agent/config/ai_config.json", k_default_items_json},
    };

    for (size_t i = 0; i < sizeof(json_files) / sizeof(json_files[0]); i++) {
        esp_err_t err = ensure_json_file(json_files[i].path, json_files[i].text);
        if (err != ESP_OK) {
            set_error(json_files[i].path, err);
            return err;
        }
    }
    esp_err_t config_err = remove_deprecated_agent_config_fields();
    if (config_err != ESP_OK) {
        set_error("agent/config/agent.json", config_err);
        return config_err;
    }

    const struct {
        const char *path;
        const char *text;
    } text_files[] = {
        {"agent/MEMORY.md", k_default_memory_md},
        {"agent/docs/README.md", k_docs_readme},
        {"agent/docs/AGENT_ARCH.md", k_docs_arch},
        {"agent/docs/TOOLS.md", k_docs_tools},
        {"agent/docs/SKILLS.md", k_docs_skills},
        {"agent/docs/WORKFLOWS.md", k_docs_workflows},
        {"agent/docs/MCP.md", k_docs_mcp},
    };

    for (size_t i = 0; i < sizeof(text_files) / sizeof(text_files[0]); i++) {
        esp_err_t err = ensure_text_file(text_files[i].path, text_files[i].text);
        if (err != ESP_OK) {
            set_error(text_files[i].path, err);
            return err;
        }
    }
    esp_err_t memory_md_err = migrate_legacy_default_memory_md();
    if (memory_md_err != ESP_OK) {
        set_error("agent/MEMORY.md migration", memory_md_err);
        return memory_md_err;
    }

    s_status.ready = true;
    s_status.last_error[0] = '\0';
    ESP_LOGI(TAG, "Agent TF storage ready: dirs_created=%u, files_created=%u",
             (unsigned)s_status.created_dirs,
             (unsigned)s_status.created_files);
    return ESP_OK;
}

void fridge_agent_storage_get_status(fridge_agent_storage_status_t *out)
{
    if (!out) {
        return;
    }
    *out = s_status;
}
