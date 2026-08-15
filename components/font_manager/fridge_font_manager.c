// 冰箱小精灵 P4 字体管理器。
// 设计为“Flash 应急字体 + TF BIN 常用字 + TF FreeType 全量回退”，避免 AI 文本缺字或频繁卡顿。

#include "fridge_font_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "font/lv_binfont_loader.h"
#if LV_USE_FREETYPE
#include "libs/freetype/lv_freetype.h"
#endif

LV_FONT_DECLARE(fridge_font_cn_16);

static const char *TAG = "fridge_font";
static const char *const s_bin_paths[FRIDGE_FONT_ROLE_COUNT] = {
    [FRIDGE_FONT_BODY_SMALL] = "S:/fridge/assets/fontpack/v1/bitmap/core_18_2bpp.bin",
    [FRIDGE_FONT_BODY_SMALL_EMPHASIS] = "S:/fridge/assets/fontpack/v1/bitmap/core_18_2bpp.bin",
    [FRIDGE_FONT_BODY] = "S:/fridge/assets/fontpack/v1/bitmap/core_22_2bpp.bin",
    [FRIDGE_FONT_BODY_EMPHASIS] = "S:/fridge/assets/fontpack/v1/bitmap/core_22_2bpp.bin",
    [FRIDGE_FONT_TITLE] = "S:/fridge/assets/fontpack/v1/bitmap/ui_28_2bpp.bin",
    [FRIDGE_FONT_SECTION] = "S:/fridge/assets/fontpack/v1/bitmap/ui_36_2bpp.bin",
    // 48px 必须由对应字号的 FreeType 角色生成，不能把 36px BIN 冒充大字。
    [FRIDGE_FONT_LARGE] = NULL,
    // 64px 时钟/数字优先走专用低色深 BIN，非数字字形再回退到 64px FreeType。
    [FRIDGE_FONT_HUGE] = "S:/fridge/assets/fontpack/v1/bitmap/digits_64_2bpp.bin",
    [FRIDGE_FONT_SYMBOL] = NULL,
    [FRIDGE_FONT_TITLE_STRONG] = NULL,
    [FRIDGE_FONT_SECTION_STRONG] = NULL,
    [FRIDGE_FONT_LARGE_STRONG] = NULL,
    [FRIDGE_FONT_CAPTION_EMPHASIS] = NULL,
    [FRIDGE_FONT_STATUS_TIME_STRONG] = NULL,
};

static const uint32_t s_sizes[FRIDGE_FONT_ROLE_COUNT] = {
    [FRIDGE_FONT_BODY_SMALL] = 18,
    [FRIDGE_FONT_BODY_SMALL_EMPHASIS] = 18,
    [FRIDGE_FONT_BODY] = 22,
    [FRIDGE_FONT_BODY_EMPHASIS] = 22,
    [FRIDGE_FONT_TITLE] = 28,
    [FRIDGE_FONT_SECTION] = 36,
    [FRIDGE_FONT_LARGE] = 48,
    [FRIDGE_FONT_HUGE] = 64,
    [FRIDGE_FONT_SYMBOL] = 22,
    [FRIDGE_FONT_TITLE_STRONG] = 28,
    [FRIDGE_FONT_SECTION_STRONG] = 36,
    [FRIDGE_FONT_LARGE_STRONG] = 48,
    [FRIDGE_FONT_CAPTION_EMPHASIS] = 15,
    [FRIDGE_FONT_STATUS_TIME_STRONG] = 40,
};
static const lv_font_t *s_fonts[FRIDGE_FONT_ROLE_COUNT];
#if LV_USE_FREETYPE
static lv_font_t *s_ft_fonts[FRIDGE_FONT_ROLE_COUNT];
#endif
static fridge_font_status_t s_status;
#if !LV_USE_FONT_COMPRESSED
static bool s_compressed_font_warning_logged;
#endif

// 参考稿中 18/22px 的胶囊、卡片标题和按钮也有 500/600 字重，
// 因此为同字号保留 Regular/Medium 两套角色；不做像素描边伪粗体。
static bool role_uses_emphasis_weight(fridge_font_role_t role)
{
    return role == FRIDGE_FONT_BODY_SMALL_EMPHASIS ||
           role == FRIDGE_FONT_CAPTION_EMPHASIS ||
           role == FRIDGE_FONT_BODY_EMPHASIS ||
           role == FRIDGE_FONT_TITLE || role == FRIDGE_FONT_SECTION ||
           role == FRIDGE_FONT_LARGE || role == FRIDGE_FONT_HUGE;
}

