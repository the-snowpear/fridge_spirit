// 冰箱小精灵 Skill Manager 最小实现。
//
// 本组件只维护 TF 卡上的 Skill 文档种子与轻量 catalog，不调用模型、不联网、不控制硬件。
// 后续接云端模型时，可把 manifest 与 SKILL.md 裁剪后注入 Context Composer。

#include "fridge_skill_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "fridge_tfcard.h"

static const char *TAG = "fridge_skill_manager";

typedef struct {
    fridge_skill_summary_t summary;
    const char *keywords[6];
    const char *manifest;
    const char *doc;
} skill_seed_t;

static const skill_seed_t k_skills[] = {
    {
        .summary = {
            .skill_id = "kitchen.inventory_query",
            .name = "库存查询",
            .description = "读取本地库存并回答还有什么食材。",
            .manifest_path = "agent/skills/kitchen/inventory_query/manifest.json",
            .doc_path = "agent/skills/kitchen/inventory_query/SKILL.md",
        },
        .keywords = {"库存", "食材", "还有", "临期", NULL, NULL},
        .manifest = "{\n  \"skill_id\": \"kitchen.inventory_query\",\n  \"name\": \"库存查询\",\n  \"version\": \"1.0.0\",\n  \"description\": \"读取本地库存并回答还有什么食材。\",\n  \"triggers\": {\"keywords\": [\"库存\", \"食材\", \"还有\", \"临期\"]},\n  \"required_tools\": [\"inventory.list\"],\n  \"execution_mode\": \"agent_guided\",\n  \"max_steps\": 3\n}\n",
        .doc = "# 库存查询\n\n## Goal\n读取 TF 卡库存数据，回答用户还有哪些食材。\n\n## Use When\n用户询问库存、食材、临期或冰箱里还有什么。\n\n## Procedure\n1. 调用 `inventory.list` 或 `inventory.list_expiring`。\n2. 只基于工具返回的数据回答。\n3. 不猜测不存在的库存。\n\n## Completion Criteria\n回复中明确说明数据来自本地库存。\n",
    },
    {
        .summary = {
            .skill_id = "kitchen.expiring_meal",
            .name = "临期食材晚饭推荐",
            .description = "优先使用快过期食材推荐一顿饭。",
            .manifest_path = "agent/skills/kitchen/expiring_meal/manifest.json",
            .doc_path = "agent/skills/kitchen/expiring_meal/SKILL.md",
        },
        .keywords = {"快过期", "临期", "今晚吃什么", "优先用掉", NULL, NULL},
        .manifest = "{\n  \"skill_id\": \"kitchen.expiring_meal\",\n  \"name\": \"临期食材晚饭推荐\",\n  \"version\": \"1.0.0\",\n  \"description\": \"根据临期库存和家庭偏好推荐晚饭。\",\n  \"triggers\": {\"keywords\": [\"快过期\", \"临期\", \"今晚吃什么\", \"优先用掉\"]},\n  \"required_tools\": [\"inventory.list_expiring\", \"memory.search\"],\n  \"execution_mode\": \"agent_guided\",\n  \"max_steps\": 6\n}\n",
        .doc = "# 临期食材晚饭推荐\n\n## Goal\n用快过期食材生成可执行的晚饭建议。\n\n## Procedure\n1. 调用 `inventory.list_expiring` 获取临期食材。\n2. 调用 `memory.search` 检查家庭口味偏好。\n3. 给出 1-2 个方案，并标明优先消耗哪些食材。\n\n## Failure Handling\n如果没有临期食材，说明库存未发现临期项，再建议普通菜谱。\n",
    },
    {
        .summary = {
            .skill_id = "kitchen.cooking_timer",
            .name = "厨房计时器",
            .description = "把用户的计时需求转成本地任务记录。",
            .manifest_path = "agent/skills/kitchen/cooking_timer/manifest.json",
            .doc_path = "agent/skills/kitchen/cooking_timer/SKILL.md",
        },
        .keywords = {"计时", "定时", "提醒我", "分钟", NULL, NULL},
        .manifest = "{\n  \"skill_id\": \"kitchen.cooking_timer\",\n  \"name\": \"厨房计时器\",\n  \"version\": \"1.0.0\",\n  \"description\": \"创建本地厨房计时任务。\",\n  \"triggers\": {\"keywords\": [\"计时\", \"定时\", \"提醒我\", \"分钟\"]},\n  \"required_tools\": [\"task.create_timer\"],\n  \"execution_mode\": \"workflow_backed\",\n  \"workflow_id\": \"task.create_timer\",\n  \"max_steps\": 4\n}\n",
        .doc = "# 厨房计时器\n\n## Goal\n把明确的厨房计时需求写成本地任务。\n\n## Required Information\n需要标题和时长。信息不足时先询问，不要猜时间。\n\n## Procedure\n调用工作流 `task.create_timer`，最终通过 `task.create_timer` 工具写入 TF 卡。\n",
    },
    {
        .summary = {
            .skill_id = "memory.preference_confirm",
            .name = "偏好记忆确认",
            .description = "确认后保存家庭饮食偏好。",
            .manifest_path = "agent/skills/user/preference_confirm/manifest.json",
            .doc_path = "agent/skills/user/preference_confirm/SKILL.md",
        },
        .keywords = {"记住", "偏好", "不吃", "喜欢", NULL, NULL},
        .manifest = "{\n  \"skill_id\": \"memory.preference_confirm\",\n  \"name\": \"偏好记忆确认\",\n  \"version\": \"1.0.0\",\n  \"description\": \"经用户确认后写入家庭偏好记忆。\",\n  \"triggers\": {\"keywords\": [\"记住\", \"偏好\", \"不吃\", \"喜欢\"]},\n  \"required_tools\": [\"memory.add\", \"memory_md.append\"],\n  \"execution_mode\": \"workflow_backed\",\n  \"workflow_id\": \"memory.preference_confirm\",\n  \"max_steps\": 5\n}\n",
        .doc = "# 偏好记忆确认\n\n## Goal\n把稳定的家庭偏好保存到结构化记忆或 MEMORY.md。\n\n## Conflict Handling\n如果新偏好与旧偏好冲突，应向用户说明冲突并等待确认。\n\n## Procedure\n确认后通过 `memory.add` 或 `memory_md.append` 写入，不能由模型直接改文件。\n",
    },
    {
        .summary = {
            .skill_id = "system.device_help",
            .name = "设备帮助",
            .description = "解释当前设备状态、可用命令和安全限制。",
            .manifest_path = "agent/skills/system/device_help/manifest.json",
            .doc_path = "agent/skills/system/device_help/SKILL.md",
        },
        .keywords = {"帮助", "状态", "设备", "命令", NULL, NULL},
        .manifest = "{\n  \"skill_id\": \"system.device_help\",\n  \"name\": \"设备帮助\",\n  \"version\": \"1.0.0\",\n  \"description\": \"读取设备状态并说明当前可用能力。\",\n  \"triggers\": {\"keywords\": [\"帮助\", \"状态\", \"设备\", \"命令\"]},\n  \"required_tools\": [\"device.state.get\"],\n  \"execution_mode\": \"agent_guided\",\n  \"max_steps\": 3\n}\n",
        .doc = "# 设备帮助\n\n## Goal\n说明当前本地 Agent、Tool、Workflow 和硬件状态。\n\n## Procedure\n调用 `device.state.get`，结合静态安全规则回答。不要建议用户改 GPIO、时钟或电源。\n",
    },
};

