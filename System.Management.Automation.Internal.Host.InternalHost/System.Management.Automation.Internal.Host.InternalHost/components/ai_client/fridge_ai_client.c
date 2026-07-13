// 冰箱小精灵 AI API 客户端组件。
// 负责保存开发阶段的 OpenAI-compatible API 配置，并通过 HTTPS 发起文本聊天测试。
// 硬件注意：本组件不控制 GPIO；测试请求需要 Wi-Fi 与 TLS，Wi-Fi 发射峰值仍要求稳定 USB/5V 供电。

#include "fridge_ai_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_network.h"
#include "nvs.h"

#define AI_NVS_NAMESPACE "fridge_ai"
#define AI_NVS_KEY_BASE_URL "base_url"
#define AI_NVS_KEY_API_KEY "api_key"
#define AI_NVS_KEY_MODEL "model"
#define AI_NVS_KEY_SYSTEM "system"
#define AI_NVS_KEY_TIMEOUT "timeout_ms"
#define AI_NVS_KEY_FORMAT "api_format"
#define AI_NVS_KEY_ACTIVE_PROFILE "active"
#define AI_NVS_KEY_UPDATED_MS "updated_ms"
#define AI_HTTP_RESPONSE_CAP 12288
#define AI_TEST_MAX_TOKENS 128
#define AI_ASSISTANT_MAX_TOKENS 1600
#define AI_KITCHEN_MAX_TOKENS 1600
#define AI_IMAGE_MAX_TOKENS 700
#define AI_COMPLETIONS_PATH "/chat/completions"
// Google Gemini 原生端点后缀：完整 URL 形如 {base}/models/{model}:generateContent。
#define AI_GOOGLE_MODELS_SEGMENT "/models/"
#define AI_GOOGLE_GENERATE_SUFFIX ":generateContent"

static const char *TAG = "fridge_ai";

static bool s_initialized;
static char s_last_error[FRIDGE_AI_MAX_ERROR_LEN + 1];

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} ai_http_buffer_t;

static void *ai_large_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *ai_large_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = ai_large_alloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static void set_last_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
}

static uint32_t clamp_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms < 5000) {
        return 5000;
    }
    if (timeout_ms > 60000) {
        return 60000;
    }
    return timeout_ms;
}

static int64_t ai_config_now_ms(void)
{
    time_t now = time(NULL);
    if (now >= 1735689600LL) {
        return (int64_t)now * 1000;
    }
    return esp_timer_get_time() / 1000;
}

static bool starts_with(const char *text, const char *prefix)
{
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

static size_t utf8_sequence_len(unsigned char ch)
{
    if (ch < 0x80) {
        return 1;
    }
    if (ch >= 0xC2 && ch <= 0xDF) {
        return 2;
    }
    if (ch >= 0xE0 && ch <= 0xEF) {
        return 3;
    }
    if (ch >= 0xF0 && ch <= 0xF4) {
        return 4;
    }
    return 0;
}

static bool utf8_sequence_valid(const unsigned char *p, size_t len)
{
    if (!p || len == 0) {
        return false;
    }
    if (len == 1) {
        return p[0] < 0x80;
    }
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return false;
        }
    }
    if (len == 3) {
        if (p[0] == 0xE0 && p[1] < 0xA0) {
            return false;
        }
        if (p[0] == 0xED && p[1] >= 0xA0) {
            return false;
        }
    }
    if (len == 4) {
        if (p[0] == 0xF0 && p[1] < 0x90) {
            return false;
        }
        if (p[0] == 0xF4 && p[1] > 0x8F) {
            return false;
        }
    }
    return true;
}

static size_t json_escaped_len(const char *text)
{
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;) {
        switch (*p) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
            len += 2;
            p++;
            break;
        default:
            if (*p < 0x20) {
                len += 6;
                p++;
            } else {
                size_t seq_len = utf8_sequence_len(*p);
                if (seq_len > 0 && utf8_sequence_valid(p, seq_len)) {
                    len += seq_len;
                    p += seq_len;
                } else {
                    // 任何非法 UTF-8 或被 strlcpy 截断的半个中文字符都替换成 '?'，避免 AI 服务拒绝 JSON。
                    len += 1;
                    p++;
                }
            }
            break;
        }
    }
    return len;
}

static char *json_escape_alloc(const char *text)
{
    size_t len = json_escaped_len(text);
    char *out = calloc(1, len + 1);
    if (!out) {
        return NULL;
    }

    char *w = out;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;) {
        switch (*p) {
        case '"':
            *w++ = '\\';
            *w++ = '"';
            p++;
            break;
        case '\\':
            *w++ = '\\';
            *w++ = '\\';
            p++;
            break;
        case '\n':
            *w++ = '\\';
            *w++ = 'n';
            p++;
            break;
        case '\r':
            *w++ = '\\';
            *w++ = 'r';
            p++;
            break;
        case '\t':
            *w++ = '\\';
            *w++ = 't';
            p++;
            break;
        default:
            if (*p < 0x20) {
                snprintf(w, 7, "\\u%04x", *p);
                w += 6;
                p++;
            } else {
                size_t seq_len = utf8_sequence_len(*p);
                if (seq_len > 0 && utf8_sequence_valid(p, seq_len)) {
                    for (size_t i = 0; i < seq_len; i++) {
                        *w++ = (char)*p++;
                    }
                } else {
                    *w++ = '?';
                    p++;
                }
            }
            break;
        }
    }
    *w = '\0';
    return out;
}

static char *utf8_clean_alloc(const char *text)
{
    size_t len = json_escaped_len(text);
    char *out = calloc(1, len + 1);
    if (!out) {
        return NULL;
    }

    char *w = out;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p;) {
        if (*p < 0x20) {
            *w++ = ' ';
            p++;
            continue;
        }
        size_t seq_len = utf8_sequence_len(*p);
        if (seq_len > 0 && utf8_sequence_valid(p, seq_len)) {
            memcpy(w, p, seq_len);
            w += seq_len;
            p += seq_len;
        } else {
            // NVS 固定缓冲按字节截断中文时可能留下半个 UTF-8 字符；同步上行前先替换为 '?'。
            *w++ = '?';
            p++;
        }
    }
    *w = '\0';
    return out;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool append_utf8(char *out, size_t out_size, size_t *written, uint32_t codepoint)
{
    if (!out || !written || *written >= out_size) {
        return false;
    }

    if (codepoint < 0x20) {
        if (*written + 1 >= out_size) {
            return false;
        }
        // AI 服务偶尔会在 JSON 字符串里返回 \u0000 等控制字符；
        // 这些字符进入 C 字符串会提前截断回复，页面解析只能看到 JSON 开头。
        out[(*written)++] = ' ';
        return true;
    }

    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7F) {
        bytes[count++] = (unsigned char)codepoint;
    } else if (codepoint <= 0x7FF) {
        bytes[count++] = (unsigned char)(0xC0 | (codepoint >> 6));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        bytes[count++] = (unsigned char)(0xE0 | (codepoint >> 12));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        bytes[count++] = (unsigned char)(0xF0 | (codepoint >> 18));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    }

    if (*written + count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        out[(*written)++] = (char)bytes[i];
    }
    return true;
}

static const char *find_json_key(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) {
        return NULL;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return NULL;
    }
    pos++;
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    return pos;
}

static bool json_get_string_value(const char *json, const char *key, char *out, size_t out_size)
{
    const char *pos = find_json_key(json, key);
    if (!pos || *pos != '"' || !out || out_size == 0) {
        return false;
    }
    pos++;

    size_t written = 0;
    while (*pos && *pos != '"') {
        unsigned char ch = (unsigned char)*pos++;
        if (ch == '\\' && *pos) {
            char esc = *pos++;
            if (esc == 'n') {
                ch = '\n';
            } else if (esc == 'r') {
                ch = '\r';
            } else if (esc == 't') {
                ch = '\t';
            } else if (esc == 'u') {
                uint32_t codepoint = 0;
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    int value = hex_value(*pos++);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | (uint32_t)value;
                }
                if (valid) {
                    append_utf8(out, out_size, &written, codepoint);
                    continue;
                }
                ch = '?';
            } else {
                ch = (unsigned char)esc;
            }
        }
        if (written + 1 < out_size) {
            out[written++] = (char)ch;
        }
    }
    out[written] = '\0';
    return true;
}

static bool json_get_string_value_after(const char *json, const char *anchor, const char *key, char *out, size_t out_size)
{
    if (!json || !anchor || !key || !out || out_size == 0) {
        return false;
    }
    const char *start = strstr(json, anchor);
    return start ? json_get_string_value(start, key, out, out_size) : false;
}

static void compact_json_preview(const char *json, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!json) {
        return;
    }

    size_t written = 0;
    bool last_space = false;
    for (const unsigned char *p = (const unsigned char *)json; *p && written + 1 < out_size; p++) {
        if (isspace(*p)) {
            if (!last_space && written > 0) {
                out[written++] = ' ';
                last_space = true;
            }
            continue;
        }
        out[written++] = (char)*p;
        last_space = false;
    }
    out[written] = '\0';
}

static esp_err_t open_ai_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NVS handle is NULL");
    return nvs_open(AI_NVS_NAMESPACE, mode, handle);
}

static void read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }

    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err != ESP_OK) {
        strlcpy(out, fallback ? fallback : "", out_size);
    }
}

static void read_nvs_text(nvs_handle_t handle, const char *key, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }

    // 长系统提示词使用 NVS blob 保存，避免 nvs_set_str 的单页字符串长度限制。
    // 旧固件已经保存过的短字符串仍可回读，保证升级后不丢原配置。
    size_t len = out_size;
    esp_err_t err = nvs_get_blob(handle, key, out, &len);
    if (err == ESP_OK && len > 0) {
        out[out_size - 1] = '\0';
        return;
    }

    read_nvs_string(handle, key, out, out_size, fallback);
}

