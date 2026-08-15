// P4 库存页的本地数据模型实现。
//
// 本文件不操作 LVGL、GPIO 或 TF 卡。所有库存确认动作统一提交给 storage 组件，
// 由其负责 cache LittleFS 的原子替换、同步版本号和断电恢复边界。

#include "fridge_ui_inventory_model.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "fridge_storage.h"

static const char *TAG = "ui_inv_model";

static bool text_fits(const char *text, size_t capacity, bool required)
{
    return text && strnlen(text, capacity) < capacity && (!required || text[0] != '\0');
}

static bool copy_optional_string(const cJSON *object, const char *key, char *out, size_t out_size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value) {
        out[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= out_size) {
        return false;
    }
    strlcpy(out, value->valuestring, out_size);
    return true;
}

static bool copy_required_string(const cJSON *object, const char *key, char *out, size_t out_size)
{
    return copy_optional_string(object, key, out, out_size) && out[0] != '\0';
}

static int find_zone(const fridge_ui_inventory_model_t *model, const char *zone_id)
{
    if (!model || !zone_id || !zone_id[0]) return -1;
    for (size_t i = 0; i < model->zone_count; i++) {
        if (strcmp(model->zones[i].id, zone_id) == 0) return (int)i;
    }
    return -1;
}

static int find_layer(const fridge_ui_inventory_model_t *model, const char *layer_id)
{
    if (!model || !layer_id || !layer_id[0]) return -1;
    for (size_t i = 0; i < model->layer_count; i++) {
        if (strcmp(model->layers[i].id, layer_id) == 0) return (int)i;
    }
    return -1;
}

static int find_item(const fridge_ui_inventory_model_t *model, const char *item_id)
{
    if (!model || !item_id || !item_id[0]) return -1;
    for (size_t i = 0; i < model->item_count; i++) {
        if (strcmp(model->items[i].id, item_id) == 0) return (int)i;
    }
    return -1;
}

static int find_draft(const fridge_ui_inventory_model_t *model, const char *draft_id)
{
    if (!model || !draft_id || !draft_id[0]) return -1;
    for (size_t i = 0; i < model->layout_draft_count; i++) {
        if (strcmp(model->layout_drafts[i].id, draft_id) == 0) return (int)i;
    }
    return -1;
}

static void create_unique_id(const fridge_ui_inventory_model_t *model, const char *prefix,
                             char *out, size_t out_size)
{
    for (unsigned int candidate = 1; candidate < 100000; candidate++) {
        snprintf(out, out_size, "%s_%u", prefix, candidate);
        if (find_zone(model, out) < 0 && find_layer(model, out) < 0 &&
            find_item(model, out) < 0 && find_draft(model, out) < 0) {
            return;
        }
    }
    out[0] = '\0';
}