static fridge_skill_manager_status_t s_status = {
    .ready = false,
    .seeded_files = 0,
    .skill_count = 0,
    .last_error = "not initialized",
};

static void set_error(const char *prefix, esp_err_t err)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s", prefix, esp_err_to_name(err));
    s_status.ready = false;
}

static esp_err_t ensure_dir_for_skill(const char *path)
{
    char dir[FRIDGE_SKILL_MAX_PATH_LEN] = {0};
    strlcpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash) return ESP_OK;
    *slash = '\0';

    char partial[FRIDGE_SKILL_MAX_PATH_LEN] = {0};
    const char *cursor = dir;
    while (*cursor) {
        const char *next = strchr(cursor, '/');
        if (next) {
            strncat(partial, cursor, (size_t)(next - cursor));
        } else {
            strlcat(partial, cursor, sizeof(partial));
        }
        fridge_tfcard_entry_t entry = {0};
        esp_err_t stat_ret = fridge_tfcard_stat(partial, &entry);
        if (stat_ret == ESP_ERR_NOT_FOUND) {
            esp_err_t mk_ret = fridge_tfcard_mkdir(partial);
            if (mk_ret != ESP_OK) return mk_ret;
        } else if (stat_ret != ESP_OK) {
            return stat_ret;
        } else if (!entry.is_dir) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!next) break;
        strlcat(partial, "/", sizeof(partial));
        cursor = next + 1;
    }
    return ESP_OK;
}