static uint8_t clamp_profile_id(uint8_t profile_id)
{
    return profile_id < FRIDGE_AI_MAX_PROFILES ? profile_id : 0;
}

static void make_profile_key(uint8_t profile_id, const char *field, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    profile_id = clamp_profile_id(profile_id);

    // 0 号配置沿用旧 NVS key，保证已保存的开发 Key 不会因为升级丢失。
    if (profile_id == 0) {
        if (strcmp(field, "base") == 0) {
            strlcpy(out, AI_NVS_KEY_BASE_URL, out_size);
        } else if (strcmp(field, "key") == 0) {
            strlcpy(out, AI_NVS_KEY_API_KEY, out_size);
        } else if (strcmp(field, "model") == 0) {
            strlcpy(out, AI_NVS_KEY_MODEL, out_size);
        } else if (strcmp(field, "system") == 0) {
            strlcpy(out, AI_NVS_KEY_SYSTEM, out_size);
        } else if (strcmp(field, "timeout") == 0) {
            strlcpy(out, AI_NVS_KEY_TIMEOUT, out_size);
        } else if (strcmp(field, "format") == 0) {
            strlcpy(out, AI_NVS_KEY_FORMAT, out_size);
        } else if (strcmp(field, "updated") == 0) {
            strlcpy(out, AI_NVS_KEY_UPDATED_MS, out_size);
        } else {
            strlcpy(out, "name0", out_size);
        }
        return;
    }

    char prefix = 'n';
    if (strcmp(field, "base") == 0) {
        prefix = 'b';
    } else if (strcmp(field, "key") == 0) {
        prefix = 'k';
    } else if (strcmp(field, "model") == 0) {
        prefix = 'm';
    } else if (strcmp(field, "system") == 0) {
        prefix = 's';
    } else if (strcmp(field, "timeout") == 0) {
        prefix = 't';
    } else if (strcmp(field, "format") == 0) {
        prefix = 'f';
    } else if (strcmp(field, "updated") == 0) {
        prefix = 'u';
    }
    snprintf(out, out_size, "%c%u", prefix, (unsigned)profile_id);
}

static int64_t load_profile_updated_ms(uint8_t profile_id)
{
    nvs_handle_t handle;
    if (open_ai_nvs(NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }
    char key[16];
    int64_t updated_ms = 0;
    make_profile_key(profile_id, "updated", key, sizeof(key));
    (void)nvs_get_i64(handle, key, &updated_ms);
    nvs_close(handle);
    return updated_ms > 0 ? updated_ms : 0;
}

static uint8_t get_active_profile_id(void)
{
    nvs_handle_t handle;
    uint8_t active = 0;
    if (open_ai_nvs(NVS_READONLY, &handle) == ESP_OK) {
        uint8_t stored = 0;
        if (nvs_get_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, &stored) == ESP_OK) {
            active = clamp_profile_id(stored);
        }
        nvs_close(handle);
    }
    return active;
}

static bool profile_exists_in_handle(nvs_handle_t handle, uint8_t profile_id)
{
    if (profile_id == 0) {
        return true;
    }

    char key[16];
    size_t len = 0;
    make_profile_key(profile_id, "name", key, sizeof(key));
    if (nvs_get_str(handle, key, NULL, &len) == ESP_OK) {
        return true;
    }

    len = 0;
    make_profile_key(profile_id, "base", key, sizeof(key));
    if (nvs_get_str(handle, key, NULL, &len) == ESP_OK) {
        return true;
    }

    len = 0;
    make_profile_key(profile_id, "key", key, sizeof(key));
    return nvs_get_str(handle, key, NULL, &len) == ESP_OK;
}

static void default_profile_name(uint8_t profile_id, char *out, size_t out_size)
{
    if (profile_id == 0) {
        strlcpy(out, "默认配置", out_size);
    } else {
        snprintf(out, out_size, "备用配置 %u", (unsigned)profile_id);
    }
}

static void make_key_preview(const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!key || key[0] == '\0') {
        return;
    }

    size_t len = strlen(key);
    if (len <= 8) {
        strlcpy(out, "已保存", out_size);
        return;
    }

    const char *suffix = key + len - 4;
    int prefix_len = starts_with(key, "sk-") ? 3 : 4;
    snprintf(out, out_size, "%.*s...%s", prefix_len, key, suffix);
}

static esp_err_t load_profile_config(uint8_t profile_id, fridge_ai_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    memset(config, 0, sizeof(*config));
    profile_id = clamp_profile_id(profile_id);
    config->profile_id = profile_id;

    nvs_handle_t handle;
    esp_err_t err = open_ai_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        default_profile_name(profile_id, config->profile_name, sizeof(config->profile_name));
        strlcpy(config->model, FRIDGE_AI_DEFAULT_MODEL, sizeof(config->model));
        strlcpy(config->system_prompt, "你是冰箱小精灵的开发测试助手，请用简短中文回答。", sizeof(config->system_prompt));
        config->timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;
        config->api_format = FRIDGE_AI_API_FORMAT_OPENAI;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open AI NVS failed");

    char key[16];
    make_profile_key(profile_id, "name", key, sizeof(key));
    default_profile_name(profile_id, config->profile_name, sizeof(config->profile_name));
    read_nvs_string(handle, key, config->profile_name, sizeof(config->profile_name), config->profile_name);

    make_profile_key(profile_id, "base", key, sizeof(key));
    read_nvs_string(handle, key, config->api_base_url, sizeof(config->api_base_url), "");
    make_profile_key(profile_id, "model", key, sizeof(key));
    read_nvs_string(handle, key, config->model, sizeof(config->model), FRIDGE_AI_DEFAULT_MODEL);
    make_profile_key(profile_id, "system", key, sizeof(key));
    read_nvs_text(handle, key, config->system_prompt, sizeof(config->system_prompt),
                  "你是冰箱小精灵的开发测试助手，请用简短中文回答。");
    size_t key_len = sizeof(config->api_key);
    make_profile_key(profile_id, "key", key, sizeof(key));
    esp_err_t key_err = nvs_get_str(handle, key, config->api_key, &key_len);
    config->update_api_key = (key_err == ESP_OK && config->api_key[0] != '\0');
    uint32_t timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    make_profile_key(profile_id, "timeout", key, sizeof(key));
    if (nvs_get_u32(handle, key, &timeout_ms) != ESP_OK) {
        timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    }
    config->timeout_ms = clamp_timeout_ms(timeout_ms);
    // 读取接口格式；旧固件没有该 key 时缺省 OpenAI，保证升级后旧配置仍按原协议出网。
    uint8_t api_format = FRIDGE_AI_API_FORMAT_OPENAI;
    make_profile_key(profile_id, "format", key, sizeof(key));
    if (nvs_get_u8(handle, key, &api_format) != ESP_OK) {
        api_format = FRIDGE_AI_API_FORMAT_OPENAI;
    }
    config->api_format = (api_format == FRIDGE_AI_API_FORMAT_GOOGLE) ? FRIDGE_AI_API_FORMAT_GOOGLE
                                                                     : FRIDGE_AI_API_FORMAT_OPENAI;
    int64_t updated_ms = 0;
    make_profile_key(profile_id, "updated", key, sizeof(key));
    if (nvs_get_i64(handle, key, &updated_ms) == ESP_OK && updated_ms > 0) {
        config->config_updated_at_ms = updated_ms;
    }
    nvs_close(handle);
    return ESP_OK;
}

static esp_err_t load_full_config(fridge_ai_config_update_t *config)
{
    return load_profile_config(get_active_profile_id(), config);
}

static esp_err_t compose_chat_url(const char *base_url, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(base_url && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid URL args");
    ESP_RETURN_ON_FALSE(starts_with(base_url, "https://") || starts_with(base_url, "http://"),
                        ESP_ERR_INVALID_ARG, TAG, "AI base URL must use http or https");

    strlcpy(out, base_url, out_size);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }

    ESP_RETURN_ON_FALSE(strlen(out) + strlen(AI_COMPLETIONS_PATH) + 1 < out_size,
                        ESP_FAIL, TAG, "AI URL too long");
    strlcat(out, AI_COMPLETIONS_PATH, out_size);
    return ESP_OK;
}

// 构造 Google Gemini 原生端点：{base}/models/{model}:generateContent。
// 与 OpenAI 不同，模型名拼进 URL；base 末尾的 '/' 会被去掉，避免出现 //models。
static esp_err_t compose_google_url(const char *base_url, const char *model, char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(base_url && model && model[0] && out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid Google URL args");
    ESP_RETURN_ON_FALSE(starts_with(base_url, "https://") || starts_with(base_url, "http://"),
                        ESP_ERR_INVALID_ARG, TAG, "AI base URL must use http or https");

    strlcpy(out, base_url, out_size);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }

    size_t need = strlen(out) + strlen(AI_GOOGLE_MODELS_SEGMENT) + strlen(model) + strlen(AI_GOOGLE_GENERATE_SUFFIX) + 1;
    ESP_RETURN_ON_FALSE(need < out_size, ESP_FAIL, TAG, "Google AI URL too long");
    strlcat(out, AI_GOOGLE_MODELS_SEGMENT, out_size);
    strlcat(out, model, out_size);
    strlcat(out, AI_GOOGLE_GENERATE_SUFFIX, out_size);
    return ESP_OK;
}