static bool role_uses_strong_weight(fridge_font_role_t role)
{
    return role == FRIDGE_FONT_TITLE_STRONG ||
           role == FRIDGE_FONT_SECTION_STRONG ||
           role == FRIDGE_FONT_LARGE_STRONG ||
           role == FRIDGE_FONT_STATUS_TIME_STRONG;
}

static fridge_font_role_t strong_fallback_role(fridge_font_role_t role)
{
    switch (role) {
        case FRIDGE_FONT_TITLE_STRONG:
            return FRIDGE_FONT_TITLE;
        case FRIDGE_FONT_SECTION_STRONG:
            return FRIDGE_FONT_SECTION;
        case FRIDGE_FONT_LARGE_STRONG:
            return FRIDGE_FONT_LARGE;
        default:
            return FRIDGE_FONT_BODY;
    }
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void set_error(const char *text)
{
    strlcpy(s_status.last_error, text ? text : "", sizeof(s_status.last_error));
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
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    actual[64] = '\0';
    return strcasecmp(actual, expected) == 0;
}

static bool validate_manifest(void)
{
    FILE *file = fopen("/sdcard/fridge/assets/fontpack/v1/manifest.json", "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || size > 64 * 1024) { fclose(file); return false; }
    rewind(file);
    char *json = malloc((size_t)size + 1);
    if (!json) { fclose(file); return false; }
    bool read_ok = fread(json, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read_ok) { free(json); return false; }
    json[size] = '\0';
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "lvgl_version");
    cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    bool valid = cJSON_IsString(version) && strcmp(version->valuestring, "9.2.2") == 0 &&
                 cJSON_IsArray(files);
    // 启动时校验运行必需的 BIN 和拼音索引；其余大 OTF 文件由电脑端工具完整校验，
    // 避免把启动时间拖过 3 秒，同时拒绝最常见的半包/拷贝错误。
    const char *required[] = {
        "bitmap/core_18_2bpp.bin", "bitmap/core_22_2bpp.bin", "bitmap/ui_28_2bpp.bin",
        "bitmap/ui_36_2bpp.bin", "bitmap/digits_64_2bpp.bin", "ime/pinyin_simp.bin",
        "outline/NotoSansCJKsc-Bold-UI.otf",
    };
    for (size_t r = 0; valid && r < sizeof(required) / sizeof(required[0]); r++) {
        bool found = false;
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, files) {
            cJSON *path = cJSON_GetObjectItemCaseSensitive(entry, "path");
            cJSON *hash = cJSON_GetObjectItemCaseSensitive(entry, "sha256");
            if (cJSON_IsString(path) && cJSON_IsString(hash) && strcmp(path->valuestring, required[r]) == 0) {
                char absolute[192];
                snprintf(absolute, sizeof(absolute), "/sdcard/fridge/assets/fontpack/v1/%s", required[r]);
                found = sha256_file_matches(absolute, hash->valuestring);
                break;
            }
        }
        valid = found;
    }
    cJSON_Delete(root);
    return valid;
}

static void load_role(fridge_font_role_t role)
{
    if (s_status.loaded_roles & (1UL << role)) return;
    s_fonts[role] = &fridge_font_cn_16;
    if (!s_status.manifest_verified) return;

    lv_font_t *bin = NULL;
#if LV_USE_FONT_COMPRESSED
    if (s_bin_paths[role]) {
        char absolute[192];
        // LVGL 通过 S: 盘读取 BIN；fopen 检查则使用 FatFs 实际挂载点 /sdcard。
        int written = snprintf(absolute, sizeof(absolute), "/sdcard/%s", s_bin_paths[role] + 3);
        if (written > 0 && (size_t)written < sizeof(absolute) && file_exists(absolute)) {
            bin = lv_binfont_create(s_bin_paths[role]);
        }
    }
#else
    // 当前 TF 字体包由 lv_font_conv 以压缩 BIN 格式生成。若构建配置漂移，
    // 必须跳过 BIN，让 FreeType 成为主字体；否则 LVGL 会命中 BIN 字形描述，
    // 随后因无法解压位图返回 NULL，并阻断 FreeType fallback，表现为中文方框。
    if (!s_compressed_font_warning_logged) {
        s_compressed_font_warning_logged = true;
        ESP_LOGW(TAG, "compressed TF fonts disabled; bypassing BIN and using FreeType");
    }
#endif
    if (bin) {
#if LV_USE_FREETYPE
        if (s_ft_fonts[role]) {
            bin->fallback = s_ft_fonts[role];
        }
#endif
        s_fonts[role] = bin;
        s_status.loaded_roles |= 1UL << role;
        s_status.tf_pack_loaded = true;
    }
}

