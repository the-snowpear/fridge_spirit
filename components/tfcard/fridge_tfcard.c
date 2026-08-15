// 冰箱小精灵 TF 卡存储核心 —— ESP32-P4 实现。
//
// 板载 TF 使用 SDMMC Slot0（D0-D3=39-42、CLK=43、CMD=44），C6 Hosted 使用 Slot1。
// 两者共享 SDMMC 控制器与 LDO4：本组件只新增/释放 Slot0，绝不释放控制器或 LDO4。
// 所有 FATFS 与传输状态由同一互斥锁串行化，避免串口、HTTP 和后台任务并发破坏目录项。

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

#include "fridge_board_pins.h"
#include "fridge_tfcard.h"

static const char *TAG = "fridge_tfcard";
static const char *NVS_NAMESPACE = "fridge_tf";
static const char *NVS_EXPERT_KEY = "expert";

typedef struct {
    bool active;
    uint32_t id;
    fridge_tfcard_transfer_direction_t direction;
    uint64_t total_size;
    uint64_t offset;
    bool overwrite;
    FILE *file;
    uint64_t last_offset;
    size_t last_length;
    uint32_t last_crc32;
    char path[FRIDGE_TFCARD_MAX_PATH + 1];
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    char temporary[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 8];
} tfcard_transfer_t;

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_mutex;
static bool s_format_required;
static bool s_formatted_this_boot;
static bool s_unavailable_this_boot;
static bool s_expert_mode;
static char s_last_error[FRIDGE_TFCARD_MAX_ERROR_LEN + 1];
static tfcard_transfer_t s_transfer;
static uint32_t s_next_transfer_id = 1;
static uint32_t s_trash_sequence;

// SDMMC DMA 只能使用内部可寻址内存；在真实读取前后输出连续块，便于定位碎片化。
static void log_internal_dma_heap(const char *phase)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG, "internal_dma[%s]: free=%u largest=%u minimum=%u",
             phase ? phase : "unknown",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