static esp_err_t ai_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }

    ai_http_buffer_t *buffer = (ai_http_buffer_t *)event->user_data;
    size_t available = buffer->cap > buffer->len ? buffer->cap - buffer->len - 1 : 0;
    size_t copy_len = (size_t)event->data_len;
    if (copy_len > available) {
        copy_len = available;
        buffer->overflow = true;
    }
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, event->data, copy_len);
        buffer->len += copy_len;
        buffer->data[buffer->len] = '\0';
    }
    return ESP_OK;
}

static char *build_chat_request_openai(const fridge_ai_config_update_t *config, const char *message)
{
    char *model = json_escape_alloc(config->model);
    char *system_prompt = json_escape_alloc(config->system_prompt);
    char *user_message = json_escape_alloc(message);
    if (!model || !system_prompt || !user_message) {
        free(model);
        free(system_prompt);
        free(user_message);
        return NULL;
    }

    size_t body_len = strlen(model) + strlen(system_prompt) + strlen(user_message) + 256;
    char *body = ai_large_calloc(1, body_len);
    if (body) {
        snprintf(body, body_len,
                 "{\"model\":\"%s\",\"messages\":["
                 "{\"role\":\"system\",\"content\":\"%s\"},"
                 "{\"role\":\"user\",\"content\":\"%s\"}],"
                 "\"temperature\":0.2,\"max_tokens\":%d,\"stream\":false}",
                 model, system_prompt, user_message, AI_TEST_MAX_TOKENS);
    }
    free(model);
    free(system_prompt);
    free(user_message);
    return body;
}

// Google Gemini 文字测试请求体：system_instruction + 单条 user content。
// 模型名已拼进 URL，body 内不带 model 字段；max_tokens 对应 generationConfig.maxOutputTokens。
static char *build_chat_request_google(const fridge_ai_config_update_t *config, const char *message)
{
    char *system_prompt = json_escape_alloc(config->system_prompt);
    char *user_message = json_escape_alloc(message);
    if (!system_prompt || !user_message) {
        free(system_prompt);
        free(user_message);
        return NULL;
    }

    size_t body_len = strlen(system_prompt) + strlen(user_message) + 256;
    char *body = ai_large_calloc(1, body_len);
    if (body) {
        snprintf(body, body_len,
                 "{\"system_instruction\":{\"parts\":[{\"text\":\"%s\"}]},"
                 "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}],"
                 "\"generationConfig\":{\"temperature\":0.2,\"maxOutputTokens\":%d}}",
                 system_prompt, user_message, AI_TEST_MAX_TOKENS);
    }
    free(system_prompt);
    free(user_message);
    return body;
}

// 文字测试请求分发：按配置的接口格式选择 OpenAI 或 Google 请求体。
static char *build_chat_request(const fridge_ai_config_update_t *config, const char *message)
{
    if (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE) {
        return build_chat_request_google(config, message);
    }
    return build_chat_request_openai(config, message);
}

static const char *safe_history_role(const char *role)
{
    return (role && strcmp(role, "assistant") == 0) ? "assistant" : "user";
}

// Google Gemini 历史角色映射：助手在 Gemini 里叫 "model"，其余一律按 "user"。
// 注意：Gemini 要求 contents 以 user 开头、user/model 交替、末条为 user；上游历史裁剪已保证该顺序。
static const char *google_history_role(const char *role)
{
    return (role && strcmp(role, "assistant") == 0) ? "model" : "user";
}

static bool is_kitchen_json_task(const char *task_type)
{
    return task_type &&
           (strcmp(task_type, "recipe_generate") == 0 ||
            strcmp(task_type, "kitchen_recipe") == 0 ||
            strcmp(task_type, "kitchen_nutrition") == 0 ||
            strcmp(task_type, "kitchen_shopping") == 0);
}

// 冰箱小精灵 AI 助手策略提示词：约束 AI 优先依据项目上下文、保守处理库存/识别，并定义工具 JSON 与隐藏记忆指令。
// OpenAI 与 Google 两种格式共用同一段策略，避免两边行为漂移。
static const char *assistant_policy_text(void)
{
    return
        "你现在运行在冰箱小精灵硬件测试模式。必须优先依据下方项目上下文回答；"
        "不要编造库存、日期、数量、识别结果或传感器状态；涉及库存变更、识别候选、过敏、过期和霉变时要保守，并提醒用户确认。"
        "如果用户明确要求定时器、秒表或闹钟，请优先输出一段可执行紧凑 JSON，例如 {\"tool\":\"timer\",\"action\":\"start\",\"duration_seconds\":480,\"label\":\"煮蛋\"}、{\"tool\":\"stopwatch\",\"action\":\"start\"} 或 {\"tool\":\"alarm\",\"action\":\"set\",\"hour\":7,\"minute\":0,\"label\":\"拿牛奶\"}；不要把缺失的时间或时长编出来。"
        "如果用户明确要求切换屏幕页面，只能输出白名单 UI JSON：{\"tool\":\"ui\",\"action\":\"switch_page\",\"page\":\"home|standby|zone|door|recipe|nutrition|shopping|settings|wifi|more|offline|ai|timer|stopwatch|alarm\"}。"
        "不要输出 camera 或 camera_result 页面指令，因为拍照页会启动摄像头预览，必须由用户手动确认进入；不要尝试控制亮度、音量、Wi-Fi 连接、拍照、库存写入、OTA 或 GPIO。"
        "你可以自主决定是否更新长期结构化记忆，但只能在用户明确表达稳定偏好、家庭人数、忌口、过敏、常用工具、长期习惯，或用户明确要求记住/忘记时使用；闲聊、单次菜谱、传感器状态、临时库存、未确认识别结果不要写入长期记忆。"
        "需要写记忆时，在回复最后另起一行输出给固件的隐藏指令：MEMORY_OP:{\"action\":\"append|replace|clear\",\"key\":\"taste|avoid|allergies|family_size|kitchen_tools|recent_summary\",\"value\":\"不超过80字的中文摘要\",\"reason\":\"简短原因\"}。"
        "如果不需要写记忆，不要输出 MEMORY_OP。用户可见回答不要解释这条隐藏指令。";
}

static char *build_assistant_request_openai(const fridge_ai_config_update_t *config, const fridge_ai_assistant_request_t *request)
{
    char *model = json_escape_alloc(config->model);
    char *system_prompt = json_escape_alloc(config->system_prompt);
    char *task_type = json_escape_alloc(request->task_type);
    char *context_json = json_escape_alloc(request->context_json);
    char *user_message = json_escape_alloc(request->message);
    if (!model || !system_prompt || !task_type || !context_json || !user_message) {
        free(model);
        free(system_prompt);
        free(task_type);
        free(context_json);
        free(user_message);
        return NULL;
    }

    size_t history_len = 0;
    size_t history_count = request->history ? request->history_count : 0;
    history_count = history_count > FRIDGE_AI_MAX_CHAT_HISTORY ? FRIDGE_AI_MAX_CHAT_HISTORY : history_count;
    char **history_roles = calloc(history_count ? history_count : 1, sizeof(char *));
    char **history_contents = calloc(history_count ? history_count : 1, sizeof(char *));
    if (!history_roles || !history_contents) {
        free(model);
        free(system_prompt);
        free(task_type);
        free(context_json);
        free(user_message);
        free(history_roles);
        free(history_contents);
        return NULL;
    }
    for (size_t i = 0; i < history_count; i++) {
        history_roles[i] = json_escape_alloc(safe_history_role(request->history[i].role));
        history_contents[i] = json_escape_alloc(request->history[i].content);
        if (!history_roles[i] || !history_contents[i]) {
            for (size_t j = 0; j <= i; j++) {
                free(history_roles[j]);
                free(history_contents[j]);
            }
            free(model);
            free(system_prompt);
            free(task_type);
            free(context_json);
            free(user_message);
            free(history_roles);
            free(history_contents);
            return NULL;
        }
        history_len += strlen(history_roles[i]) + strlen(history_contents[i]) + 48;
    }

    bool kitchen_json_task = is_kitchen_json_task(request->task_type);
    const char *policy_src = kitchen_json_task
                                 ? "你现在只执行厨房页面内容生成任务。必须严格服从用户消息中的 JSON schema；最终回复只能是一个 JSON object。"
                                 : assistant_policy_text();
    int max_tokens = kitchen_json_task ? AI_KITCHEN_MAX_TOKENS : AI_ASSISTANT_MAX_TOKENS;
    char *policy = json_escape_alloc(policy_src);
    if (!policy) {
        for (size_t i = 0; i < history_count; i++) {
            free(history_roles[i]);
            free(history_contents[i]);
        }
        free(model);
        free(system_prompt);
        free(task_type);
        free(context_json);
        free(user_message);
        free(history_roles);
        free(history_contents);
        return NULL;
    }

    size_t body_len = strlen(model) + strlen(system_prompt) + strlen(policy) + strlen(task_type) +
                      strlen(context_json) + strlen(user_message) + history_len + 768;
    char *body = ai_large_calloc(1, body_len);
    if (body) {
        size_t used = 0;
        // 兼容更多 OpenAI-compatible 服务：只保留一个 system 消息。
        // 部分服务在第二轮带 history 时会拒绝多个 system 消息，导致 HTTP 400。
        int written = snprintf(body,
                               body_len,
                               "{\"model\":\"%s\",\"messages\":["
                               "{\"role\":\"system\",\"content\":\"%s\\n%s\\n项目AI任务类型：%s\\n项目上下文JSON：%s\"}",
                               model,
                               system_prompt,
                               policy,
                               task_type,
                               context_json);
        if (written < 0 || (size_t)written >= body_len) {
            free(body);
            body = NULL;
        } else {
            used = (size_t)written;
        }
        for (size_t i = 0; body && i < history_count; i++) {
            written = snprintf(body + used,
                               body_len - used,
                               ",{\"role\":\"%s\",\"content\":\"%s\"}",
                               history_roles[i],
                               history_contents[i]);
            if (written < 0 || (size_t)written >= body_len - used) {
                free(body);
                body = NULL;
                break;
            }
            used += (size_t)written;
        }
        if (body) {
            written = snprintf(body + used,
                               body_len - used,
                               ",{\"role\":\"user\",\"content\":\"%s\"}],"
                               "\"temperature\":0.1,\"max_tokens\":%d,%s\"stream\":false}",
                               user_message,
                               max_tokens,
                               kitchen_json_task ? "\"response_format\":{\"type\":\"json_object\"}," : "");
            if (written < 0 || (size_t)written >= body_len - used) {
                free(body);
                body = NULL;
            }
        }
    }

    for (size_t i = 0; i < history_count; i++) {
        free(history_roles[i]);
        free(history_contents[i]);
    }
    free(history_roles);
    free(history_contents);
    free(model);
    free(system_prompt);
    free(policy);
    free(task_type);
    free(context_json);
    free(user_message);
    return body;
}