#if LV_USE_FREETYPE
static void load_freetype_fallback(void)
{
    if (!s_status.manifest_verified) return;
    static const char *const regular_path =
        "/sdcard/fridge/assets/fontpack/v1/outline/NotoSansCJKsc-Regular.otf";
    static const char *const emphasis_path =
        "/sdcard/fridge/assets/fontpack/v1/outline/NotoSansCJKsc-Medium.otf";
    static const char *const strong_path =
        "/sdcard/fridge/assets/fontpack/v1/outline/NotoSansCJKsc-Bold-UI.otf";
    if (!file_exists(regular_path)) return;
    const bool emphasis_available = file_exists(emphasis_path);
    const bool strong_available = file_exists(strong_path);

    // lv_init() 会按 CONFIG_LV_USE_FREETYPE 自动初始化 FreeType。这里不能再次
    // 调用 lv_freetype_init()，否则 LVGL 会以“已初始化”返回失败并跳过 OTF 回退。
    size_t created = 0;
    size_t emphasized = 0;
    size_t strong = 0;
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        const fridge_font_role_t role = (fridge_font_role_t)i;
        const bool use_strong = role_uses_strong_weight(role);
        const bool use_emphasis = role_uses_emphasis_weight(role);
        const char *font_path = use_strong && strong_available
                                    ? strong_path
                                    : ((use_strong || use_emphasis) && emphasis_available
                                           ? emphasis_path
                                           : regular_path);
        if (!s_ft_fonts[i]) {
            s_ft_fonts[i] = lv_freetype_font_create(
                font_path,
                LV_FREETYPE_FONT_RENDER_MODE_BITMAP, s_sizes[i], LV_FREETYPE_FONT_STYLE_NORMAL);
        }
        if (s_ft_fonts[i]) {
            created++;
            lv_font_t *bin = s_fonts[i] && s_fonts[i] != &fridge_font_cn_16
                                 ? (lv_font_t *)s_fonts[i]
                                 : NULL;
            if (use_strong) {
                // Bold 子集只保存 Shell 高频字形；其它字形按同字号回退到 Medium，
                // 既保持 700 字重层级，也避免在 TF 再放一份 17MB 全量 Bold。
                const fridge_font_role_t fallback = strong_fallback_role(role);
                const lv_font_t *fallback_font = s_fonts[fallback]
                                                     ? s_fonts[fallback]
                                                     : &fridge_font_cn_16;
                s_ft_fonts[i]->fallback = fallback_font;
                s_fonts[i] = s_ft_fonts[i];
                if (strong_available) strong++;
            } else if (use_emphasis && emphasis_available) {
                // 加重角色必须以 Medium 为主字体，否则已有 Regular BIN 会先命中常用字，
                // 视觉上仍然没有字重差异。BIN 只在轮廓字体缺字时作为后备。
                s_ft_fonts[i]->fallback = bin ? bin : &fridge_font_cn_16;
                if (bin) bin->fallback = &fridge_font_cn_16;
                s_fonts[i] = s_ft_fonts[i];
                emphasized++;
            } else {
                // 正文保留 BIN 快路径，FreeType 只负责全量 CJK 缺字回退。
                s_ft_fonts[i]->fallback = &fridge_font_cn_16;
                if (bin) {
                    bin->fallback = s_ft_fonts[i];
                } else {
                    s_fonts[i] = s_ft_fonts[i];
                }
            }
            s_status.loaded_roles |= 1UL << i;
        }
        // 14 个字号/字重角色逐个建立时主动让出 CPU0，保证 IDLE0 能喂看门狗。
        // 字体仍在同一 UI 任务串行绑定，不会引入跨线程 LVGL 访问。
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    s_status.freetype_ready = created == FRIDGE_FONT_ROLE_COUNT;
    if (!s_status.freetype_ready) {
        set_error("FreeType 字体角色创建不完整");
    } else {
        set_error("");
    }
    ESP_LOGI(TAG, "FreeType CJK roles=%u/%u emphasis=%u strong=%u weight=%s/%s",
             (unsigned)created, (unsigned)FRIDGE_FONT_ROLE_COUNT, (unsigned)emphasized,
             (unsigned)strong, emphasis_available ? "Medium" : "Regular fallback",
             strong_available ? "Bold UI" : "Medium fallback");
}
#endif