static esp_err_t lock_storage(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_storage(void)
{
    xSemaphoreGive(s_mutex);
}

static void set_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static esp_err_t errno_to_esp(void)
{
    switch (errno) {
        case ENOENT: return ESP_ERR_NOT_FOUND;
        case EEXIST: return ESP_ERR_INVALID_STATE;
        case ENOMEM: return ESP_ERR_NO_MEM;
        case EINVAL: return ESP_ERR_INVALID_ARG;
        default: return ESP_FAIL;
    }
}

// FATFS 的 UTF-8 API 仍需在进入 VFS 前拒绝畸形编码，避免截断中文路径或制造不可访问目录项。
static bool valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        unsigned need = 0;
        uint32_t code = 0;
        if ((*p & 0xE0) == 0xC0) { need = 1; code = *p & 0x1F; if (code < 2) return false; }
        else if ((*p & 0xF0) == 0xE0) { need = 2; code = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { need = 3; code = *p & 0x07; }
        else return false;
        p++;
        for (unsigned i = 0; i < need; ++i, ++p) {
            if ((*p & 0xC0) != 0x80) return false;
            code = (code << 6) | (*p & 0x3F);
        }
        if ((need == 2 && code < 0x800) || (need == 3 && code < 0x10000) ||
            (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return false;
    }
    return true;
}

// 只接受相对路径；不做隐式规范化，直接拒绝 ./、../、反斜杠、控制字符及 FAT 禁用字符。
static esp_err_t validate_relative_path(const char *path, bool allow_root)
{
    if (!path || (!allow_root && path[0] == '\0')) return ESP_ERR_INVALID_ARG;
    const size_t length = strlen(path);
    if (length > FRIDGE_TFCARD_MAX_PATH || path[0] == '/' || path[length ? length - 1 : 0] == '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_utf8(path)) return ESP_ERR_INVALID_ARG;
    const char *segment = path;
    size_t segment_length = 0;
    for (const unsigned char *p = (const unsigned char *)path;; ++p) {
        const unsigned char c = *p;
        if (c == '/' || c == '\0') {
            if (segment_length == 0 && !(allow_root && length == 0)) return ESP_ERR_INVALID_ARG;
            if ((segment_length == 1 && segment[0] == '.') ||
                (segment_length == 2 && segment[0] == '.' && segment[1] == '.')) return ESP_ERR_INVALID_ARG;
            if (segment_length > FRIDGE_TFCARD_MAX_NAME) return ESP_ERR_INVALID_SIZE;
            if (c == '\0') break;
            segment = (const char *)p + 1;
            segment_length = 0;
            continue;
        }
        if (c < 0x20 || c == '\\' || c == '<' || c == '>' || c == ':' ||
            c == '"' || c == '|' || c == '?' || c == '*') return ESP_ERR_INVALID_ARG;
        segment_length++;
    }
    return ESP_OK;
}

// 判断路径是否为指定目录本身或其子路径。所有输入在调用前都必须已经通过路径校验。
static bool path_in_directory(const char *path, const char *directory)
{
    if (!path || !directory) return false;
    const size_t path_length = strlen(path);
    const size_t directory_length = strlen(directory);
    return path_length >= directory_length && strncmp(path, directory, directory_length) == 0 &&
           (path_length == directory_length || path[directory_length] == '/');
}

static bool protected_path(const char *path)
{
    return path_in_directory(path, FRIDGE_TFCARD_SYSTEM_DIR);
}

// 诊断目录保存 coredump 等敏感内容，禁止通用浏览、读取和传输接口触达。
static bool diagnostic_path(const char *path)
{
    return path_in_directory(path, FRIDGE_TFCARD_DIAGNOSTICS_DIR);
}

// 日志不包含敏感 coredump，允许通过普通文本接口轮转清空；追加接口仍会再次校验该前缀。
static bool log_path(const char *path)
{
    return path_in_directory(path, FRIDGE_TFCARD_LOG_DIR);
}

static esp_err_t require_mutable_path(const char *path)
{
    esp_err_t err = validate_relative_path(path, false);
    if (err != ESP_OK) return err;
    // diagnostics 目录始终只允许组件内部原子写入，即使用户开启 TF 专家模式也不得通过通用接口触达。
    if (diagnostic_path(path)) return ESP_ERR_NOT_ALLOWED;
    return protected_path(path) && !log_path(path) && !s_expert_mode ? ESP_ERR_NOT_ALLOWED : ESP_OK;
}

static esp_err_t make_absolute(const char *relative, char *out, size_t out_size)
{
    const int written = relative[0]
        ? snprintf(out, out_size, "%s/%s", FRIDGE_TFCARD_MOUNT_POINT, relative)
        : snprintf(out, out_size, "%s", FRIDGE_TFCARD_MOUNT_POINT);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static bool path_exists(const char *absolute, struct stat *out)
{
    struct stat value;
    const bool exists = stat(absolute, &value) == 0;
    if (exists && out) *out = value;
    return exists;
}

static esp_err_t mkdir_if_missing(const char *absolute)
{
    struct stat st;
    if (path_exists(absolute, &st)) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    return mkdir(absolute, 0775) == 0 ? ESP_OK : errno_to_esp();
}

static esp_err_t ensure_system_dirs_locked(void)
{
    const char *dirs[] = {FRIDGE_TFCARD_SYSTEM_DIR, FRIDGE_TFCARD_LOG_DIR,
                          FRIDGE_TFCARD_DIAGNOSTICS_DIR, FRIDGE_TFCARD_TRASH_DIR,
                          FRIDGE_TFCARD_UPLOAD_DIR, FRIDGE_TFCARD_EXPORT_DIR};
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        ESP_RETURN_ON_ERROR(make_absolute(dirs[i], absolute, sizeof(absolute)), TAG, "系统目录路径过长");
        ESP_RETURN_ON_ERROR(mkdir_if_missing(absolute), TAG, "无法创建系统目录 %s", dirs[i]);
    }
    return ESP_OK;
}

static esp_err_t tfcard_shared_host_init(void)
{
    sdmmc_host_state_t state = {0};
    esp_err_t err = sdmmc_host_get_state(&state);
    if (err != ESP_OK) return err;
    if (state.host_initialized) return ESP_OK;
    ESP_LOGW(TAG, "SDMMC 控制器尚未初始化，TF 独立创建控制器");
    return sdmmc_host_init();
}

static esp_err_t power_on_slot(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FRIDGE_PIN_TF_PWR_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) err = gpio_set_level(FRIDGE_PIN_TF_PWR_EN, 0);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    return err;
}

static esp_err_t mount_locked(bool allow_format)
{
    if (s_card) return ESP_OK;
    esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = allow_format,
        .max_files = 12,
        .allocation_unit_size = 32 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.init = tfcard_shared_host_init;
    host.slot = SDMMC_HOST_SLOT_0;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = FRIDGE_PIN_TF_CLK;
    slot.cmd = FRIDGE_PIN_TF_CMD;
    slot.d0 = FRIDGE_PIN_TF_D0;
    slot.d1 = FRIDGE_PIN_TF_D1;
    slot.d2 = FRIDGE_PIN_TF_D2;
    slot.d3 = FRIDGE_PIN_TF_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    log_internal_dma_heap("sdmmc_mount_before");
    esp_err_t err = esp_vfs_fat_sdmmc_mount(FRIDGE_TFCARD_MOUNT_POINT, &host, &slot, &config, &s_card);
    log_internal_dma_heap("sdmmc_mount_after");
    if (err != ESP_OK) {
        s_card = NULL;
        s_format_required = err == ESP_FAIL;
        set_error(esp_err_to_name(err));
        return err;
    }
    s_format_required = false;
    set_error("");
    err = ensure_system_dirs_locked();
    if (err != ESP_OK) set_error("create system directories failed");
    return err;
}

static void transfer_close_locked(bool remove_temporary)
{
    if (s_transfer.file) fclose(s_transfer.file);
    if (remove_temporary && s_transfer.direction == FRIDGE_TFCARD_TRANSFER_WRITE && s_transfer.temporary[0]) {
        unlink(s_transfer.temporary);
    }
    memset(&s_transfer, 0, sizeof(s_transfer));
}

static esp_err_t remove_tree_locked(const char *absolute)
{
    struct stat st;
    if (stat(absolute, &st) != 0) return errno_to_esp();
    if (!S_ISDIR(st.st_mode)) return unlink(absolute) == 0 ? ESP_OK : errno_to_esp();
    DIR *dir = opendir(absolute);
    if (!dir) return errno_to_esp();
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while (result == ESP_OK && (entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
        int n = snprintf(child, sizeof(child), "%s/%s", absolute, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) result = ESP_ERR_INVALID_SIZE;
        else result = remove_tree_locked(child);
    }
    closedir(dir);
    return result == ESP_OK && rmdir(absolute) != 0 ? errno_to_esp() : result;
}

// FATFS 不能保证 rename 覆盖既有目标；用同卷 .bak 完成可回滚替换，任一步失败都保留旧文件。
static esp_err_t replace_file_locked(const char *temporary, const char *target,
                                     bool overwrite, bool keep_backup)
{
    struct stat st;
    if (!path_exists(target, &st)) return rename(temporary, target) == 0 ? ESP_OK : errno_to_esp();
    if (!overwrite || S_ISDIR(st.st_mode)) return ESP_ERR_INVALID_STATE;
    char backup[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 8];
    if (snprintf(backup, sizeof(backup), "%s.bak", target) >= (int)sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    unlink(backup);
    if (rename(target, backup) != 0) return errno_to_esp();
    if (rename(temporary, target) != 0) {
        const esp_err_t err = errno_to_esp();
        rename(backup, target);
        return err;
    }
    if (!keep_backup) unlink(backup);
    return ESP_OK;
}

static void fill_entry(const char *relative, const char *name, const struct stat *st,
                       fridge_tfcard_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, name, sizeof(out->name));
    strlcpy(out->path, relative, sizeof(out->path));
    out->size = (uint64_t)st->st_size;
    out->mtime = st->st_mtime;
    out->is_dir = S_ISDIR(st->st_mode);
    out->is_hidden = name[0] == '.';
    out->is_protected = protected_path(relative);
}

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static esp_err_t load_expert_mode(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { s_expert_mode = false; return ESP_OK; }
    if (err != ESP_OK) return err;
    uint8_t value = 0;
    err = nvs_get_u8(handle, NVS_EXPERT_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) s_expert_mode = value != 0;
    return err;
}

esp_err_t fridge_tfcard_init(void)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "创建 TF 互斥锁失败");
    esp_err_t err = load_expert_mode();
    if (err == ESP_OK && !s_unavailable_this_boot) err = power_on_slot();
    if (err == ESP_OK && !s_unavailable_this_boot) err = mount_locked(false);
    if (s_unavailable_this_boot) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK && !s_last_error[0]) set_error(esp_err_to_name(err));
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_mount(void)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_unavailable_this_boot ? ESP_ERR_INVALID_STATE : power_on_slot();
    if (err == ESP_OK) err = mount_locked(false);
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_unmount(void)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (!s_card) { unlock_storage(); return ESP_OK; }
    if (s_transfer.active) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    // esp_vfs_fat_sdcard_unmount 只调用 mount 时保存的 Slot0 deinit，不释放 Hosted 的 Slot1/LDO4。
    esp_err_t err = esp_vfs_fat_sdcard_unmount(FRIDGE_TFCARD_MOUNT_POINT, s_card);
    if (err == ESP_OK) s_card = NULL;
    else set_error(esp_err_to_name(err));
    unlock_storage();
    return err;
}

