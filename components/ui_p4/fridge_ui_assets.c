// ver2 TF 资源包校验与装载。
// manifest、文件大小和 SHA-256 全部通过后才提交资源，半包或损坏文件不会污染当前 UI。

#include "fridge_ui_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#define UI_MANIFEST_MAX (64U * 1024U)
#define UI_ICON_MAX_BYTES (64U * 64U)
#define UI_ICON_TOTAL_MAX (128U * 1024U)

typedef struct {
    fridge_ui_icon_id_t id;
    const char *name;
    const char *relative_path;
} icon_spec_t;

static const char *TAG = "fridge_ui_asset";
static const icon_spec_t s_icon_specs[FRIDGE_UI_ICON_COUNT] = {
    {FRIDGE_UI_ICON_HOME, "home", "icons/home.a8"},
    {FRIDGE_UI_ICON_BOX, "box", "icons/box.a8"},
    {FRIDGE_UI_ICON_CHAT, "chat", "icons/chat.a8"},
    {FRIDGE_UI_ICON_PLAN, "plan", "icons/plan.a8"},
    {FRIDGE_UI_ICON_MORE, "more", "icons/more.a8"},
    {FRIDGE_UI_ICON_SEARCH, "search", "icons/search.a8"},
    {FRIDGE_UI_ICON_PLUS, "plus", "icons/plus.a8"},
    {FRIDGE_UI_ICON_CLOCK, "clock", "icons/clock.a8"},
    {FRIDGE_UI_ICON_WARN, "warn", "icons/warn.a8"},
    {FRIDGE_UI_ICON_LOCATION, "location", "icons/location.a8"},
    {FRIDGE_UI_ICON_FOOD, "food", "icons/food.a8"},
    {FRIDGE_UI_ICON_INFO, "info", "icons/info.a8"},
    {FRIDGE_UI_ICON_STAR, "star", "icons/star.a8"},
    {FRIDGE_UI_ICON_ZAP, "zap", "icons/zap.a8"},
    {FRIDGE_UI_ICON_CLOSE, "close", "icons/close.a8"},
    {FRIDGE_UI_ICON_CART, "cart", "icons/cart.a8"},
    {FRIDGE_UI_ICON_FWD, "fwd", "icons/fwd.a8"},
    {FRIDGE_UI_ICON_TEMP, "temp", "icons/temp.a8"},
    {FRIDGE_UI_ICON_DROP, "drop", "icons/drop.a8"},
    {FRIDGE_UI_ICON_WIFI, "wifi", "icons/wifi.a8"},
    {FRIDGE_UI_ICON_CLOUD, "cloud", "icons/cloud.a8"},
    {FRIDGE_UI_ICON_TF, "tf", "icons/tf.a8"},
    {FRIDGE_UI_ICON_CAMERA, "camera", "icons/camera.a8"},
    {FRIDGE_UI_ICON_MIC, "mic", "icons/mic.a8"},
    {FRIDGE_UI_ICON_TIMER, "timer", "icons/timer.a8"},
    {FRIDGE_UI_ICON_CHECK, "check", "icons/check.a8"},
    {FRIDGE_UI_ICON_BELL, "bell", "icons/bell.a8"},
    {FRIDGE_UI_ICON_STOPWATCH, "stopwatch", "icons/stopwatch.a8"},
    {FRIDGE_UI_ICON_BACK, "back", "icons/back.a8"},
    {FRIDGE_UI_ICON_RETRY, "retry", "icons/retry.a8"},
    {FRIDGE_UI_ICON_PLAY, "play", "icons/play.a8"},
    {FRIDGE_UI_ICON_PAUSE, "pause", "icons/pause.a8"},
    {FRIDGE_UI_ICON_LIST, "list", "icons/list.a8"},
    {FRIDGE_UI_ICON_LOCK, "lock", "icons/lock.a8"},
    {FRIDGE_UI_ICON_GRID, "grid", "icons/grid.a8"},
    {FRIDGE_UI_ICON_EDIT, "edit", "icons/edit.a8"},
    {FRIDGE_UI_ICON_MINUS, "minus", "icons/minus.a8"},
    {FRIDGE_UI_ICON_TRASH, "trash", "icons/trash.a8"},
    {FRIDGE_UI_ICON_UNDO, "undo", "icons/undo.a8"},
    {FRIDGE_UI_ICON_SHIELD, "shield", "icons/shield.a8"},
    {FRIDGE_UI_ICON_KEYBOARD, "keyboard", "icons/keyboard.a8"},
    {FRIDGE_UI_ICON_DOOR, "door", "icons/door.a8"},
    {FRIDGE_UI_ICON_FRIDGE, "fridge", "icons/fridge.a8"},
};