esp_err_t fridge_font_manager_init(void)
{
    if (s_status.initialized) return ESP_OK;
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.package_version, "builtin", sizeof(s_status.package_version));
    // 启动早期 TF 可能尚未挂载：manifest 失败不致命，后续 try_load_tf_pack/load_freetype 会重试。
    s_status.manifest_verified = validate_manifest();
    if (!s_status.manifest_verified && file_exists("/sdcard/fridge/assets/fontpack/v1/manifest.json")) {
        set_error("字体包 manifest 或 SHA-256 校验失败");
    } else if (!s_status.manifest_verified) {
        set_error("TF 字体包尚未就绪（等待挂载后重试）");
    }
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        s_fonts[i] = &fridge_font_cn_16;
    }
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) load_role((fridge_font_role_t)i);
    // FreeType 全量 CJK OTF 在 SD 上首次创建很慢，放到 load_freetype()，等 UI 首帧后再做。
    s_status.initialized = true;
    ESP_LOGI(TAG, "font manager ready (fast): tf=%d freetype=%d compressed=%d roles=0x%lx err=%s",
             s_status.tf_pack_loaded, s_status.freetype_ready, LV_USE_FONT_COMPRESSED,
             (unsigned long)s_status.loaded_roles,
             s_status.last_error[0] ? s_status.last_error : "-");
    return ESP_OK;
}

esp_err_t fridge_font_manager_try_load_tf_pack(void)
{
    if (!s_status.initialized) {
        return fridge_font_manager_init();
    }
    uint32_t bin_roles = 0;
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        if (s_bin_paths[i]) bin_roles |= 1UL << i;
    }
    if (s_status.manifest_verified && (s_status.loaded_roles & bin_roles) == bin_roles) {
        return ESP_OK;
    }
    // TF 挂载后重新校验 manifest 并加载 BIN；可重复调用。
    bool ok = validate_manifest();
    s_status.manifest_verified = ok;
    if (!ok) {
        if (file_exists("/sdcard/fridge/assets/fontpack/v1/manifest.json")) {
            set_error("字体包 manifest 或 SHA-256 校验失败");
        } else {
            set_error("TF 字体包未找到");
        }
        return ESP_ERR_NOT_FOUND;
    }
    set_error("");
    for (size_t i = 0; i < FRIDGE_FONT_ROLE_COUNT; i++) {
        load_role((fridge_font_role_t)i);
    }
    ESP_LOGI(TAG, "TF font pack reload: loaded=%d roles=0x%lx",
             s_status.tf_pack_loaded, (unsigned long)s_status.loaded_roles);
    if ((s_status.loaded_roles & bin_roles) != bin_roles) {
        set_error("TF BIN 字体角色加载不完整");
        return ESP_ERR_NOT_FOUND;
    }
    set_error("");
    return ESP_OK;
}

esp_err_t fridge_font_manager_load_freetype(void)
{
    if (!s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
#if LV_USE_FREETYPE
    if (s_status.freetype_ready) {
        return ESP_OK;
    }
    // TF 可能在 init 之后才挂载：先重试 BIN/manifest，再开 FreeType。
    (void)fridge_font_manager_try_load_tf_pack();
    if (!s_status.manifest_verified) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "loading FreeType CJK fallback (deferred after first frame)...");
    load_freetype_fallback();
    ESP_LOGI(TAG, "font manager FreeType done: ready=%d", s_status.freetype_ready);
    return s_status.freetype_ready ? ESP_OK : ESP_ERR_NOT_FOUND;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const lv_font_t *fridge_font_manager_get(fridge_font_role_t role)
{
    if (role >= FRIDGE_FONT_ROLE_COUNT) return &fridge_font_cn_16;
    return s_fonts[role] ? s_fonts[role] : &fridge_font_cn_16;
}

esp_err_t fridge_font_manager_reload(void)
{
    // 当前版本只允许在 LVGL 尚未创建页面时初始化/重载，避免悬空 font 指针。
    if (s_status.initialized) return ESP_ERR_INVALID_STATE;
    return fridge_font_manager_init();
}

void fridge_font_manager_get_status(fridge_font_status_t *out)
{
    if (out) *out = s_status;
}

bool fridge_font_manager_has_codepoint(uint32_t codepoint)
{
    const lv_font_t *font = fridge_font_manager_get(FRIDGE_FONT_BODY);
    lv_font_glyph_dsc_t glyph = {0};
    return font && lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) &&
           glyph.resolved_font != NULL && !glyph.is_placeholder;
}