static esp_err_t ensure_text_file(const char *path, const char *text)
{
    fridge_tfcard_entry_t entry = {0};
    esp_err_t stat_ret = fridge_tfcard_stat(path, &entry);
    if (stat_ret == ESP_OK && !entry.is_dir) {
        return ESP_OK;
    }
    if (stat_ret == ESP_OK && entry.is_dir) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ensure_dir_for_skill(path), TAG, "创建 Skill 目录失败");
    esp_err_t err = fridge_tfcard_text_write(path, text, strlen(text), false);
    if (err == ESP_OK) {
        s_status.seeded_files++;
        ESP_LOGI(TAG, "seeded Skill file: /sdcard/%s", path);
    }
    return err;
}

esp_err_t fridge_skill_manager_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.skill_count = sizeof(k_skills) / sizeof(k_skills[0]);
    strlcpy(s_status.last_error, "initializing", sizeof(s_status.last_error));

    for (size_t i = 0; i < sizeof(k_skills) / sizeof(k_skills[0]); i++) {
        esp_err_t err = ensure_text_file(k_skills[i].summary.manifest_path, k_skills[i].manifest);
        if (err != ESP_OK) {
            set_error(k_skills[i].summary.manifest_path, err);
            return err;
        }
        err = ensure_text_file(k_skills[i].summary.doc_path, k_skills[i].doc);
        if (err != ESP_OK) {
            set_error(k_skills[i].summary.doc_path, err);
            return err;
        }
    }

    s_status.ready = true;
    s_status.last_error[0] = '\0';
    ESP_LOGI(TAG, "Skill Manager ready, catalog=%u, seeded_files=%u", (unsigned)s_status.skill_count, (unsigned)s_status.seeded_files);
    return ESP_OK;
}

void fridge_skill_manager_get_status(fridge_skill_manager_status_t *out)
{
    if (!out) return;
    *out = s_status;
}

esp_err_t fridge_skill_manager_list(fridge_skill_summary_t *out, size_t capacity, size_t *out_count)
{
    if (!out_count) return ESP_ERR_INVALID_ARG;
    const size_t total = sizeof(k_skills) / sizeof(k_skills[0]);
    const size_t n = total < capacity ? total : capacity;
    if (out && capacity > 0) {
        for (size_t i = 0; i < n; i++) {
            out[i] = k_skills[i].summary;
        }
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t fridge_skill_manager_read(const char *skill_id, char *out_doc, size_t out_size, size_t *out_len)
{
    if (!skill_id || !out_doc || out_size == 0 || !out_len) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(k_skills) / sizeof(k_skills[0]); i++) {
        if (strcmp(k_skills[i].summary.skill_id, skill_id) == 0) {
            return fridge_tfcard_text_read(k_skills[i].summary.doc_path, out_doc, out_size, out_len);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t fridge_skill_manager_match(const char *input, fridge_skill_summary_t *out)
{
    if (!input || !out) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(k_skills) / sizeof(k_skills[0]); i++) {
        for (size_t k = 0; k < sizeof(k_skills[i].keywords) / sizeof(k_skills[i].keywords[0]); k++) {
            const char *keyword = k_skills[i].keywords[k];
            if (keyword && strstr(input, keyword)) {
                *out = k_skills[i].summary;
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}
