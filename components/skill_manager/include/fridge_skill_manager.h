// 冰箱小精灵 Skill Manager 公共接口。
//
// Skill 用于描述 Agent 在厨房场景下如何选择工具和工作流。当前最小版从 TF 卡
// /sdcard/agent/skills 加载少量默认 Skill 文档，并提供 USB 调试所需的列表、读取和关键词匹配。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_SKILL_MAX_ID_LEN 64
#define FRIDGE_SKILL_MAX_NAME_LEN 64
#define FRIDGE_SKILL_MAX_DESC_LEN 160
#define FRIDGE_SKILL_MAX_PATH_LEN 160
#define FRIDGE_SKILL_MAX_ERROR_LEN 160
#define FRIDGE_SKILL_MAX_DOC_LEN 2048
#define FRIDGE_SKILL_MAX_COUNT 8

typedef struct {
    char skill_id[FRIDGE_SKILL_MAX_ID_LEN];      // Skill 唯一 ID，例如 kitchen.inventory_query。
    char name[FRIDGE_SKILL_MAX_NAME_LEN];        // 中文展示名。
    char description[FRIDGE_SKILL_MAX_DESC_LEN]; // 何时使用该 Skill。
    char manifest_path[FRIDGE_SKILL_MAX_PATH_LEN];
    char doc_path[FRIDGE_SKILL_MAX_PATH_LEN];
} fridge_skill_summary_t;

typedef struct {
    bool ready;                         // Skill Manager 是否初始化完成。
    uint32_t seeded_files;              // 本次启动补齐的默认 Skill 文件数量。
    uint32_t skill_count;               // 当前内置 catalog 数量。
    char last_error[FRIDGE_SKILL_MAX_ERROR_LEN + 1];
} fridge_skill_manager_status_t;

esp_err_t fridge_skill_manager_init(void);
void fridge_skill_manager_get_status(fridge_skill_manager_status_t *out);
esp_err_t fridge_skill_manager_list(fridge_skill_summary_t *out, size_t capacity, size_t *out_count);
esp_err_t fridge_skill_manager_read(const char *skill_id, char *out_doc, size_t out_size, size_t *out_len);
esp_err_t fridge_skill_manager_match(const char *input, fridge_skill_summary_t *out);

#ifdef __cplusplus
}
#endif