void fridge_tfcard_mark_unavailable(const char *reason)
{
    if (lock_storage() != ESP_OK) return;
    if (!s_card) {
        s_unavailable_this_boot = true;
        s_format_required = false;
        set_error(reason && reason[0] ? reason : "TF 卡前置条件未满足，本次启动未初始化");
    }
    unlock_storage();
}

esp_err_t fridge_tfcard_format_fat32(void)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (s_unavailable_this_boot || s_transfer.active) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = power_on_slot();
    if (err == ESP_OK && !s_card) {
        err = mount_locked(false);
        if (err != ESP_OK) err = mount_locked(true);
    } else if (err == ESP_OK) {
        esp_vfs_fat_mount_config_t config = {
            .format_if_mount_failed = false, .max_files = 12, .allocation_unit_size = 32 * 1024,
        };
        err = esp_vfs_fat_sdcard_format_cfg(FRIDGE_TFCARD_MOUNT_POINT, s_card, &config);
    }
    if (err == ESP_OK) {
        s_formatted_this_boot = true;
        s_format_required = false;
        err = ensure_system_dirs_locked();
    }
    if (err != ESP_OK) set_error(esp_err_to_name(err));
    unlock_storage();
    return err;
}

void fridge_tfcard_get_status(fridge_tfcard_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (lock_storage() != ESP_OK) { strlcpy(out->last_error, "TF mutex timeout", sizeof(out->last_error)); return; }
    out->mounted = s_card != NULL;
    out->format_required = s_format_required;
    out->formatted_this_boot = s_formatted_this_boot;
    out->expert_mode = s_expert_mode;
    out->transfer_active = s_transfer.active;
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    if (s_card) {
        strlcpy(out->card_name, s_card->cid.name, sizeof(out->card_name));
        out->capacity_mb = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024);
        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info(FRIDGE_TFCARD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) out->free_mb = free_bytes / (1024 * 1024);
    }
    unlock_storage();
}

esp_err_t fridge_tfcard_get_expert_mode(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    *enabled = s_expert_mode;
    unlock_storage();
    return ESP_OK;
}

esp_err_t fridge_tfcard_set_expert_mode(bool enabled, const char *confirmation)
{
    if (enabled && (!confirmation || strcmp(confirmation, "ENABLE_TF_EXPERT_MODE") != 0)) return ESP_ERR_NOT_ALLOWED;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, NVS_EXPERT_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) s_expert_mode = enabled;
    if (handle) nvs_close(handle);
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_list(const char *relative_dir, uint32_t cursor, size_t limit,
                             fridge_tfcard_list_result_t *out)
{
    if (!out || limit == 0 || limit > FRIDGE_TFCARD_LIST_PAGE_MAX) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_relative_path(relative_dir, true), TAG, "非法 TF 路径");
    if (diagnostic_path(relative_dir)) return ESP_ERR_NOT_ALLOWED;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    memset(out, 0, sizeof(*out));
    if (!s_card) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    make_absolute(relative_dir, absolute, sizeof(absolute));
    DIR *dir = opendir(absolute);
    if (!dir) { esp_err_t err = errno_to_esp(); unlock_storage(); return err; }
    struct dirent *entry;
    uint32_t position = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        // 只在系统目录的直接列表中隐藏 diagnostics，避免通用浏览器发现敏感目录。
        if (!strcmp(relative_dir, FRIDGE_TFCARD_SYSTEM_DIR) &&
            !strcmp(entry->d_name, "diagnostics")) continue;
        if (position++ < cursor) continue;
        if (out->count == limit) { out->has_more = true; break; }
        char relative[FRIDGE_TFCARD_MAX_PATH + 1];
        int n = relative_dir[0] ? snprintf(relative, sizeof(relative), "%s/%s", relative_dir, entry->d_name)
                                : snprintf(relative, sizeof(relative), "%s", entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(relative)) continue;
        char child[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
        make_absolute(relative, child, sizeof(child));
        struct stat st;
        if (stat(child, &st) == 0) fill_entry(relative, entry->d_name, &st, &out->entries[out->count++]);
    }
    closedir(dir);
    out->next_cursor = cursor + out->count;
    unlock_storage();
    return ESP_OK;
}