// Google Gemini 助手请求体：system_instruction 承载系统提示词+策略+任务类型+上下文，
// contents 依次放历史（user/model 交替）和当前 user 提问。模型名在 URL，不入 body。
static char *build_assistant_request_google(const fridge_ai_config_update_t *config, const fridge_ai_assistant_request_t *request)
{
    char *system_prompt = json_escape_alloc(config->system_prompt);
    bool kitchen_json_task = is_kitchen_json_task(request->task_type);
    const char *policy_src = kitchen_json_task
                                 ? "你现在只执行厨房页面内容生成任务。必须严格服从用户消息中的 JSON schema；最终回复只能是一个 JSON object。"
                                 : assistant_policy_text();
    int max_tokens = kitchen_json_task ? AI_KITCHEN_MAX_TOKENS : AI_ASSISTANT_MAX_TOKENS;
    char *policy = json_escape_alloc(policy_src);
    char *task_type = json_escape_alloc(request->task_type);
    char *context_json = json_escape_alloc(request->context_json);
    char *user_message = json_escape_alloc(request->message);
    if (!system_prompt || !policy || !task_type || !context_json || !user_message) {
        free(system_prompt);
        free(policy);
        free(task_type);
        free(context_json);
        free(user_message);
        return NULL;
    }

    size_t history_len = 0;
    size_t history_count = request->history ? request->history_count : 0;
    history_count = history_count > FRIDGE_AI_MAX_CHAT_HISTORY ? FRIDGE_AI_MAX_CHAT_HISTORY : history_count;
    char **history_roles = calloc(history_count ? history_count : 1, sizeof(char *));
    char **history_contents = calloc(history_count ? history_count : 1, sizeof(char *));
    if (!history_roles || !history_contents) {
        free(system_prompt);
        free(policy);
        free(task_type);
        free(context_json);
        free(user_message);
        free(history_roles);
        free(history_contents);
        return NULL;
    }
    for (size_t i = 0; i < history_count; i++) {
        // 角色用固定字面量（user/model），无需转义；只转义历史文本内容。
        history_roles[i] = json_escape_alloc(google_history_role(request->history[i].role));
        history_contents[i] = json_escape_alloc(request->history[i].content);
        if (!history_roles[i] || !history_contents[i]) {
            for (size_t j = 0; j <= i; j++) {
                free(history_roles[j]);
                free(history_contents[j]);
            }
            free(system_prompt);
            free(policy);
            free(task_type);
            free(context_json);
            free(user_message);
            free(history_roles);
            free(history_contents);
            return NULL;
        }
        history_len += strlen(history_roles[i]) + strlen(history_contents[i]) + 48;
    }

    size_t body_len = strlen(system_prompt) + strlen(policy) + strlen(task_type) +
                      strlen(context_json) + strlen(user_message) + history_len + 768;
    char *body = ai_large_calloc(1, body_len);
    if (body) {
        size_t used = 0;
        int written = snprintf(body,
                               body_len,
                               "{\"system_instruction\":{\"parts\":[{\"text\":\"%s\\n%s\\n项目AI任务类型：%s\\n项目上下文JSON：%s\"}]},"
                               "\"contents\":[",
                               system_prompt,
                               policy,
                               task_type,
                               context_json);
        if (written < 0 || (size_t)written >= body_len) {
            free(body);
            body = NULL;
        } else {
            used = (size_t)written;
        }
        for (size_t i = 0; body && i < history_count; i++) {
            written = snprintf(body + used,
                               body_len - used,
                               "%s{\"role\":\"%s\",\"parts\":[{\"text\":\"%s\"}]}",
                               i == 0 ? "" : ",",
                               history_roles[i],
                               history_contents[i]);
            if (written < 0 || (size_t)written >= body_len - used) {
                free(body);
                body = NULL;
                break;
            }
            used += (size_t)written;
        }
        if (body) {
            written = snprintf(body + used,
                               body_len - used,
                               "%s{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}],"
                               "\"generationConfig\":{\"temperature\":0.1,\"maxOutputTokens\":%d%s}}",
                               history_count > 0 ? "," : "",
                               user_message,
                               max_tokens,
                               kitchen_json_task ? ",\"responseMimeType\":\"application/json\"" : "");
            if (written < 0 || (size_t)written >= body_len - used) {
                free(body);
                body = NULL;
            }
        }
    }

    for (size_t i = 0; i < history_count; i++) {
        free(history_roles[i]);
        free(history_contents[i]);
    }
    free(history_roles);
    free(history_contents);
    free(system_prompt);
    free(policy);
    free(task_type);
    free(context_json);
    free(user_message);
    return body;
}

// AI 助手请求分发：按配置的接口格式选择 OpenAI 或 Google 请求体。
static char *build_assistant_request(const fridge_ai_config_update_t *config, const fridge_ai_assistant_request_t *request)
{
    if (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE) {
        return build_assistant_request_google(config, request);
    }
    return build_assistant_request_openai(config, request);
}

static char *base64_encode_alloc(const uint8_t *data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || len == 0) {
        return NULL;
    }
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = ai_large_alloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = table[(triple >> 6) & 0x3F];
        out[j++] = table[triple & 0x3F];
    }

    size_t mod = len % 3;
    if (mod) {
        out[out_len - 1] = '=';
        if (mod == 1) {
            out[out_len - 2] = '=';
        }
    }
    out[out_len] = '\0';
    return out;
}

// 图片识别系统提示词：约束输出为待确认的主体物体候选 JSON，OpenAI 与 Google 共用。
static const char *image_system_prompt_text(void)
{
    return
        "你是冰箱小精灵的主体物体视觉识别助手。图片来自低分辨率 ESP32 摄像头，结果只能作为候选，必须由用户确认后入库。\n"
        "测试模式允许登记非食品；请识别画面中最主要、最清晰、用户最可能想登记的主体物体，不要因为它不是食物就降低置信度。\n"
        "你必须只输出一个 UTF-8 JSON 对象，禁止输出 Markdown、代码块、解释、前后缀或多余文本。\n"
        "JSON schema 固定为：{\"schema_version\":1,\"type\":\"recognize_ingredients\",\"candidates\":[{\"name\":\"具体物品中文名\",\"quantity\":\"数量估计\",\"confidence\":0.0,\"doubt\":\"疑点或空字符串\"}],\"needs_confirmation\":true,\"confirm_fields\":[\"名称\",\"数量\",\"位置\"],\"safety_note\":\"安全提示或空字符串\"}。\n"
        "字段名必须完全使用 schema_version、type、candidates、name、quantity、confidence、doubt、needs_confirmation、confirm_fields、safety_note。\n"
        "candidates 必须包含 1 到 3 项，并按 confidence 从高到低排序；candidates[0] 是 UI 默认展示给用户确认的候选。\n"
        "confidence 必须是 0 到 1 的 JSON number，例如 0.72，不能加引号，不能写百分比或“高/低”。\n"
        "name 使用简短中文物体名，不超过 6 个汉字；quantity 不超过 8 个汉字。\n"
        "无论图像是否模糊、反光、遮挡或过曝，name 都必须是一个具体物品名，可以低置信度猜测，但禁止使用\"待确认物体\"、\"未知物体\"、\"无法识别\"、\"不确定\"。\n"
        "禁止使用 ingredients、items、foods、food_candidates 等其它字段名替代 candidates。\n"
        "看得清主体但不是食品时，按主体物体正常给出候选；画面模糊、遮挡或无法细分时，仍必须猜一个具体物品名，例如\"玻璃杯\"、\"饮料瓶\"、\"零食袋\"、\"纸盒\"、\"手机\"、\"水杯\"，confidence 可以较低。\n"
        "看不清或不能确定具体品类时，也要返回 candidates 数组，name 用最可能的具体物品名，quantity 用\"1件\"，doubt 说明低置信度原因。\n"
        "禁止把粗分类当 name，例如禁止输出\"包装物\"、\"瓶罐\"、\"袋装物\"、\"盒装物\"、\"电子设备\"、\"反光物体\"、\"模糊物体\"。\n"
        "不要把背景、屏幕文字、桌面纹理或边框误当主体；优先识别画面中央或占比最大的实物。";
}

