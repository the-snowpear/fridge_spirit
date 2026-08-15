// P4 库存页的本地数据模型。
//
// 数据只通过 fridge_storage 的库存快照接口持久化；UI Shell 传入副本进行增删改，
// 模型在原子写入成功前不修改调用方快照，避免掉电或 LittleFS 写失败造成页面状态漂移。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_UI_INVENTORY_SCHEMA_VERSION 2
#define FRIDGE_UI_INVENTORY_ZONE_MAX 8
#define FRIDGE_UI_INVENTORY_LAYER_MAX 16
#define FRIDGE_UI_INVENTORY_ITEM_MAX 24
#define FRIDGE_UI_INVENTORY_LAYOUT_DRAFT_MAX 8
#define FRIDGE_UI_INVENTORY_GRID_CELLS 9
#define FRIDGE_UI_INVENTORY_CELL_UNLOCATED UINT8_MAX

// 冰箱区域。legacy_id 保留旧快照的数字 zone 字段，兼容云端同步和已存在的演示数据。
typedef struct {
    char id[24];
    char name[32];
    uint8_t legacy_id;
    bool custom;
} fridge_ui_inventory_zone_t;

// 每个区域可定义多层，每层固定映射到一个 3x3 九宫格。
typedef struct {
    char id[24];
    char zone_id[24];
    char name[32];
    uint8_t order;
} fridge_ui_inventory_layer_t;

// 食材位置由区域、层和 0~8 的九宫格单元共同确定；未定位食材的区域/层为空，
// cell 使用 FRIDGE_UI_INVENTORY_CELL_UNLOCATED。location 为旧页面和语音展示文本。
typedef struct {
    char id[24];
    char name[48];
    char quantity[24];
    char expire_date[24];
    int16_t days_left;
    char zone_id[24];
    char layer_id[24];
    uint8_t cell;
    char location[64];
    char note[80];
} fridge_ui_inventory_item_t;

// 布局草稿保存用户为某层九宫格配置的单元标签；不会替代已确认的库存数据。
typedef struct {
    char id[24];
    char zone_id[24];
    char layer_id[24];
    char name[32];
    char cell_labels[FRIDGE_UI_INVENTORY_GRID_CELLS][24];
} fridge_ui_inventory_layout_draft_t;

typedef struct {
    uint32_t schema_version;
    uint32_t snapshot_version;
    fridge_ui_inventory_zone_t zones[FRIDGE_UI_INVENTORY_ZONE_MAX];
    size_t zone_count;
    fridge_ui_inventory_layer_t layers[FRIDGE_UI_INVENTORY_LAYER_MAX];
    size_t layer_count;
    fridge_ui_inventory_item_t items[FRIDGE_UI_INVENTORY_ITEM_MAX];
    size_t item_count;
    fridge_ui_inventory_layout_draft_t layout_drafts[FRIDGE_UI_INVENTORY_LAYOUT_DRAFT_MAX];
    size_t layout_draft_count;
} fridge_ui_inventory_model_t;

// 从 LittleFS 读取库存快照。旧 schema v1 会在内存中补齐区域 ID、默认层和食材 ID；
// 下次任一 CRUD 成功时一并写为 schema v2。
esp_err_t fridge_ui_inventory_model_load(fridge_ui_inventory_model_t *out);

// 按名称、位置、数量或备注检索。query 为空时返回全部条目；返回的是 items 中的下标。
esp_err_t fridge_ui_inventory_model_search(const fridge_ui_inventory_model_t *model,
                                           const char *query, size_t *indices,
                                           size_t capacity, size_t *out_count);

// 以下增删改接口均先在副本中校验并写入 LittleFS；仅成功后才覆盖 model。
// id 为空表示新增，成功时会回填生成的稳定 ID；id 非空表示更新既有记录。
esp_err_t fridge_ui_inventory_model_upsert_zone(fridge_ui_inventory_model_t *model,
                                                fridge_ui_inventory_zone_t *zone);
esp_err_t fridge_ui_inventory_model_remove_zone(fridge_ui_inventory_model_t *model,
                                                const char *zone_id);
esp_err_t fridge_ui_inventory_model_upsert_layer(fridge_ui_inventory_model_t *model,
                                                 fridge_ui_inventory_layer_t *layer);
esp_err_t fridge_ui_inventory_model_remove_layer(fridge_ui_inventory_model_t *model,
                                                 const char *layer_id);
esp_err_t fridge_ui_inventory_model_upsert_item(fridge_ui_inventory_model_t *model,
                                                fridge_ui_inventory_item_t *item);
esp_err_t fridge_ui_inventory_model_remove_item(fridge_ui_inventory_model_t *model,
                                                const char *item_id);
esp_err_t fridge_ui_inventory_model_upsert_layout_draft(
    fridge_ui_inventory_model_t *model, fridge_ui_inventory_layout_draft_t *draft);
esp_err_t fridge_ui_inventory_model_remove_layout_draft(fridge_ui_inventory_model_t *model,
                                                         const char *draft_id);

#ifdef __cplusplus
}
#endif