static uint8_t allocate_legacy_zone_id(const fridge_ui_inventory_model_t *model)
{
    for (unsigned int candidate = 0; candidate < UINT8_MAX; candidate++) {
        bool used = false;
        for (size_t i = 0; i < model->zone_count; i++) {
            if (model->zones[i].legacy_id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) return (uint8_t)candidate;
    }
    return UINT8_MAX;
}

static const fridge_ui_inventory_layer_t *first_layer_for_zone(
    const fridge_ui_inventory_model_t *model, const char *zone_id)
{
    for (size_t i = 0; i < model->layer_count; i++) {
        if (strcmp(model->layers[i].zone_id, zone_id) == 0) return &model->layers[i];
    }
    return NULL;
}

static esp_err_t add_default_layer(fridge_ui_inventory_model_t *model, const char *zone_id,
                                   const char *zone_name)
{
    if (model->layer_count >= FRIDGE_UI_INVENTORY_LAYER_MAX) return ESP_ERR_NO_MEM;
    fridge_ui_inventory_layer_t *layer = &model->layers[model->layer_count++];
    memset(layer, 0, sizeof(*layer));
    create_unique_id(model, "layer", layer->id, sizeof(layer->id));
    if (!layer->id[0]) return ESP_ERR_NO_MEM;
    strlcpy(layer->zone_id, zone_id, sizeof(layer->zone_id));
    snprintf(layer->name, sizeof(layer->name), "%s层", zone_name && zone_name[0] ? zone_name : "默认");
    layer->order = 0;
    return ESP_OK;
}

static esp_err_t parse_zones(const cJSON *root, fridge_ui_inventory_model_t *model)
{
    const cJSON *zones = cJSON_GetObjectItemCaseSensitive(root, "zones");
    if (!cJSON_IsArray(zones) || cJSON_GetArraySize(zones) > FRIDGE_UI_INVENTORY_ZONE_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *node = NULL;
    cJSON_ArrayForEach(node, zones) {
        if (!cJSON_IsObject(node) || model->zone_count >= FRIDGE_UI_INVENTORY_ZONE_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        fridge_ui_inventory_zone_t *zone = &model->zones[model->zone_count];
        memset(zone, 0, sizeof(*zone));
        if (!copy_required_string(node, "name", zone->name, sizeof(zone->name))) return ESP_ERR_INVALID_RESPONSE;
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(node, "key");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(node, "id");
        if (cJSON_IsString(key) && key->valuestring && strlen(key->valuestring) < sizeof(zone->id)) {
            strlcpy(zone->id, key->valuestring, sizeof(zone->id));
        } else if (cJSON_IsString(id) && id->valuestring && strlen(id->valuestring) < sizeof(zone->id)) {
            strlcpy(zone->id, id->valuestring, sizeof(zone->id));
        } else if (cJSON_IsNumber(id) && id->valueint >= 0 && id->valueint < UINT8_MAX) {
            zone->legacy_id = (uint8_t)id->valueint;
            snprintf(zone->id, sizeof(zone->id), "zone_%u", (unsigned int)zone->legacy_id);
        } else {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!zone->id[0] || find_zone(model, zone->id) >= 0) return ESP_ERR_INVALID_RESPONSE;
        if (cJSON_IsNumber(id) && id->valueint >= 0 && id->valueint < UINT8_MAX) {
            zone->legacy_id = (uint8_t)id->valueint;
        } else {
            zone->legacy_id = allocate_legacy_zone_id(model);
        }
        const cJSON *custom = cJSON_GetObjectItemCaseSensitive(node, "custom");
        zone->custom = cJSON_IsTrue(custom);
        model->zone_count++;
    }
    return model->zone_count > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t parse_layers(const cJSON *root, fridge_ui_inventory_model_t *model)
{
    const cJSON *layers = cJSON_GetObjectItemCaseSensitive(root, "layers");
    if (layers && !cJSON_IsArray(layers)) return ESP_ERR_INVALID_RESPONSE;
    if (layers && cJSON_GetArraySize(layers) > FRIDGE_UI_INVENTORY_LAYER_MAX) return ESP_ERR_INVALID_SIZE;
    cJSON *node = NULL;
    if (layers) cJSON_ArrayForEach(node, layers) {
        if (!cJSON_IsObject(node) || model->layer_count >= FRIDGE_UI_INVENTORY_LAYER_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        fridge_ui_inventory_layer_t *layer = &model->layers[model->layer_count];
        memset(layer, 0, sizeof(*layer));
        if (!copy_required_string(node, "id", layer->id, sizeof(layer->id)) ||
            !copy_required_string(node, "zone_id", layer->zone_id, sizeof(layer->zone_id)) ||
            !copy_required_string(node, "name", layer->name, sizeof(layer->name)) ||
            find_zone(model, layer->zone_id) < 0 || find_layer(model, layer->id) >= 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const cJSON *order = cJSON_GetObjectItemCaseSensitive(node, "order");
        if (cJSON_IsNumber(order) && order->valueint >= 0 && order->valueint <= UINT8_MAX) {
            layer->order = (uint8_t)order->valueint;
        }
        model->layer_count++;
    }
    // schema v1 只有区域与九宫格，迁移时为每个区域补一层，不丢弃原有位置。
    for (size_t i = 0; i < model->zone_count; i++) {
        if (!first_layer_for_zone(model, model->zones[i].id)) {
            ESP_RETURN_ON_ERROR(add_default_layer(model, model->zones[i].id, model->zones[i].name), TAG,
                                "add migration layer failed");
        }
    }
    return ESP_OK;
}

static esp_err_t parse_items(const cJSON *root, fridge_ui_inventory_model_t *model)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > FRIDGE_UI_INVENTORY_ITEM_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t ordinal = 1;
    cJSON *node = NULL;
    cJSON_ArrayForEach(node, items) {
        if (!cJSON_IsObject(node) || model->item_count >= FRIDGE_UI_INVENTORY_ITEM_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        fridge_ui_inventory_item_t *item = &model->items[model->item_count];
        memset(item, 0, sizeof(*item));
        if (!copy_required_string(node, "name", item->name, sizeof(item->name)) ||
            !copy_optional_string(node, "quantity", item->quantity, sizeof(item->quantity)) ||
            !copy_optional_string(node, "expire_date", item->expire_date, sizeof(item->expire_date)) ||
            !copy_optional_string(node, "location", item->location, sizeof(item->location)) ||
            !copy_optional_string(node, "note", item->note, sizeof(item->note))) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!copy_optional_string(node, "id", item->id, sizeof(item->id)) || !item->id[0]) {
            snprintf(item->id, sizeof(item->id), "item_%u", (unsigned int)ordinal);
        }
        if (find_item(model, item->id) >= 0) return ESP_ERR_INVALID_RESPONSE;
        bool located = copy_optional_string(node, "zone_id", item->zone_id, sizeof(item->zone_id)) &&
                       item->zone_id[0];
        if (!located) {
            const cJSON *legacy_zone = cJSON_GetObjectItemCaseSensitive(node, "zone");
            if (cJSON_IsNumber(legacy_zone) && legacy_zone->valueint >= 0 &&
                legacy_zone->valueint < UINT8_MAX) {
                snprintf(item->zone_id, sizeof(item->zone_id), "zone_%u",
                         (unsigned int)legacy_zone->valueint);
                located = true;
            }
        }
        int zone_index = located ? find_zone(model, item->zone_id) : -1;
        if (located && zone_index < 0) return ESP_ERR_INVALID_RESPONSE;
        if (located &&
            (!copy_optional_string(node, "layer_id", item->layer_id, sizeof(item->layer_id)) ||
             !item->layer_id[0])) {
            const fridge_ui_inventory_layer_t *layer = first_layer_for_zone(model, item->zone_id);
            if (!layer) return ESP_ERR_INVALID_STATE;
            strlcpy(item->layer_id, layer->id, sizeof(item->layer_id));
        }
        int layer_index = located ? find_layer(model, item->layer_id) : -1;
        const cJSON *cell = cJSON_GetObjectItemCaseSensitive(node, "cell");
        const cJSON *days = cJSON_GetObjectItemCaseSensitive(node, "days_left");
        if ((located &&
             (layer_index < 0 ||
              strcmp(model->layers[layer_index].zone_id, item->zone_id) != 0 ||
              !cJSON_IsNumber(cell) || cell->valueint < 0 ||
              cell->valueint >= FRIDGE_UI_INVENTORY_GRID_CELLS)) ||
            (days && (!cJSON_IsNumber(days) || days->valueint < INT16_MIN || days->valueint > INT16_MAX))) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        item->cell = located ? (uint8_t)cell->valueint : FRIDGE_UI_INVENTORY_CELL_UNLOCATED;
        item->days_left = cJSON_IsNumber(days) ? (int16_t)days->valueint : -1;
        model->item_count++;
        ordinal++;
    }
    return ESP_OK;
}

static esp_err_t parse_layout_drafts(const cJSON *root, fridge_ui_inventory_model_t *model)
{
    const cJSON *drafts = cJSON_GetObjectItemCaseSensitive(root, "layout_drafts");
    if (!drafts) return ESP_OK;
    if (!cJSON_IsArray(drafts) || cJSON_GetArraySize(drafts) > FRIDGE_UI_INVENTORY_LAYOUT_DRAFT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *node = NULL;
    if (drafts) cJSON_ArrayForEach(node, drafts) {
        if (!cJSON_IsObject(node) || model->layout_draft_count >= FRIDGE_UI_INVENTORY_LAYOUT_DRAFT_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        fridge_ui_inventory_layout_draft_t *draft = &model->layout_drafts[model->layout_draft_count];
        memset(draft, 0, sizeof(*draft));
        if (!copy_required_string(node, "id", draft->id, sizeof(draft->id)) ||
            !copy_required_string(node, "zone_id", draft->zone_id, sizeof(draft->zone_id)) ||
            !copy_required_string(node, "layer_id", draft->layer_id, sizeof(draft->layer_id)) ||
            !copy_required_string(node, "name", draft->name, sizeof(draft->name)) ||
            find_draft(model, draft->id) >= 0 || find_zone(model, draft->zone_id) < 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        int layer = find_layer(model, draft->layer_id);
        if (layer < 0 || strcmp(model->layers[layer].zone_id, draft->zone_id) != 0) return ESP_ERR_INVALID_RESPONSE;
        const cJSON *labels = cJSON_GetObjectItemCaseSensitive(node, "cell_labels");
        if (!cJSON_IsArray(labels) || cJSON_GetArraySize(labels) != FRIDGE_UI_INVENTORY_GRID_CELLS) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (size_t i = 0; i < FRIDGE_UI_INVENTORY_GRID_CELLS; i++) {
            const cJSON *label = cJSON_GetArrayItem(labels, i);
            if (!cJSON_IsString(label) || !label->valuestring || strlen(label->valuestring) >= sizeof(draft->cell_labels[i])) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            strlcpy(draft->cell_labels[i], label->valuestring, sizeof(draft->cell_labels[i]));
        }
        model->layout_draft_count++;
    }
    return ESP_OK;
}

static esp_err_t model_validate(const fridge_ui_inventory_model_t *model)
{
    if (!model || model->zone_count == 0 || model->zone_count > FRIDGE_UI_INVENTORY_ZONE_MAX ||
        model->layer_count > FRIDGE_UI_INVENTORY_LAYER_MAX || model->item_count > FRIDGE_UI_INVENTORY_ITEM_MAX ||
        model->layout_draft_count > FRIDGE_UI_INVENTORY_LAYOUT_DRAFT_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < model->zone_count; i++) {
        if (!text_fits(model->zones[i].id, sizeof(model->zones[i].id), true) ||
            !text_fits(model->zones[i].name, sizeof(model->zones[i].name), true)) return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < model->layer_count; i++) {
        if (!text_fits(model->layers[i].id, sizeof(model->layers[i].id), true) ||
            !text_fits(model->layers[i].zone_id, sizeof(model->layers[i].zone_id), true) ||
            !text_fits(model->layers[i].name, sizeof(model->layers[i].name), true) ||
            find_zone(model, model->layers[i].zone_id) < 0) return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < model->item_count; i++) {
        const fridge_ui_inventory_item_t *item = &model->items[i];
        const bool located = item->zone_id[0] || item->layer_id[0];
        int layer = located ? find_layer(model, item->layer_id) : -1;
        if (!text_fits(item->id, sizeof(item->id), true) || !text_fits(item->name, sizeof(item->name), true) ||
            !text_fits(item->quantity, sizeof(item->quantity), false) ||
            !text_fits(item->expire_date, sizeof(item->expire_date), false) ||
            !text_fits(item->zone_id, sizeof(item->zone_id), located) ||
            !text_fits(item->layer_id, sizeof(item->layer_id), located) ||
            (located &&
             (item->cell >= FRIDGE_UI_INVENTORY_GRID_CELLS || layer < 0 ||
              strcmp(model->layers[layer].zone_id, item->zone_id) != 0)) ||
            (!located && item->cell != FRIDGE_UI_INVENTORY_CELL_UNLOCATED)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (size_t i = 0; i < model->layout_draft_count; i++) {
        const fridge_ui_inventory_layout_draft_t *draft = &model->layout_drafts[i];
        int layer = find_layer(model, draft->layer_id);
        if (!text_fits(draft->id, sizeof(draft->id), true) ||
            !text_fits(draft->zone_id, sizeof(draft->zone_id), true) ||
            !text_fits(draft->layer_id, sizeof(draft->layer_id), true) ||
            !text_fits(draft->name, sizeof(draft->name), true) || find_zone(model, draft->zone_id) < 0 ||
            layer < 0 || strcmp(model->layers[layer].zone_id, draft->zone_id) != 0) return ESP_ERR_INVALID_ARG;
        for (size_t cell = 0; cell < FRIDGE_UI_INVENTORY_GRID_CELLS; cell++) {
            if (!text_fits(draft->cell_labels[cell], sizeof(draft->cell_labels[cell]), false)) return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t model_persist(const fridge_ui_inventory_model_t *model)
{
    ESP_RETURN_ON_ERROR(model_validate(model), TAG, "invalid inventory model");
    cJSON *root = cJSON_CreateObject();
    cJSON *zones = cJSON_AddArrayToObject(root, "zones");
    cJSON *layers = cJSON_AddArrayToObject(root, "layers");
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    cJSON *drafts = cJSON_AddArrayToObject(root, "layout_drafts");
    if (!root || !zones || !layers || !items || !drafts) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", FRIDGE_UI_INVENTORY_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "source", "ui_p4_local");
    for (size_t i = 0; i < model->zone_count; i++) {
        const fridge_ui_inventory_zone_t *zone = &model->zones[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
        cJSON_AddNumberToObject(entry, "id", zone->legacy_id);
        cJSON_AddStringToObject(entry, "key", zone->id);
        cJSON_AddStringToObject(entry, "name", zone->name);
        cJSON_AddBoolToObject(entry, "custom", zone->custom);
        cJSON_AddItemToArray(zones, entry);
    }
    for (size_t i = 0; i < model->layer_count; i++) {
        const fridge_ui_inventory_layer_t *layer = &model->layers[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
        cJSON_AddStringToObject(entry, "id", layer->id);
        cJSON_AddStringToObject(entry, "zone_id", layer->zone_id);
        cJSON_AddStringToObject(entry, "name", layer->name);
        cJSON_AddNumberToObject(entry, "order", layer->order);
        cJSON_AddItemToArray(layers, entry);
    }
    for (size_t i = 0; i < model->item_count; i++) {
        const fridge_ui_inventory_item_t *item = &model->items[i];
        const bool located = item->zone_id[0] && item->layer_id[0] &&
                             item->cell < FRIDGE_UI_INVENTORY_GRID_CELLS;
        int zone = located ? find_zone(model, item->zone_id) : -1;
        cJSON *entry = cJSON_CreateObject();
        if (!entry || (located && zone < 0)) {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_STATE;
        }
        cJSON_AddStringToObject(entry, "id", item->id);
        cJSON_AddStringToObject(entry, "name", item->name);
        cJSON_AddStringToObject(entry, "quantity", item->quantity);
        cJSON_AddStringToObject(entry, "expire_date", item->expire_date);
        cJSON_AddNumberToObject(entry, "days_left", item->days_left);
        cJSON_AddStringToObject(entry, "location", item->location);
        if (located) {
            cJSON_AddNumberToObject(entry, "zone", model->zones[zone].legacy_id);
            cJSON_AddStringToObject(entry, "zone_id", item->zone_id);
            cJSON_AddStringToObject(entry, "layer_id", item->layer_id);
            cJSON_AddNumberToObject(entry, "cell", item->cell);
        } else {
            cJSON_AddStringToObject(entry, "zone_id", "");
            cJSON_AddStringToObject(entry, "layer_id", "");
        }
        cJSON_AddStringToObject(entry, "note", item->note);
        cJSON_AddItemToArray(items, entry);
    }
    for (size_t i = 0; i < model->layout_draft_count; i++) {
        const fridge_ui_inventory_layout_draft_t *draft = &model->layout_drafts[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON *labels = entry ? cJSON_AddArrayToObject(entry, "cell_labels") : NULL;
        if (!entry || !labels) { cJSON_Delete(entry); cJSON_Delete(root); return ESP_ERR_NO_MEM; }
        cJSON_AddStringToObject(entry, "id", draft->id);
        cJSON_AddStringToObject(entry, "zone_id", draft->zone_id);
        cJSON_AddStringToObject(entry, "layer_id", draft->layer_id);
        cJSON_AddStringToObject(entry, "name", draft->name);
        for (size_t cell = 0; cell < FRIDGE_UI_INVENTORY_GRID_CELLS; cell++) {
            cJSON_AddItemToArray(labels, cJSON_CreateString(draft->cell_labels[cell]));
        }
        cJSON_AddItemToArray(drafts, entry);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = fridge_storage_set_ui_inventory_snapshot(json);
    cJSON_free(json);
    return err;
}

static esp_err_t commit_model(fridge_ui_inventory_model_t *model, const fridge_ui_inventory_model_t *next)
{
    esp_err_t err = model_persist(next);
    if (err != ESP_OK) return err;
    *model = *next;
    model->schema_version = FRIDGE_UI_INVENTORY_SCHEMA_VERSION;
    (void)fridge_storage_get_inventory_snapshot_version(&model->snapshot_version);
    return ESP_OK;
}

esp_err_t fridge_ui_inventory_model_load(fridge_ui_inventory_model_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "inventory output is NULL");
    char json[FRIDGE_STORAGE_MAX_JSON_LEN] = {0};
    ESP_RETURN_ON_ERROR(fridge_storage_get_ui_inventory_snapshot(json, sizeof(json)), TAG,
                        "load inventory snapshot failed");
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    fridge_ui_inventory_model_t parsed = {0};
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    parsed.schema_version = cJSON_IsNumber(schema) ? (uint32_t)schema->valueint : 1;
    const cJSON *snapshot = cJSON_GetObjectItemCaseSensitive(root, "snapshot_version");
    parsed.snapshot_version = cJSON_IsNumber(snapshot) ? (uint32_t)snapshot->valueint : 0;
    esp_err_t err = parse_zones(root, &parsed);
    if (err == ESP_OK) err = parse_layers(root, &parsed);
    if (err == ESP_OK) err = parse_items(root, &parsed);
    if (err == ESP_OK) err = parse_layout_drafts(root, &parsed);
    cJSON_Delete(root);
    if (err != ESP_OK) return err;
    *out = parsed;
    return ESP_OK;
}

static bool ascii_contains_case_insensitive(const char *text, const char *query)
{
    if (!query || !query[0]) return true;
    if (!text) return false;
    size_t query_len = strlen(query);
    for (const char *start = text; *start; start++) {
        size_t i = 0;
        while (i < query_len) {
            unsigned char a = (unsigned char)start[i];
            unsigned char b = (unsigned char)query[i];
            if (!a) break;
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) break;
            i++;
        }
        if (i == query_len) return true;
    }
    return false;
}

esp_err_t fridge_ui_inventory_model_search(const fridge_ui_inventory_model_t *model,
                                           const char *query, size_t *indices,
                                           size_t capacity, size_t *out_count)
{
    ESP_RETURN_ON_FALSE(model && out_count && (capacity == 0 || indices), ESP_ERR_INVALID_ARG, TAG,
                        "invalid inventory search args");
    *out_count = 0;
    for (size_t i = 0; i < model->item_count; i++) {
        const fridge_ui_inventory_item_t *item = &model->items[i];
        if (!ascii_contains_case_insensitive(item->name, query) &&
            !ascii_contains_case_insensitive(item->location, query) &&
            !ascii_contains_case_insensitive(item->quantity, query) &&
            !ascii_contains_case_insensitive(item->note, query)) continue;
        if (*out_count >= capacity) return ESP_ERR_NO_MEM;
        indices[(*out_count)++] = i;
    }
    return ESP_OK;
}

esp_err_t fridge_ui_inventory_model_upsert_zone(fridge_ui_inventory_model_t *model,
                                                fridge_ui_inventory_zone_t *zone)
{
    ESP_RETURN_ON_FALSE(model && zone && text_fits(zone->name, sizeof(zone->name), true), ESP_ERR_INVALID_ARG, TAG,
                        "invalid zone");
    fridge_ui_inventory_model_t next = *model;
    int index = find_zone(&next, zone->id);
    if (zone->id[0] && index < 0) return ESP_ERR_NOT_FOUND;
    if (index < 0) {
        if (next.zone_count >= FRIDGE_UI_INVENTORY_ZONE_MAX) return ESP_ERR_NO_MEM;
        index = (int)next.zone_count;
        if (!zone->id[0]) create_unique_id(&next, "zone", zone->id, sizeof(zone->id));
        if (!zone->id[0]) return ESP_ERR_NO_MEM;
        zone->legacy_id = allocate_legacy_zone_id(&next);
        next.zone_count++;
    }
    if (!text_fits(zone->id, sizeof(zone->id), true)) return ESP_ERR_INVALID_ARG;
    next.zones[index] = *zone;
    esp_err_t err = commit_model(model, &next);
    if (err == ESP_OK) *zone = model->zones[index];
    return err;
}

esp_err_t fridge_ui_inventory_model_remove_zone(fridge_ui_inventory_model_t *model, const char *zone_id)
{
    ESP_RETURN_ON_FALSE(model && zone_id && zone_id[0], ESP_ERR_INVALID_ARG, TAG, "invalid zone remove");
    fridge_ui_inventory_model_t next = *model;
    int index = find_zone(&next, zone_id);
    if (index < 0) return ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < next.layer_count; i++) if (strcmp(next.layers[i].zone_id, zone_id) == 0) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < next.item_count; i++) if (strcmp(next.items[i].zone_id, zone_id) == 0) return ESP_ERR_INVALID_STATE;
    memmove(&next.zones[index], &next.zones[index + 1], (next.zone_count - (size_t)index - 1) * sizeof(next.zones[0]));
    next.zone_count--;
    return commit_model(model, &next);
}

esp_err_t fridge_ui_inventory_model_upsert_layer(fridge_ui_inventory_model_t *model,
                                                 fridge_ui_inventory_layer_t *layer)
{
    ESP_RETURN_ON_FALSE(model && layer && text_fits(layer->zone_id, sizeof(layer->zone_id), true) &&
                            text_fits(layer->name, sizeof(layer->name), true),
                        ESP_ERR_INVALID_ARG, TAG, "invalid layer");
    fridge_ui_inventory_model_t next = *model;
    int index = find_layer(&next, layer->id);
    if (layer->id[0] && index < 0) return ESP_ERR_NOT_FOUND;
    if (find_zone(&next, layer->zone_id) < 0) return ESP_ERR_NOT_FOUND;
    if (index < 0) {
        if (next.layer_count >= FRIDGE_UI_INVENTORY_LAYER_MAX) return ESP_ERR_NO_MEM;
        index = (int)next.layer_count;
        create_unique_id(&next, "layer", layer->id, sizeof(layer->id));
        if (!layer->id[0]) return ESP_ERR_NO_MEM;
        next.layer_count++;
    }
    next.layers[index] = *layer;
    esp_err_t err = commit_model(model, &next);
    if (err == ESP_OK) *layer = model->layers[index];
    return err;
}

esp_err_t fridge_ui_inventory_model_remove_layer(fridge_ui_inventory_model_t *model, const char *layer_id)
{
    ESP_RETURN_ON_FALSE(model && layer_id && layer_id[0], ESP_ERR_INVALID_ARG, TAG, "invalid layer remove");
    fridge_ui_inventory_model_t next = *model;
    int index = find_layer(&next, layer_id);
    if (index < 0) return ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < next.item_count; i++) if (strcmp(next.items[i].layer_id, layer_id) == 0) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < next.layout_draft_count; i++) if (strcmp(next.layout_drafts[i].layer_id, layer_id) == 0) return ESP_ERR_INVALID_STATE;
    memmove(&next.layers[index], &next.layers[index + 1], (next.layer_count - (size_t)index - 1) * sizeof(next.layers[0]));
    next.layer_count--;
    return commit_model(model, &next);
}

esp_err_t fridge_ui_inventory_model_upsert_item(fridge_ui_inventory_model_t *model,
                                                fridge_ui_inventory_item_t *item)
{
    const bool located = item && item->zone_id[0] && item->layer_id[0] &&
                         item->cell < FRIDGE_UI_INVENTORY_GRID_CELLS;
    const bool unlocated = item && !item->zone_id[0] && !item->layer_id[0] &&
                           item->cell == FRIDGE_UI_INVENTORY_CELL_UNLOCATED;
    ESP_RETURN_ON_FALSE(model && item && text_fits(item->name, sizeof(item->name), true) &&
                            (located || unlocated),
                        ESP_ERR_INVALID_ARG, TAG, "invalid inventory item");
    fridge_ui_inventory_model_t next = *model;
    int index = find_item(&next, item->id);
    if (item->id[0] && index < 0) return ESP_ERR_NOT_FOUND;
    int layer = located ? find_layer(&next, item->layer_id) : -1;
    if (located &&
        (find_zone(&next, item->zone_id) < 0 || layer < 0 ||
         strcmp(next.layers[layer].zone_id, item->zone_id) != 0)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (index < 0) {
        if (next.item_count >= FRIDGE_UI_INVENTORY_ITEM_MAX) return ESP_ERR_NO_MEM;
        index = (int)next.item_count;
        create_unique_id(&next, "item", item->id, sizeof(item->id));
        if (!item->id[0]) return ESP_ERR_NO_MEM;
        next.item_count++;
    }
    next.items[index] = *item;
    esp_err_t err = commit_model(model, &next);
    if (err == ESP_OK) *item = model->items[index];
    return err;
}

esp_err_t fridge_ui_inventory_model_remove_item(fridge_ui_inventory_model_t *model, const char *item_id)
{
    ESP_RETURN_ON_FALSE(model && item_id && item_id[0], ESP_ERR_INVALID_ARG, TAG, "invalid item remove");
    fridge_ui_inventory_model_t next = *model;
    int index = find_item(&next, item_id);
    if (index < 0) return ESP_ERR_NOT_FOUND;
    memmove(&next.items[index], &next.items[index + 1], (next.item_count - (size_t)index - 1) * sizeof(next.items[0]));
    next.item_count--;
    return commit_model(model, &next);
}

esp_err_t fridge_ui_inventory_model_upsert_layout_draft(fridge_ui_inventory_model_t *model,
                                                         fridge_ui_inventory_layout_draft_t *draft)
{
    ESP_RETURN_ON_FALSE(model && draft && text_fits(draft->zone_id, sizeof(draft->zone_id), true) &&
                            text_fits(draft->layer_id, sizeof(draft->layer_id), true) &&
                            text_fits(draft->name, sizeof(draft->name), true),
                        ESP_ERR_INVALID_ARG, TAG, "invalid layout draft");
    fridge_ui_inventory_model_t next = *model;
    int index = find_draft(&next, draft->id);
    if (draft->id[0] && index < 0) return ESP_ERR_NOT_FOUND;
    int layer = find_layer(&next, draft->layer_id);
    if (find_zone(&next, draft->zone_id) < 0 || layer < 0 || strcmp(next.layers[layer].zone_id, draft->zone_id) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (index < 0) {
        for (size_t i = 0; i < next.layout_draft_count; i++) {
            if (strcmp(next.layout_drafts[i].zone_id, draft->zone_id) == 0 &&
                strcmp(next.layout_drafts[i].layer_id, draft->layer_id) == 0) {
                index = (int)i;
                strlcpy(draft->id, next.layout_drafts[i].id, sizeof(draft->id));
                break;
            }
        }
    }
    if (index < 0) {
        if (next.layout_draft_count >= FRIDGE_UI_INVENTORY_LAYOUT_DRAFT_MAX) return ESP_ERR_NO_MEM;
        index = (int)next.layout_draft_count;
        create_unique_id(&next, "layout", draft->id, sizeof(draft->id));
        if (!draft->id[0]) return ESP_ERR_NO_MEM;
        next.layout_draft_count++;
    }
    for (size_t i = 0; i < FRIDGE_UI_INVENTORY_GRID_CELLS; i++) {
        if (!text_fits(draft->cell_labels[i], sizeof(draft->cell_labels[i]), false)) return ESP_ERR_INVALID_ARG;
    }
    next.layout_drafts[index] = *draft;
    esp_err_t err = commit_model(model, &next);
    if (err == ESP_OK) *draft = model->layout_drafts[index];
    return err;
}

esp_err_t fridge_ui_inventory_model_remove_layout_draft(fridge_ui_inventory_model_t *model, const char *draft_id)
{
    ESP_RETURN_ON_FALSE(model && draft_id && draft_id[0], ESP_ERR_INVALID_ARG, TAG, "invalid layout draft remove");
    fridge_ui_inventory_model_t next = *model;
    int index = find_draft(&next, draft_id);
    if (index < 0) return ESP_ERR_NOT_FOUND;
    memmove(&next.layout_drafts[index], &next.layout_drafts[index + 1],
            (next.layout_draft_count - (size_t)index - 1) * sizeof(next.layout_drafts[0]));
    next.layout_draft_count--;
    return commit_model(model, &next);
}