static char *build_image_request_openai(const fridge_ai_config_update_t *config, const fridge_ai_image_request_t *request)
{
    char *model = json_escape_alloc(config->model);
    char *task_type = json_escape_alloc(request->task_type[0] ? request->task_type : "recognize_ingredients");
    char *image_b64 = base64_encode_alloc(request->jpeg, request->jpeg_len);
    if (!model || !task_type || !image_b64) {
        free(model);
        free(task_type);
        free(image_b64);
        return NULL;
    }

    char *system = json_escape_alloc(image_system_prompt_text());
    if (!system) {
        free(model);
        free(task_type);
        free(image_b64);
        return NULL;
    }

    size_t body_len = strlen(model) + strlen(task_type) + strlen(system) + strlen(image_b64) + 1024;
    char *body = ai_large_alloc(body_len);
    if (body) {
        int written = snprintf(body,
                               body_len,
                               "{\"model\":\"%s\",\"messages\":["
                               "{\"role\":\"system\",\"content\":\"%s\"},"
                               "{\"role\":\"user\",\"content\":["
                               "{\"type\":\"text\",\"text\":\"任务类型：%s。请识别这张照片中的主体物体，非食品也允许登记，并严格按系统提示只返回一个 JSON 对象。图像尺寸 %dx%d，JPEG %u 字节。\"},"
                               "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,%s\"}}"
                               "]}],\"temperature\":0.1,\"max_tokens\":%d,\"response_format\":{\"type\":\"json_object\"},\"stream\":false}",
                               model,
                               system,
                               task_type,
                               request->width,
                               request->height,
                               (unsigned)request->jpeg_len,
                               image_b64,
                               AI_IMAGE_MAX_TOKENS);
        if (written < 0 || (size_t)written >= body_len) {
            free(body);
            body = NULL;
        }
    }

    free(model);
    free(task_type);
    free(system);
    free(image_b64);
    return body;
}

// Google Gemini 图片识别请求体：system_instruction + 单条 user content。
// 图片用 inline_data{mime_type,data} 内联 base64，与 OpenAI 的 image_url(data:) 不同。
static char *build_image_request_google(const fridge_ai_config_update_t *config, const fridge_ai_image_request_t *request)
{
    char *task_type = json_escape_alloc(request->task_type[0] ? request->task_type : "recognize_ingredients");
    char *image_b64 = base64_encode_alloc(request->jpeg, request->jpeg_len);
    char *system = json_escape_alloc(image_system_prompt_text());
    if (!task_type || !image_b64 || !system) {
        free(task_type);
        free(image_b64);
        free(system);
        return NULL;
    }

    size_t body_len = strlen(task_type) + strlen(system) + strlen(image_b64) + 1024;
    char *body = ai_large_alloc(body_len);
    if (body) {
        int written = snprintf(body,
                               body_len,
                               "{\"system_instruction\":{\"parts\":[{\"text\":\"%s\"}]},"
                               "\"contents\":[{\"role\":\"user\",\"parts\":["
                               "{\"text\":\"任务类型：%s。请识别这张照片中的主体物体，非食品也允许登记，并严格按系统提示只返回一个 JSON 对象。图像尺寸 %dx%d，JPEG %u 字节。\"},"
                               "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"%s\"}}"
                               "]}],\"generationConfig\":{\"temperature\":0.1,\"maxOutputTokens\":%d,\"responseMimeType\":\"application/json\"}}",
                               system,
                               task_type,
                               request->width,
                               request->height,
                               (unsigned)request->jpeg_len,
                               image_b64,
                               AI_IMAGE_MAX_TOKENS);
        if (written < 0 || (size_t)written >= body_len) {
            free(body);
            body = NULL;
        }
    }

    free(task_type);
    free(image_b64);
    free(system);
    return body;
}

// 图片识别请求分发：按配置的接口格式选择 OpenAI 或 Google 请求体。
static char *build_image_request(const fridge_ai_config_update_t *config, const fridge_ai_image_request_t *request)
{
    if (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE) {
        return build_image_request_google(config, request);
    }
    return build_image_request_openai(config, request);
}

static bool cjson_copy_string(const cJSON *item, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return false;
    }
    strlcpy(out, item->valuestring, out_size);
    return out[0] != '\0';
}

static bool cjson_append_string(const cJSON *item, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return false;
    }
    size_t before = strlen(out);
    if (before + 1 >= out_size) {
        return before > 0;
    }
    strlcat(out, item->valuestring, out_size);
    return strlen(out) > before;
}

static size_t cjson_string_len(const cJSON *item)
{
    return cJSON_IsString(item) && item->valuestring ? strlen(item->valuestring) : 0;
}