esp_err_t fridge_tfcard_stat(const char *relative_path, fridge_tfcard_entry_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_relative_path(relative_path, true), TAG, "非法 TF 路径");
    if (diagnostic_path(relative_path)) return ESP_ERR_NOT_ALLOWED;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    make_absolute(relative_path, absolute, sizeof(absolute));
    struct stat st;
    esp_err_t err = stat(absolute, &st) == 0 ? ESP_OK : errno_to_esp();
    if (err == ESP_OK) {
        const char *name = strrchr(relative_path, '/');
        fill_entry(relative_path, relative_path[0] ? (name ? name + 1 : relative_path) : "/", &st, out);
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_mkdir(const char *relative_path)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_card ? require_mutable_path(relative_path) : ESP_ERR_INVALID_STATE;
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    if (err == ESP_OK) err = make_absolute(relative_path, absolute, sizeof(absolute));
    if (err == ESP_OK && mkdir(absolute, 0775) != 0) err = errno_to_esp();
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_move(const char *from_path, const char *to_path, bool overwrite)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_card ? require_mutable_path(from_path) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = require_mutable_path(to_path);
    char from_abs[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    char to_abs[sizeof(from_abs)];
    if (err == ESP_OK) err = make_absolute(from_path, from_abs, sizeof(from_abs));
    if (err == ESP_OK) err = make_absolute(to_path, to_abs, sizeof(to_abs));
    struct stat st;
    if (err == ESP_OK && path_exists(to_abs, &st)) {
        if (!overwrite) err = ESP_ERR_INVALID_STATE;
        else err = S_ISDIR(st.st_mode) ? remove_tree_locked(to_abs) : (unlink(to_abs) == 0 ? ESP_OK : errno_to_esp());
    }
    if (err == ESP_OK && rename(from_abs, to_abs) != 0) err = errno_to_esp();
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_delete_to_trash(const char *relative_path, bool recursive,
                                        char *trash_id, size_t trash_id_size)
{
    if (!trash_id || trash_id_size < 16) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_card ? require_mutable_path(relative_path) : ESP_ERR_INVALID_STATE;
    char source[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    struct stat st;
    if (err == ESP_OK) err = make_absolute(relative_path, source, sizeof(source));
    if (err == ESP_OK && stat(source, &st) != 0) err = errno_to_esp();
    if (err == ESP_OK && S_ISDIR(st.st_mode) && !recursive) err = ESP_ERR_NOT_ALLOWED;
    const int64_t now = esp_timer_get_time();
    char id[48];
    snprintf(id, sizeof(id), "%08" PRIx32 "-%08" PRIx32, (uint32_t)(now / 1000), ++s_trash_sequence);
    char data_rel[FRIDGE_TFCARD_MAX_PATH + 1], meta_rel[FRIDGE_TFCARD_MAX_PATH + 1];
    snprintf(data_rel, sizeof(data_rel), "%s/%s.data", FRIDGE_TFCARD_TRASH_DIR, id);
    snprintf(meta_rel, sizeof(meta_rel), "%s/%s.meta", FRIDGE_TFCARD_TRASH_DIR, id);
    char data_abs[sizeof(source)], meta_abs[sizeof(source)];
    if (err == ESP_OK) err = make_absolute(data_rel, data_abs, sizeof(data_abs));
    if (err == ESP_OK) err = make_absolute(meta_rel, meta_abs, sizeof(meta_abs));
    if (err == ESP_OK && rename(source, data_abs) != 0) err = errno_to_esp();
    if (err == ESP_OK) {
        FILE *meta = fopen(meta_abs, "wb");
        if (!meta || fwrite(relative_path, 1, strlen(relative_path), meta) != strlen(relative_path) || fflush(meta) != 0) err = ESP_FAIL;
        if (meta) { fsync(fileno(meta)); fclose(meta); }
        if (err != ESP_OK) rename(data_abs, source);
    }
    if (err == ESP_OK) strlcpy(trash_id, id, trash_id_size);
    unlock_storage();
    return err;
}

static esp_err_t validate_trash_id(const char *id)
{
    if (!id || !id[0] || strlen(id) > 47) return ESP_ERR_INVALID_ARG;
    for (const char *p = id; *p; ++p) if (!( (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-')) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t fridge_tfcard_restore(const char *trash_id, bool overwrite,
                               char *restored_path, size_t restored_path_size)
{
    ESP_RETURN_ON_ERROR(validate_trash_id(trash_id), TAG, "非法回收站 ID");
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (!s_card) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    char data_rel[FRIDGE_TFCARD_MAX_PATH + 1], meta_rel[FRIDGE_TFCARD_MAX_PATH + 1];
    snprintf(data_rel, sizeof(data_rel), "%s/%s.data", FRIDGE_TFCARD_TRASH_DIR, trash_id);
    snprintf(meta_rel, sizeof(meta_rel), "%s/%s.meta", FRIDGE_TFCARD_TRASH_DIR, trash_id);
    char data_abs[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    char meta_abs[sizeof(data_abs)];
    make_absolute(data_rel, data_abs, sizeof(data_abs));
    make_absolute(meta_rel, meta_abs, sizeof(meta_abs));
    FILE *meta = fopen(meta_abs, "rb");
    if (!meta) { esp_err_t err = errno_to_esp(); unlock_storage(); return err; }
    char target_rel[FRIDGE_TFCARD_MAX_PATH + 1];
    size_t n = fread(target_rel, 1, sizeof(target_rel) - 1, meta);
    target_rel[n] = '\0';
    fclose(meta);
    esp_err_t err = require_mutable_path(target_rel);
    char target_abs[sizeof(data_abs)];
    if (err == ESP_OK) err = make_absolute(target_rel, target_abs, sizeof(target_abs));
    struct stat st;
    if (err == ESP_OK && path_exists(target_abs, &st)) {
        if (!overwrite) err = ESP_ERR_INVALID_STATE;
        else err = S_ISDIR(st.st_mode) ? remove_tree_locked(target_abs) : (unlink(target_abs) == 0 ? ESP_OK : errno_to_esp());
    }
    if (err == ESP_OK && rename(data_abs, target_abs) != 0) err = errno_to_esp();
    if (err == ESP_OK) {
        unlink(meta_abs);
        if (restored_path && restored_path_size) strlcpy(restored_path, target_rel, restored_path_size);
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_empty_trash(void)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (!s_card) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    make_absolute(FRIDGE_TFCARD_TRASH_DIR, absolute, sizeof(absolute));
    DIR *dir = opendir(absolute);
    if (!dir) { esp_err_t err = errno_to_esp(); unlock_storage(); return err; }
    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while (result == ESP_OK && (entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[sizeof(absolute)];
        int n = snprintf(child, sizeof(child), "%s/%s", absolute, entry->d_name);
        result = n > 0 && (size_t)n < sizeof(child) ? remove_tree_locked(child) : ESP_ERR_INVALID_SIZE;
    }
    closedir(dir);
    unlock_storage();
    return result;
}

esp_err_t fridge_tfcard_text_read(const char *relative_path, char *buffer, size_t buffer_size,
                                  size_t *out_size)
{
    if (!buffer || buffer_size == 0 || !out_size) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_relative_path(relative_path, false), TAG, "非法 TF 路径");
    if (diagnostic_path(relative_path)) return ESP_ERR_NOT_ALLOWED;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    make_absolute(relative_path, absolute, sizeof(absolute));
    struct stat st;
    esp_err_t err = stat(absolute, &st) == 0 ? ESP_OK : errno_to_esp();
    if (err == ESP_OK && ((uint64_t)st.st_size > FRIDGE_TFCARD_TEXT_MAX || (size_t)st.st_size + 1 > buffer_size)) err = ESP_ERR_INVALID_SIZE;
    FILE *file = err == ESP_OK ? fopen(absolute, "rb") : NULL;
    if (err == ESP_OK && !file) err = errno_to_esp();
    if (err == ESP_OK) {
        log_internal_dma_heap("text_read_before");
        *out_size = fread(buffer, 1, st.st_size, file);
        buffer[*out_size] = '\0';
        if (*out_size != (size_t)st.st_size) err = ESP_FAIL;
        log_internal_dma_heap("text_read_after");
    }
    if (file) fclose(file);
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_text_read_range(const char *relative_path, size_t offset,
                                       char *buffer, size_t buffer_size,
                                       size_t *out_size, size_t *out_total_size)
{
    if (!buffer || buffer_size == 0 || !out_size || !out_total_size) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_relative_path(relative_path, false), TAG, "非法 TF 路径");
    if (diagnostic_path(relative_path)) return ESP_ERR_NOT_ALLOWED;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");

    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    make_absolute(relative_path, absolute, sizeof(absolute));
    struct stat st;
    esp_err_t err = stat(absolute, &st) == 0 ? ESP_OK : errno_to_esp();
    if (err == ESP_OK && ((uint64_t)st.st_size > FRIDGE_TFCARD_TEXT_MAX || offset > (size_t)st.st_size)) {
        err = ESP_ERR_INVALID_SIZE;
    }

    FILE *file = err == ESP_OK ? fopen(absolute, "rb") : NULL;
    if (err == ESP_OK && !file) err = errno_to_esp();
    if (err == ESP_OK && offset > 0 && fseek(file, (long)offset, SEEK_SET) != 0) err = errno_to_esp();
    if (err == ESP_OK) {
        const size_t remaining = (size_t)st.st_size - offset;
        const size_t wanted = remaining < (buffer_size - 1) ? remaining : (buffer_size - 1);
        log_internal_dma_heap("text_read_range_before");
        *out_size = fread(buffer, 1, wanted, file);
        buffer[*out_size] = '\0';
        *out_total_size = (size_t)st.st_size;
        if (*out_size != wanted) err = ESP_FAIL;
        log_internal_dma_heap("text_read_range_after");
    }
    if (file) fclose(file);
    unlock_storage();
    return err;
}

static esp_err_t text_write_internal(const char *relative_path, const char *text,
                                    size_t text_size, bool overwrite, bool keep_backup)
{
    if (!text || text_size > FRIDGE_TFCARD_TEXT_MAX) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_card ? require_mutable_path(relative_path) : ESP_ERR_INVALID_STATE;
    char target[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2], temporary[sizeof(target) + 6];
    if (err == ESP_OK) err = make_absolute(relative_path, target, sizeof(target));
    if (err == ESP_OK && path_exists(target, NULL) && !overwrite) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && snprintf(temporary, sizeof(temporary), "%s.part", target) >= (int)sizeof(temporary)) err = ESP_ERR_INVALID_SIZE;
    FILE *file = err == ESP_OK ? fopen(temporary, "wb") : NULL;
    if (err == ESP_OK && !file) err = errno_to_esp();
    if (err == ESP_OK && fwrite(text, 1, text_size, file) != text_size) err = ESP_FAIL;
    if (file) { if (fflush(file) != 0 || fsync(fileno(file)) != 0) err = ESP_FAIL; fclose(file); }
    if (err == ESP_OK) err = replace_file_locked(temporary, target, overwrite, keep_backup);
    if (err != ESP_OK) unlink(temporary);
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_text_write(const char *relative_path, const char *text, size_t text_size,
                                  bool overwrite)
{
    return text_write_internal(relative_path, text, text_size, overwrite, false);
}

esp_err_t fridge_tfcard_text_write_recoverable(const char *relative_path, const char *text,
                                              size_t text_size, bool overwrite)
{
    return text_write_internal(relative_path, text, text_size, overwrite, true);
}

// 校验只能由内部诊断/日志接口使用的单文件路径；目录本身不是可写文件目标。
static esp_err_t validate_scoped_file_path(const char *relative_path, const char *directory)
{
    esp_err_t err = validate_relative_path(relative_path, false);
    if (err != ESP_OK) return err;
    const size_t directory_length = strlen(directory);
    if (!path_in_directory(relative_path, directory) ||
        relative_path[directory_length] != '/' || relative_path[directory_length + 1] == '\0') {
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

// 在锁内以追加模式写入日志文件。每次调用关闭文件，确保即使后台任务随后被暂停，
// stdio 缓冲也已刷入 FATFS；sync=true 时再额外执行 fsync，降低掉电丢失窗口。
esp_err_t fridge_tfcard_append_log(const char *relative_path, const void *data,
                                   size_t data_size, bool sync)
{
    // data_size 已经是 size_t；此接口不需要再与 64 位文件上限比较，避免在
    // 32 位 P4 构建中产生“比较恒为假”的无效编译警告。
    if (!data && data_size != 0) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_scoped_file_path(relative_path, FRIDGE_TFCARD_LOG_DIR), TAG,
                        "日志路径必须位于 %s", FRIDGE_TFCARD_LOG_DIR);
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");

    esp_err_t err = s_card ? ESP_OK : ESP_ERR_INVALID_STATE;
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    if (err == ESP_OK) err = make_absolute(relative_path, absolute, sizeof(absolute));
    FILE *file = err == ESP_OK ? fopen(absolute, "ab") : NULL;
    if (err == ESP_OK && !file) err = errno_to_esp();
    if (err == ESP_OK && data_size != 0 && fwrite(data, 1, data_size, file) != data_size) err = ESP_FAIL;
    if (file) {
        if (fflush(file) != 0) err = ESP_FAIL;
        if (err == ESP_OK && sync && fsync(fileno(file)) != 0) err = ESP_FAIL;
        if (fclose(file) != 0) err = ESP_FAIL;
    }
    if (err != ESP_OK) set_error("log append failed");
    unlock_storage();
    return err;
}

// 将诊断文本写入临时文件并原子替换目标。诊断目录不经过普通 TF 写接口，避免被 USB/HTTP
// 浏览器意外读到；覆盖时沿用同卷 .bak 回滚策略，成功后删除备份。
esp_err_t fridge_tfcard_write_diagnostic_text(const char *relative_path, const char *text,
                                              size_t text_size, bool overwrite)
{
    if (!text || text_size > FRIDGE_TFCARD_TEXT_MAX) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_scoped_file_path(relative_path, FRIDGE_TFCARD_DIAGNOSTICS_DIR), TAG,
                        "诊断路径必须位于 %s", FRIDGE_TFCARD_DIAGNOSTICS_DIR);
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");

    esp_err_t err = s_card ? ESP_OK : ESP_ERR_INVALID_STATE;
    char target[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    char temporary[sizeof(target) + 6];
    if (err == ESP_OK) err = make_absolute(relative_path, target, sizeof(target));
    if (err == ESP_OK && snprintf(temporary, sizeof(temporary), "%s.part", target) >= (int)sizeof(temporary)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    FILE *file = err == ESP_OK ? fopen(temporary, "wb") : NULL;
    if (err == ESP_OK && !file) err = errno_to_esp();
    if (err == ESP_OK && fwrite(text, 1, text_size, file) != text_size) err = ESP_FAIL;
    if (file) {
        if (fflush(file) != 0 || fsync(fileno(file)) != 0) err = ESP_FAIL;
        if (fclose(file) != 0) err = ESP_FAIL;
    }
    if (err == ESP_OK) err = replace_file_locked(temporary, target, overwrite, false);
    if (err != ESP_OK) {
        unlink(temporary);
        set_error("diagnostic text write failed");
    }
    unlock_storage();
    return err;
}

// 从只读回调分块复制诊断二进制数据，固定 4 KiB 缓冲避免把 Flash coredump 整体搬入内存。
// 回调必须按 offset 返回本次实际字节数，不能访问 TF 卡，避免在全局 TF 锁内产生递归死锁。
esp_err_t fridge_tfcard_write_diagnostic_stream(const char *relative_path, uint64_t total_size,
                                                fridge_tfcard_diagnostic_read_cb_t read_cb,
                                                void *context, bool overwrite)
{
    if (!read_cb || total_size > FRIDGE_TFCARD_FILE_MAX) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(validate_scoped_file_path(relative_path, FRIDGE_TFCARD_DIAGNOSTICS_DIR), TAG,
                        "诊断路径必须位于 %s", FRIDGE_TFCARD_DIAGNOSTICS_DIR);
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");

    esp_err_t err = s_card ? ESP_OK : ESP_ERR_INVALID_STATE;
    char target[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    char temporary[sizeof(target) + 6];
    uint8_t *buffer = NULL;
    FILE *file = NULL;
    if (err == ESP_OK) err = make_absolute(relative_path, target, sizeof(target));
    if (err == ESP_OK && snprintf(temporary, sizeof(temporary), "%s.part", target) >= (int)sizeof(temporary)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        file = fopen(temporary, "wb");
        if (!file) err = errno_to_esp();
    }
    if (err == ESP_OK) {
        buffer = malloc(4U * 1024U);
        if (!buffer) err = ESP_ERR_NO_MEM;
    }
    uint64_t offset = 0;
    while (err == ESP_OK && offset < total_size) {
        const size_t capacity = (size_t)((total_size - offset) > 4U * 1024U ? 4U * 1024U : (total_size - offset));
        size_t received = 0;
        err = read_cb(context, offset, buffer, capacity, &received);
        if (err != ESP_OK) break;
        if (received == 0 || received > capacity || fwrite(buffer, 1, received, file) != received) {
            err = received == 0 || received > capacity ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
            break;
        }
        offset += received;
    }
    if (err == ESP_OK && offset != total_size) err = ESP_ERR_INVALID_SIZE;
    if (file) {
        if (fflush(file) != 0 || fsync(fileno(file)) != 0) err = ESP_FAIL;
        if (fclose(file) != 0) err = ESP_FAIL;
        file = NULL;
    }
    free(buffer);
    if (err == ESP_OK) err = replace_file_locked(temporary, target, overwrite, false);
    if (err != ESP_OK) {
        if (file) fclose(file);
        unlink(temporary);
        set_error("diagnostic stream write failed");
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_write_diagnostic_export(const char *file_name, const char *text,
                                                size_t text_size, char *out_path,
                                                size_t out_path_size)
{
    if (!file_name || !file_name[0] || strchr(file_name, '/') || strchr(file_name, '\\') ||
        !text || text_size > FRIDGE_TFCARD_TEXT_MAX || !out_path || out_path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char relative[FRIDGE_TFCARD_MAX_PATH + 1];
    int n = snprintf(relative, sizeof(relative), "%s/%s", FRIDGE_TFCARD_EXPORT_DIR, file_name);
    if (n <= 0 || (size_t)n >= sizeof(relative)) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(validate_relative_path(relative, false), TAG, "非法诊断导出文件名");
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_card ? ESP_OK : ESP_ERR_INVALID_STATE;
    char target[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2], temporary[sizeof(target) + 6];
    if (err == ESP_OK) err = make_absolute(relative, target, sizeof(target));
    if (err == ESP_OK && snprintf(temporary, sizeof(temporary), "%s.part", target) >= (int)sizeof(temporary)) err = ESP_ERR_INVALID_SIZE;
    FILE *file = err == ESP_OK ? fopen(temporary, "wb") : NULL;
    if (err == ESP_OK && !file) err = errno_to_esp();
    if (err == ESP_OK && fwrite(text, 1, text_size, file) != text_size) err = ESP_FAIL;
    if (file) { if (fflush(file) != 0 || fsync(fileno(file)) != 0) err = ESP_FAIL; fclose(file); }
    if (err == ESP_OK) err = replace_file_locked(temporary, target, true, false);
    if (err != ESP_OK) unlink(temporary);
    else strlcpy(out_path, relative, out_path_size);
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_transfer_begin(const char *relative_path,
                                      fridge_tfcard_transfer_direction_t direction,
                                      uint64_t total_size, bool overwrite,
                                      fridge_tfcard_transfer_status_t *out)
{
    if (!out || (direction != FRIDGE_TFCARD_TRANSFER_READ && direction != FRIDGE_TFCARD_TRANSFER_WRITE) ||
        total_size > FRIDGE_TFCARD_FILE_MAX) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = s_card ? validate_relative_path(relative_path, false) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && diagnostic_path(relative_path)) err = ESP_ERR_NOT_ALLOWED;
    if (err == ESP_OK && direction == FRIDGE_TFCARD_TRANSFER_WRITE) err = require_mutable_path(relative_path);
    if (err == ESP_OK && s_transfer.active) err = ESP_ERR_INVALID_STATE;
    char absolute[sizeof(FRIDGE_TFCARD_MOUNT_POINT) + FRIDGE_TFCARD_MAX_PATH + 2];
    if (err == ESP_OK) err = make_absolute(relative_path, absolute, sizeof(absolute));
    struct stat st;
    if (err == ESP_OK && direction == FRIDGE_TFCARD_TRANSFER_READ) {
        if (stat(absolute, &st) != 0 || S_ISDIR(st.st_mode)) err = ESP_ERR_NOT_FOUND;
        else total_size = st.st_size;
    } else if (err == ESP_OK && path_exists(absolute, NULL) && !overwrite) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        memset(&s_transfer, 0, sizeof(s_transfer));
        s_transfer.active = true;
        s_transfer.id = s_next_transfer_id++;
        if (s_transfer.id == 0) s_transfer.id = s_next_transfer_id++;
        s_transfer.direction = direction;
        s_transfer.total_size = total_size;
        s_transfer.overwrite = overwrite;
        strlcpy(s_transfer.path, relative_path, sizeof(s_transfer.path));
        strlcpy(s_transfer.absolute, absolute, sizeof(s_transfer.absolute));
        const char *open_path = absolute;
        const char *mode = "rb";
        if (direction == FRIDGE_TFCARD_TRANSFER_WRITE) {
            snprintf(s_transfer.temporary, sizeof(s_transfer.temporary), "%s.part", absolute);
            open_path = s_transfer.temporary;
            mode = "wb";
        }
        s_transfer.file = fopen(open_path, mode);
        if (!s_transfer.file) { err = errno_to_esp(); transfer_close_locked(false); }
    }
    if (err == ESP_OK) {
        out->active = true; out->session_id = s_transfer.id; out->direction = direction;
        out->total_size = total_size; out->offset = 0; strlcpy(out->path, relative_path, sizeof(out->path));
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_transfer_status(fridge_tfcard_transfer_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    memset(out, 0, sizeof(*out));
    if (s_transfer.active) {
        out->active = true; out->session_id = s_transfer.id; out->direction = s_transfer.direction;
        out->total_size = s_transfer.total_size; out->offset = s_transfer.offset;
        strlcpy(out->path, s_transfer.path, sizeof(out->path));
    }
    unlock_storage();
    return ESP_OK;
}

esp_err_t fridge_tfcard_transfer_write(uint32_t session_id, uint64_t offset,
                                      const uint8_t *data, size_t length, uint32_t crc32)
{
    if (!data || length == 0 || length > FRIDGE_TFCARD_TRANSFER_CHUNK) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = !s_transfer.active || s_transfer.id != session_id || s_transfer.direction != FRIDGE_TFCARD_TRANSFER_WRITE
        ? ESP_ERR_INVALID_STATE : ESP_OK;
    // ACK 丢失时上层会重发最后一块；相同 offset/长度/CRC 视为幂等成功，不重复写盘。
    if (err == ESP_OK && offset != s_transfer.offset) {
        if (offset == s_transfer.last_offset && length == s_transfer.last_length &&
            crc32 == s_transfer.last_crc32 && crc32_bytes(data, length) == crc32) {
            unlock_storage();
            return ESP_OK;
        }
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK && (offset + length > s_transfer.total_size || crc32_bytes(data, length) != crc32)) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK && fwrite(data, 1, length, s_transfer.file) != length) err = ESP_FAIL;
    if (err == ESP_OK) {
        s_transfer.last_offset = offset;
        s_transfer.last_length = length;
        s_transfer.last_crc32 = crc32;
        s_transfer.offset += length;
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_transfer_read(uint32_t session_id, uint64_t offset,
                                     uint8_t *data, size_t capacity, size_t *out_length,
                                     uint32_t *out_crc32)
{
    if (!data || capacity == 0 || capacity > FRIDGE_TFCARD_TRANSFER_CHUNK || !out_length || !out_crc32) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = !s_transfer.active || s_transfer.id != session_id || s_transfer.direction != FRIDGE_TFCARD_TRANSFER_READ
        ? ESP_ERR_INVALID_STATE : ESP_OK;
    // HTTP Range 下载允许在同一只读会话内安全跳转；写会话仍必须严格顺序推进。
    // 先验证 off_t 能无损表示目标偏移，避免 32-bit libc 在大文件上静默回绕。
    if (err == ESP_OK && offset != s_transfer.offset) {
        const off_t target = (off_t)offset;
        if (offset > s_transfer.total_size || target < 0 || (uint64_t)target != offset ||
            fseeko(s_transfer.file, target, SEEK_SET) != 0) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            s_transfer.offset = offset;
        }
    }
    if (err == ESP_OK) {
        size_t remaining = (size_t)((s_transfer.total_size - s_transfer.offset) > capacity ? capacity : (s_transfer.total_size - s_transfer.offset));
        *out_length = fread(data, 1, remaining, s_transfer.file);
        if (*out_length != remaining) err = ESP_FAIL;
        else { *out_crc32 = crc32_bytes(data, *out_length); s_transfer.offset += *out_length; }
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_transfer_end(uint32_t session_id)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    esp_err_t err = !s_transfer.active || s_transfer.id != session_id ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK && s_transfer.offset != s_transfer.total_size) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && s_transfer.direction == FRIDGE_TFCARD_TRANSFER_WRITE) {
        if (fflush(s_transfer.file) != 0 || fsync(fileno(s_transfer.file)) != 0) err = ESP_FAIL;
        fclose(s_transfer.file); s_transfer.file = NULL;
        if (err == ESP_OK) err = replace_file_locked(s_transfer.temporary, s_transfer.absolute,
                                                     s_transfer.overwrite, false);
    }
    // 完成尝试后关闭会话；失败时保留 .part 供诊断/恢复，不让已关闭 FILE* 留在活动会话中。
    if (s_transfer.direction == FRIDGE_TFCARD_TRANSFER_READ || s_transfer.file == NULL) {
        transfer_close_locked(false);
    } else if (err == ESP_OK) {
        transfer_close_locked(false);
    }
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_transfer_abort(uint32_t session_id)
{
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (!s_transfer.active || s_transfer.id != session_id) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    transfer_close_locked(true);
    unlock_storage();
    return ESP_OK;
}

esp_err_t fridge_tfcard_selftest(char *detail, size_t detail_size)
{
    if (detail && detail_size) detail[0] = '\0';
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (!s_card) { if (detail) snprintf(detail, detail_size, "TF 卡未挂载：%s", s_last_error); unlock_storage(); return ESP_ERR_INVALID_STATE; }
    const char *path = FRIDGE_TFCARD_MOUNT_POINT "/.fridge_spirit/selftest.tmp";
    char payload[96], readback[96] = "";
    snprintf(payload, sizeof(payload), "fridge_spirit tfcard selftest tick=%lu", (unsigned long)xTaskGetTickCount());
    FILE *file = fopen(path, "wb");
    esp_err_t err = file ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK && fwrite(payload, 1, strlen(payload), file) != strlen(payload)) err = ESP_FAIL;
    if (file) { fflush(file); fsync(fileno(file)); fclose(file); }
    file = err == ESP_OK ? fopen(path, "rb") : NULL;
    if (err == ESP_OK && !file) err = ESP_FAIL;
    if (file) { size_t n = fread(readback, 1, sizeof(readback) - 1, file); readback[n] = '\0'; fclose(file); }
    if (err == ESP_OK && strcmp(payload, readback) != 0) err = ESP_FAIL;
    unlink(path);
    if (detail) snprintf(detail, detail_size, err == ESP_OK ? "TF 写入-同步-读回校验通过" : "TF 写入-读回校验失败");
    if (err != ESP_OK) set_error("selftest failed");
    unlock_storage();
    return err;
}

esp_err_t fridge_tfcard_benchmark(size_t test_bytes, fridge_tfcard_benchmark_result_t *out)
{
    if (!out || test_bytes == 0 || test_bytes > 16U * 1024U * 1024U) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(lock_storage(), TAG, "TF 互斥锁超时");
    if (!s_card || s_transfer.active) { unlock_storage(); return ESP_ERR_INVALID_STATE; }
    uint8_t *buffer = malloc(32 * 1024);
    if (!buffer) { unlock_storage(); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < 32 * 1024; ++i) buffer[i] = (uint8_t)i;
    const char *path = FRIDGE_TFCARD_MOUNT_POINT "/.fridge_spirit/benchmark.tmp";
    FILE *file = fopen(path, "wb");
    esp_err_t err = file ? ESP_OK : ESP_FAIL;
    int64_t start = esp_timer_get_time();
    size_t done = 0;
    while (err == ESP_OK && done < test_bytes) {
        size_t chunk = test_bytes - done > 32 * 1024 ? 32 * 1024 : test_bytes - done;
        if (fwrite(buffer, 1, chunk, file) != chunk) err = ESP_FAIL;
        done += chunk;
    }
    if (file) { fflush(file); fsync(fileno(file)); fclose(file); }
    int64_t write_us = esp_timer_get_time() - start;
    file = err == ESP_OK ? fopen(path, "rb") : NULL;
    if (err == ESP_OK && !file) err = ESP_FAIL;
    start = esp_timer_get_time(); done = 0;
    while (err == ESP_OK && done < test_bytes) {
        size_t chunk = test_bytes - done > 32 * 1024 ? 32 * 1024 : test_bytes - done;
        if (fread(buffer, 1, chunk, file) != chunk) err = ESP_FAIL;
        done += chunk;
    }
    if (file) fclose(file);
    int64_t read_us = esp_timer_get_time() - start;
    unlink(path); free(buffer);
    if (err == ESP_OK) {
        memset(out, 0, sizeof(*out)); out->bytes = test_bytes;
        out->write_ms = (uint32_t)((write_us + 999) / 1000); out->read_ms = (uint32_t)((read_us + 999) / 1000);
        out->write_kib_s = write_us > 0 ? (uint32_t)(((uint64_t)test_bytes * 1000000ULL) / 1024 / write_us) : 0;
        out->read_kib_s = read_us > 0 ? (uint32_t)(((uint64_t)test_bytes * 1000000ULL) / 1024 / read_us) : 0;
    }
    unlock_storage();
    return err;
}