// 旧版 TF 包已经在设备上部署过 26 个图标；新增的更多页图标允许缺省，
// 这样升级固件时不会因为少两个非核心图标而让整包资源回退。
static bool icon_is_optional(fridge_ui_icon_id_t id)
{
    return id == FRIDGE_UI_ICON_BELL || id == FRIDGE_UI_ICON_STOPWATCH ||
           id == FRIDGE_UI_ICON_BACK || id == FRIDGE_UI_ICON_RETRY ||
           id == FRIDGE_UI_ICON_PLAY || id == FRIDGE_UI_ICON_PAUSE ||
           id == FRIDGE_UI_ICON_LIST || id == FRIDGE_UI_ICON_LOCK ||
           id == FRIDGE_UI_ICON_GRID || id == FRIDGE_UI_ICON_EDIT ||
           id == FRIDGE_UI_ICON_MINUS || id == FRIDGE_UI_ICON_TRASH ||
           id == FRIDGE_UI_ICON_UNDO || id == FRIDGE_UI_ICON_SHIELD ||
           id == FRIDGE_UI_ICON_KEYBOARD || id == FRIDGE_UI_ICON_DOOR ||
           id == FRIDGE_UI_ICON_FRIDGE;
}

static void set_error(fridge_ui_assets_t *assets, const char *text)
{
    if (assets) strlcpy(assets->last_error, text ? text : "", sizeof(assets->last_error));
}

static bool read_small_file(const char *path, size_t max_size, char **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length <= 0 || (size_t)length > max_size || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *buffer = heap_caps_malloc((size_t)length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = malloc((size_t)length + 1);
    }
    if (!buffer) {
        fclose(file);
        return false;
    }
    bool ok = fread(buffer, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) {
        free(buffer);
        return false;
    }
    buffer[length] = '\0';
    *out = buffer;
    *out_size = (size_t)length;
    return true;
}

static bool sha256_file_matches(const char *path, const char *expected)
{
    if (!path || !expected || strlen(expected) != 64) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    mbedtls_sha256_context context;
    uint8_t digest[32];
    uint8_t buffer[4096];
    bool ok = true;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts(&context, 0) != 0) ok = false;
    while (ok) {
        size_t length = fread(buffer, 1, sizeof(buffer), file);
        if (length > 0 && mbedtls_sha256_update(&context, buffer, length) != 0) ok = false;
        if (length < sizeof(buffer)) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    if (ok && mbedtls_sha256_finish(&context, digest) != 0) ok = false;
    mbedtls_sha256_free(&context);
    fclose(file);
    if (!ok) return false;
    char actual[65];
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    }
    actual[64] = '\0';
    return strcasecmp(actual, expected) == 0;
}

static bool integer_field_equals(const cJSON *object, const char *key, int expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && value->valuedouble == value->valueint &&
           value->valueint == expected;
}

static bool validate_layout(const cJSON *root)
{
    const cJSON *layout = cJSON_GetObjectItemCaseSensitive(root, "layout");
    if (cJSON_IsString(layout)) {
        return strcmp(layout->valuestring, "1024x600:704+72+248") == 0 ||
               strcmp(layout->valuestring, "1024x600/704+72+248") == 0;
    }
    if (!cJSON_IsObject(layout) || !integer_field_equals(layout, "width", 1024) ||
        !integer_field_equals(layout, "height", 600)) {
        return false;
    }
    bool direct = integer_field_equals(layout, "main_width", 704) &&
                  integer_field_equals(layout, "navigation_width", 72) &&
                  integer_field_equals(layout, "cabin_width", 248);
    const cJSON *columns = cJSON_GetObjectItemCaseSensitive(layout, "columns");
    bool array = cJSON_IsArray(columns) && cJSON_GetArraySize(columns) == 3 &&
                 cJSON_GetArrayItem(columns, 0)->valueint == 704 &&
                 cJSON_GetArrayItem(columns, 1)->valueint == 72 &&
                 cJSON_GetArrayItem(columns, 2)->valueint == 248;
    return direct || array;
}