static const char *cjson_string_or(const cJSON *item, const char *fallback)
{
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

static bool reply_text_looks_complete_json(const char *text)
{
    if (!text) {
        return false;
    }
    const char *first = strchr(text, '{');
    const char *last = strrchr(text, '}');
    return first && last && last > first;
}

static void choose_better_reply_text(char *out, size_t out_size, const char *candidate)
{
    if (!out || out_size == 0 || !candidate || candidate[0] == '\0') {
        return;
    }
    bool have_complete = reply_text_looks_complete_json(out);
    bool cand_complete = reply_text_looks_complete_json(candidate);
    if (out[0] == '\0' || (!have_complete && cand_complete) || strlen(candidate) > strlen(out) + 64) {
        strlcpy(out, candidate, out_size);
    }
}

static bool parse_openai_reply_json(const cJSON *root, fridge_ai_chat_result_t *out)
{
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = cJSON_IsObject(choice) ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    if (cJSON_IsObject(message)) {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        const cJSON *reasoning = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
        if (cJSON_IsString(content) && content->valuestring && content->valuestring[0] != '\0') {
            strlcpy(out->reply, content->valuestring, sizeof(out->reply));
        }
        if (out->reply[0] == '\0' && cJSON_IsString(reasoning) && reasoning->valuestring && reasoning->valuestring[0] != '\0') {
            // reasoning_content 是模型内部推理/草稿，只在 content 为空时作为兼容兜底，避免把
            // "Drafting the Response..." 这类过程文本展示给用户或写入历史。
            ESP_LOGW(TAG, "AI response content empty, falling back to reasoning_content");
            strlcpy(out->reply, reasoning->valuestring, sizeof(out->reply));
        }
        ESP_LOGI(TAG,
                 "AI reply fields: choices=1 content_len=%u reasoning_len=%u reply_len=%u finish=%s",
                 (unsigned)cjson_string_len(content),
                 (unsigned)cjson_string_len(reasoning),
                 (unsigned)strlen(out->reply),
                 cjson_string_or(cJSON_GetObjectItemCaseSensitive(choice, "finish_reason"), "(none)"));
        return out->reply[0] != '\0';
    }
    if (cJSON_IsObject(choice)) {
        return cjson_copy_string(cJSON_GetObjectItemCaseSensitive(choice, "text"), out->reply, sizeof(out->reply));
    }
    return false;
}

static bool parse_google_reply_json(const cJSON *root, fridge_ai_chat_result_t *out)
{
    const cJSON *candidates = cJSON_GetObjectItemCaseSensitive(root, "candidates");
    const cJSON *candidate = cJSON_IsArray(candidates) ? cJSON_GetArrayItem(candidates, 0) : NULL;
    const cJSON *content = cJSON_IsObject(candidate) ? cJSON_GetObjectItemCaseSensitive(candidate, "content") : NULL;
    const cJSON *parts = cJSON_IsObject(content) ? cJSON_GetObjectItemCaseSensitive(content, "parts") : NULL;
    if (cJSON_IsArray(parts)) {
        bool appended = false;
        const cJSON *part = NULL;
        cJSON_ArrayForEach(part, parts) {
            if (cjson_append_string(cJSON_GetObjectItemCaseSensitive(part, "text"), out->reply, sizeof(out->reply))) {
                appended = true;
            }
        }
        return appended && out->reply[0] != '\0';
    }
    return false;
}

static bool parse_flat_reply_json(const cJSON *root, fridge_ai_chat_result_t *out)
{
    if (cjson_copy_string(cJSON_GetObjectItemCaseSensitive(root, "output_text"), out->reply, sizeof(out->reply))) {
        return true;
    }
    if (cjson_copy_string(cJSON_GetObjectItemCaseSensitive(root, "content"), out->reply, sizeof(out->reply))) {
        return true;
    }
    return cjson_copy_string(cJSON_GetObjectItemCaseSensitive(root, "text"), out->reply, sizeof(out->reply));
}

static esp_err_t parse_chat_response(const char *response, fridge_ai_chat_result_t *out)
{
    if (!response || response[0] == '\0') {
        strlcpy(out->error, "AI 响应为空", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    // 解析服务端 JSON 响应时必须按协议结构取文本，不能扫描第一个 "content"。
    // OpenAI-compatible、Gemini 和部分代理服务都可能在元数据里出现同名字段，手写扫描会取错内容。
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        strlcpy(out->error, "AI 响应 JSON 无法解析", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error) {
        const cJSON *message = cJSON_IsObject(error) ? cJSON_GetObjectItemCaseSensitive(error, "message") : error;
        if (!cjson_copy_string(message, out->error, sizeof(out->error))) {
            strlcpy(out->error, "AI 服务返回 error", sizeof(out->error));
        }
        cJSON_Delete(root);
        set_last_error(out->error);
        return ESP_FAIL;
    }

    (void)cjson_copy_string(cJSON_GetObjectItemCaseSensitive(root, "model"), out->model, sizeof(out->model));
    bool ok = parse_openai_reply_json(root, out) || parse_google_reply_json(root, out) || parse_flat_reply_json(root, out);
    cJSON_Delete(root);
    if (!ok || out->reply[0] == '\0') {
        char *fallback = ai_large_calloc(1, FRIDGE_AI_MAX_REPLY_LEN + 1);
        if (fallback) {
            if (json_get_string_value_after(response, "\"message\"", "content", fallback, FRIDGE_AI_MAX_REPLY_LEN + 1) ||
                json_get_string_value_after(response, "\"parts\"", "text", fallback, FRIDGE_AI_MAX_REPLY_LEN + 1)) {
                choose_better_reply_text(out->reply, sizeof(out->reply), fallback);
                ok = out->reply[0] != '\0';
                ESP_LOGI(TAG,
                         "AI reply fallback decode: fallback_len=%u reply_len=%u complete=%s",
                         (unsigned)strlen(fallback),
                         (unsigned)strlen(out->reply),
                         reply_text_looks_complete_json(out->reply) ? "yes" : "no");
            }
            free(fallback);
        } else {
            ESP_LOGW(TAG, "AI reply fallback decode skipped: no memory");
        }
    }
    if (!ok) {
        strlcpy(out->error, "AI 响应缺少模型文本 content/text", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    strlcpy(out->status, "ok", sizeof(out->status));
    set_last_error("");
    return ESP_OK;
}

static void set_http_status_error(const char *response, fridge_ai_chat_result_t *out)
{
    char service_message[FRIDGE_AI_MAX_ERROR_LEN + 1] = {0};
    if (response) {
        json_get_string_value(response, "message", service_message, sizeof(service_message));
    }
    if (response && service_message[0] == '\0') {
        json_get_string_value(response, "error_msg", service_message, sizeof(service_message));
    }
    if (response && service_message[0] == '\0') {
        json_get_string_value(response, "detail", service_message, sizeof(service_message));
    }
    if (response && service_message[0] == '\0') {
        json_get_string_value(response, "error", service_message, sizeof(service_message));
    }
    if (response && service_message[0] == '\0') {
        compact_json_preview(response, service_message, sizeof(service_message));
    }
    if (service_message[0] != '\0') {
        snprintf(out->error, sizeof(out->error), "AI HTTP %d：", out->http_status);
        strlcat(out->error, service_message, sizeof(out->error));
    } else {
        snprintf(out->error, sizeof(out->error), "AI HTTP 状态异常：%d", out->http_status);
    }
    ESP_LOGW(TAG, "AI HTTP status error: status=%d, message=%s", out->http_status, service_message[0] ? service_message : "(empty)");
    set_last_error(out->error);
}

esp_err_t fridge_ai_client_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    nvs_handle_t handle;
    esp_err_t err = open_ai_nvs(NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        s_initialized = true;
        return ESP_OK;
    }
    set_last_error("AI NVS 初始化失败");
    return err;
}

esp_err_t fridge_ai_client_get_config(fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));

    fridge_ai_config_update_t *config = ai_large_calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AI config allocation failed");

    esp_err_t err = load_full_config(config);
    if (err != ESP_OK) {
        free(config);
        ESP_RETURN_ON_ERROR(err, TAG, "load AI config failed");
    }

    out->profile_id = config->profile_id;
    strlcpy(out->profile_name, config->profile_name, sizeof(out->profile_name));
    strlcpy(out->api_base_url, config->api_base_url, sizeof(out->api_base_url));
    strlcpy(out->model, config->model, sizeof(out->model));
    strlcpy(out->system_prompt, config->system_prompt, sizeof(out->system_prompt));
    out->timeout_ms = clamp_timeout_ms(config->timeout_ms);
    out->api_format = config->api_format;
    out->has_api_key = config->api_key[0] != '\0';
    out->ready = out->api_base_url[0] != '\0' && out->model[0] != '\0' && out->has_api_key;
    make_key_preview(config->api_key, out->api_key_preview, sizeof(out->api_key_preview));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    free(config);
    return ESP_OK;
}

esp_err_t fridge_ai_client_set_config(const fridge_ai_config_update_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");

    uint8_t profile_id = clamp_profile_id(config->profile_id);
    char key[16];
    char profile_name[FRIDGE_AI_MAX_PROFILE_NAME_LEN + 1] = {0};
    if (config->profile_name[0]) {
        strlcpy(profile_name, config->profile_name, sizeof(profile_name));
    } else {
        default_profile_name(profile_id, profile_name, sizeof(profile_name));
    }

    make_profile_key(profile_id, "name", key, sizeof(key));
    esp_err_t err = nvs_set_str(handle, key, profile_name);
    if (err == ESP_OK) {
        make_profile_key(profile_id, "base", key, sizeof(key));
        err = nvs_set_str(handle, key, config->api_base_url);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "model", key, sizeof(key));
        err = nvs_set_str(handle, key, config->model[0] ? config->model : FRIDGE_AI_DEFAULT_MODEL);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "system", key, sizeof(key));
        err = nvs_set_blob(handle, key, config->system_prompt, strlen(config->system_prompt) + 1);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "timeout", key, sizeof(key));
        err = nvs_set_u32(handle, key, clamp_timeout_ms(config->timeout_ms));
    }
    if (err == ESP_OK) {
        // 持久化接口格式；非法值统一收敛到 OpenAI，避免脏数据让设备走未知协议。
        uint8_t api_format = (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE)
                                 ? FRIDGE_AI_API_FORMAT_GOOGLE
                                 : FRIDGE_AI_API_FORMAT_OPENAI;
        make_profile_key(profile_id, "format", key, sizeof(key));
        err = nvs_set_u8(handle, key, api_format);
    }
    if (err == ESP_OK && config->update_api_key) {
        make_profile_key(profile_id, "key", key, sizeof(key));
        err = nvs_set_str(handle, key, config->api_key);
    }
    if (err == ESP_OK) {
        make_profile_key(profile_id, "updated", key, sizeof(key));
        int64_t previous_ms = load_profile_updated_ms(profile_id);
        int64_t now_ms = ai_config_now_ms();
        int64_t updated_ms = config->config_updated_at_ms > 0 ? config->config_updated_at_ms : now_ms;
        if (config->config_updated_at_ms <= 0 && updated_ms <= previous_ms) {
            updated_ms = previous_ms + 1;
        }
        err = nvs_set_i64(handle, key, updated_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, profile_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        set_last_error("");
        ESP_LOGI(TAG, "AI config profile %u saved, key_updated=%s", (unsigned)profile_id, config->update_api_key ? "yes" : "no");
    } else {
        set_last_error("AI 配置保存失败");
    }
    return err;
}

esp_err_t fridge_ai_client_get_sync_payload(char *out, size_t out_size)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "invalid sync payload buffer");
    out[0] = '\0';

    fridge_ai_config_update_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AI sync config allocation failed");
    esp_err_t err = load_full_config(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }

    char *api_base_url = utf8_clean_alloc(config->api_base_url);
    char *api_key = utf8_clean_alloc(config->api_key);
    char *model = utf8_clean_alloc(config->model);
    char *system_prompt = utf8_clean_alloc(config->system_prompt);
    char *profile_name = utf8_clean_alloc(config->profile_name);
    if (!api_base_url || !api_key || !model || !system_prompt || !profile_name) {
        free(api_base_url);
        free(api_key);
        free(model);
        free(system_prompt);
        free(profile_name);
        free(config);
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(api_base_url);
        free(api_key);
        free(model);
        free(system_prompt);
        free(profile_name);
        free(config);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddNumberToObject(root, "configUpdatedAt", (double)config->config_updated_at_ms);
    cJSON_AddStringToObject(root, "apiBaseUrl", api_base_url);
    cJSON_AddStringToObject(root, "apiKey", api_key);
    cJSON_AddStringToObject(root, "chatModel", model);
    cJSON_AddStringToObject(root, "systemPrompt", system_prompt);
    cJSON_AddNumberToObject(root, "timeoutMs", config->timeout_ms);
    cJSON_AddNumberToObject(root, "apiFormat", config->api_format);
    cJSON_AddStringToObject(root, "profileName", profile_name);
    free(api_base_url);
    free(api_key);
    free(model);
    free(system_prompt);
    free(profile_name);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(config);
    ESP_RETURN_ON_FALSE(printed, ESP_ERR_NO_MEM, TAG, "print AI sync payload failed");
    size_t len = strlen(printed);
    if (len >= out_size) {
        cJSON_free(printed);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, printed, len + 1);
    cJSON_free(printed);
    return ESP_OK;
}

esp_err_t fridge_ai_client_clear_key(void)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");
    char key[16];
    make_profile_key(get_active_profile_id(), "key", key, sizeof(key));
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        set_last_error("API Key 已清除");
    }
    return err;
}

esp_err_t fridge_ai_client_load_active_config(fridge_ai_config_update_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "AI config output is NULL");
    return load_full_config(out);
}

static esp_err_t check_network_ready(char *error_out, size_t error_out_size, bool allow_sync)
{
    // HTTPS 请求前需要 Wi-Fi 与可用系统时间；调用方可在内部 SRAM 栈任务里先完成该检查，
    // 避免 PSRAM 栈 worker 在 SNTP/网络路径中触发 cache/Flash 相关风险。
    fridge_network_status_t net = {0};
    fridge_network_get_status(&net);
    if (!net.connected) {
        if (error_out && error_out_size > 0) {
            strlcpy(error_out, "Wi-Fi 未连接，请先在 Web 面板完成配网", error_out_size);
            set_last_error(error_out);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!net.internet_ready) {
        if (allow_sync) {
            esp_err_t sync_err = fridge_network_sync_time();
            fridge_network_get_status(&net);
            if (sync_err == ESP_OK && net.internet_ready) {
                if (error_out && error_out_size > 0) {
                    error_out[0] = '\0';
                }
                return ESP_OK;
            }
        }
        if (!net.internet_ready) {
            if (error_out && error_out_size > 0) {
                strlcpy(error_out, "网络未校时或外网不可用，请确认 SNTP/互联网连接", error_out_size);
                set_last_error(error_out);
            }
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (error_out && error_out_size > 0) {
        error_out[0] = '\0';
    }
    return ESP_OK;
}

esp_err_t fridge_ai_client_ensure_network_ready(char *error_out, size_t error_out_size)
{
    return check_network_ready(error_out, error_out_size, true);
}

esp_err_t fridge_ai_client_get_profiles(fridge_ai_profile_list_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));

    out->active_profile_id = get_active_profile_id();
    nvs_handle_t handle = 0;
    bool has_handle = false;
    esp_err_t err = open_ai_nvs(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "open AI NVS failed");
        has_handle = true;
    }

    for (uint8_t id = 0; id < FRIDGE_AI_MAX_PROFILES; id++) {
        bool exists = (id == 0);
        if (has_handle) {
            exists = profile_exists_in_handle(handle, id);
        }
        if (!exists) {
            continue;
        }

        fridge_ai_config_update_t *config = calloc(1, sizeof(*config));
        if (!config) {
            if (has_handle) {
                nvs_close(handle);
            }
            ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "AI profile allocation failed");
        }
        err = load_profile_config(id, config);
        if (err != ESP_OK) {
            free(config);
            if (has_handle) {
                nvs_close(handle);
            }
            ESP_RETURN_ON_ERROR(err, TAG, "load AI profile failed");
        }
        fridge_ai_config_view_t *view = &out->profiles[out->count++];
        view->profile_id = config->profile_id;
        strlcpy(view->profile_name, config->profile_name, sizeof(view->profile_name));
        strlcpy(view->api_base_url, config->api_base_url, sizeof(view->api_base_url));
        strlcpy(view->model, config->model, sizeof(view->model));
        view->system_prompt[0] = '\0';
        view->timeout_ms = clamp_timeout_ms(config->timeout_ms);
        view->api_format = config->api_format;
        view->has_api_key = config->api_key[0] != '\0';
        view->ready = view->api_base_url[0] != '\0' && view->model[0] != '\0' && view->has_api_key;
        make_key_preview(config->api_key, view->api_key_preview, sizeof(view->api_key_preview));
        strlcpy(view->last_error, s_last_error, sizeof(view->last_error));
        free(config);
    }

    if (has_handle) {
        nvs_close(handle);
    }
    return ESP_OK;
}

esp_err_t fridge_ai_client_create_profile(const char *profile_name, fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");

    uint8_t new_id = FRIDGE_AI_MAX_PROFILES;
    for (uint8_t id = 1; id < FRIDGE_AI_MAX_PROFILES; id++) {
        if (!profile_exists_in_handle(handle, id)) {
            new_id = id;
            break;
        }
    }
    nvs_close(handle);
    ESP_RETURN_ON_FALSE(new_id < FRIDGE_AI_MAX_PROFILES, ESP_ERR_NO_MEM, TAG, "AI profile slots are full");

    fridge_ai_config_update_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AI config allocation failed");
    config->profile_id = new_id;
    if (profile_name && profile_name[0]) {
        strlcpy(config->profile_name, profile_name, sizeof(config->profile_name));
    } else {
        default_profile_name(new_id, config->profile_name, sizeof(config->profile_name));
    }
    strlcpy(config->model, FRIDGE_AI_DEFAULT_MODEL, sizeof(config->model));
    strlcpy(config->system_prompt, "你是冰箱小精灵的开发测试助手，请用简短中文回答。", sizeof(config->system_prompt));
    config->timeout_ms = FRIDGE_AI_DEFAULT_TIMEOUT_MS;

    esp_err_t err = fridge_ai_client_set_config(config);
    free(config);
    ESP_RETURN_ON_ERROR(err, TAG, "create AI profile failed");
    return out ? fridge_ai_client_get_config(out) : ESP_OK;
}

esp_err_t fridge_ai_client_select_profile(uint8_t profile_id, fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");
    profile_id = clamp_profile_id(profile_id);

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");
    bool exists = profile_exists_in_handle(handle, profile_id);
    esp_err_t err = exists ? nvs_set_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, profile_id) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "select AI profile failed");
    return out ? fridge_ai_client_get_config(out) : ESP_OK;
}

esp_err_t fridge_ai_client_delete_profile(uint8_t profile_id, fridge_ai_config_view_t *out)
{
    ESP_RETURN_ON_ERROR(fridge_ai_client_init(), TAG, "AI client init failed");
    ESP_RETURN_ON_FALSE(profile_id > 0 && profile_id < FRIDGE_AI_MAX_PROFILES, ESP_ERR_INVALID_ARG, TAG, "default AI profile cannot be deleted");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_ai_nvs(NVS_READWRITE, &handle), TAG, "open AI NVS failed");

    char key[16];
    const char *fields[] = {"name", "base", "key", "model", "system", "timeout"};
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        make_profile_key(profile_id, fields[i], key, sizeof(key));
        esp_err_t erase_err = nvs_erase_key(handle, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
            break;
        }
    }
    if (err == ESP_OK && get_active_profile_id() == profile_id) {
        err = nvs_set_u8(handle, AI_NVS_KEY_ACTIVE_PROFILE, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, TAG, "delete AI profile failed");
    return out ? fridge_ai_client_get_config(out) : ESP_OK;
}