static const cJSON *find_file_entry(const cJSON *files, const char *relative_path)
{
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, files) {
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(entry, "path");
        if (cJSON_IsString(path) && strcmp(path->valuestring, relative_path) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool validate_file_entry(const cJSON *entry, const char *relative_path,
                                size_t max_size, size_t *actual_size)
{
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(entry, "size");
    const cJSON *hash = cJSON_GetObjectItemCaseSensitive(entry, "sha256");
    if (!cJSON_IsObject(entry) || !cJSON_IsNumber(size) ||
        size->valuedouble != size->valueint || size->valueint <= 0 ||
        (size_t)size->valueint > max_size || !cJSON_IsString(hash)) {
        return false;
    }
    char absolute[256];
    int written = snprintf(absolute, sizeof(absolute), "%s/%s", FRIDGE_UI_VER2_ROOT,
                           relative_path);
    if (written <= 0 || (size_t)written >= sizeof(absolute)) return false;
    FILE *file = fopen(absolute, "rb");
    if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0 && ftell(file) == size->valueint;
    fclose(file);
    if (!ok || !sha256_file_matches(absolute, hash->valuestring)) return false;
    if (actual_size) *actual_size = (size_t)size->valueint;
    return true;
}

static bool load_icon(const cJSON *entry, const icon_spec_t *spec,
                      fridge_ui_a8_icon_t *out)
{
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(entry, "format");
    const cJSON *width = cJSON_GetObjectItemCaseSensitive(entry, "width");
    const cJSON *height = cJSON_GetObjectItemCaseSensitive(entry, "height");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(entry, "size");
    if (!cJSON_IsString(format) || strcmp(format->valuestring, "A8") != 0 ||
        !cJSON_IsNumber(width) || !cJSON_IsNumber(height) || !cJSON_IsNumber(size) ||
        width->valueint <= 0 || width->valueint > 64 ||
        height->valueint <= 0 || height->valueint > 64 ||
        size->valueint != width->valueint * height->valueint) {
        return false;
    }
    char absolute[256];
    snprintf(absolute, sizeof(absolute), "%s/%s", FRIDGE_UI_VER2_ROOT, spec->relative_path);
    FILE *file = fopen(absolute, "rb");
    if (!file) return false;
    uint8_t *buffer = heap_caps_malloc((size_t)size->valueint,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        fclose(file);
        return false;
    }
    bool ok = fread(buffer, 1, (size_t)size->valueint, file) == (size_t)size->valueint;
    fclose(file);
    if (!ok) {
        heap_caps_free(buffer);
        return false;
    }
    out->data = buffer;
    out->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    out->image.header.cf = LV_COLOR_FORMAT_A8;
    out->image.header.flags = 0;
    out->image.header.w = (uint32_t)width->valueint;
    out->image.header.h = (uint32_t)height->valueint;
    out->image.header.stride = (uint32_t)width->valueint;
    out->image.data_size = (uint32_t)size->valueint;
    out->image.data = buffer;
    out->image.reserved = NULL;
    return true;
}

static void free_staged_icons(fridge_ui_assets_t *assets)
{
    for (size_t i = 0; i < FRIDGE_UI_ICON_COUNT; i++) {
        if (assets->icons[i].data) {
            heap_caps_free(assets->icons[i].data);
            assets->icons[i].data = NULL;
        }
    }
}

void fridge_ui_assets_init(fridge_ui_assets_t *assets)
{
    if (assets) memset(assets, 0, sizeof(*assets));
}

esp_err_t fridge_ui_assets_try_load(fridge_ui_assets_t *assets,
                                    home_business_model_t *home_model,
                                    more_business_model_t *more_model,
                                    plan_business_model_t *plan_model)
{
    if (!assets || !home_model || !more_model || !plan_model) return ESP_ERR_INVALID_ARG;
    if (assets->loaded) return ESP_OK;

    char *manifest_text = NULL;
    size_t manifest_size = 0;
    if (!read_small_file(FRIDGE_UI_VER2_MANIFEST, UI_MANIFEST_MAX,
                         &manifest_text, &manifest_size)) {
        set_error(assets, "ver2 manifest 尚未就绪");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *root = cJSON_ParseWithLength(manifest_text, manifest_size);
    free(manifest_text);
    const cJSON *version = root ? cJSON_GetObjectItemCaseSensitive(root, "package_version") : NULL;
    const cJSON *schema = root ? cJSON_GetObjectItemCaseSensitive(root, "schema_version") : NULL;
    const cJSON *lvgl = root ? cJSON_GetObjectItemCaseSensitive(root, "lvgl") : NULL;
    const cJSON *lvgl_version = lvgl ? cJSON_GetObjectItemCaseSensitive(lvgl, "version") : NULL;
    const cJSON *color_format = lvgl ? cJSON_GetObjectItemCaseSensitive(lvgl, "color_format") : NULL;
    const cJSON *files = root ? cJSON_GetObjectItemCaseSensitive(root, "files") : NULL;
    bool legacy_home_package = cJSON_IsString(version) &&
                               strcmp(version->valuestring, "2.0.0-home.1") == 0;
    bool version_compatible = cJSON_IsString(version) &&
                              (strcmp(version->valuestring, "2.0.0-portrait.1") == 0 ||
                               strcmp(version->valuestring, "2.0.0-more.1") == 0 ||
                               legacy_home_package);
    if (!cJSON_IsObject(root) || !version_compatible ||
        strlen(version->valuestring) >= sizeof(assets->package_version) ||
        !cJSON_IsNumber(schema) || schema->valueint != 1 ||
        !cJSON_IsObject(lvgl) || !cJSON_IsString(lvgl_version) ||
        strcmp(lvgl_version->valuestring, "9.2.2") != 0 ||
        !cJSON_IsString(color_format) || strcmp(color_format->valuestring, "RGB565") != 0 ||
        !cJSON_IsArray(files) || !validate_layout(root)) {
        cJSON_Delete(root);
        set_error(assets, "ver2 manifest 版本或布局不兼容");
        return ESP_ERR_INVALID_VERSION;
    }

    const cJSON *home_entry = find_file_entry(files, "content/home.json");
    size_t home_size = 0;
    if (!home_entry || !validate_file_entry(home_entry, "content/home.json",
                                            FRIDGE_UI_HOME_JSON_MAX, &home_size)) {
        cJSON_Delete(root);
        set_error(assets, "home.json 大小或 SHA-256 校验失败");
        return ESP_ERR_INVALID_CRC;
    }

    const cJSON *more_entry = find_file_entry(files, "content/more.json");
    size_t more_size = 0;
    if ((!more_entry && !legacy_home_package) ||
        (more_entry && !validate_file_entry(more_entry, "content/more.json",
                                            FRIDGE_UI_MORE_JSON_MAX, &more_size))) {
        cJSON_Delete(root);
        set_error(assets, "more.json 大小或 SHA-256 校验失败");
        return ESP_ERR_INVALID_CRC;
    }

    size_t icon_total = 0;
    size_t icon_count = 0;
    for (size_t i = 0; i < FRIDGE_UI_ICON_COUNT; i++) {
        const cJSON *entry = find_file_entry(files, s_icon_specs[i].relative_path);
        size_t icon_size = 0;
        if (!entry && icon_is_optional(s_icon_specs[i].id)) continue;
        if (!entry || !validate_file_entry(entry, s_icon_specs[i].relative_path,
                                           UI_ICON_MAX_BYTES, &icon_size) ||
            icon_total + icon_size > UI_ICON_TOTAL_MAX) {
            cJSON_Delete(root);
            set_error(assets, "A8 图标缺失、越界或 SHA-256 校验失败");
            return ESP_ERR_INVALID_CRC;
        }
        icon_total += icon_size;
        icon_count++;
    }

    char home_path[256];
    snprintf(home_path, sizeof(home_path), "%s/content/home.json", FRIDGE_UI_VER2_ROOT);
    char *home_text = NULL;
    size_t home_read_size = 0;
    char parse_error[128] = {0};
    home_business_model_t parsed_home = {0};
    if (!read_small_file(home_path, FRIDGE_UI_HOME_JSON_MAX, &home_text, &home_read_size) ||
        home_read_size != home_size ||
        fridge_ui_home_model_parse_json(home_text, home_read_size, &parsed_home,
                                        parse_error, sizeof(parse_error)) != ESP_OK) {
        free(home_text);
        cJSON_Delete(root);
        set_error(assets, parse_error[0] ? parse_error : "home.json 读取失败");
        return ESP_ERR_INVALID_ARG;
    }
    free(home_text);

    more_business_model_t parsed_more = *more_model;
    if (more_entry) {
        char more_path[256];
        snprintf(more_path, sizeof(more_path), "%s/content/more.json", FRIDGE_UI_VER2_ROOT);
        char *more_text = NULL;
        size_t more_read_size = 0;
        memset(parse_error, 0, sizeof(parse_error));
        if (!read_small_file(more_path, FRIDGE_UI_MORE_JSON_MAX, &more_text,
                             &more_read_size) ||
            more_read_size != more_size ||
            fridge_ui_more_model_parse_json(more_text, more_read_size, &parsed_more,
                                            parse_error, sizeof(parse_error)) != ESP_OK) {
            free(more_text);
            cJSON_Delete(root);
            set_error(assets, parse_error[0] ? parse_error : "more.json 读取失败");
            return ESP_ERR_INVALID_ARG;
        }
        free(more_text);
    }

    plan_business_model_t parsed_plan = *plan_model;
    const cJSON *plan_entry = find_file_entry(files, "content/plan.json");
    size_t plan_size = 0;
    if (plan_entry && !validate_file_entry(plan_entry, "content/plan.json",
                                           FRIDGE_UI_PLAN_JSON_MAX, &plan_size)) {
        cJSON_Delete(root);
        set_error(assets, "plan.json 大小或 SHA-256 校验失败");
        return ESP_ERR_INVALID_CRC;
    }
    if (plan_entry) {
        char plan_path[256];
        snprintf(plan_path, sizeof(plan_path), "%s/content/plan.json", FRIDGE_UI_VER2_ROOT);
        char *plan_text = NULL;
        size_t plan_read_size = 0;
        memset(parse_error, 0, sizeof(parse_error));
        if (!read_small_file(plan_path, FRIDGE_UI_PLAN_JSON_MAX, &plan_text,
                             &plan_read_size) ||
            plan_read_size != plan_size ||
            fridge_ui_plan_model_parse_json(plan_text, plan_read_size, &parsed_plan,
                                            parse_error, sizeof(parse_error)) != ESP_OK) {
            free(plan_text);
            cJSON_Delete(root);
            set_error(assets, parse_error[0] ? parse_error : "plan.json 读取失败");
            return ESP_ERR_INVALID_ARG;
        }
        free(plan_text);
    }

    fridge_ui_assets_t staged = {0};
    strlcpy(staged.package_version, version->valuestring, sizeof(staged.package_version));
    for (size_t i = 0; i < FRIDGE_UI_ICON_COUNT; i++) {
        const cJSON *entry = find_file_entry(files, s_icon_specs[i].relative_path);
        if (!entry && icon_is_optional(s_icon_specs[i].id)) continue;
        if (!load_icon(entry, &s_icon_specs[i], &staged.icons[s_icon_specs[i].id])) {
            free_staged_icons(&staged);
            cJSON_Delete(root);
            set_error(assets, "A8 图标载入 PSRAM 失败");
            return ESP_ERR_NO_MEM;
        }
    }
    cJSON_Delete(root);
    staged.manifest_verified = true;
    staged.loaded = true;
    staged.last_error[0] = '\0';
    *assets = staged;
    *home_model = parsed_home;
    *more_model = parsed_more;
    *plan_model = parsed_plan;
    ESP_LOGI(TAG, "ver2 asset pack loaded: version=%s icons=%u bytes=%u",
             assets->package_version, (unsigned)icon_count, (unsigned)icon_total);
    return ESP_OK;
}

const lv_image_dsc_t *fridge_ui_assets_get_icon(const fridge_ui_assets_t *assets,
                                                fridge_ui_icon_id_t id)
{
    if (!assets || !assets->loaded || id < 0 || id >= FRIDGE_UI_ICON_COUNT ||
        !assets->icons[id].data) {
        return NULL;
    }
    return &assets->icons[id].image;
}

fridge_ui_icon_id_t fridge_ui_assets_icon_id_from_name(const char *name)
{
    if (!name) return FRIDGE_UI_ICON_COUNT;
    for (size_t i = 0; i < FRIDGE_UI_ICON_COUNT; i++) {
        if (strcmp(name, s_icon_specs[i].name) == 0) return s_icon_specs[i].id;
    }
    return FRIDGE_UI_ICON_COUNT;
}