esp_err_t fridge_ai_client_test_chat_with_config(const fridge_ai_config_update_t *config,
                                                 const char *message,
                                                 fridge_ai_chat_result_t *out)
{
    ESP_RETURN_ON_FALSE(config && message && message[0] != '\0' && out, ESP_ERR_INVALID_ARG, TAG, "invalid chat args");
    memset(out, 0, sizeof(*out));
    int64_t start_us = esp_timer_get_time();

    esp_err_t ready_err = check_network_ready(out->error, sizeof(out->error), false);
    if (ready_err != ESP_OK) {
        return ready_err;
    }

    if (config->api_base_url[0] == '\0') {
        strlcpy(out->error, "缺少 API Base URL", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }
    if (config->api_key[0] == '\0') {
        strlcpy(out->error, "缺少 API Key，请先在 AI API 页面保存", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_INVALID_STATE;
    }

    // URL 缓冲需同时容纳 OpenAI 的 /chat/completions 与 Google 的 /models/{model}:generateContent。
    char url[FRIDGE_AI_MAX_BASE_URL_LEN + FRIDGE_AI_MAX_MODEL_LEN + 32] = {0};
    esp_err_t err = (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE)
                        ? compose_google_url(config->api_base_url, config->model, url, sizeof(url))
                        : compose_chat_url(config->api_base_url, url, sizeof(url));
    if (err != ESP_OK) {
        strlcpy(out->error, "API Base URL 必须使用 http:// 或 https://，并建议以 /v1 或 /v1beta 结尾", sizeof(out->error));
        set_last_error(out->error);
        return err;
    }

    char *request_body = build_chat_request(config, message);
    if (!request_body) {
        strlcpy(out->error, "构造 AI 请求 JSON 失败", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_NO_MEM;
    }

    char *response_body = ai_large_calloc(1, AI_HTTP_RESPONSE_CAP);
    if (!response_body) {
        free(request_body);
        strlcpy(out->error, "分配 AI 响应缓冲失败", sizeof(out->error));
        set_last_error(out->error);
        return ESP_ERR_NO_MEM;
    }

    ai_http_buffer_t rx = {
        .data = response_body,
        .len = 0,
        .cap = AI_HTTP_RESPONSE_CAP,
        .overflow = false,
    };
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)clamp_timeout_ms(config->timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ai_http_event_handler,
        .user_data = &rx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(request_body);
        free(response_body);
        strlcpy(out->error, "初始化 HTTP 客户端失败", sizeof(out->error));
        set_last_error(out->error);
        return ESP_FAIL;
    }

    char auth_header[FRIDGE_AI_MAX_API_KEY_LEN + 16] = {0};
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // 鉴权头按接口格式区分：Google Gemini 用 x-goog-api-key（不带 Bearer），OpenAI 用 Authorization: Bearer。
    if (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE) {
        esp_http_client_set_header(client, "x-goog-api-key", config->api_key);
    } else {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", config->api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    err = esp_http_client_perform(client);
    out->http_status = esp_http_client_get_status_code(client);
    out->latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    strlcpy(out->model, config->model, sizeof(out->model));
    esp_http_client_cleanup(client);
    free(request_body);

    if (err != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "HTTP 请求失败：%s", esp_err_to_name(err));
        set_last_error(out->error);
        free(response_body);
        return err;
    }
    if (rx.overflow) {
        strlcpy(out->error, "AI 响应过长，串口测试缓冲已截断", sizeof(out->error));
        set_last_error(out->error);
        free(response_body);
        return ESP_FAIL;
    }
    if (out->http_status < 200 || out->http_status >= 300) {
        set_http_status_error(response_body, out);
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_chat_response(response_body, out);
    free(response_body);
    return err;
}

esp_err_t fridge_ai_client_test_chat(const char *message, fridge_ai_chat_result_t *out)
{
    fridge_ai_config_update_t *config = ai_large_calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AI config allocation failed");
    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err == ESP_OK && out) {
        err = fridge_ai_client_ensure_network_ready(out->error, sizeof(out->error));
    }
    if (err == ESP_OK) {
        err = fridge_ai_client_test_chat_with_config(config, message, out);
    }
    free(config);
    ESP_RETURN_ON_ERROR(err, TAG, "AI chat failed");
    return ESP_OK;
}

esp_err_t fridge_ai_client_assistant_chat_with_config(const fridge_ai_config_update_t *config,
                                                      const fridge_ai_assistant_request_t *request,
                                                      fridge_ai_assistant_result_t *out)
{
    ESP_RETURN_ON_FALSE(config && request && request->message[0] != '\0' && out, ESP_ERR_INVALID_ARG, TAG, "invalid assistant args");
    memset(out, 0, sizeof(*out));
    int64_t start_us = esp_timer_get_time();

    strlcpy(out->task_type, request->task_type[0] ? request->task_type : "chat_assist", sizeof(out->task_type));
    out->context_injected = request->context_injected;
    out->needs_confirmation = request->needs_confirmation;
    out->local_snapshot_version = request->local_snapshot_version;

    esp_err_t ready_err = check_network_ready(out->chat.error, sizeof(out->chat.error), false);
    if (ready_err != ESP_OK) {
        return ready_err;
    }

    if (config->api_base_url[0] == '\0') {
        strlcpy(out->chat.error, "缺少 API Base URL", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_INVALID_STATE;
    }
    if (config->api_key[0] == '\0') {
        strlcpy(out->chat.error, "缺少 API Key，请先在 AI 设置中保存", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_INVALID_STATE;
    }

    // URL 缓冲需同时容纳 OpenAI 的 /chat/completions 与 Google 的 /models/{model}:generateContent。
    char url[FRIDGE_AI_MAX_BASE_URL_LEN + FRIDGE_AI_MAX_MODEL_LEN + 32] = {0};
    esp_err_t err = (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE)
                        ? compose_google_url(config->api_base_url, config->model, url, sizeof(url))
                        : compose_chat_url(config->api_base_url, url, sizeof(url));
    if (err != ESP_OK) {
        strlcpy(out->chat.error, "API Base URL 必须使用 http:// 或 https://，并建议以 /v1 或 /v1beta 结尾", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return err;
    }

    char *request_body = build_assistant_request(config, request);
    if (!request_body) {
        strlcpy(out->chat.error, "构造 AI 助手请求 JSON 失败", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "AI assistant request prepared: task=%s, history=%u, context=%u bytes, body=%u bytes",
             request->task_type[0] ? request->task_type : "chat_assist",
             (unsigned)(request->history ? request->history_count : 0),
             (unsigned)strlen(request->context_json),
             (unsigned)strlen(request_body));

    char *response_body = ai_large_calloc(1, AI_HTTP_RESPONSE_CAP);
    if (!response_body) {
        free(request_body);
        strlcpy(out->chat.error, "分配 AI 响应缓冲失败", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_NO_MEM;
    }

    ai_http_buffer_t rx = {
        .data = response_body,
        .len = 0,
        .cap = AI_HTTP_RESPONSE_CAP,
        .overflow = false,
    };
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)clamp_timeout_ms(config->timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ai_http_event_handler,
        .user_data = &rx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(request_body);
        free(response_body);
        strlcpy(out->chat.error, "初始化 HTTP 客户端失败", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_FAIL;
    }

    char auth_header[FRIDGE_AI_MAX_API_KEY_LEN + 16] = {0};
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // 鉴权头按接口格式区分：Google Gemini 用 x-goog-api-key（不带 Bearer），OpenAI 用 Authorization: Bearer。
    if (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE) {
        esp_http_client_set_header(client, "x-goog-api-key", config->api_key);
    } else {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", config->api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    err = esp_http_client_perform(client);
    out->chat.http_status = esp_http_client_get_status_code(client);
    out->chat.latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    strlcpy(out->chat.model, config->model, sizeof(out->chat.model));
    esp_http_client_cleanup(client);
    free(request_body);

    if (err != ESP_OK) {
        snprintf(out->chat.error, sizeof(out->chat.error), "HTTP 请求失败：%s", esp_err_to_name(err));
        set_last_error(out->chat.error);
        free(response_body);
        return err;
    }
    ESP_LOGI(TAG,
             "AI assistant HTTP done: status=%d, response=%u bytes, latency=%lu ms",
             out->chat.http_status,
             (unsigned)rx.len,
             (unsigned long)out->chat.latency_ms);
    if (rx.overflow) {
        strlcpy(out->chat.error, "AI 响应过长，串口测试缓冲已截断", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        free(response_body);
        return ESP_FAIL;
    }
    if (out->chat.http_status < 200 || out->chat.http_status >= 300) {
        set_http_status_error(response_body, &out->chat);
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_chat_response(response_body, &out->chat);
    free(response_body);
    return err;
}

esp_err_t fridge_ai_client_assistant_chat(const fridge_ai_assistant_request_t *request, fridge_ai_assistant_result_t *out)
{
    if (request && request->preloaded_config) {
        return fridge_ai_client_assistant_chat_with_config(request->preloaded_config, request, out);
    }

    fridge_ai_config_update_t *config = ai_large_calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AI config allocation failed");
    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err == ESP_OK && out) {
        err = fridge_ai_client_ensure_network_ready(out->chat.error, sizeof(out->chat.error));
    }
    if (err == ESP_OK) {
        err = fridge_ai_client_assistant_chat_with_config(config, request, out);
    }
    free(config);
    ESP_RETURN_ON_ERROR(err, TAG, "AI assistant failed");
    return ESP_OK;
}

esp_err_t fridge_ai_client_analyze_image_with_config(const fridge_ai_config_update_t *config,
                                                     const fridge_ai_image_request_t *request,
                                                     fridge_ai_image_result_t *out)
{
    ESP_RETURN_ON_FALSE(request && request->jpeg && request->jpeg_len > 0 && out,
                        ESP_ERR_INVALID_ARG, TAG, "invalid image analyze args");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "image AI config is NULL");
    memset(out, 0, sizeof(*out));
    int64_t start_us = esp_timer_get_time();

    strlcpy(out->task_type, request->task_type[0] ? request->task_type : "recognize_ingredients", sizeof(out->task_type));
    out->width = request->width;
    out->height = request->height;
    out->jpeg_bytes = request->jpeg_len;
    out->needs_confirmation = true;

    if (config->api_base_url[0] == '\0') {
        strlcpy(out->chat.error, "缺少 API Base URL", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_INVALID_STATE;
    }
    if (config->api_key[0] == '\0') {
        strlcpy(out->chat.error, "缺少 API Key，请先在 AI 设置中保存", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_INVALID_STATE;
    }

    // URL 缓冲需同时容纳 OpenAI 的 /chat/completions 与 Google 的 /models/{model}:generateContent。
    char url[FRIDGE_AI_MAX_BASE_URL_LEN + FRIDGE_AI_MAX_MODEL_LEN + 32] = {0};
    esp_err_t err = (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE)
                        ? compose_google_url(config->api_base_url, config->model, url, sizeof(url))
                        : compose_chat_url(config->api_base_url, url, sizeof(url));
    if (err != ESP_OK) {
        strlcpy(out->chat.error, "API Base URL 必须使用 http:// 或 https://，并建议以 /v1 或 /v1beta 结尾", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return err;
    }

    char *request_body = build_image_request(config, request);
    if (!request_body) {
        strlcpy(out->chat.error, "构造图片 AI 请求 JSON 失败", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "AI image request prepared: task=%s, jpeg=%u bytes, body=%u bytes",
             out->task_type,
             (unsigned)request->jpeg_len,
             (unsigned)strlen(request_body));

    char *response_body = ai_large_calloc(1, AI_HTTP_RESPONSE_CAP);
    if (!response_body) {
        free(request_body);
        strlcpy(out->chat.error, "分配 AI 响应缓冲失败", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_ERR_NO_MEM;
    }

    ai_http_buffer_t rx = {
        .data = response_body,
        .len = 0,
        .cap = AI_HTTP_RESPONSE_CAP,
        .overflow = false,
    };
    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)clamp_timeout_ms(config->timeout_ms),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ai_http_event_handler,
        .user_data = &rx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(request_body);
        free(response_body);
        strlcpy(out->chat.error, "初始化 HTTP 客户端失败", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        return ESP_FAIL;
    }

    char auth_header[FRIDGE_AI_MAX_API_KEY_LEN + 16] = {0};
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    // 鉴权头按接口格式区分：Google Gemini 用 x-goog-api-key（不带 Bearer），OpenAI 用 Authorization: Bearer。
    if (config->api_format == FRIDGE_AI_API_FORMAT_GOOGLE) {
        esp_http_client_set_header(client, "x-goog-api-key", config->api_key);
    } else {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", config->api_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    err = esp_http_client_perform(client);
    out->chat.http_status = esp_http_client_get_status_code(client);
    out->chat.latency_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    strlcpy(out->chat.model, config->model, sizeof(out->chat.model));
    esp_http_client_cleanup(client);
    free(request_body);

    if (err != ESP_OK) {
        snprintf(out->chat.error, sizeof(out->chat.error), "HTTP 请求失败：%s", esp_err_to_name(err));
        set_last_error(out->chat.error);
        free(response_body);
        return err;
    }
    ESP_LOGI(TAG,
             "AI image HTTP done: status=%d, response=%u bytes, latency=%lu ms",
             out->chat.http_status,
             (unsigned)rx.len,
             (unsigned long)out->chat.latency_ms);
    if (rx.overflow) {
        strlcpy(out->chat.error, "AI 响应过长，串口测试缓冲已截断", sizeof(out->chat.error));
        set_last_error(out->chat.error);
        free(response_body);
        return ESP_FAIL;
    }
    if (out->chat.http_status < 200 || out->chat.http_status >= 300) {
        set_http_status_error(response_body, &out->chat);
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_chat_response(response_body, &out->chat);
    free(response_body);
    return err;
}

esp_err_t fridge_ai_client_analyze_image(const fridge_ai_image_request_t *request, fridge_ai_image_result_t *out)
{
    ESP_RETURN_ON_FALSE(request && request->jpeg && request->jpeg_len > 0 && out,
                        ESP_ERR_INVALID_ARG, TAG, "invalid image analyze args");
    memset(out, 0, sizeof(*out));

    fridge_ai_config_update_t *config = ai_large_calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "AI config allocation failed");
    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err == ESP_OK) {
        err = fridge_ai_client_ensure_network_ready(out->chat.error, sizeof(out->chat.error));
    }
    if (err == ESP_OK) {
        err = fridge_ai_client_analyze_image_with_config(config, request, out);
    }
    free(config);
    return err;
}
