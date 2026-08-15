// 冰箱小精灵 ESP32-P4 最小移植串口协议。
// 提供 Wi-Fi、板载麦克风、MPU6050、WakeNet/云端 ASR、TF 卡和基础诊断命令，
// 避免联调阶段拉入尚未迁移的完整 AI、扬声器和 LVGL 业务组件。
// 协议使用 115200 baud JSON Lines；密码只传给 network 组件，任何响应和日志都不得回显。

#include "fridge_usb_protocol.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "fridge_agent_runtime.h"
#include "fridge_agent_storage.h"
#include "fridge_ai_client.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_c6_ota.h"
#include "fridge_camera.h"
#include "fridge_screenshot.h"
#include "fridge_diagnostics.h"
#include "fridge_diagnostics_log.h"
#include "fridge_device_binding.h"
#include "fridge_glm_realtime.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_network.h"
#include "fridge_skill_manager.h"
#include "fridge_task_scheduler.h"
#include "fridge_tfcard.h"
#include "fridge_tf_http.h"
#include "fridge_tool_broker.h"
#include "fridge_tool_registry.h"
#include "fridge_touch_gt911.h"
#include "fridge_sht4x.h"
#include "fridge_sensors.h"
#include "fridge_trace_service.h"
#include "fridge_ui_p4.h"
#include "fridge_voice_asr.h"
#include "fridge_wake_word.h"
#include "fridge_workflow_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// Realtime instructions 解码后最多 2048 字节；JSON 转义后最坏可膨胀到 6 倍，
// 再为 URL、模型、API Key 和协议字段预留空间，避免合法配置行被静默截断。
#define USB_MIN_LINE_BUFFER_SIZE 16384
#define USB_MIN_VALUE_BUFFER_SIZE 128
#define USB_MIN_SCAN_MAX_AP 50
// Tool Broker 返回 20 条秒表计次时会进入 newlib JSON/printf 深层格式化；8KB 实测触发栈保护异常。
#define USB_MIN_TASK_STACK_BYTES 12288
#define USB_C6_OTA_B64_BUFFER_SIZE 1400
#define USB_TF_B64_BUFFER_SIZE 1400
#define USB_MIC_WAV_CHUNK_BYTES 1024U
#define USB_MIC_WAV_PREVIEW_SECONDS 2U
#define USB_TF_TEXT_INPUT_SIZE 1536
// 与 Agent TF 会话文件上限保持一致；读取缓冲由 usb_large_alloc() 优先放入 PSRAM。
#define USB_AGENT_SESSION_BUFFER_SIZE (192U * 1024U + 1U)
#define USB_AGENT_SESSION_PATH "agent/data/sessions.json"
#define USB_TTS_NS "fridge_tts"
#define USB_TTS_URL_KEY "url"
#define USB_TTS_MODEL_KEY "model"
#define USB_TTS_VOICE_KEY "voice"
#define USB_TTS_KEY_KEY "api_key"
#define USB_TTS_TIMEOUT_KEY "timeout_ms"
#define USB_TTS_DEFAULT_URL "https://api.siliconflow.cn/v1/audio/speech"
#define USB_TTS_DEFAULT_MODEL "fnlp/MOSS-TTSD-v0.5"
#define USB_TTS_DEFAULT_VOICE "fnlp/MOSS-TTSD-v0.5:alex"
#define USB_TTS_DEFAULT_TIMEOUT_MS 45000U
#define USB_TTS_TASK_STACK_BYTES 12288U

// P4 Agent TTS 的配置视图。配置只存在 NVS；API Key 绝不放入此结构的串口响应。
typedef struct {
    char api_base_url[193];
    char model[65];
    char voice[33];
    uint32_t timeout_ms;
    bool has_api_key;
    char api_key_preview[16];
    bool ready;
} usb_tts_config_view_t;

typedef struct {
    char text[FRIDGE_AGENT_RUNTIME_TTS_MAX_TEXT_LEN + 1];
} usb_tts_job_t;

static const char *TAG = "usb_p4_min";
// 16KiB JSON 行缓冲不需要 DMA，优先放 PSRAM，避免长期占用内部 DIRAM。
static char *s_line_buffer;
static fridge_wifi_ap_t s_scan_aps[USB_MIN_SCAN_MAX_AP];
static char s_c6_ota_b64[USB_C6_OTA_B64_BUFFER_SIZE];
static uint8_t s_c6_ota_chunk[FRIDGE_C6_OTA_MAX_CHUNK_SIZE];
static char s_tf_b64[USB_TF_B64_BUFFER_SIZE];
static uint8_t s_tf_chunk[FRIDGE_TFCARD_TRANSFER_CHUNK];
static uint8_t s_mic_wav_chunk[USB_MIC_WAV_CHUNK_BYTES];
static uint8_t *s_mic_wav_snapshot;
static size_t s_mic_wav_snapshot_bytes;
static uint32_t s_mic_wav_session;
static char s_tf_text_input[USB_TF_TEXT_INPUT_SIZE];
static TaskHandle_t s_protocol_task;
static atomic_bool s_hardware_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_ready_event_sent = ATOMIC_VAR_INIT(false);
static atomic_bool s_usb_tts_running = ATOMIC_VAR_INIT(false);
static uint32_t s_command_count;

// 敏感配置会经过串口行缓冲；使用 volatile 写入，避免编译器删除处理后的清零操作。
static void usb_secure_zero(void *data, size_t size)
{
    volatile uint8_t *ptr = (volatile uint8_t *)data;
    while (size-- > 0) {
        *ptr++ = 0;
    }
}

// 大块 WAV 数据优先放入 PSRAM；PSRAM 尚未可用时退回普通 8-bit heap，并在不足时明确报错。
static void *usb_large_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

// 把录音 WAV 编码为 Web 面板可直接播放的 base64 文本。
static char *base64_encode_alloc(const uint8_t *data, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || len == 0 || len > (SIZE_MAX - 1) / 4 * 3) {
        return NULL;
    }

    const size_t out_len = ((len + 2) / 3) * 4;
    char *out = usb_large_alloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;
    while (i < len) {
        const uint32_t a = i < len ? data[i++] : 0;
        const uint32_t b = i < len ? data[i++] : 0;
        const uint32_t c = i < len ? data[i++] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = table[(triple >> 6) & 0x3F];
        out[j++] = table[triple & 0x3F];
    }

    if (len % 3 != 0) {
        out[out_len - 1] = '=';
        if (len % 3 == 1) {
            out[out_len - 2] = '=';
        }
    }
    out[out_len] = '\0';
    return out;
}

// 计算与 Web 端一致的 IEEE CRC32，用于逐块发现 USB Serial/JTAG 静默丢字节。
static uint32_t usb_crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc ^ UINT32_MAX;
}

// 生成单声道、16-bit little-endian PCM 的标准 44 字节 WAV 头。
// 采样率取自最近录音格式；当前 ES8311 与 FY-SP703 均为 16kHz。
static void write_wav_header(uint8_t *header, size_t pcm_bytes, uint32_t sample_rate)
{
    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
    const uint16_t block_align = channels * bits_per_sample / 8;
    const uint32_t riff_size = (uint32_t)(36 + pcm_bytes);
    const uint32_t data_size = (uint32_t)pcm_bytes;

    memcpy(header + 0, "RIFF", 4);
    header[4] = (uint8_t)(riff_size & 0xFF);
    header[5] = (uint8_t)((riff_size >> 8) & 0xFF);
    header[6] = (uint8_t)((riff_size >> 16) & 0xFF);
    header[7] = (uint8_t)((riff_size >> 24) & 0xFF);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    header[20] = 1;
    header[21] = 0;
    header[22] = (uint8_t)(channels & 0xFF);
    header[23] = (uint8_t)((channels >> 8) & 0xFF);
    header[24] = (uint8_t)(sample_rate & 0xFF);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
    header[28] = (uint8_t)(byte_rate & 0xFF);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
    header[32] = (uint8_t)(block_align & 0xFF);
    header[33] = (uint8_t)((block_align >> 8) & 0xFF);
    header[34] = (uint8_t)(bits_per_sample & 0xFF);
    header[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);
    memcpy(header + 36, "data", 4);
    header[40] = (uint8_t)(data_size & 0xFF);
    header[41] = (uint8_t)((data_size >> 8) & 0xFF);
    header[42] = (uint8_t)((data_size >> 16) & 0xFF);
    header[43] = (uint8_t)((data_size >> 24) & 0xFF);
}

// 输出经过 JSON 转义的 UTF-8 字符串；控制字符统一转为转义序列，保证每条响应仍是一行。
static void json_print_escaped(const char *text)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*p < 0x20) {
                printf("\\u%04x", (unsigned)*p);
            } else {
                putchar((int)*p);
            }
            break;
        }
    }
    putchar('"');
}

// 轻量 JSON 字段定位器：最小模式的请求结构简单，避免为少量字符串/布尔字段引入额外解析库。
static const char *find_json_key(const char *json, const char *key)
{
    char pattern[64];
    if (!json || !key || snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return NULL;
    }

    const char *pos = strstr(json, pattern);
    if (!pos) {
        return NULL;
    }
    pos += strlen(pattern);
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos++ != ':') {
        return NULL;
    }
    while (*pos && isspace((unsigned char)*pos)) {
        pos++;
    }
    return pos;
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

// 把 \uXXXX 解码为 UTF-8，兼容 PowerShell 等会转义中文的串口脚本。
static bool append_utf8(char *out, size_t out_size, size_t *written, uint32_t codepoint)
{
    unsigned char bytes[3];
    size_t count = 0;
    if (codepoint <= 0x7F) {
        bytes[count++] = (unsigned char)codepoint;
    } else if (codepoint <= 0x7FF) {
        bytes[count++] = (unsigned char)(0xC0 | (codepoint >> 6));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    } else {
        bytes[count++] = (unsigned char)(0xE0 | (codepoint >> 12));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3F));
    }
    if (!out || !written || *written + count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        out[(*written)++] = (char)bytes[i];
    }
    return true;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *pos = find_json_key(json, key);
    if (!pos || *pos != '"' || !out || out_size == 0) {
        return false;
    }
    pos++;

    size_t written = 0;
    bool truncated = false;
    while (*pos && *pos != '"') {
        char ch = *pos++;
        if (ch == '\\' && *pos) {
            const char escaped = *pos++;
            if (escaped == 'n') {
                ch = '\n';
            } else if (escaped == 'r') {
                ch = '\r';
            } else if (escaped == 't') {
                ch = '\t';
            } else if (escaped == 'u') {
                uint32_t codepoint = 0;
                bool valid = true;
                for (int i = 0; i < 4; i++) {
                    if (!*pos) {
                        valid = false;
                        break;
                    }
                    const int value = hex_value(*pos++);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | (uint32_t)value;
                }
                if (valid) {
                    if (!append_utf8(out, out_size, &written, codepoint)) {
                        truncated = true;
                    }
                    continue;
                }
                ch = '?';
            } else {
                ch = escaped;
            }
        }
        if (written + 1 < out_size) {
            out[written++] = ch;
        } else {
            truncated = true;
        }
    }
    out[written] = '\0';
    if (truncated) {
        out[0] = '\0';
        return false;
    }
    return *pos == '"';
}

static bool json_get_raw_value(const char *json, const char *key, char *out, size_t out_size)
{
    // USB 调试命令里的 arguments 常用 JSON object 直传；这里只做原样切片，不尝试解释业务字段。
    const char *pos = find_json_key(json, key);
    if (!pos || !out || out_size == 0) {
        return false;
    }

    const char open = *pos;
    char close = '\0';
    if (open == '{') {
        close = '}';
    } else if (open == '[') {
        close = ']';
    } else {
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *end = pos;
    for (; *end; end++) {
        const char ch = *end;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == open) {
            depth++;
        } else if (ch == close) {
            depth--;
            if (depth == 0) {
                end++;
                break;
            }
        }
    }

    if (depth != 0 || end <= pos) {
        return false;
    }
    const size_t len = (size_t)(end - pos);
    if (len >= out_size) {
        return false;
    }
    memcpy(out, pos, len);
    out[len] = '\0';
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool default_value)
{
    const char *pos = find_json_key(json, key);
    if (!pos) {
        return default_value;
    }
    if (strncmp(pos, "true", 4) == 0) {
        return true;
    }
    if (strncmp(pos, "false", 5) == 0) {
        return false;
    }
    return default_value;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out)
{
    const char *pos = find_json_key(json, key);
    if (!pos || !out || !isdigit((unsigned char)*pos)) {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(pos, &end, 10);
    if (end == pos || value > UINT32_MAX || (*end != ',' && *end != '}' && !isspace((unsigned char)*end))) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool json_get_u64(const char *json, const char *key, uint64_t *out)
{
    const char *pos = find_json_key(json, key);
    if (!pos || !out || !isdigit((unsigned char)*pos)) {
        return false;
    }
    char *end = NULL;
    const unsigned long long value = strtoull(pos, &end, 10);
    if (end == pos || (*end != ',' && *end != '}' && !isspace((unsigned char)*end))) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

// 严格解码单个 OTA 分块：只接受规范 padding，避免非法串口数据越界或被静默改写。
static bool base64_decode_strict(const char *src, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!src || !out || !out_len) return false;
    const size_t len = strlen(src);
    if (len == 0 || (len % 4) != 0) return false;

    size_t written = 0;
    for (size_t i = 0; i < len; i += 4) {
        const bool last = i + 4 == len;
        const int a = base64_value(src[i]);
        const int b = base64_value(src[i + 1]);
        const int c = src[i + 2] == '=' ? 0 : base64_value(src[i + 2]);
        const int d = src[i + 3] == '=' ? 0 : base64_value(src[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        if ((!last && (src[i + 2] == '=' || src[i + 3] == '=')) ||
            (src[i + 2] == '=' && src[i + 3] != '=')) return false;

        const size_t bytes = src[i + 2] == '=' ? 1 : (src[i + 3] == '=' ? 2 : 3);
        if (written + bytes > out_size) return false;
        const uint32_t triple = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                                ((uint32_t)c << 6) | (uint32_t)d;
        out[written++] = (uint8_t)(triple >> 16);
        if (bytes > 1) out[written++] = (uint8_t)(triple >> 8);
        if (bytes > 2) out[written++] = (uint8_t)triple;
    }
    *out_len = written;
    return true;
}

// 响应输出期间锁住 stdout，避免普通 ESP_LOG 插入 JSON 中间破坏 Web Serial 解析。
static void response_begin(const char *request_id, const char *command, bool ok)
{
    flockfile(stdout);
    fputs("{\"type\":\"response\",\"request_id\":", stdout);
    json_print_escaped(request_id);
    fputs(",\"ok\":", stdout);
    fputs(ok ? "true" : "false", stdout);
    fputs(",\"command\":", stdout);
    json_print_escaped(command);
}

static void response_end(void)
{
    fputs("}\n", stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void send_error(const char *request_id, const char *command, const char *error)
{
    response_begin(request_id, command, false);
    fputs(",\"error\":", stdout);
    json_print_escaped(error);
    fputs(",\"payload\":{}", stdout);
    response_end();
}

// 外设初始化期间只报告 booting；日志仍会持续输出，但不得提前触发可重入的硬件命令。
static void send_booting_event(void)
{
    flockfile(stdout);
    // hardware_ready 在取得 stdout 锁前可能已由 main 发布；此时不能让 booting 排到 ready 之后。
    if (atomic_load_explicit(&s_hardware_ready, memory_order_acquire)) {
        funlockfile(stdout);
        return;
    }
    fputs("{\"type\":\"event\",\"event\":\"booting\",\"payload\":{"
          "\"profile\":\"p4_minimal\",\"baud\":115200,"
          "\"message\":\"hardware initialization in progress; wait for ready\"}}\n",
          stdout);
    fflush(stdout);
    funlockfile(stdout);
}

// 全部首阶段外设完成一次初始化尝试后只发送一次 ready，调试端可据此开放硬件命令。
static void send_ready_event(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_ready_event_sent,
                                                  &expected,
                                                  true,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;
    }
    flockfile(stdout);
    fputs("{\"type\":\"event\",\"event\":\"ready\",\"payload\":{"
          "\"profile\":\"p4_minimal\",\"baud\":115200,\"commands\":["
          "\"get_status\",\"get_network\",\"scan_wifi\",\"set_network\","
          "\"retry_hosted\",\"c6_ota_status\",\"c6_ota_begin\",\"c6_ota_chunk\","
          "\"c6_ota_end\",\"c6_ota_activate\","
          "\"mic_record_start\",\"mic_record_status\",\"mic_record_stop\",\"mic_record_wav\","
          "\"mic_record_wav_info\",\"mic_record_wav_chunk\",\"mic_record_wav_end\","
          "\"speaker_beep\",\"wake_maintenance_hold\","
          "\"get_tfcard_status\",\"format_tfcard\",\"tfcard_selftest\","
          "\"tf_list\",\"tf_stat\",\"tf_mkdir\",\"tf_move\",\"tf_delete\",\"tf_restore\",\"tf_trash_empty\","
          "\"tf_text_read\",\"tf_text_write\",\"tf_transfer_begin\",\"tf_transfer_status\","
          "\"tf_transfer_chunk\",\"tf_transfer_end\",\"tf_transfer_abort\","
          "\"tf_mount\",\"tf_unmount\",\"tf_health_test\",\"tf_benchmark\","
          "\"agent_storage_status\",\"task_scheduler_status\",\"tool_list\",\"tool_call\","
          "\"tool_permission_status\",\"tool_permission_set\","
          "\"skill_status\",\"skill_list\",\"skill_read\",\"skill_match\","
          "\"workflow_status\",\"workflow_start\","
          "\"device_provision_set\",\"device_binding_status\",\"device_binding_refresh\","
          "\"mqtt_status\",\"get_mqtt_config\",\"get_mqtt_status\",\"set_mqtt_config\","
          "\"clear_mqtt_secret\",\"mqtt_publish_state\","
          "\"agent_status\",\"agent_run\",\"agent_submit\",\"agent_run_status\","
          "\"agent_cancel\",\"agent_confirm\",\"agent_reject\","
          "\"agent_session_read\",\"agent_session_clear\","
          "\"agent_trace_status\",\"agent_trace_read\","
          "\"get_ai_config\",\"get_ai_profiles\",\"set_ai_config\",\"clear_ai_key\","
          "\"get_tts_config\",\"set_tts_config\",\"clear_tts_key\","
          "\"tts_play\",\"tts_status\",\"tts_stop\","
          "\"create_ai_profile\",\"select_ai_profile\",\"delete_ai_profile\",\"test_ai_chat\","
          "\"get_realtime_config\",\"set_realtime_config\",\"clear_realtime_key\","
          "\"realtime_start\",\"realtime_stop\",\"realtime_status\",\"realtime_send_text\",\"realtime_cancel\","
          "\"tf_get_access_mode\",\"tf_set_access_mode\","
          "\"tf_http_enable\",\"tf_http_status\",\"tf_http_disable\",\"tf_export_diagnostics\"," 
          "\"touch_status\",\"touch_scan\",\"get_sensors\","
          "\"mq5_confirm_burn_in\",\"mq5_capture_baseline\",\"mq5_reset_calibration\","
          "\"ui_status\",\"screenshot_capture\","
          "\"get_diagnostics\",\"get_pins\",\"get_logs\"]}}\n",
          stdout);
    fflush(stdout);
    funlockfile(stdout);
}

// 到期事件只输出协议消息，不在调度任务里直接驱动功放；后续语音阶段可在该边界挂接播报。
static void emit_task_due_event(const fridge_task_due_event_t *event, void *context)
{
    (void)context;
    if (!event) return;
    flockfile(stdout);
    fputs("{\"type\":\"event\",\"event\":\"task_due\",\"payload\":{\"id\":", stdout);
    json_print_escaped(event->id);
    fputs(",\"title\":", stdout);
    json_print_escaped(event->title);
    fputs(",\"kind\":", stdout);
    json_print_escaped(event->kind);
    printf(",\"triggeredAtEpochMs\":%" PRId64 "}}\n", event->triggered_at_epoch_ms);
    fflush(stdout);
    funlockfile(stdout);
}

static void print_network_payload(void)
{
    fridge_network_status_t status = {0};
    (void)fridge_network_get_status(&status);

    fputs("{\"ssid\":", stdout);
    json_print_escaped(status.ssid);
    fputs(",\"wifiPassword\":\"\",\"mqttHost\":\"\",\"apiBaseUrl\":\"\",\"ntpServer\":", stdout);
    json_print_escaped(status.ntp_server);
    fputs(",\"saveAiKey\":false,\"connected\":", stdout);
    fputs(status.connected ? "true" : "false", stdout);
    fputs(",\"connecting\":", stdout);
    fputs(status.connecting ? "true" : "false", stdout);
    fputs(",\"saved\":", stdout);
    fputs(status.saved ? "true" : "false", stdout);
    fputs(",\"internet\":", stdout);
    fputs(status.internet_ready ? "true" : "false", stdout);
    fputs(",\"hostedReady\":", stdout);
    fputs(status.hosted_ready ? "true" : "false", stdout);
    fputs(",\"c6OtaGuard\":", stdout);
    fputs(status.c6_ota_guard ? "true" : "false", stdout);
    fputs(",\"hostedHostVersion\":", stdout);
    json_print_escaped(status.hosted_host_version);
    fputs(",\"hostedSlaveVersion\":", stdout);
    json_print_escaped(status.hosted_slave_version);
    fputs(",\"hostedLastError\":", stdout);
    json_print_escaped(status.hosted_last_error);
    fputs(",\"status\":", stdout);
    json_print_escaped(status.connected ? (status.internet_ready ? "online" : "wifi_connected")
                                        : (status.connecting ? "connecting" : "offline"));
    fputs(",\"ip\":", stdout);
    json_print_escaped(status.ip);
    printf(",\"rssi\":%d,\"lastError\":", status.rssi);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
}

static void print_c6_ota_payload(const fridge_c6_ota_status_t *status)
{
    fputs("{\"state\":", stdout);
    json_print_escaped(fridge_c6_ota_state_name(status->state));
    printf(",\"session\":%lu,\"expectedSize\":%lu,\"receivedBytes\":%lu,"
           "\"nextOffset\":%lu,\"maxChunkBytes\":%u,\"slaveVersion\":\"%lu.%lu.%lu\","
           "\"requiresActivation\":%s,\"lastChunkDuplicate\":%s,\"expectedSha256\":",
           (unsigned long)status->session,
           (unsigned long)status->expected_size,
           (unsigned long)status->received_size,
           (unsigned long)status->received_size,
           (unsigned)FRIDGE_C6_OTA_MAX_CHUNK_SIZE,
           (unsigned long)status->slave_major,
           (unsigned long)status->slave_minor,
           (unsigned long)status->slave_patch,
           status->requires_activation ? "true" : "false",
           status->last_chunk_duplicate ? "true" : "false");
    json_print_escaped(status->expected_sha256);
    fputs(",\"actualSha256\":", stdout);
    json_print_escaped(status->actual_sha256);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status->last_error);
    fputs("}", stdout);
}

static void handle_c6_ota_status(const char *request_id, const char *command)
{
    fridge_c6_ota_status_t status = {0};
    fridge_c6_ota_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_c6_ota_payload(&status);
    response_end();
}

static void handle_c6_ota_begin(const char *line, const char *request_id, const char *command)
{
    char sha256[FRIDGE_C6_OTA_SHA256_HEX_LEN + 1] = {0};
    char confirm[32] = {0};
    char activation[32] = {0};
    uint32_t image_size = 0;
    (void)json_get_string(line, "sha256", sha256, sizeof(sha256));
    (void)json_get_string(line, "confirm", confirm, sizeof(confirm));
    (void)json_get_string(line, "activation_mode", activation, sizeof(activation));
    if (!json_get_u32(line, "image_size", &image_size) ||
        strcmp(confirm, "UPDATE_C6_FIRMWARE") != 0) {
        send_error(request_id, command, "image_size and confirm=UPDATE_C6_FIRMWARE are required");
        return;
    }

    fridge_c6_ota_activation_t mode;
    if (strcmp(activation, "legacy_end_reboots") == 0) {
        mode = FRIDGE_C6_OTA_LEGACY_END_REBOOTS;
    } else if (strcmp(activation, "explicit_activate") == 0) {
        mode = FRIDGE_C6_OTA_EXPLICIT_ACTIVATE;
    } else {
        send_error(request_id, command, "invalid activation_mode");
        return;
    }

    esp_err_t err = fridge_network_enter_c6_ota();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    uint32_t session = 0;
    err = fridge_c6_ota_begin(image_size, sha256, mode, &session);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_c6_ota_status(request_id, command);
}

static void handle_c6_ota_chunk(const char *line, const char *request_id, const char *command)
{
    uint32_t session = 0;
    uint32_t offset = 0;
    uint32_t crc32 = 0;
    if (!json_get_u32(line, "session", &session) || !json_get_u32(line, "offset", &offset) ||
        !json_get_u32(line, "crc32", &crc32) ||
        !json_get_string(line, "data_b64", s_c6_ota_b64, sizeof(s_c6_ota_b64))) {
        send_error(request_id, command, "session, offset, crc32 and data_b64 are required");
        return;
    }
    size_t chunk_len = 0;
    if (!base64_decode_strict(s_c6_ota_b64, s_c6_ota_chunk, sizeof(s_c6_ota_chunk), &chunk_len)) {
        send_error(request_id, command, "invalid base64 OTA chunk");
        return;
    }
    uint32_t next_offset = 0;
    bool duplicate = false;
    const esp_err_t err = fridge_c6_ota_write(session,
                                               offset,
                                               s_c6_ota_chunk,
                                               chunk_len,
                                               crc32,
                                               &next_offset,
                                               &duplicate);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"nextOffset\":%lu,\"receivedBytes\":%lu,\"duplicate\":%s}",
           (unsigned long)next_offset,
           (unsigned long)next_offset,
           duplicate ? "true" : "false");
    response_end();
}

static void delayed_restart_task(void *arg)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    // C6 OTA 完成后的主动重启写入短原因，便于下次启动把计划重启与异常复位区分开。
    (void)fridge_diagnostics_set_planned_restart("c6_ota");
    esp_restart();
}

static void *usb_large_calloc(size_t count, size_t size)
{
    if (!count || !size || count > SIZE_MAX / size) return NULL;
    void *ptr = usb_large_alloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

static void schedule_restart(uint32_t delay_ms)
{
    // 计划重启前会经过诊断 NVS 与格式化日志路径；2KB 栈在 P4 上已触发栈保护异常。
    (void)xTaskCreate(delayed_restart_task,
                      "c6_ota_restart",
                      4096,
                      (void *)(uintptr_t)delay_ms,
                      5,
                      NULL);
}

static void handle_c6_ota_end(const char *line, const char *request_id, const char *command)
{
    uint32_t session = 0;
    if (!json_get_u32(line, "session", &session)) {
        send_error(request_id, command, "session is required");
        return;
    }
    const esp_err_t err = fridge_c6_ota_end(session);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    fridge_c6_ota_status_t status = {0};
    fridge_c6_ota_get_status(&status);
    handle_c6_ota_status(request_id, command);
    if (!status.requires_activation) {
        // 旧0.0.6会在end后约5秒重启C6；随后重启P4，避免两端序列号/队列不同步。
        schedule_restart(7000);
    }
}

static void handle_c6_ota_activate(const char *line, const char *request_id, const char *command)
{
    uint32_t session = 0;
    if (!json_get_u32(line, "session", &session)) {
        send_error(request_id, command, "session is required");
        return;
    }
    const esp_err_t err = fridge_c6_ota_activate(session);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_c6_ota_status(request_id, command);
    schedule_restart(7000);
}

static void handle_retry_hosted(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"action\":\"reboot\",\"delayMs\":500}", stdout);
    response_end();
    // 禁止在仍有SDIO read task时原地重枚举card；整板复位是无竞态恢复路径。
    schedule_restart(500);
}

static void handle_get_network(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_network_payload();
    response_end();
}

static void handle_scan_wifi(const char *request_id, const char *command)
{
    // 扫描数组放在静态区，避免 50 个 AP 结果挤占串口任务栈。
    memset(s_scan_aps, 0, sizeof(s_scan_aps));
    size_t count = 0;
    const esp_err_t err = fridge_network_scan(s_scan_aps, USB_MIN_SCAN_MAX_AP, &count);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":[", stdout);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            putchar(',');
        }
        fputs("{\"ssid\":", stdout);
        json_print_escaped(s_scan_aps[i].ssid);
        printf(",\"rssi\":%d,\"signal\":%d,\"channel\":%u,\"secured\":%s,\"band\":\"2.4G\",\"authmode\":",
               s_scan_aps[i].rssi,
               s_scan_aps[i].rssi <= -100 ? 0 : (s_scan_aps[i].rssi >= -50 ? 100 : 2 * (s_scan_aps[i].rssi + 100)),
               s_scan_aps[i].channel,
               s_scan_aps[i].secured ? "true" : "false");
        json_print_escaped(s_scan_aps[i].authmode);
        fputs(",\"note\":\"真实扫描结果\"}", stdout);
    }
    fputs("]", stdout);
    response_end();
}

static void handle_set_network(const char *line, const char *request_id, const char *command)
{
    fridge_wifi_config_t config = {0};
    (void)json_get_string(line, "ssid", config.ssid, sizeof(config.ssid));
    if (!json_get_string(line, "wifiPassword", config.password, sizeof(config.password))) {
        (void)json_get_string(line, "password", config.password, sizeof(config.password));
    }
    const bool save = json_get_bool(line, "save", true);

    if (config.ssid[0] == '\0') {
        send_error(request_id, command, "ssid is required");
        return;
    }

    // password 在本函数作用域内只交给网络组件，响应、日志和错误文本均不包含它。
    const esp_err_t err = fridge_network_connect(&config, save);
    memset(config.password, 0, sizeof(config.password));
    if (err != ESP_OK) {
        fridge_network_status_t status = {0};
        (void)fridge_network_get_status(&status);
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    handle_get_network(request_id, command);
}

static const char *audio_state_text(fridge_audio_state_t state)
{
    switch (state) {
    case FRIDGE_AUDIO_STATE_RECORDING:
        return "recording";
    case FRIDGE_AUDIO_STATE_WAKE_LISTENING:
        return "wake_listening";
    case FRIDGE_AUDIO_STATE_READY:
        return "ready";
    case FRIDGE_AUDIO_STATE_ERROR:
        return "error";
    case FRIDGE_AUDIO_STATE_IDLE:
    default:
        return "idle";
    }
}

static void print_audio_status_payload(const fridge_audio_status_t *status)
{
    fputs("{\"state\":", stdout);
    json_print_escaped(audio_state_text(status->state));
    fputs(",\"source\":", stdout);
    json_print_escaped(fridge_audio_source_name(status->source));
    printf(",\"durationMs\":%lu,\"pcmBytes\":%u,\"rms\":%ld,"
           "\"sampleCount\":%lu,\"peakAbs\":%ld,\"minSample\":%d,\"maxSample\":%d,"
           "\"meanSample\":%ld,\"clipCount\":%lu,\"timeoutCount\":%lu,"
           "\"overflowCount\":%lu,\"nativeSampleRate\":%lu,"
           "\"nativeBitsPerSample\":%u,\"nativeChannels\":%u,"
           "\"selectedChannel\":",
           (unsigned long)status->duration_ms,
           (unsigned)status->pcm_bytes,
           (long)status->rms,
           (unsigned long)status->sample_count,
           (long)status->peak_abs,
           (int)status->min_sample,
           (int)status->max_sample,
           (long)status->mean_sample,
           (unsigned long)status->clip_count,
           (unsigned long)status->timeout_count,
           (unsigned long)status->overflow_count,
           (unsigned long)status->native_sample_rate,
           (unsigned)status->native_bits_per_sample,
           (unsigned)status->native_channels);
    json_print_escaped(fridge_audio_channel_name(status->selected_channel));
    printf(",\"leftRms\":%ld,\"rightRms\":%ld",
           (long)status->left_rms,
           (long)status->right_rms);
    fputs(",\"qualityHint\":", stdout);
    json_print_escaped(status->quality_hint);
    fputs(",\"error\":", stdout);
    json_print_escaped(status->error);
    fputs("}", stdout);
}

static void print_asr_config_payload(const fridge_asr_config_view_t *config)
{
    fputs("{\"apiBaseUrl\":", stdout);
    json_print_escaped(config->api_base_url);
    fputs(",\"model\":", stdout);
    json_print_escaped(config->model);
    printf(",\"timeoutMs\":%lu,\"hasApiKey\":%s,\"ready\":%s,\"apiKeyPreview\":",
           (unsigned long)config->timeout_ms,
           config->has_api_key ? "true" : "false",
           config->ready ? "true" : "false");
    json_print_escaped(config->api_key_preview);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(config->last_error);
    fputs("}", stdout);
}

static void handle_get_asr_config(const char *request_id, const char *command)
{
    fridge_asr_config_view_t config = {0};
    esp_err_t err = fridge_asr_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_asr_config_payload(&config);
    response_end();
}

// P4 摄像头诊断响应只返回元数据，不经 115200 串口回传大 JPEG，避免阻塞协议任务。
// 截屏命令：同步截取当前 LVGL 显示帧并写入 TF 卡 exports 目录。
// 摄像头忙（占用硬件 JPEG 编码器）时自动降级为 BMP；通常耗时 <500ms，直接在串口任务执行。
static void handle_screenshot_capture(const char *request_id, const char *command)
{
    char path[256] = {0};
    char format[8] = {0};
    esp_err_t err = fridge_screenshot_capture(path, sizeof(path), format, sizeof(format));
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"path\":", stdout);
    json_print_escaped(path);
    fputs(",\"format\":", stdout);
    json_print_escaped(format);
    fputs("}", stdout);
    response_end();
}

// 返回 Codex/Web 截屏前必须满足的 UI 稳定性门槛；只读原子快照，不访问 LVGL。
static void handle_ui_status(const char *request_id, const char *command)
{
    fridge_ui_p4_status_t status = {0};
    esp_err_t err = fridge_ui_p4_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    fputs(",\"resourceReady\":", stdout);
    fputs(status.resource_ready ? "true" : "false", stdout);
    fputs(",\"framebuffersSynced\":", stdout);
    fputs(status.framebuffers_synced ? "true" : "false", stdout);
    fputs(",\"pageFrameReady\":", stdout);
    fputs(status.page_frame_ready ? "true" : "false", stdout);
    fputs(",\"page\":", stdout);
    json_print_escaped(fridge_ui_p4_page_key(status.page));
    fputs(",\"orientation\":", stdout);
    json_print_escaped(fridge_ui_p4_orientation_key(status.orientation));
    fputs(",\"privacyMode\":", stdout);
    fputs(status.privacy_mode ? "true" : "false", stdout);
    fputs("}", stdout);
    response_end();
}

static void handle_camera_status(const char *request_id, const char *command)
{
    fridge_camera_status_t status = {0};
    fridge_camera_get_status(&status);
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"initialized\":%s,\"hasFrame\":%s,"
           "\"width\":%d,\"height\":%d,\"jpegBytes\":%u,\"captureMs\":%lu,"
           "\"frameId\":%lu,\"freePsramKb\":%lu,\"pixelFormat\":",
           status.initialized ? "true" : "false",
           status.has_frame ? "true" : "false",
           status.width, status.height, (unsigned)status.jpeg_bytes,
           (unsigned long)status.capture_ms, (unsigned long)status.frame_id,
           (unsigned long)status.free_psram_kb);
    json_print_escaped(status.pixel_format);
    fputs(",\"frameSize\":", stdout);
    json_print_escaped(status.frame_size);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_camera_capture(const char *request_id, const char *command)
{
    esp_err_t err = fridge_camera_capture_ai_fullres();
    if (err != ESP_OK) {
        fridge_camera_status_t status = {0};
        fridge_camera_get_status(&status);
        send_error(request_id, command,
                   status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    handle_camera_status(request_id, command);
}

static void handle_camera_clear(const char *request_id, const char *command)
{
    esp_err_t err = fridge_camera_clear_frame();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_camera_status(request_id, command);
}

static void handle_camera_preview(const char *request_id, const char *command, bool start)
{
    esp_err_t err = start ? fridge_camera_start_hd_preview() : fridge_camera_stop_hd_preview();
    if (err != ESP_OK) {
        fridge_camera_status_t status = {0};
        fridge_camera_get_status(&status);
        send_error(request_id, command,
                   status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    handle_camera_status(request_id, command);
}

static void handle_set_asr_config(const char *line, const char *request_id, const char *command)
{
    fridge_asr_config_view_t current = {0};
    fridge_asr_config_update_t update = {0};
    esp_err_t err = fridge_asr_get_config(&current);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    strlcpy(update.api_base_url, current.api_base_url, sizeof(update.api_base_url));
    strlcpy(update.model, current.model, sizeof(update.model));
    update.timeout_ms = current.timeout_ms;
    (void)json_get_string(line, "apiBaseUrl", update.api_base_url, sizeof(update.api_base_url));
    (void)json_get_string(line, "model", update.model, sizeof(update.model));
    (void)json_get_u32(line, "timeoutMs", &update.timeout_ms);
    if (json_get_string(line, "apiKey", update.api_key, sizeof(update.api_key)) && update.api_key[0] != '\0') {
        update.update_api_key = true;
    }

    err = fridge_asr_set_config(&update);
    memset(update.api_key, 0, sizeof(update.api_key));
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_asr_config(request_id, command);
}

static void handle_clear_asr_key(const char *request_id, const char *command)
{
    esp_err_t err = fridge_asr_clear_key();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_asr_config(request_id, command);
}

static void print_asr_result_payload(const fridge_asr_result_t *result)
{
    fputs("{\"text\":", stdout);
    json_print_escaped(result->text);
    fputs(",\"model\":", stdout);
    json_print_escaped(result->model);
    printf(",\"httpStatus\":%d,\"latencyMs\":%lu,\"audioBytes\":%u,\"error\":",
           result->http_status,
           (unsigned long)result->latency_ms,
           (unsigned)result->audio_bytes);
    json_print_escaped(result->error);
    fputs("}", stdout);
}

static void handle_asr_transcribe(const char *request_id, const char *command)
{
    fridge_asr_result_t result = {0};
    esp_err_t err = fridge_voice_asr_transcribe_latest(&result);
    fridge_voice_asr_manual_session_finish();
    if (err != ESP_OK) {
        response_begin(request_id, command, false);
        fputs(",\"error\":", stdout);
        json_print_escaped(result.error[0] ? result.error : esp_err_to_name(err));
        fputs(",\"payload\":", stdout);
        print_asr_result_payload(&result);
        response_end();
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_asr_result_payload(&result);
    response_end();
}

static void print_wake_status_payload(void)
{
    fridge_wake_word_status_t status = {0};
    (void)fridge_wake_word_get_status(&status);
    printf("{\"enabled\":%s,\"state\":",
           status.enabled ? "true" : "false");
    json_print_escaped(fridge_wake_word_state_text(status.state));
    fputs(",\"wakeWord\":", stdout);
    json_print_escaped(status.wake_word);
    fputs(",\"model\":", stdout);
    json_print_escaped(status.model);
    printf(",\"triggerCount\":%lu,\"lastTriggerMs\":%lu,\"vadState\":%ld,"
           "\"rms\":%ld,\"peakAbs\":%ld,\"timeoutCount\":%lu,"
           "\"userEnabled\":%s,\"autoAllowed\":%s,\"policyReason\":",
           (unsigned long)status.trigger_count,
           (unsigned long)status.last_trigger_ms,
           (long)status.vad_state,
           (long)status.rms,
           (long)status.peak_abs,
           (unsigned long)status.timeout_count,
           status.user_enabled ? "true" : "false",
           status.auto_allowed ? "true" : "false");
    json_print_escaped(fridge_wake_policy_reason_text(status.policy_reason));
    printf(",\"largestInternalDmaBlock\":%u,\"minDmaBlockThreshold\":%u,\"audioSource\":",
           (unsigned)status.largest_internal_dma_block,
           (unsigned)status.min_dma_block_threshold);
    json_print_escaped(status.audio_source);
    fputs(",\"error\":", stdout);
    json_print_escaped(status.error);
    fputs("}", stdout);
}

static void emit_wake_detected_event(void *user_ctx)
{
    (void)user_ctx;
    flockfile(stdout);
    fputs("{\"type\":\"event\",\"event\":\"wake_word_detected\",\"payload\":", stdout);
    print_wake_status_payload();
    fputs("}\n", stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void emit_wake_asr_event(const fridge_voice_asr_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    flockfile(stdout);
    fputs("{\"type\":\"event\",\"event\":\"wake_asr_state\",\"payload\":{\"state\":", stdout);
    json_print_escaped(fridge_voice_asr_state_text(event->state));
    printf(",\"triggerId\":%lu,\"text\":", (unsigned long)event->trigger_id);
    json_print_escaped(event->result.text);
    fputs(",\"model\":", stdout);
    json_print_escaped(event->result.model);
    printf(",\"httpStatus\":%d,\"latencyMs\":%lu,\"audioBytes\":%u,\"error\":",
           event->result.http_status,
           (unsigned long)event->result.latency_ms,
           (unsigned)event->result.audio_bytes);
    json_print_escaped(event->error);
    fputs("}}\n", stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void handle_wake_status(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_wake_status_payload();
    response_end();
}

static void handle_wake_set_enabled(const char *line,
                                    const char *request_id,
                                    const char *command,
                                    int forced_value)
{
    bool enabled = forced_value >= 0 ? forced_value != 0 : json_get_bool(line, "enabled", false);
    esp_err_t err = fridge_wake_word_set_user_enabled(enabled);
    if (err == ESP_OK) {
        fridge_wake_word_policy_update(true, true);
        if (!enabled) {
            err = fridge_wake_word_stop();
        }
    }
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_wake_status(request_id, command);
}

static void handle_wake_reset_stats(const char *request_id, const char *command)
{
    esp_err_t err = fridge_wake_word_reset_stats();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_wake_status(request_id, command);
}

static void handle_wake_maintenance_hold(const char *line,
                                         const char *request_id,
                                         const char *command)
{
    const bool enabled = json_get_bool(line, "enabled", true);
    const esp_err_t err = fridge_wake_word_set_maintenance_hold(enabled);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"enabled\":%s}", enabled ? "true" : "false");
    response_end();
}

static void handle_audio_status(const char *request_id, const char *command)
{
    fridge_audio_status_t status = {0};
    const esp_err_t err = fridge_audio_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_audio_status_payload(&status);
    response_end();
}

static void handle_audio_start(const char *line, const char *request_id, const char *command)
{
    const bool for_asr = json_get_bool(line, "forAsr", false);
    // 未传 source 或空 source 时使用默认音源（当前为 FY-SP703）；
    // 显式 es8311 / fy_sp703 / fy-sp703 仍支持，未知值直接报错。
    fridge_audio_source_t source = FRIDGE_AUDIO_DEFAULT_SOURCE;
    char source_name[USB_MIN_VALUE_BUFFER_SIZE] = {0};
    if (json_get_string(line, "source", source_name, sizeof(source_name)) && source_name[0] != '\0') {
        if (strcmp(source_name, "fy_sp703") == 0 || strcmp(source_name, "fy-sp703") == 0) {
            source = FRIDGE_AUDIO_SOURCE_FY_SP703;
        } else if (strcmp(source_name, "es8311") == 0) {
            source = FRIDGE_AUDIO_SOURCE_ES8311;
        } else {
            send_error(request_id, command, "unsupported audio source");
            return;
        }
    }

    esp_err_t err = for_asr ? fridge_voice_asr_manual_record_begin() : ESP_OK;
    if (err == ESP_OK) {
        err = fridge_audio_start_recording_source(source);
    }
    if (err != ESP_OK) {
        if (for_asr) {
            fridge_voice_asr_manual_session_finish();
        }
        send_error(request_id, command, err == ESP_ERR_INVALID_STATE ? "audio busy" : esp_err_to_name(err));
        return;
    }
    handle_audio_status(request_id, command);
}

static void handle_audio_stop(const char *request_id, const char *command)
{
    const esp_err_t err = fridge_audio_stop_recording();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_audio_status(request_id, command);
}

// P4 板载扬声器硬件自检：暂停 WakeNet 后直接播放本地音调，不依赖云端 TTS 或 API Key。
static void handle_speaker_beep(const char *request_id, const char *command)
{
    fridge_glm_rt_status_t rt_status = {0};
    bool realtime_busy = fridge_glm_realtime_get_status(&rt_status) == ESP_OK &&
                         (rt_status.session_active ||
                          (rt_status.state != FRIDGE_GLM_RT_STATE_IDLE &&
                           rt_status.state != FRIDGE_GLM_RT_STATE_ERROR));
    if (realtime_busy || fridge_audio_realtime_is_active()) {
        send_error(request_id, command, "audio busy: stop Realtime first");
        return;
    }

    fridge_wake_word_notify_dialog_started();
    esp_err_t err = fridge_wake_word_stop();
    if (err == ESP_OK) {
        err = fridge_audio_speaker_beep_test();
    }
    fridge_wake_word_notify_dialog_finished();

    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE ? "audio busy" : esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"played\":true,\"frequencyHz\":1000,\"durationMs\":500}", stdout);
    response_end();
}

static void handle_audio_wav(const char *request_id, const char *command)
{
    const int16_t *pcm = NULL;
    size_t pcm_bytes = 0;
    uint32_t duration_ms = 0;
    const esp_err_t err = fridge_audio_get_pcm(&pcm, &pcm_bytes, &duration_ms);
    if (err != ESP_OK) {
        send_error(request_id, command, err == ESP_ERR_INVALID_STATE ? "no recorded PCM ready" : esp_err_to_name(err));
        return;
    }
    fridge_audio_pcm_format_t format = {0};
    if (fridge_audio_get_pcm_format(&format) != ESP_OK ||
        !pcm || pcm_bytes == 0 || pcm_bytes > FRIDGE_AUDIO_MAX_PCM_BYTES) {
        send_error(request_id, command, "recorded PCM size invalid");
        return;
    }
    (void)duration_ms;

    // 浏览器回放限制为最长 2 秒，避免 USB JSON Lines 的超长 base64 单行丢字节。
    // 当前 FY-SP703 的 16k PCM 直接编码 WAV；只有旧 48k 固件才执行 3:1 抽取。
    const size_t preview_seconds = USB_MIC_WAV_PREVIEW_SECONDS;
    const int16_t *preview_pcm = pcm;
    int16_t *preview_pcm_alloc = NULL;
    uint32_t preview_sample_rate = format.sample_rate;
    size_t preview_bytes = pcm_bytes;
    if (format.sample_rate == FRIDGE_AUDIO_SAMPLE_RATE * 3) {
        const size_t max_input_samples = (size_t)format.sample_rate * preview_seconds;
        const size_t input_samples = pcm_bytes / sizeof(int16_t);
        const size_t preview_input_samples = input_samples < max_input_samples ? input_samples : max_input_samples;
        const size_t output_capacity = (preview_input_samples + 2) / 3;
        size_t preview_output_samples = 0;
        preview_pcm_alloc = usb_large_alloc(output_capacity * sizeof(int16_t));
        if (!preview_pcm_alloc) {
            send_error(request_id, command, "preview PCM allocation failed");
            return;
        }
        const esp_err_t resample_err = fridge_audio_downsample_48k_to_16k(
            pcm, preview_input_samples, preview_pcm_alloc, output_capacity, &preview_output_samples);
        if (resample_err != ESP_OK) {
            heap_caps_free(preview_pcm_alloc);
            send_error(request_id, command, esp_err_to_name(resample_err));
            return;
        }
        preview_pcm = preview_pcm_alloc;
        preview_bytes = preview_output_samples * sizeof(int16_t);
        preview_sample_rate = FRIDGE_AUDIO_SAMPLE_RATE;
    } else {
        const size_t preview_limit = (size_t)format.sample_rate * preview_seconds * sizeof(int16_t);
        if (preview_bytes > preview_limit) {
            preview_bytes = preview_limit;
        }
        preview_sample_rate = format.sample_rate;
    }
    const size_t wav_bytes = preview_bytes + 44;
    uint8_t *wav = usb_large_alloc(wav_bytes);
    if (!wav) {
        heap_caps_free(preview_pcm_alloc);
        send_error(request_id, command, "WAV allocation failed");
        return;
    }
    write_wav_header(wav, preview_bytes, preview_sample_rate);
    memcpy(wav + 44, preview_pcm, preview_bytes);
    char *wav_b64 = base64_encode_alloc(wav, wav_bytes);
    heap_caps_free(wav);
    heap_caps_free(preview_pcm_alloc);
    if (!wav_b64) {
        send_error(request_id, command, "WAV base64 allocation failed");
        return;
    }

    response_begin(request_id, command, true);
    printf(",\"payload\":{\"source\":");
    json_print_escaped(fridge_audio_source_name(format.source));
    printf(",\"sampleRate\":%lu,\"channels\":1,\"bitsPerSample\":16,"
           "\"durationMs\":%lu,\"pcmBytes\":%u,\"wavBytes\":%u,\"wavBase64\":\"",
           (unsigned long)preview_sample_rate,
           (unsigned long)(preview_bytes / sizeof(int16_t) * 1000 / preview_sample_rate),
           (unsigned)preview_bytes,
           (unsigned)wav_bytes);
    fputs(wav_b64, stdout);
    fputs("\"}", stdout);
    response_end();
    heap_caps_free(wav_b64);
}

// 返回当前 16k 录音的浏览器预览视图。这里只暴露只读 PCM 指针；Web 在分块
// 读取完成前禁止开始新录音，避免公共录音缓冲在传输中被覆盖。
static esp_err_t get_audio_wav_direct_view(const int16_t **pcm,
                                           size_t *preview_bytes,
                                           fridge_audio_pcm_format_t *format)
{
    uint32_t duration_ms = 0;
    size_t pcm_bytes = 0;
    esp_err_t err = fridge_audio_get_pcm(pcm, &pcm_bytes, &duration_ms);
    if (err != ESP_OK) {
        return err;
    }
    (void)duration_ms;
    err = fridge_audio_get_pcm_format(format);
    if (err != ESP_OK) {
        return err;
    }
    if (!*pcm || pcm_bytes == 0 || pcm_bytes > FRIDGE_AUDIO_MAX_PCM_BYTES ||
        format->sample_rate != FRIDGE_AUDIO_SAMPLE_RATE) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t limit = (size_t)format->sample_rate * USB_MIC_WAV_PREVIEW_SECONDS * sizeof(int16_t);
    *preview_bytes = pcm_bytes < limit ? pcm_bytes : limit;
    return ESP_OK;
}

// 先返回短小元数据，随后由 Web 通过 mic_record_wav_chunk 逐块拉取完整 WAV。
static void handle_audio_wav_info(const char *request_id, const char *command)
{
    const int16_t *pcm = NULL;
    size_t preview_bytes = 0;
    fridge_audio_pcm_format_t format = {0};
    const esp_err_t err = get_audio_wav_direct_view(&pcm, &preview_bytes, &format);
    if (err != ESP_OK) {
        send_error(request_id, command, err == ESP_ERR_INVALID_STATE ? "no 16k recorded PCM ready" : esp_err_to_name(err));
        return;
    }
    const size_t wav_bytes = preview_bytes + 44;
    uint8_t *snapshot = usb_large_alloc(wav_bytes);
    if (!snapshot) {
        send_error(request_id, command, "WAV snapshot allocation failed");
        return;
    }
    write_wav_header(snapshot, preview_bytes, format.sample_rate);
    memcpy(snapshot + 44, pcm, preview_bytes);
    heap_caps_free(s_mic_wav_snapshot);
    s_mic_wav_snapshot = snapshot;
    s_mic_wav_snapshot_bytes = wav_bytes;
    s_mic_wav_session++;
    if (s_mic_wav_session == 0) {
        s_mic_wav_session = 1;
    }
    const uint32_t full_crc32 = usb_crc32_bytes(snapshot, wav_bytes);
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"source\":");
    json_print_escaped(fridge_audio_source_name(format.source));
    printf(",\"sampleRate\":%lu,\"channels\":1,\"bitsPerSample\":16,"
           "\"durationMs\":%lu,\"pcmBytes\":%u,\"wavBytes\":%u,\"chunkBytes\":%u,"
           "\"session\":%lu,\"fullCrc32\":%lu}",
           (unsigned long)format.sample_rate,
           (unsigned long)(preview_bytes / sizeof(int16_t) * 1000 / format.sample_rate),
           (unsigned)preview_bytes,
           (unsigned)wav_bytes,
           (unsigned)USB_MIC_WAV_CHUNK_BYTES,
           (unsigned long)s_mic_wav_session,
           (unsigned long)full_crc32);
    response_end();
}

// 每个请求最多返回 1024 个原始 WAV 字节，使 USB 输出始终保持为短 JSON 行；
// offset/length/CRC32/eof 由 Web 严格校验，发现丢字节立即失败而不是生成坏音频。
static void handle_audio_wav_chunk(const char *line, const char *request_id, const char *command)
{
    uint64_t offset64 = 0;
    uint32_t session = 0;
    if (!json_get_u32(line, "session", &session) || !json_get_u64(line, "offset", &offset64)) {
        send_error(request_id, command, "session and offset are required");
        return;
    }
    if (!s_mic_wav_snapshot || session != s_mic_wav_session) {
        send_error(request_id, command, "WAV transfer session is not active");
        return;
    }
    const size_t wav_bytes = s_mic_wav_snapshot_bytes;
    if (offset64 >= wav_bytes) {
        send_error(request_id, command, "WAV offset out of range");
        return;
    }
    const size_t offset = (size_t)offset64;
    size_t length = wav_bytes - offset;
    if (length > sizeof(s_mic_wav_chunk)) {
        length = sizeof(s_mic_wav_chunk);
    }

    memcpy(s_mic_wav_chunk, s_mic_wav_snapshot + offset, length);

    char *encoded = base64_encode_alloc(s_mic_wav_chunk, length);
    if (!encoded) {
        send_error(request_id, command, "WAV chunk base64 allocation failed");
        return;
    }
    const uint32_t crc32 = usb_crc32_bytes(s_mic_wav_chunk, length);
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"session\":%lu,\"offset\":%u,\"length\":%u,\"crc32\":%lu,\"eof\":%s,\"data_b64\":\"",
           (unsigned long)session,
           (unsigned)offset,
           (unsigned)length,
           (unsigned long)crc32,
           offset + length == wav_bytes ? "true" : "false");
    fputs(encoded, stdout);
    fputs("\"}", stdout);
    response_end();
    heap_caps_free(encoded);
}

// Web 完成或放弃读取后显式释放不可变快照；新 info 也会覆盖并释放旧快照。
static void handle_audio_wav_end(const char *line, const char *request_id, const char *command)
{
    uint32_t session = 0;
    if (!json_get_u32(line, "session", &session)) {
        send_error(request_id, command, "session is required");
        return;
    }
    if (!s_mic_wav_snapshot || session != s_mic_wav_session) {
        send_error(request_id, command, "WAV transfer session is not active");
        return;
    }
    heap_caps_free(s_mic_wav_snapshot);
    s_mic_wav_snapshot = NULL;
    s_mic_wav_snapshot_bytes = 0;
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"session\":%lu,\"released\":true}", (unsigned long)session);
    response_end();
}

static void print_tfcard_status_payload(const fridge_tfcard_status_t *status)
{
    fputs("{\"mounted\":", stdout);
    fputs(status->mounted ? "true" : "false", stdout);
    fputs(",\"formatRequired\":", stdout);
    fputs(status->format_required ? "true" : "false", stdout);
    fputs(",\"formattedThisBoot\":", stdout);
    fputs(status->formatted_this_boot ? "true" : "false", stdout);
    fputs(",\"expertMode\":", stdout);
    fputs(status->expert_mode ? "true" : "false", stdout);
    fputs(",\"transferActive\":", stdout);
    fputs(status->transfer_active ? "true" : "false", stdout);
    fputs(",\"mountPoint\":\"" FRIDGE_TFCARD_MOUNT_POINT "\"", stdout);
    printf(",\"capacityMb\":%llu,\"freeMb\":%llu,\"cardName\":",
           (unsigned long long)status->capacity_mb,
           (unsigned long long)status->free_mb);
    json_print_escaped(status->card_name);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status->last_error);
    fputs("}", stdout);
}

static void handle_tfcard_status(const char *request_id, const char *command)
{
    fridge_tfcard_status_t status = {0};
    fridge_tfcard_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tfcard_status_payload(&status);
    response_end();
}

static void handle_agent_storage_status(const char *request_id, const char *command)
{
    // Agent 存储状态只读取内存快照，不直接扫描 TF 卡，避免状态查询命令造成额外 I/O 抖动。
    fridge_agent_storage_status_t status = {0};
    fridge_agent_storage_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    fputs(",\"createdDefaults\":", stdout);
    fputs(status.created_defaults ? "true" : "false", stdout);
    printf(",\"createdDirs\":%u,\"createdFiles\":%u,\"recoveredFiles\":%u,\"root\":",
           (unsigned)status.created_dirs,
           (unsigned)status.created_files,
           (unsigned)status.recovered_files);
    json_print_escaped(status.root);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_task_scheduler_status(const char *request_id, const char *command)
{
    fridge_task_scheduler_status_t status = {0};
    fridge_task_scheduler_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    printf(",\"trackedTimers\":%u,\"triggeredThisBoot\":%u,\"reloadCount\":%u,\"lastTriggeredId\":",
           (unsigned)status.tracked_timers,
           (unsigned)status.triggered_this_boot,
           (unsigned)status.reload_count);
    json_print_escaped(status.last_triggered_id);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_tool_list(const char *request_id, const char *command)
{
    // Tool 列表只导出描述与 Schema，不暴露执行函数指针；供 Web/串口调试和后续模型上下文裁剪使用。
    fridge_tool_definition_t *tools = usb_large_alloc(sizeof(*tools) * FRIDGE_TOOL_MAX_TOOLS);
    if (!tools) {
        send_error(request_id, command, "tool list allocation failed");
        return;
    }
    memset(tools, 0, sizeof(*tools) * FRIDGE_TOOL_MAX_TOOLS);
    size_t count = 0;
    esp_err_t err = fridge_tool_registry_list(tools, FRIDGE_TOOL_MAX_TOOLS, &count);
    if (err != ESP_OK) {
        heap_caps_free(tools);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    printf(",\"payload\":{\"count\":%u,\"tools\":[", (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        if (i) fputc(',', stdout);
        fputs("{\"name\":", stdout);
        json_print_escaped(tools[i].name);
        fputs(",\"description\":", stdout);
        json_print_escaped(tools[i].description);
        fputs(",\"permission\":", stdout);
        json_print_escaped(fridge_tool_permission_name(tools[i].permission));
        fputs(",\"idempotent\":", stdout);
        fputs(tools[i].idempotent ? "true" : "false", stdout);
        fputs(",\"requiresConfirmation\":", stdout);
        fputs(tools[i].requires_confirmation ? "true" : "false", stdout);
        fputs(",\"inputSchema\":", stdout);
        json_print_escaped(tools[i].input_schema);
        fputc('}', stdout);
    }
    fputs("]}", stdout);
    heap_caps_free(tools);
    response_end();
}

static void handle_tool_permission_status(const char *request_id, const char *command)
{
    fridge_agent_permission_mode_t mode = FRIDGE_AGENT_PERMISSION_CONFIRM;
    esp_err_t err = fridge_tool_broker_get_permission_mode(&mode);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"mode\":", stdout);
    json_print_escaped(fridge_agent_permission_mode_name(mode));
    fputs(",\"modes\":[\"confirm\",\"standard\",\"auto\"],", stdout);
    fputs("\"rules\":{\"confirm\":\"READ自动执行，WRITE/DANGEROUS等待确认\",", stdout);
    fputs("\"standard\":\"READ/WRITE自动执行，DANGEROUS等待确认\",", stdout);
    fputs("\"auto\":\"READ/WRITE/DANGEROUS全部自动执行（完全访问）\"}}", stdout);
    response_end();
}

static void handle_tool_permission_set(const char *line, const char *request_id, const char *command)
{
    char mode_name[24] = {0};
    if (!json_get_string(line, "mode", mode_name, sizeof(mode_name))) {
        send_error(request_id, command, "mode is required: confirm, standard or auto");
        return;
    }
    fridge_agent_permission_mode_t mode = FRIDGE_AGENT_PERMISSION_CONFIRM;
    if (!fridge_agent_permission_mode_from_name(mode_name, &mode)) {
        send_error(request_id, command, "invalid mode: expected confirm, standard or auto");
        return;
    }
    esp_err_t err = fridge_tool_broker_set_permission_mode(mode);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_tool_permission_status(request_id, command);
}

static void print_ai_config_payload(const fridge_ai_config_view_t *config)
{
    printf("{\"profileId\":%u,\"profileName\":", (unsigned)config->profile_id);
    json_print_escaped(config->profile_name);
    fputs(",\"apiBaseUrl\":", stdout);
    json_print_escaped(config->api_base_url);
    fputs(",\"model\":", stdout);
    json_print_escaped(config->model);
    fputs(",\"systemPrompt\":", stdout);
    json_print_escaped(config->system_prompt);
    printf(",\"timeoutMs\":%lu,\"apiFormat\":%u,\"hasApiKey\":%s",
           (unsigned long)config->timeout_ms, (unsigned)config->api_format,
           config->has_api_key ? "true" : "false");
    fputs(",\"apiKeyPreview\":", stdout);
    json_print_escaped(config->api_key_preview);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(config->last_error);
    printf(",\"ready\":%s}", config->ready ? "true" : "false");
}

// Agent TTS 与 Web 面板共用 fridge_tts NVS namespace。只读配置时保留默认值，确保首次
// 打开面板就能看到有效的 SiliconFlow 16k PCM 参数，API Key 仅生成脱敏预览。
static void tts_read_string(nvs_handle_t handle, const char *key, char *out, size_t out_size,
                            const char *fallback)
{
    size_t len = out_size;
    if (nvs_get_str(handle, key, out, &len) != ESP_OK || !out[0]) {
        strlcpy(out, fallback, out_size);
    }
}

static void tts_make_key_preview(const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!key || !key[0]) return;
    size_t len = strlen(key);
    if (len <= 7U) {
        strlcpy(out, "已配置", out_size);
        return;
    }
    snprintf(out, out_size, "%.3s...%.4s", key, key + len - 4U);
}

static esp_err_t tts_load_config_view(usb_tts_config_view_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    strlcpy(out->api_base_url, USB_TTS_DEFAULT_URL, sizeof(out->api_base_url));
    strlcpy(out->model, USB_TTS_DEFAULT_MODEL, sizeof(out->model));
    strlcpy(out->voice, USB_TTS_DEFAULT_VOICE, sizeof(out->voice));
    out->timeout_ms = USB_TTS_DEFAULT_TIMEOUT_MS;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(USB_TTS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    tts_read_string(handle, USB_TTS_URL_KEY, out->api_base_url, sizeof(out->api_base_url),
                    USB_TTS_DEFAULT_URL);
    tts_read_string(handle, USB_TTS_MODEL_KEY, out->model, sizeof(out->model),
                    USB_TTS_DEFAULT_MODEL);
    tts_read_string(handle, USB_TTS_VOICE_KEY, out->voice, sizeof(out->voice),
                    USB_TTS_DEFAULT_VOICE);
    uint32_t timeout = USB_TTS_DEFAULT_TIMEOUT_MS;
    if (nvs_get_u32(handle, USB_TTS_TIMEOUT_KEY, &timeout) == ESP_OK &&
        timeout >= 5000U && timeout <= 120000U) {
        out->timeout_ms = timeout;
    }
    char api_key[257] = {0};
    size_t key_len = sizeof(api_key);
    if (nvs_get_str(handle, USB_TTS_KEY_KEY, api_key, &key_len) == ESP_OK && api_key[0]) {
        out->has_api_key = true;
        tts_make_key_preview(api_key, out->api_key_preview, sizeof(out->api_key_preview));
    }
    nvs_close(handle);
    usb_secure_zero(api_key, sizeof(api_key));
    out->ready = out->has_api_key && out->api_base_url[0] && out->model[0] && out->voice[0];
    return ESP_OK;
}

static void print_tts_config_payload(const usb_tts_config_view_t *config)
{
    fputs("{\"apiBaseUrl\":", stdout);
    json_print_escaped(config->api_base_url);
    fputs(",\"model\":", stdout);
    json_print_escaped(config->model);
    fputs(",\"voice\":", stdout);
    json_print_escaped(config->voice);
    printf(",\"timeoutMs\":%lu,\"hasApiKey\":%s", (unsigned long)config->timeout_ms,
           config->has_api_key ? "true" : "false");
    fputs(",\"apiKeyPreview\":", stdout);
    json_print_escaped(config->api_key_preview);
    fputs(",\"lastError\":\"\",\"ready\":", stdout);
    fputs(config->ready ? "true}" : "false}", stdout);
}

static void handle_get_tts_config(const char *request_id, const char *command)
{
    usb_tts_config_view_t config = {0};
    esp_err_t err = tts_load_config_view(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tts_config_payload(&config);
    response_end();
}

static void handle_set_tts_config(const char *line, const char *request_id, const char *command)
{
    usb_tts_config_view_t current = {0};
    esp_err_t err = tts_load_config_view(&current);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    char api_key[257] = {0};
    bool update_key = json_get_string(line, "apiKey", api_key, sizeof(api_key)) && api_key[0];
    (void)json_get_string(line, "apiBaseUrl", current.api_base_url, sizeof(current.api_base_url));
    (void)json_get_string(line, "model", current.model, sizeof(current.model));
    (void)json_get_string(line, "voice", current.voice, sizeof(current.voice));
    uint32_t timeout = current.timeout_ms;
    (void)json_get_u32(line, "timeoutMs", &timeout);
    if (strncmp(current.api_base_url, "https://", 8U) != 0 || !current.model[0] || !current.voice[0] ||
        timeout < 5000U || timeout > 120000U) {
        usb_secure_zero(api_key, sizeof(api_key));
        send_error(request_id, command, "TTS 配置无效：URL 必须为 HTTPS，超时范围 5000-120000ms");
        return;
    }
    nvs_handle_t handle;
    err = nvs_open(USB_TTS_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, USB_TTS_URL_KEY, current.api_base_url);
        if (err == ESP_OK) err = nvs_set_str(handle, USB_TTS_MODEL_KEY, current.model);
        if (err == ESP_OK) err = nvs_set_str(handle, USB_TTS_VOICE_KEY, current.voice);
        if (err == ESP_OK) err = nvs_set_u32(handle, USB_TTS_TIMEOUT_KEY, timeout);
        if (err == ESP_OK && update_key) err = nvs_set_str(handle, USB_TTS_KEY_KEY, api_key);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    usb_secure_zero(api_key, sizeof(api_key));
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_tts_config(request_id, command);
}

static void handle_clear_tts_key(const char *request_id, const char *command)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USB_TTS_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, USB_TTS_KEY_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_tts_config(request_id, command);
}

static void print_tts_runtime_payload(const fridge_agent_runtime_tts_status_t *status)
{
    fputs("{\"state\":", stdout);
    json_print_escaped(status->state[0] ? status->state : "idle");
    printf(",\"sampleRate\":%lu,\"audioBytes\":%u,\"playedBytes\":%u,"
           "\"durationMs\":%lu,\"latencyMs\":%lu,\"httpStatus\":%d",
           (unsigned long)status->sample_rate,
           (unsigned)status->audio_bytes,
           (unsigned)status->played_bytes,
           (unsigned long)status->duration_ms,
           (unsigned long)status->latency_ms,
           status->http_status);
    fputs(",\"model\":", stdout);
    json_print_escaped(status->model);
    fputs(",\"voice\":", stdout);
    json_print_escaped(status->voice);
    fputs(",\"error\":", stdout);
    json_print_escaped(status->error);
    fputs("}", stdout);
}

static void handle_tts_status(const char *request_id, const char *command)
{
    fridge_agent_runtime_tts_status_t status = {0};
    fridge_agent_runtime_tts_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tts_runtime_payload(&status);
    response_end();
}

static void usb_tts_task(void *arg)
{
    usb_tts_job_t *job = (usb_tts_job_t *)arg;
    if (job) {
        esp_err_t err = fridge_agent_runtime_tts_play_blocking(job->text);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Web TTS failed: %s", esp_err_to_name(err));
        }
        usb_secure_zero(job, sizeof(*job));
        free(job);
    }
    fridge_wake_word_notify_dialog_finished();
    atomic_store_explicit(&s_usb_tts_running, false, memory_order_release);
    vTaskDelete(NULL);
}

static void handle_tts_play(const char *line, const char *request_id, const char *command)
{
    if (fridge_agent_runtime_tts_is_active() ||
        atomic_exchange_explicit(&s_usb_tts_running, true, memory_order_acq_rel)) {
        send_error(request_id, command, "audio busy: TTS is already active");
        return;
    }

    usb_tts_job_t *job = calloc(1, sizeof(*job));
    if (!job || !json_get_string(line, "text", job->text, sizeof(job->text)) ||
        !job->text[0]) {
        free(job);
        atomic_store_explicit(&s_usb_tts_running, false, memory_order_release);
        send_error(request_id, command, "text is required");
        return;
    }
    // Web TTS 播放前暂停 WakeNet 输入，避免 ES8311 仍处于 RX 状态而拒绝切换到 TX。
    fridge_wake_word_notify_dialog_started();
    esp_err_t wake_err = fridge_wake_word_stop();
    if (wake_err != ESP_OK && wake_err != ESP_ERR_INVALID_STATE) {
        fridge_wake_word_notify_dialog_finished();
        usb_secure_zero(job, sizeof(*job));
        free(job);
        atomic_store_explicit(&s_usb_tts_running, false, memory_order_release);
        send_error(request_id, command, esp_err_to_name(wake_err));
        return;
    }
    // TTS 会读取 NVS 并执行 HTTPS，任务栈必须保留在内部 SRAM；PCM 大缓冲仍由组件放入 PSRAM。
    if (xTaskCreate(usb_tts_task, "usb_tts", USB_TTS_TASK_STACK_BYTES, job, 5, NULL) != pdPASS) {
        fridge_wake_word_notify_dialog_finished();
        usb_secure_zero(job, sizeof(*job));
        free(job);
        atomic_store_explicit(&s_usb_tts_running, false, memory_order_release);
        send_error(request_id, command, "TTS task allocation failed");
        return;
    }

    fridge_agent_runtime_tts_status_t status = {0};
    fridge_agent_runtime_tts_get_status(&status);
    if (strcmp(status.state, "idle") == 0 || strcmp(status.state, "done") == 0 ||
        strcmp(status.state, "error") == 0) {
        strlcpy(status.state, "synthesizing", sizeof(status.state));
        status.error[0] = '\0';
        status.sample_rate = 16000U;
        usb_tts_config_view_t config = {0};
        if (tts_load_config_view(&config) == ESP_OK) {
            strlcpy(status.model, config.model, sizeof(status.model));
            strlcpy(status.voice, config.voice, sizeof(status.voice));
        }
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tts_runtime_payload(&status);
    response_end();
}

static void handle_tts_stop(const char *request_id, const char *command)
{
    fridge_agent_runtime_tts_stop();
    handle_tts_status(request_id, command);
}

static void handle_get_ai_config(const char *request_id, const char *command)
{
    fridge_ai_config_view_t *config = usb_large_calloc(1, sizeof(*config));
    if (!config) {
        send_error(request_id, command, "AI config allocation failed");
        return;
    }
    esp_err_t err = fridge_ai_client_get_config(config);
    if (err != ESP_OK) {
        free(config);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(config);
    response_end();
    free(config);
}

static void handle_get_ai_profiles(const char *request_id, const char *command)
{
    fridge_ai_profile_list_t *profiles = usb_large_calloc(1, sizeof(*profiles));
    if (!profiles) {
        send_error(request_id, command, "AI profiles allocation failed");
        return;
    }
    esp_err_t err = fridge_ai_client_get_profiles(profiles);
    if (err != ESP_OK) {
        free(profiles);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"activeProfileId\":%u,\"profiles\":[",
           (unsigned)profiles->active_profile_id);
    for (size_t i = 0; i < profiles->count; i++) {
        if (i) putchar(',');
        print_ai_config_payload(&profiles->profiles[i]);
    }
    fputs("]}", stdout);
    response_end();
    free(profiles);
}

static void handle_set_ai_config(const char *line, const char *request_id, const char *command)
{
    fridge_ai_config_view_t *current = usb_large_calloc(1, sizeof(*current));
    fridge_ai_config_update_t *update = usb_large_calloc(1, sizeof(*update));
    if (!current || !update) {
        free(current);
        free(update);
        send_error(request_id, command, "AI config allocation failed");
        return;
    }
    esp_err_t err = fridge_ai_client_get_config(current);
    if (err != ESP_OK) {
        free(current);
        free(update);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    uint32_t value = current->profile_id;
    (void)json_get_u32(line, "profileId", &value);
    update->profile_id = (uint8_t)value;
    strlcpy(update->profile_name, current->profile_name, sizeof(update->profile_name));
    strlcpy(update->api_base_url, current->api_base_url, sizeof(update->api_base_url));
    strlcpy(update->model, current->model[0] ? current->model : FRIDGE_AI_DEFAULT_MODEL,
            sizeof(update->model));
    strlcpy(update->system_prompt, current->system_prompt, sizeof(update->system_prompt));
    update->timeout_ms = current->timeout_ms ? current->timeout_ms : FRIDGE_AI_DEFAULT_TIMEOUT_MS;
    update->api_format = current->api_format;
    (void)json_get_string(line, "profileName", update->profile_name, sizeof(update->profile_name));
    (void)json_get_string(line, "apiBaseUrl", update->api_base_url, sizeof(update->api_base_url));
    (void)json_get_string(line, "model", update->model, sizeof(update->model));
    (void)json_get_string(line, "systemPrompt", update->system_prompt, sizeof(update->system_prompt));
    value = update->timeout_ms;
    (void)json_get_u32(line, "timeoutMs", &value);
    update->timeout_ms = value;
    value = update->api_format;
    (void)json_get_u32(line, "apiFormat", &value);
    update->api_format = (uint8_t)value;
    if (json_get_string(line, "apiKey", update->api_key, sizeof(update->api_key)) && update->api_key[0]) {
        update->update_api_key = true;
    }
    err = fridge_ai_client_set_config(update);
    usb_secure_zero(update, sizeof(*update));
    free(update);
    free(current);
    if (err != ESP_OK) {
        send_error(request_id, command, err == ESP_ERR_INVALID_ARG ? "AI Base URL 必须使用 HTTPS" : esp_err_to_name(err));
        return;
    }
    handle_get_ai_config(request_id, command);
}

static void handle_clear_ai_key(const char *request_id, const char *command)
{
    esp_err_t err = fridge_ai_client_clear_key();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_ai_config(request_id, command);
}

static void handle_create_ai_profile(const char *line, const char *request_id, const char *command)
{
    char name[FRIDGE_AI_MAX_PROFILE_NAME_LEN + 1] = {0};
    (void)json_get_string(line, "profileName", name, sizeof(name));
    // AI 配置视图约 8.7KB，必须放到堆上，避免挤爆 USB 协议任务的内部 SRAM 栈。
    fridge_ai_config_view_t *config = usb_large_calloc(1, sizeof(*config));
    if (!config) {
        send_error(request_id, command, "AI config allocation failed");
        return;
    }
    esp_err_t err = fridge_ai_client_create_profile(name, config);
    if (err != ESP_OK) {
        free(config);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(config);
    response_end();
    free(config);
}

static void handle_select_ai_profile(const char *line, const char *request_id, const char *command)
{
    uint32_t id = 0;
    (void)json_get_u32(line, "profileId", &id);
    // Profile 切换会返回完整配置视图；大对象放堆上，避免 NVS/JSON 调用链触发栈保护。
    fridge_ai_config_view_t *config = usb_large_calloc(1, sizeof(*config));
    if (!config) {
        send_error(request_id, command, "AI config allocation failed");
        return;
    }
    esp_err_t err = fridge_ai_client_select_profile((uint8_t)id, config);
    if (err != ESP_OK) {
        free(config);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(config);
    response_end();
    free(config);
}

static void handle_delete_ai_profile(const char *line, const char *request_id, const char *command)
{
    uint32_t id = 0;
    (void)json_get_u32(line, "profileId", &id);
    // 删除后返回活动 Profile 的完整视图；大对象放堆上，保证 USB 任务栈有余量。
    fridge_ai_config_view_t *config = usb_large_calloc(1, sizeof(*config));
    if (!config) {
        send_error(request_id, command, "AI config allocation failed");
        return;
    }
    esp_err_t err = fridge_ai_client_delete_profile((uint8_t)id, config);
    if (err != ESP_OK) {
        free(config);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_ai_config_payload(config);
    response_end();
    free(config);
}

static void handle_test_ai_chat(const char *line, const char *request_id, const char *command)
{
    char input[FRIDGE_AGENT_RUNTIME_MAX_INPUT_LEN + 1] = {0};
    if (!json_get_string(line, "message", input, sizeof(input)) || !input[0]) {
        send_error(request_id, command, "message is required");
        return;
    }
    fridge_agent_run_result_t *result = usb_large_calloc(1, sizeof(*result));
    if (!result) {
        send_error(request_id, command, "Agent result allocation failed");
        return;
    }
    int64_t started = esp_timer_get_time();
    esp_err_t err = fridge_agent_runtime_run_text(input, result);
    response_begin(request_id, command, err == ESP_OK && result->ok);
    fputs(",\"payload\":{\"reply\":", stdout);
    json_print_escaped(result->reply);
    printf(",\"latencyMs\":%lu,\"modelLatencyMs\":%lu,\"httpStatus\":%d,\"runId\":",
           (unsigned long)((esp_timer_get_time() - started) / 1000),
           (unsigned long)result->model_latency_ms,
           result->http_status);
    json_print_escaped(result->run_id);
    fputs(",\"status\":", stdout);
    json_print_escaped(fridge_agent_run_state_name(result->state));
    fputs("}", stdout);
    if (err != ESP_OK || !result->ok) {
        fputs(",\"error\":", stdout);
        json_print_escaped(result->error[0] ? result->error : esp_err_to_name(err));
    }
    response_end();
    free(result);
}

static void handle_agent_status(const char *request_id, const char *command)
{
    fridge_agent_runtime_status_t status = {0};
    fridge_agent_runtime_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    printf(",\"runSequence\":%u,\"lastRunId\":", (unsigned)status.run_sequence);
    json_print_escaped(status.last_run_id);
    fputs(",\"lastState\":", stdout);
    json_print_escaped(status.last_state);
    fputs(",\"lastIntent\":", stdout);
    json_print_escaped(status.last_intent);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    printf(",\"contextBytes\":%lu,\"toolSchemaCount\":%u,\"modelSteps\":%u"
           ",\"cloudStackHighWater\":%lu,\"freePsramBytes\":%lu"
           ",\"internalDmaLargestBytes\":%lu,\"cloudPending\":%s",
           (unsigned long)status.context_bytes,
           (unsigned)status.tool_schema_count,
           (unsigned)status.model_steps,
           (unsigned long)status.cloud_stack_high_water,
           (unsigned long)status.free_psram_bytes,
           (unsigned long)status.internal_dma_largest_bytes,
           status.cloud_pending ? "true" : "false");
    fputs("}", stdout);
    response_end();
}

static void print_agent_run_status_payload(const fridge_agent_runtime_run_snapshot_t *run,
                                           const fridge_agent_runtime_status_t *status)
{
    fputs("{\"active\":", stdout);
    fputs(run->active ? "true" : "false", stdout);
    fputs(",\"runId\":", stdout);
    json_print_escaped(run->run_id);
    fputs(",\"state\":", stdout);
    json_print_escaped(run->state);
    fputs(",\"intent\":", stdout);
    json_print_escaped(run->intent);
    fputs(",\"reply\":", stdout);
    json_print_escaped(run->reply);
    fputs(",\"error\":", stdout);
    json_print_escaped(run->error);
    printf(",\"httpStatus\":%d,\"modelLatencyMs\":%lu,\"contextBytes\":%lu,"
           "\"toolSchemaCount\":%u,\"modelSteps\":%u,\"cloudStackHighWater\":%lu,"
           "\"freePsramBytes\":%lu,\"internalDmaLargestBytes\":%lu,\"cloudPending\":%s,"
           "\"confirmation\":",
           run->http_status,
           (unsigned long)run->model_latency_ms,
           (unsigned long)status->context_bytes,
           (unsigned)status->tool_schema_count,
           (unsigned)status->model_steps,
           (unsigned long)status->cloud_stack_high_water,
           (unsigned long)status->free_psram_bytes,
           (unsigned long)status->internal_dma_largest_bytes,
           status->cloud_pending ? "true" : "false");
    if (!run->requires_confirmation) {
        fputs("null", stdout);
    } else {
        fputs("{\"callId\":", stdout);
        json_print_escaped(run->call_id);
        fputs(",\"toolName\":", stdout);
        json_print_escaped(run->tool_name);
        fputs(",\"arguments\":", stdout);
        fputs(run->tool_arguments_json[0] ? run->tool_arguments_json : "{}", stdout);
        fputs(",\"permission\":", stdout);
        json_print_escaped(run->tool_permission);
        fputs("}", stdout);
    }
    fputs("}", stdout);
}

static void handle_agent_run_status(const char *request_id, const char *command)
{
    fridge_agent_runtime_run_snapshot_t *run = usb_large_calloc(1, sizeof(*run));
    if (!run) {
        send_error(request_id, command, "Agent status allocation failed");
        return;
    }
    fridge_agent_runtime_status_t status = {0};
    fridge_agent_runtime_get_run_snapshot(run);
    fridge_agent_runtime_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_agent_run_status_payload(run, &status);
    response_end();
    free(run);
}

static void handle_agent_submit(const char *line, const char *request_id, const char *command)
{
    char input[FRIDGE_AGENT_RUNTIME_MAX_INPUT_LEN + 1] = {0};
    if ((!json_get_string(line, "message", input, sizeof(input)) &&
         !json_get_string(line, "input", input, sizeof(input))) || !input[0]) {
        send_error(request_id, command, "message/input is required");
        return;
    }
    char run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN] = {0};
    esp_err_t err = fridge_agent_runtime_submit_text_with_id(input, run_id, sizeof(run_id));
    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE ? "Agent 正在处理上一条请求" : esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"queued\":true,\"runId\":", stdout);
    json_print_escaped(run_id);
    fputs(",\"state\":\"ROUTING\"}", stdout);
    response_end();
}

static void handle_agent_cancel(const char *request_id, const char *command)
{
    esp_err_t err = fridge_agent_runtime_cancel_active();
    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE ? "没有可取消的 Agent Run" : esp_err_to_name(err));
        return;
    }
    handle_agent_run_status(request_id, command);
}

static void handle_agent_confirmation(const char *line, const char *request_id,
                                      const char *command, bool confirmed)
{
    char run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN] = {0};
    char call_id[FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN] = {0};
    if (!json_get_string(line, "runId", run_id, sizeof(run_id)) || !run_id[0] ||
        !json_get_string(line, "callId", call_id, sizeof(call_id)) || !call_id[0]) {
        send_error(request_id, command, "runId/callId is required");
        return;
    }
    esp_err_t err = fridge_agent_runtime_resolve_confirmation(run_id, call_id, confirmed);
    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE ? "待确认工具已变化或不存在" : esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"queued\":true,\"confirmed\":%s,\"runId\":",
           confirmed ? "true" : "false");
    json_print_escaped(run_id);
    fputs(",\"callId\":", stdout);
    json_print_escaped(call_id);
    fputs("}", stdout);
    response_end();
}

static void handle_agent_session_read(const char *request_id, const char *command)
{
    char *json = usb_large_alloc(USB_AGENT_SESSION_BUFFER_SIZE);
    if (!json) {
        send_error(request_id, command, "Agent session allocation failed");
        return;
    }
    size_t json_size = 0;
    static const char empty_session[] =
        "{\"schema_version\":1,\"foreground\":{\"next_sequence\":0,\"messages\":[]}}";
    esp_err_t err = fridge_agent_storage_json_read(USB_AGENT_SESSION_PATH, json,
                                                   USB_AGENT_SESSION_BUFFER_SIZE,
                                                   &json_size, empty_session);
    if (err != ESP_OK || json_size == 0) {
        free(json);
        send_error(request_id, command, err == ESP_OK ? "Agent session is empty" : esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fwrite(json, 1, json_size, stdout);
    response_end();
    free(json);
}

static void handle_agent_session_clear(const char *request_id, const char *command)
{
    esp_err_t err = fridge_agent_runtime_clear_session();
    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE ? "Agent 运行中，不能清空会话" : esp_err_to_name(err));
        return;
    }
    handle_agent_session_read(request_id, command);
}

static void handle_agent_run(const char *line, const char *request_id, const char *command)
{
    char input[FRIDGE_AGENT_RUNTIME_MAX_INPUT_LEN + 1] = {0};
    if (!json_get_string(line, "input", input, sizeof(input)) &&
        !json_get_string(line, "message", input, sizeof(input)) &&
        !json_get_string(line, "text", input, sizeof(input))) {
        send_error(request_id, command, "input/message/text is required");
        return;
    }

    fridge_agent_run_result_t *result = usb_large_alloc(sizeof(*result));
    if (!result) {
        send_error(request_id, command, "agent result allocation failed");
        return;
    }
    memset(result, 0, sizeof(*result));
    const esp_err_t err = fridge_agent_runtime_run_text(input, result);
    response_begin(request_id, command, err == ESP_OK && result->ok);
    fputs(",\"payload\":{\"runId\":", stdout);
    json_print_escaped(result->run_id);
    fputs(",\"state\":", stdout);
    json_print_escaped(fridge_agent_run_state_name(result->state));
    fputs(",\"intent\":", stdout);
    json_print_escaped(fridge_agent_intent_name(result->intent));
    fputs(",\"selectedTool\":", stdout);
    json_print_escaped(result->selected_tool);
    fputs(",\"reply\":", stdout);
    json_print_escaped(result->reply);
    printf(",\"httpStatus\":%d,\"modelLatencyMs\":%lu",
           result->http_status, (unsigned long)result->model_latency_ms);
    fputs(",\"trace\":", stdout);
    fputs(result->trace_json[0] ? result->trace_json : "{}", stdout);
    fputs("}", stdout);
    if (err != ESP_OK || !result->ok) {
        fputs(",\"error\":", stdout);
        json_print_escaped(result->error[0] ? result->error : esp_err_to_name(err));
    }
    heap_caps_free(result);
    response_end();
}

static void handle_agent_trace_status(const char *request_id, const char *command)
{
    fridge_trace_status_t status = {0};
    fridge_trace_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    printf(",\"appendedRuns\":%u,\"rotatedCount\":%u,\"runsBytes\":%llu,\"path\":",
           (unsigned)status.appended_runs,
           (unsigned)status.rotated_count,
           (unsigned long long)status.runs_bytes);
    json_print_escaped(FRIDGE_TRACE_RUNS_PATH);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_agent_trace_read(const char *request_id, const char *command)
{
    char *text = usb_large_alloc(FRIDGE_TRACE_READ_MAX_BYTES + 1);
    if (!text) {
        send_error(request_id, command, "trace read allocation failed");
        return;
    }
    size_t n = 0;
    const esp_err_t err = fridge_trace_read_runs_tail(text, FRIDGE_TRACE_READ_MAX_BYTES + 1, &n);
    if (err != ESP_OK) {
        heap_caps_free(text);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"bytes\":%u,\"path\":", (unsigned)n);
    json_print_escaped(FRIDGE_TRACE_RUNS_PATH);
    fputs(",\"text\":", stdout);
    json_print_escaped(text);
    fputs("}", stdout);
    heap_caps_free(text);
    response_end();
}

// ---- GLM Realtime USB 调试接口：仅转发配置/启停/状态；事件经 emit 异步输出 ----

static void emit_realtime_event(const fridge_glm_rt_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event) {
        return;
    }
    flockfile(stdout);
    fputs("{\"type\":\"event\",\"event\":", stdout);
    json_print_escaped(event->name[0] ? event->name : "realtime_event");
    fputs(",\"payload\":{", stdout);
    fputs("\"state\":", stdout);
    json_print_escaped(event->state);
    fputs(",\"role\":", stdout);
    json_print_escaped(event->role);
    fputs(",\"text\":", stdout);
    json_print_escaped(event->text);
    fputs(",\"tool\":", stdout);
    json_print_escaped(event->tool_name);
    fputs(",\"callId\":", stdout);
    json_print_escaped(event->call_id);
    fputs(",\"arguments\":", stdout);
    json_print_escaped(event->arguments_json);
    fputs(",\"summary\":", stdout);
    json_print_escaped(event->summary);
    fputs(",\"code\":", stdout);
    json_print_escaped(event->code);
    fputs(",\"status\":", stdout);
    json_print_escaped(event->status);
    printf(",\"isFinal\":%s,\"latencyMs\":%lu", event->is_final ? "true" : "false",
           (unsigned long)event->latency_ms);
    fputs("}}\n", stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void print_realtime_config_payload(const fridge_glm_rt_config_view_t *cfg)
{
    fputs("{\"url\":", stdout);
    json_print_escaped(cfg->url);
    fputs(",\"model\":", stdout);
    json_print_escaped(cfg->model);
    fputs(",\"voice\":", stdout);
    json_print_escaped(cfg->voice);
    fputs(",\"instructions\":", stdout);
    json_print_escaped(cfg->instructions);
    fputs(",\"vadMode\":", stdout);
    json_print_escaped(cfg->vad_mode);
    printf(",\"timeoutMs\":%lu,\"enabled\":%s,\"hasApiKey\":%s,\"ready\":%s",
           (unsigned long)cfg->timeout_ms,
           cfg->enabled ? "true" : "false",
           cfg->has_api_key ? "true" : "false",
           cfg->ready ? "true" : "false");
    fputs(",\"apiKeyPreview\":", stdout);
    json_print_escaped(cfg->api_key_preview);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(cfg->last_error);
    fputc('}', stdout);
}

static void print_realtime_status_payload(const fridge_glm_rt_status_t *st)
{
    fputs("{\"ready\":", stdout);
    fputs(st->ready ? "true" : "false", stdout);
    fputs(",\"sessionActive\":", stdout);
    fputs(st->session_active ? "true" : "false", stdout);
    fputs(",\"networkOk\":", stdout);
    fputs(st->network_ok ? "true" : "false", stdout);
    fputs(",\"internetReady\":", stdout);
    fputs(st->internet_ready ? "true" : "false", stdout);
    fputs(",\"audioUpActive\":", stdout);
    fputs(st->audio_up_active ? "true" : "false", stdout);
    fputs(",\"audioDownActive\":", stdout);
    fputs(st->audio_down_active ? "true" : "false", stdout);
    fputs(",\"toolsReady\":", stdout);
    fputs(st->tools_ready ? "true" : "false", stdout);
    fputs(",\"state\":", stdout);
    json_print_escaped(st->state_name);
    fputs(",\"model\":", stdout);
    json_print_escaped(st->model);
    fputs(",\"voice\":", stdout);
    json_print_escaped(st->voice);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(st->last_error);
    printf(",\"toolsExported\":%lu,\"audioUpBytes\":%lu,\"audioDownBytes\":%lu,"
           "\"toolCallsOk\":%lu,\"toolCallsFail\":%lu,\"lastEventAgeMs\":%lu}",
           (unsigned long)st->tools_exported,
           (unsigned long)st->audio_up_bytes,
           (unsigned long)st->audio_down_bytes,
           (unsigned long)st->tool_calls_ok,
           (unsigned long)st->tool_calls_fail,
           (unsigned long)st->last_event_age_ms);
}

static void handle_get_realtime_config(const char *request_id, const char *command)
{
    // Realtime 配置包含 2KB instructions；必须放 PSRAM，避免 USB 8KB 任务栈与
    // get_config 内部/NVS 调用叠加后触发 P4 Stack protection fault。
    fridge_glm_rt_config_view_t *cfg = usb_large_alloc(sizeof(*cfg));
    if (!cfg) {
        send_error(request_id, command, "allocate realtime config view failed");
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    esp_err_t err = fridge_glm_realtime_get_config(cfg);
    if (err != ESP_OK) {
        heap_caps_free(cfg);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_realtime_config_payload(cfg);
    response_end();
    heap_caps_free(cfg);
}

static void handle_set_realtime_config(const char *line, const char *request_id, const char *command)
{
    // update 约 2.6KB，后续还会进入 NVS 保存和配置回读；禁止把它放在 USB 任务栈上。
    fridge_glm_rt_config_update_t *update = usb_large_alloc(sizeof(*update));
    if (!update) {
        send_error(request_id, command, "allocate realtime config update failed");
        return;
    }
    memset(update, 0, sizeof(*update));
    if ((find_json_key(line, "url") && !json_get_string(line, "url", update->url, sizeof(update->url))) ||
        (find_json_key(line, "model") && !json_get_string(line, "model", update->model, sizeof(update->model))) ||
        (find_json_key(line, "voice") && !json_get_string(line, "voice", update->voice, sizeof(update->voice))) ||
        (find_json_key(line, "instructions") &&
         !json_get_string(line, "instructions", update->instructions, sizeof(update->instructions))) ||
        (find_json_key(line, "vadMode") &&
         !json_get_string(line, "vadMode", update->vad_mode, sizeof(update->vad_mode)))) {
        send_error(request_id, command, "Realtime config string is invalid or too long");
        usb_secure_zero(update, sizeof(*update));
        heap_caps_free(update);
        return;
    }
    (void)json_get_u32(line, "timeoutMs", &update->timeout_ms);
    if (find_json_key(line, "enabled")) {
        // 仅当 JSON 显式带 enabled 时更新。
        update->update_enabled = true;
        update->enabled = json_get_bool(line, "enabled", true);
    }
    const bool has_api_key = find_json_key(line, "apiKey") || find_json_key(line, "api_key");
    if (has_api_key &&
        !json_get_string(line, "apiKey", update->api_key, sizeof(update->api_key)) &&
        !json_get_string(line, "api_key", update->api_key, sizeof(update->api_key))) {
        send_error(request_id, command, "Realtime API Key is invalid or too long");
        usb_secure_zero(update, sizeof(*update));
        heap_caps_free(update);
        return;
    }
    if (update->api_key[0]) {
        update->update_api_key = true;
    }
    esp_err_t err = fridge_glm_realtime_set_config(update);
    // 整体清零后再释放，确保 API Key 不残留在 PSRAM allocator 的空闲块中。
    usb_secure_zero(update, sizeof(*update));
    heap_caps_free(update);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_realtime_config(request_id, command);
}

static void handle_clear_realtime_key(const char *request_id, const char *command)
{
    esp_err_t err = fridge_glm_realtime_clear_key();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_realtime_config(request_id, command);
}

static void handle_realtime_status(const char *request_id, const char *command)
{
    fridge_glm_rt_status_t st = {0};
    esp_err_t err = fridge_glm_realtime_get_status(&st);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_realtime_status_payload(&st);
    response_end();
}

static void handle_realtime_async(const char *request_id, const char *command, esp_err_t err)
{
    // 异步命令：立即 accepted；最终结果走 realtime_* 事件。
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"accepted\":true}", stdout);
    response_end();
}

static void handle_realtime_start(const char *request_id, const char *command)
{
    handle_realtime_async(request_id, command, fridge_glm_realtime_start());
}

static void handle_realtime_stop(const char *request_id, const char *command)
{
    handle_realtime_async(request_id, command, fridge_glm_realtime_stop());
}

static void handle_realtime_cancel(const char *request_id, const char *command)
{
    handle_realtime_async(request_id, command, fridge_glm_realtime_cancel());
}

static void handle_realtime_send_text(const char *line, const char *request_id, const char *command)
{
    char text[FRIDGE_GLM_RT_MAX_TEXT_LEN + 1] = {0};
    if (!json_get_string(line, "text", text, sizeof(text)) &&
        !json_get_string(line, "message", text, sizeof(text)) &&
        !json_get_string(line, "input", text, sizeof(text))) {
        send_error(request_id, command, "text/message/input is required");
        return;
    }
    handle_realtime_async(request_id, command, fridge_glm_realtime_send_text(text));
}

static void handle_tool_call(const char *line, const char *request_id, const char *command)
{
    // 调试入口：只允许通过 Tool Broker 调用；confirmed 字段代表调用方已经完成用户确认。
    fridge_tool_call_t *call = usb_large_alloc(sizeof(*call));
    fridge_tool_result_t *result = usb_large_alloc(sizeof(*result));
    if (!call || !result) {
        heap_caps_free(call);
        heap_caps_free(result);
        send_error(request_id, command, "tool call allocation failed");
        return;
    }
    memset(call, 0, sizeof(*call));
    memset(result, 0, sizeof(*result));
    strlcpy(call->call_id, request_id ? request_id : "usb", sizeof(call->call_id));
    strlcpy(call->run_id, "usb-debug", sizeof(call->run_id));
    call->user_confirmed = json_get_bool(line, "confirmed", false) || json_get_bool(line, "userConfirmed", false);
    if (!json_get_string(line, "tool", call->tool_name, sizeof(call->tool_name)) &&
        !json_get_string(line, "toolName", call->tool_name, sizeof(call->tool_name))) {
        heap_caps_free(call);
        heap_caps_free(result);
        send_error(request_id, command, "tool is required");
        return;
    }
    if (!json_get_raw_value(line, "arguments", call->arguments_json, sizeof(call->arguments_json)) &&
        !json_get_string(line, "arguments", call->arguments_json, sizeof(call->arguments_json))) {
        (void)json_get_string(line, "argsJson", call->arguments_json, sizeof(call->arguments_json));
    }
    if (!json_get_string(line, "idempotencyKey", call->idempotency_key, sizeof(call->idempotency_key))) {
        (void)json_get_string(line, "idempotency_key", call->idempotency_key, sizeof(call->idempotency_key));
    }

    const esp_err_t err = fridge_tool_broker_call(call, result);
    if (err != ESP_OK && result->status != FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION) {
        response_begin(request_id, command, false);
    } else {
        response_begin(request_id, command, true);
    }
    fputs(",\"payload\":{\"tool\":", stdout);
    json_print_escaped(call->tool_name);
    fputs(",\"confirmed\":", stdout);
    fputs(call->user_confirmed ? "true" : "false", stdout);
    fputs(",\"status\":", stdout);
    json_print_escaped(fridge_tool_result_status_name(result->status));
    fputs(",\"code\":", stdout);
    json_print_escaped(result->code);
    fputs(",\"summary\":", stdout);
    json_print_escaped(result->summary);
    printf(",\"latencyMs\":%u,\"data\":", (unsigned)result->latency_ms);
    fputs(result->data_json[0] ? result->data_json : "{}", stdout);
    fputs("}", stdout);
    if (err != ESP_OK && result->status != FRIDGE_TOOL_RESULT_PENDING_CONFIRMATION) {
        fputs(",\"error\":", stdout);
        json_print_escaped(result->summary[0] ? result->summary : esp_err_to_name(err));
    }
    heap_caps_free(call);
    heap_caps_free(result);
    response_end();
}

static void print_skill_summary(const fridge_skill_summary_t *skill)
{
    fputs("{\"skillId\":", stdout);
    json_print_escaped(skill->skill_id);
    fputs(",\"name\":", stdout);
    json_print_escaped(skill->name);
    fputs(",\"description\":", stdout);
    json_print_escaped(skill->description);
    fputs(",\"manifestPath\":", stdout);
    json_print_escaped(skill->manifest_path);
    fputs(",\"docPath\":", stdout);
    json_print_escaped(skill->doc_path);
    fputc('}', stdout);
}

static void handle_skill_status(const char *request_id, const char *command)
{
    fridge_skill_manager_status_t status = {0};
    fridge_skill_manager_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    printf(",\"seededFiles\":%u,\"skillCount\":%u,\"lastError\":",
           (unsigned)status.seeded_files,
           (unsigned)status.skill_count);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
    response_end();
}

static void handle_skill_list(const char *request_id, const char *command)
{
    fridge_skill_summary_t *skills = usb_large_alloc(sizeof(*skills) * FRIDGE_SKILL_MAX_COUNT);
    if (!skills) {
        send_error(request_id, command, "skill list allocation failed");
        return;
    }
    memset(skills, 0, sizeof(*skills) * FRIDGE_SKILL_MAX_COUNT);
    size_t count = 0;
    esp_err_t err = fridge_skill_manager_list(skills, FRIDGE_SKILL_MAX_COUNT, &count);
    if (err != ESP_OK) {
        heap_caps_free(skills);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"count\":%u,\"skills\":[", (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        if (i) fputc(',', stdout);
        print_skill_summary(&skills[i]);
    }
    fputs("]}", stdout);
    heap_caps_free(skills);
    response_end();
}

static void handle_skill_read(const char *line, const char *request_id, const char *command)
{
    char skill_id[FRIDGE_SKILL_MAX_ID_LEN] = {0};
    if (!json_get_string(line, "skillId", skill_id, sizeof(skill_id)) &&
        !json_get_string(line, "skill_id", skill_id, sizeof(skill_id))) {
        send_error(request_id, command, "skillId is required");
        return;
    }
    char *doc = usb_large_alloc(FRIDGE_SKILL_MAX_DOC_LEN + 1);
    if (!doc) {
        send_error(request_id, command, "skill doc allocation failed");
        return;
    }
    size_t n = 0;
    esp_err_t err = fridge_skill_manager_read(skill_id, doc, FRIDGE_SKILL_MAX_DOC_LEN + 1, &n);
    if (err != ESP_OK) {
        heap_caps_free(doc);
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    doc[n < FRIDGE_SKILL_MAX_DOC_LEN ? n : FRIDGE_SKILL_MAX_DOC_LEN] = '\0';
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"skillId\":");
    json_print_escaped(skill_id);
    printf(",\"bytes\":%u,\"truncated\":%s,\"doc\":", (unsigned)n, n >= FRIDGE_SKILL_MAX_DOC_LEN ? "true" : "false");
    json_print_escaped(doc);
    fputs("}", stdout);
    heap_caps_free(doc);
    response_end();
}

static void handle_skill_match(const char *line, const char *request_id, const char *command)
{
    char input[256] = {0};
    if (!json_get_string(line, "input", input, sizeof(input)) &&
        !json_get_string(line, "text", input, sizeof(input)) &&
        !json_get_string(line, "message", input, sizeof(input))) {
        send_error(request_id, command, "input/text/message is required");
        return;
    }
    fridge_skill_summary_t skill = {0};
    esp_err_t err = fridge_skill_manager_match(input, &skill);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"matched\":", stdout);
    fputs(err == ESP_OK ? "true" : "false", stdout);
    fputs(",\"skill\":", stdout);
    if (err == ESP_OK) {
        print_skill_summary(&skill);
    } else {
        fputs("null", stdout);
    }
    fputs("}", stdout);
    response_end();
}

static void handle_workflow_status(const char *request_id, const char *command)
{
    fridge_workflow_status_t status = {0};
    fridge_workflow_engine_get_status(&status);
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(status.ready ? "true" : "false", stdout);
    printf(",\"runSequence\":%u,\"lastWorkflowId\":", (unsigned)status.run_sequence);
    json_print_escaped(status.last_workflow_id);
    fputs(",\"lastRunId\":", stdout);
    json_print_escaped(status.last_run_id);
    fputs(",\"lastState\":", stdout);
    json_print_escaped(status.last_state);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status.last_error);
    fputs(",\"activeRunPath\":", stdout);
    json_print_escaped(FRIDGE_WORKFLOW_ACTIVE_RUN_PATH);
    fputs("}", stdout);
    response_end();
}

static void handle_workflow_start(const char *line, const char *request_id, const char *command)
{
    char workflow_id[FRIDGE_WORKFLOW_MAX_ID_LEN] = {0};
    char *arguments = usb_large_alloc(FRIDGE_TOOL_MAX_ARGS_LEN + 1);
    fridge_workflow_run_result_t *result = usb_large_alloc(sizeof(*result));
    if (!arguments || !result) {
        heap_caps_free(arguments);
        heap_caps_free(result);
        send_error(request_id, command, "workflow allocation failed");
        return;
    }
    memset(arguments, 0, FRIDGE_TOOL_MAX_ARGS_LEN + 1);
    memset(result, 0, sizeof(*result));
    if (!json_get_string(line, "workflowId", workflow_id, sizeof(workflow_id)) &&
        !json_get_string(line, "workflow_id", workflow_id, sizeof(workflow_id))) {
        heap_caps_free(arguments);
        heap_caps_free(result);
        send_error(request_id, command, "workflowId is required");
        return;
    }
    if (!json_get_raw_value(line, "arguments", arguments, FRIDGE_TOOL_MAX_ARGS_LEN + 1) &&
        !json_get_string(line, "arguments", arguments, FRIDGE_TOOL_MAX_ARGS_LEN + 1)) {
        (void)json_get_string(line, "argsJson", arguments, FRIDGE_TOOL_MAX_ARGS_LEN + 1);
    }
    const bool confirmed = json_get_bool(line, "confirmed", false) || json_get_bool(line, "userConfirmed", false);
    esp_err_t err = fridge_workflow_engine_start(workflow_id,
                                                 arguments[0] ? arguments : "{}",
                                                 confirmed,
                                                 result);
    const bool ok = (err == ESP_OK && result->ok) || result->state == FRIDGE_WORKFLOW_STATE_WAITING_CONFIRMATION;
    response_begin(request_id, command, ok);
    fputs(",\"payload\":{\"workflowId\":", stdout);
    json_print_escaped(result->workflow_id[0] ? result->workflow_id : workflow_id);
    fputs(",\"runId\":", stdout);
    json_print_escaped(result->run_id);
    fputs(",\"state\":", stdout);
    json_print_escaped(fridge_workflow_state_name(result->state));
    fputs(",\"confirmed\":", stdout);
    fputs(confirmed ? "true" : "false", stdout);
    fputs(",\"reply\":", stdout);
    json_print_escaped(result->reply);
    fputs(",\"result\":", stdout);
    fputs(result->result_json[0] ? result->result_json : "{}", stdout);
    fputs("}", stdout);
    if (!ok) {
        fputs(",\"error\":", stdout);
        json_print_escaped(result->error[0] ? result->error : esp_err_to_name(err));
    }
    heap_caps_free(arguments);
    heap_caps_free(result);
    response_end();
}

static void handle_tfcard_format(const char *line, const char *request_id, const char *command)
{
    char confirm[32] = {0};
    (void)json_get_string(line, "confirm", confirm, sizeof(confirm));
    if (strcmp(confirm, "ERASE_TF_CARD") != 0) {
        send_error(request_id, command, "confirm must equal ERASE_TF_CARD");
        return;
    }

    // 此调用会清空整张卡且 64GB 卡可能阻塞数十秒；必须只在精确确认口令通过后执行。
    const esp_err_t err = fridge_tfcard_format_fat32();
    if (err != ESP_OK) {
        fridge_tfcard_status_t status = {0};
        fridge_tfcard_get_status(&status);
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    handle_tfcard_status(request_id, command);
}

static void handle_tfcard_selftest(const char *request_id, const char *command)
{
    char detail[160] = {0};
    const esp_err_t err = fridge_tfcard_selftest(detail, sizeof(detail));
    if (err != ESP_OK) {
        send_error(request_id, command, detail[0] ? detail : esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"detail\":", stdout);
    json_print_escaped(detail);
    fputs("}", stdout);
    response_end();
}

static void send_tf_error(const char *request_id, const char *command, esp_err_t err)
{
    fridge_tfcard_status_t status = {0};
    fridge_tfcard_get_status(&status);
    send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
}

static void print_tf_entry(const fridge_tfcard_entry_t *entry)
{
    fputs("{\"name\":", stdout);
    json_print_escaped(entry->name);
    fputs(",\"path\":", stdout);
    json_print_escaped(entry->path);
    printf(",\"size\":%" PRIu64 ",\"mtime\":%" PRId64 ",\"isDir\":%s,\"hidden\":%s,\"protected\":%s}",
           entry->size,
           entry->mtime,
           entry->is_dir ? "true" : "false",
           entry->is_hidden ? "true" : "false",
           entry->is_protected ? "true" : "false");
}

static void print_tf_transfer(const fridge_tfcard_transfer_status_t *status)
{
    printf("{\"active\":%s,\"session\":%lu,\"direction\":\"%s\",\"totalSize\":%" PRIu64
           ",\"offset\":%" PRIu64 ",\"chunkSize\":%u,\"path\":",
           status->active ? "true" : "false",
           (unsigned long)status->session_id,
           status->direction == FRIDGE_TFCARD_TRANSFER_WRITE ? "write" : "read",
           status->total_size,
           status->offset,
           (unsigned)FRIDGE_TFCARD_TRANSFER_CHUNK);
    json_print_escaped(status->path);
    fputs("}", stdout);
}

static void handle_tf_list(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    uint32_t cursor = 0;
    uint32_t limit = 16;
    (void)json_get_string(line, "path", path, sizeof(path));
    (void)json_get_u32(line, "cursor", &cursor);
    (void)json_get_u32(line, "limit", &limit);
    if (limit == 0 || limit > FRIDGE_TFCARD_LIST_PAGE_MAX) limit = FRIDGE_TFCARD_LIST_PAGE_MAX;

    fridge_tfcard_list_result_t *result = usb_large_alloc(sizeof(*result));
    if (!result) {
        send_error(request_id, command, "TF list allocation failed");
        return;
    }
    const esp_err_t err = fridge_tfcard_list(path, cursor, limit, result);
    if (err != ESP_OK) {
        heap_caps_free(result);
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"path\":", stdout);
    json_print_escaped(path);
    printf(",\"cursor\":%lu,\"nextCursor\":%lu,\"hasMore\":%s,\"entries\":[",
           (unsigned long)cursor, (unsigned long)result->next_cursor, result->has_more ? "true" : "false");
    for (size_t i = 0; i < result->count; i++) {
        if (i) putchar(',');
        print_tf_entry(&result->entries[i]);
    }
    fputs("]}", stdout);
    response_end();
    heap_caps_free(result);
}

static void handle_tf_stat(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    if (!json_get_string(line, "path", path, sizeof(path))) {
        send_error(request_id, command, "path is required");
        return;
    }
    fridge_tfcard_entry_t entry = {0};
    const esp_err_t err = fridge_tfcard_stat(path, &entry);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tf_entry(&entry);
    response_end();
}

static void handle_tf_mkdir(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    if (!json_get_string(line, "path", path, sizeof(path))) {
        send_error(request_id, command, "path is required");
        return;
    }
    const esp_err_t err = fridge_tfcard_mkdir(path);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"path\":", stdout);
    json_print_escaped(path);
    fputs("}", stdout);
    response_end();
}

static void handle_tf_move(const char *line, const char *request_id, const char *command)
{
    char from[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    char to[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    if (!json_get_string(line, "from", from, sizeof(from)) ||
        !json_get_string(line, "to", to, sizeof(to))) {
        send_error(request_id, command, "from and to are required");
        return;
    }
    const esp_err_t err = fridge_tfcard_move(from, to, json_get_bool(line, "overwrite", false));
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"path\":", stdout);
    json_print_escaped(to);
    fputs("}", stdout);
    response_end();
}

static void handle_tf_delete(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    char trash_id[64] = "";
    if (!json_get_string(line, "path", path, sizeof(path))) {
        send_error(request_id, command, "path is required");
        return;
    }
    const esp_err_t err = fridge_tfcard_delete_to_trash(path,
                                                        json_get_bool(line, "recursive", true),
                                                        trash_id,
                                                        sizeof(trash_id));
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"trashId\":", stdout);
    json_print_escaped(trash_id);
    fputs("}", stdout);
    response_end();
}

static void handle_tf_restore(const char *line, const char *request_id, const char *command)
{
    char trash_id[64] = "";
    char restored[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    if (!json_get_string(line, "trashId", trash_id, sizeof(trash_id))) {
        send_error(request_id, command, "trashId is required");
        return;
    }
    const esp_err_t err = fridge_tfcard_restore(trash_id,
                                                json_get_bool(line, "overwrite", false),
                                                restored,
                                                sizeof(restored));
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"path\":", stdout);
    json_print_escaped(restored);
    fputs("}", stdout);
    response_end();
}

static void handle_tf_trash_empty(const char *line, const char *request_id, const char *command)
{
    char confirm[32] = "";
    (void)json_get_string(line, "confirm", confirm, sizeof(confirm));
    if (strcmp(confirm, "EMPTY_TF_TRASH") != 0) {
        send_error(request_id, command, "confirm must equal EMPTY_TF_TRASH");
        return;
    }
    const esp_err_t err = fridge_tfcard_empty_trash();
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"emptied\":true}", stdout);
    response_end();
}

static void handle_tf_text_read(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    if (!json_get_string(line, "path", path, sizeof(path))) {
        send_error(request_id, command, "path is required");
        return;
    }
    char *text = usb_large_alloc(FRIDGE_TFCARD_TEXT_MAX + 1);
    if (!text) {
        send_error(request_id, command, "TF text allocation failed");
        return;
    }
    size_t size = 0;
    const esp_err_t err = fridge_tfcard_text_read(path, text, FRIDGE_TFCARD_TEXT_MAX + 1, &size);
    if (err != ESP_OK) {
        heap_caps_free(text);
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"size\":%u,\"text\":", (unsigned)size);
    json_print_escaped(text);
    fputs("}", stdout);
    response_end();
    heap_caps_free(text);
}

static void handle_tf_text_write(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    if (!json_get_string(line, "path", path, sizeof(path)) ||
        !json_get_string(line, "text", s_tf_text_input, sizeof(s_tf_text_input))) {
        send_error(request_id, command, "path and text are required; use chunk transfer for larger text");
        return;
    }
    const esp_err_t err = fridge_tfcard_text_write(path,
                                                   s_tf_text_input,
                                                   strlen(s_tf_text_input),
                                                   json_get_bool(line, "overwrite", true));
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"size\":%u}", (unsigned)strlen(s_tf_text_input));
    response_end();
}

static void handle_tf_transfer_status(const char *request_id, const char *command)
{
    fridge_tfcard_transfer_status_t status = {0};
    const esp_err_t err = fridge_tfcard_transfer_status(&status);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tf_transfer(&status);
    response_end();
}

static void handle_tf_transfer_begin(const char *line, const char *request_id, const char *command)
{
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    char direction[16] = "";
    uint64_t total_size = 0;
    (void)json_get_u64(line, "totalSize", &total_size);
    if (total_size == 0) (void)json_get_u64(line, "total_size", &total_size);
    if (!json_get_string(line, "path", path, sizeof(path)) ||
        !json_get_string(line, "direction", direction, sizeof(direction))) {
        send_error(request_id, command, "path and direction are required");
        return;
    }
    fridge_tfcard_transfer_direction_t dir;
    if (strcmp(direction, "write") == 0 || strcmp(direction, "upload") == 0) {
        dir = FRIDGE_TFCARD_TRANSFER_WRITE;
        if (total_size == 0) {
            send_error(request_id, command, "totalSize is required for upload");
            return;
        }
    } else if (strcmp(direction, "read") == 0 || strcmp(direction, "download") == 0) {
        dir = FRIDGE_TFCARD_TRANSFER_READ;
    } else {
        send_error(request_id, command, "direction must be read/download or write/upload");
        return;
    }
    fridge_tfcard_transfer_status_t status = {0};
    const esp_err_t err = fridge_tfcard_transfer_begin(path,
                                                       dir,
                                                       total_size,
                                                       json_get_bool(line, "overwrite", false),
                                                       &status);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tf_transfer(&status);
    response_end();
}

static void handle_tf_transfer_chunk(const char *line, const char *request_id, const char *command)
{
    uint32_t session = 0;
    uint64_t offset = 0;
    if (!json_get_u32(line, "session", &session) || !json_get_u64(line, "offset", &offset)) {
        send_error(request_id, command, "session and offset are required");
        return;
    }
    fridge_tfcard_transfer_status_t status = {0};
    esp_err_t err = fridge_tfcard_transfer_status(&status);
    if (err != ESP_OK || !status.active || status.session_id != session) {
        send_error(request_id, command, "transfer session is not active");
        return;
    }
    if (status.direction == FRIDGE_TFCARD_TRANSFER_WRITE) {
        uint32_t crc32 = 0;
        size_t length = 0;
        if (!json_get_u32(line, "crc32", &crc32) ||
            !json_get_string(line, "data_b64", s_tf_b64, sizeof(s_tf_b64)) ||
            !base64_decode_strict(s_tf_b64, s_tf_chunk, sizeof(s_tf_chunk), &length)) {
            send_error(request_id, command, "crc32 and valid data_b64 are required");
            return;
        }
        err = fridge_tfcard_transfer_write(session, offset, s_tf_chunk, length, crc32);
        if (err != ESP_OK) {
            send_tf_error(request_id, command, err);
            return;
        }
        (void)fridge_tfcard_transfer_status(&status);
        response_begin(request_id, command, true);
        printf(",\"payload\":{\"nextOffset\":%" PRIu64 "}", status.offset);
        response_end();
        return;
    }

    size_t length = 0;
    uint32_t crc32 = 0;
    err = fridge_tfcard_transfer_read(session, offset, s_tf_chunk, sizeof(s_tf_chunk), &length, &crc32);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    char *encoded = length ? base64_encode_alloc(s_tf_chunk, length) : NULL;
    if (length && !encoded) {
        send_error(request_id, command, "TF chunk base64 allocation failed");
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"offset\":%" PRIu64 ",\"length\":%u,\"crc32\":%lu,\"eof\":%s,\"data_b64\":\"",
           offset, (unsigned)length, (unsigned long)crc32,
           offset + length >= status.total_size ? "true" : "false");
    if (encoded) fputs(encoded, stdout);
    fputs("\"}", stdout);
    response_end();
    if (encoded) heap_caps_free(encoded);
}

static void handle_tf_transfer_finish(const char *line, const char *request_id, const char *command, bool aborting)
{
    uint32_t session = 0;
    if (!json_get_u32(line, "session", &session)) {
        send_error(request_id, command, "session is required");
        return;
    }
    const esp_err_t err = aborting ? fridge_tfcard_transfer_abort(session)
                                   : fridge_tfcard_transfer_end(session);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"session\":%lu,\"state\":\"%s\"}",
           (unsigned long)session, aborting ? "aborted" : "complete");
    response_end();
}

static void handle_tf_access_mode(const char *line, const char *request_id, const char *command, bool update)
{
    if (update) {
        char confirm[48] = "";
        (void)json_get_string(line, "confirm", confirm, sizeof(confirm));
        const esp_err_t err = fridge_tfcard_set_expert_mode(json_get_bool(line, "enabled", false), confirm);
        if (err != ESP_OK) {
            send_tf_error(request_id, command, err);
            return;
        }
    }
    bool enabled = false;
    const esp_err_t err = fridge_tfcard_get_expert_mode(&enabled);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"expertMode\":%s,\"protectedRoot\":\"%s\"}",
           enabled ? "true" : "false", FRIDGE_TFCARD_SYSTEM_DIR);
    response_end();
}

static void handle_tf_mount(const char *request_id, const char *command, bool mount)
{
    const esp_err_t err = mount ? fridge_tfcard_mount() : fridge_tfcard_unmount();
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    handle_tfcard_status(request_id, command);
}

static void handle_tf_health(const char *request_id, const char *command)
{
    char detail[160] = "";
    const esp_err_t err = fridge_tfcard_selftest(detail, sizeof(detail));
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"ioOk\":true,\"smartAvailable\":false,\"detail\":", stdout);
    json_print_escaped(detail);
    fputs("}", stdout);
    response_end();
}

static void handle_tf_benchmark(const char *line, const char *request_id, const char *command)
{
    uint32_t bytes = 4U * 1024U * 1024U;
    (void)json_get_u32(line, "bytes", &bytes);
    if (bytes < 64U * 1024U || bytes > 16U * 1024U * 1024U) {
        send_error(request_id, command, "bytes must be between 65536 and 16777216");
        return;
    }
    fridge_tfcard_benchmark_result_t result = {0};
    const esp_err_t err = fridge_tfcard_benchmark(bytes, &result);
    if (err != ESP_OK) {
        send_tf_error(request_id, command, err);
        return;
    }
    response_begin(request_id, command, true);
    printf(",\"payload\":{\"bytes\":%" PRIu64 ",\"writeMs\":%lu,\"readMs\":%lu,"
           "\"writeKibS\":%lu,\"readKibS\":%lu}",
           result.bytes,
           (unsigned long)result.write_ms,
           (unsigned long)result.read_ms,
           (unsigned long)result.write_kib_s,
           (unsigned long)result.read_kib_s);
    response_end();
}

static void print_tf_http_status(const fridge_tf_http_status_t *status, bool include_token)
{
    printf("{\"serverRunning\":%s,\"authorized\":%s,\"session\":%lu,\"port\":%u,"
           "\"expiresInSeconds\":%lu,\"url\":",
           status->server_running ? "true" : "false",
           status->authorized ? "true" : "false",
           (unsigned long)status->session_id,
           (unsigned)status->port,
           (unsigned long)status->expires_in_seconds);
    json_print_escaped(status->url);
    if (include_token) {
        fputs(",\"token\":", stdout);
        json_print_escaped(status->token);
    }
    fputs(",\"lastError\":", stdout);
    json_print_escaped(status->last_error);
    fputs("}", stdout);
}

static void handle_tf_http(const char *request_id, const char *command, int action)
{
    fridge_tf_http_status_t status = {0};
    esp_err_t err = ESP_OK;
    if (action > 0) err = fridge_tf_http_enable(&status);
    else if (action < 0) err = fridge_tf_http_disable();
    if (err == ESP_OK && action <= 0) err = fridge_tf_http_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, status.last_error[0] ? status.last_error : esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    print_tf_http_status(&status, action > 0);
    response_end();
}

static void handle_get_status(const char *request_id, const char *command)
{
    fridge_device_status_t status = {0};
    const esp_err_t err = fridge_diagnostics_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"model\":", stdout);
    json_print_escaped(status.model);
    fputs(",\"chip\":", stdout);
    json_print_escaped(status.chip);
    fputs(",\"firmware\":", stdout);
    json_print_escaped(status.firmware);
    fputs(",\"uptime\":", stdout);
    json_print_escaped(status.uptime);
    fputs(",\"flash\":", stdout);
    json_print_escaped(status.flash);
    fputs(",\"psram\":", stdout);
    json_print_escaped(status.psram);
    {
        fridge_sht4x_snapshot_t env = {0};
        fridge_sht4x_get_snapshot(&env);
        if (env.valid) {
            printf(",\"freeHeapKb\":%lu,\"minHeapKb\":%lu,\"freePsramKb\":%lu"
                   ",\"temperatureC\":%.1f,\"humidityRh\":%.0f,\"envValid\":true",
                   (unsigned long)status.free_heap_kb,
                   (unsigned long)status.min_heap_kb,
                   (unsigned long)status.free_psram_kb,
                   env.temperature_c, env.humidity_rh);
        } else {
            printf(",\"freeHeapKb\":%lu,\"minHeapKb\":%lu,\"freePsramKb\":%lu"
                   ",\"temperatureC\":null,\"humidityRh\":null,\"envValid\":false",
                   (unsigned long)status.free_heap_kb,
                   (unsigned long)status.min_heap_kb,
                   (unsigned long)status.free_psram_kb);
        }
    }
    fputs(",\"wifi\":", stdout);
    json_print_escaped(status.wifi_health);
    fputs(",\"mqtt\":", stdout);
    json_print_escaped(status.mqtt_health);
    fputs(",\"usb\":", stdout);
    json_print_escaped(status.usb_health);
    fputs(",\"ota\":", stdout);
    json_print_escaped(status.ota_health);
    fputs(",\"page\":", stdout);
    json_print_escaped(status.page);
    fputs(",\"powerNote\":", stdout);
    json_print_escaped(status.power_note);
    fputs(",\"tasks\":[", stdout);
    for (size_t i = 0; i < status.task_count; i++) {
        if (i > 0) {
            putchar(',');
        }
        fputs("{\"name\":", stdout);
        json_print_escaped(status.tasks[i].name);
        fputs(",\"priority\":", stdout);
        json_print_escaped(status.tasks[i].priority);
        fputs(",\"state\":", stdout);
        json_print_escaped(status.tasks[i].state);
        fputs(",\"heartbeat\":", stdout);
        json_print_escaped(status.tasks[i].heartbeat);
        fputs("}", stdout);
    }
    fputs("]}", stdout);
    response_end();
}

static void handle_get_diagnostics(const char *request_id, const char *command)
{
    fridge_diagnostic_snapshot_t diag = {0};
    fridge_diagnostics_log_status_t log_status = {0};
    const esp_err_t err = fridge_diagnostics_get_snapshot(&diag);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    // USB 只返回日志/coredump 状态与计数，不读取受保护目录中的日志正文或 ELF 内容。
    (void)fridge_diagnostics_log_get_status(&log_status);

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"psram\":", stdout);
    json_print_escaped(diag.psram);
    fputs(",\"flashPartition\":", stdout);
    json_print_escaped(diag.flash_partition);
    fputs(",\"littlefs\":", stdout);
    json_print_escaped(diag.littlefs);
    fputs(",\"otaSlot\":", stdout);
    json_print_escaped(diag.ota_slot);
    fputs(",\"resetReason\":", stdout);
    json_print_escaped(diag.reset_reason);
    printf(",\"resetReasonCode\":%lu,\"bootSequence\":%lu"
           ",\"brownoutCount\":%lu,\"panicCount\":%lu"
           ",\"taskWdtCount\":%lu,\"interruptWdtCount\":%lu"
           ",\"watchdogOtherCount\":%lu,\"watchdogCount\":%lu"
           ",\"plannedRestartReason\":",
           (unsigned long)diag.reset_reason_code,
           (unsigned long)diag.boot_sequence,
           (unsigned long)diag.brownout_count,
           (unsigned long)diag.panic_count,
           (unsigned long)diag.task_wdt_count,
           (unsigned long)diag.interrupt_wdt_count,
           (unsigned long)diag.watchdog_other_count,
           (unsigned long)diag.watchdog_count);
    json_print_escaped(diag.planned_restart_reason);
    fputs(",\"lastError\":", stdout);
    json_print_escaped(diag.last_error);
    fputs(",\"logger\":{\"installed\":", stdout);
    fputs(log_status.installed ? "true" : "false", stdout);
    fputs(",\"running\":", stdout);
    fputs(log_status.running ? "true" : "false", stdout);
    fputs(",\"tfReady\":", stdout);
    fputs(log_status.tf_ready ? "true" : "false", stdout);
    printf(",\"currentFile\":%u,\"droppedRecords\":%lu"
           ",\"droppedBytes\":%llu,\"writtenBytes\":%llu"
           ",\"writeErrors\":%lu,\"currentPath\":",
           (unsigned)log_status.current_file,
           (unsigned long)log_status.dropped_records,
           (unsigned long long)log_status.dropped_bytes,
           (unsigned long long)log_status.written_bytes,
           (unsigned long)log_status.write_errors);
    json_print_escaped(log_status.current_path);
    fputs(",\"coredump\":{\"pending\":", stdout);
    fputs(log_status.coredump_pending ? "true" : "false", stdout);
    fputs(",\"exported\":", stdout);
    fputs(log_status.coredump_exported ? "true" : "false", stdout);
    printf(",\"size\":%lu,\"path\":",
           (unsigned long)log_status.coredump_size);
    json_print_escaped(log_status.coredump_path);
    fputs(",\"reason\":", stdout);
    json_print_escaped(log_status.coredump_reason);
    fputs(",\"error\":", stdout);
    json_print_escaped(log_status.coredump_error);
    fputs("}},\"riskItems\":["
          "\"TR230S 屏供电为 5V、逻辑为 3.3V，禁止把 20pin FPC 直接插入板载 SPI 座。\","
          "\"ES8311 与 OV5647 共用 GPIO28/29，后续摄像头接入时必须分时占用 I2C。\","
          "\"TF 卡格式化会清空整卡，只接受 ERASE_TF_CARD 精确确认口令。\"]}",
          stdout);
    response_end();
}

static void handle_tf_export_diagnostics(const char *request_id, const char *command)
{
    // 诊断包只写入非敏感状态：不包含 Wi-Fi 密码、API Key、MQTT 密钥或明文凭据。
    fridge_device_status_t device = {0};
    fridge_diagnostic_snapshot_t diag = {0};
    fridge_network_status_t network = {0};
    fridge_tfcard_status_t tf = {0};
    if (fridge_diagnostics_get_status(&device) != ESP_OK ||
        fridge_diagnostics_get_snapshot(&diag) != ESP_OK ||
        fridge_network_get_status(&network) != ESP_OK) {
        send_error(request_id, command, "failed to collect diagnostics");
        return;
    }
    fridge_tfcard_get_status(&tf);
    char *json = usb_large_alloc(2048);
    if (!json) {
        send_error(request_id, command, "no memory for diagnostics export");
        return;
    }
    int length = snprintf(json, 2048,
        "{\n  \"schemaVersion\": 1,\n  \"device\": {\"chip\": \"ESP32-P4NRW32\", \"freeHeapKb\": %lu, \"minHeapKb\": %lu, \"freePsramKb\": %lu},\n"
        "  \"network\": {\"hostedReady\": %s, \"connected\": %s, \"internetReady\": %s, \"rssi\": %d},\n"
        "  \"tf\": {\"mounted\": %s, \"capacityMb\": %" PRIu64 ", \"freeMb\": %" PRIu64 ", \"expertMode\": %s},\n"
        "  \"diagnostics\": {\"brownoutCount\": %lu, \"watchdogCount\": %lu},\n"
        "  \"logs\": [{\"source\": \"firmware\", \"level\": \"info\", \"message\": \"P4 minimal JSON protocol running\"}]\n}\n",
        (unsigned long)device.free_heap_kb, (unsigned long)device.min_heap_kb,
        (unsigned long)device.free_psram_kb,
        network.hosted_ready ? "true" : "false", network.connected ? "true" : "false",
        network.internet_ready ? "true" : "false", (int)network.rssi,
        tf.mounted ? "true" : "false", tf.capacity_mb, tf.free_mb,
        tf.expert_mode ? "true" : "false", (unsigned long)diag.brownout_count,
        (unsigned long)diag.watchdog_count);
    if (length <= 0 || length >= 2048) {
        free(json);
        send_error(request_id, command, "diagnostics export too large");
        return;
    }
    char file_name[48];
    snprintf(file_name, sizeof(file_name), "diagnostics-%lu.json", (unsigned long)xTaskGetTickCount());
    char path[FRIDGE_TFCARD_MAX_PATH + 1] = "";
    const esp_err_t err = fridge_tfcard_write_diagnostic_export(file_name, json, (size_t)length,
                                                                path, sizeof(path));
    free(json);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"path\":", stdout);
    json_print_escaped(path);
    printf(",\"bytes\":%d}", length);
    response_end();
}

// GT911 触摸诊断：先主动读一次触点再回快照，便于 Web/串口在手指按下时抓坐标。
static void handle_touch_status(const char *request_id, const char *command)
{
#if CONFIG_FRIDGE_P4_GT911_ENABLE
    fridge_touch_point_t point = {0};
    (void)fridge_touch_gt911_read(&point);

    fridge_touch_gt911_status_t st = {0};
    (void)fridge_touch_gt911_get_status(&st);

    response_begin(request_id, command, st.ready);
    fputs(",\"payload\":{\"ready\":", stdout);
    fputs(st.ready ? "true" : "false", stdout);
    printf(",\"i2cAddress\":%u,\"productId\":", (unsigned)st.i2c_address);
    json_print_escaped(st.product_id);
    printf(",\"statusReg\":%u,\"rawX\":%u,\"rawY\":%u,\"x\":%u,\"y\":%u,\"points\":%u,\"pressed\":%s,\"readErr\":",
           (unsigned)st.last_status,
           (unsigned)st.last_raw_x,
           (unsigned)st.last_raw_y,
           (unsigned)st.last_x,
           (unsigned)st.last_y,
           (unsigned)st.last_points,
           st.last_pressed ? "true" : "false");
    json_print_escaped(esp_err_to_name(st.last_read_err));
    fputs(",\"pins\":{\"int\":15,\"scl\":16,\"sda\":17,\"rst\":18,\"controller\":\"GT911\"}", stdout);
    fputs("}", stdout);
    if (!st.ready) {
        fputs(",\"error\":", stdout);
        json_print_escaped("GT911 not ready; check 26-pin FPC touch lines and power");
    }
    response_end();
#else
    send_error(request_id, command, "GT911 touch is disabled in this firmware profile");
#endif
}

// 在触摸 I2C 上探测 0x14/0x5D，确认 WKS70 GT911 是否在线。
static void handle_touch_scan(const char *request_id, const char *command)
{
#if CONFIG_FRIDGE_P4_GT911_ENABLE
    uint8_t mask = 0;
    uint8_t addrs[4] = {0};
    size_t count = 0;
    const esp_err_t err = fridge_touch_gt911_scan(&mask, addrs, sizeof(addrs), &count);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, count > 0);
    fputs(",\"payload\":{\"foundMask\":", stdout);
    printf("%u,\"count\":%u,\"addresses\":[", (unsigned)mask, (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        if (i) putchar(',');
        printf("%u", (unsigned)addrs[i]);
    }
    fputs("],\"expectPrimary\":20,\"expectBackup\":93,\"pins\":{\"scl\":16,\"sda\":17}}", stdout);
    if (count == 0) {
        fputs(",\"error\":", stdout);
        json_print_escaped("no GT911 at 0x14/0x5D; check INT/SCL/SDA/RST and 3.3V pull-ups");
    }
    response_end();
#else
    send_error(request_id, command, "GT911 touch is disabled in this firmware profile");
#endif
}

// MQ-5 状态枚举到协议字符串的映射；unknown 为防御性 fallback，避免未知枚举值破坏 JSON。
static const char *mq5_state_to_string(uint8_t state)
{
    switch (state) {
    case FRIDGE_MQ5_STATE_DISABLED: return "disabled";
    case FRIDGE_MQ5_STATE_ERROR: return "error";
    case FRIDGE_MQ5_STATE_WARMING_UP: return "warming_up";
    case FRIDGE_MQ5_STATE_NEEDS_BURN_IN: return "needs_burn_in";
    case FRIDGE_MQ5_STATE_READY_TO_CALIBRATE: return "ready_to_calibrate";
    case FRIDGE_MQ5_STATE_MONITORING: return "monitoring";
    default: return "unknown";
    }
}

// P4 MPU6050 诊断快照：USB 只读内存，不在 Web 的 300ms 轮询路径中直接访问 I2C。
// 设备离线时仍返回 ok=true 和明确的 imuError，便于区分“命令不支持”和“硬件未就绪”。
// 光照字段来自 GPIO19 光敏 ADC1_CH3，同为内存快照，USB 层不直接读 ADC。
static void handle_get_sensors(const char *request_id, const char *command)
{
    fridge_sensor_snapshot_t sensors = {0};
    esp_err_t snapshot_err = fridge_sensors_get_snapshot(&sensors);
    if (snapshot_err != ESP_OK) sensors.imu_error = snapshot_err;

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"profile\":\"p4_mpu6050\",\"imuReady\":", stdout);
    fputs(sensors.imu_ready ? "true" : "false", stdout);
    fputs(",\"baselineReady\":", stdout);
    fputs(sensors.baseline_ready ? "true" : "false", stdout);
    printf(",\"imuAddress\":%u,\"imuWhoAmI\":%u,\"imuError\":%d,"
           "\"consecutiveFailures\":%lu,\"sampleCount\":%lu,"
           "\"i2cBus\":0,\"sclGpio\":16,\"sdaGpio\":17,\"sampleRateHz\":50,"
           "\"imuErrorName\":",
           (unsigned)sensors.imu_address,
           (unsigned)sensors.imu_who_am_i,
           sensors.imu_error,
           (unsigned long)sensors.consecutive_failures,
           (unsigned long)sensors.sample_count);
    json_print_escaped(esp_err_to_name(sensors.imu_error));
    // 光照：GPIO19 光敏 ADC1_CH3，实机极性 raw_high_dark（数值越高越暗）。
    // lux 仅为兼容旧 Web 面板的相对亮度 0-1023（恒等于 lightValue10bit），不是物理 lux，禁止按 lux 解读。
    printf(",\"lightReady\":%s,\"lightRaw12bit\":%u,\"lightValue10bit\":%u,"
           "\"lightPercent\":%u,\"lightDelta\":%d,\"lightPolarity\":",
           sensors.light_ready ? "true" : "false",
           (unsigned)sensors.light_raw12bit,
           (unsigned)sensors.light_value10bit,
           (unsigned)sensors.light_percent,
           (int)sensors.light_delta);
    json_print_escaped(sensors.light_polarity ? sensors.light_polarity
                                              : FRIDGE_LIGHT_POLARITY_RAW_HIGH_DARK);
    printf(",\"lux\":%u,\"lightError\":%d",
           (unsigned)sensors.light_value10bit,
           (int)sensors.light_error);
    // MQ-5 燃气模块（仅 AO 接入，DO 物理悬空）：GPIO20 ADC1_CH4，经 22k/33k 分压 + 100nF。
    // 全部为相对 ADC 读数，不是 ppm；当前不触发正式燃气报警，不能替代认证报警器。
    printf(",\"mq5State\":");
    json_print_escaped(mq5_state_to_string((uint8_t)sensors.mq5_state));
    printf(",\"mq5Ready\":%s,\"mq5Error\":%d,\"mq5Raw12bit\":%u,\"mq5Value10bit\":%u,"
           "\"mq5AdcPercent\":%u,\"mq5SampleCount\":%lu,\"mq5UpdatedAtMs\":%lld,"
           "\"mq5WarmupElapsedSec\":%lu,\"mq5DailyWarmupComplete\":%s,\"mq5BurnInConfirmed\":%s,"
           "\"mq5BaselineReady\":%s,\"mq5BaselineRaw12bit\":%u,\"mq5DeltaFromBaseline\":%d",
           sensors.mq5_ready ? "true" : "false",
           (int)sensors.mq5_error,
           (unsigned)sensors.mq5_raw_12bit,
           (unsigned)sensors.mq5_value_10bit,
           (unsigned)sensors.mq5_adc_percent,
           (unsigned long)sensors.mq5_sample_count,
           (long long)sensors.mq5_updated_at_ms,
           (unsigned long)sensors.mq5_warmup_elapsed_sec,
           sensors.mq5_daily_warmup_complete ? "true" : "false",
           sensors.mq5_burn_in_confirmed ? "true" : "false",
           sensors.mq5_baseline_ready ? "true" : "false",
           (unsigned)sensors.mq5_baseline_raw_12bit,
           (int)sensors.mq5_delta_from_baseline);
    if (sensors.sample_count > 0) {
        printf(",\"accelXG\":%.4f,\"accelYG\":%.4f,\"accelZG\":%.4f,"
               "\"gyroXDps\":%.4f,\"gyroYDps\":%.4f,\"gyroZDps\":%.4f,"
               "\"imuTemperatureC\":%.2f,\"pitchDeg\":%.2f,\"rollDeg\":%.2f,"
               "\"angleDelta\":%.2f,\"vibrationPeak\":%.4f",
               (double)sensors.accel_x_g,
               (double)sensors.accel_y_g,
               (double)sensors.accel_z_g,
               (double)sensors.gyro_x_dps,
               (double)sensors.gyro_y_dps,
               (double)sensors.gyro_z_dps,
               (double)sensors.imu_temperature_c,
               (double)sensors.pitch_deg,
               (double)sensors.roll_deg,
               (double)sensors.angle_delta,
               (double)sensors.vibration_peak);
    }
    fputs(",\"updatedAt\":", stdout);
    if (sensors.sample_count > 0) {
        char updated_at[40];
        snprintf(updated_at, sizeof(updated_at), "%lld ms",
                 (long long)sensors.updated_at_ms);
        json_print_escaped(updated_at);
    } else {
        json_print_escaped("waiting for MPU6050");
    }
    fputs("}", stdout);
    response_end();
}

// MQ-5 首次老化确认。confirm 必须精确等于固定口令，防止 Web 面板误触；
// find_json_key 为全行搜索，兼容 {arguments:{confirm}} 与 {payload:{confirm}} 两种请求形态。
static void handle_mq5_confirm_burn_in(const char *line, const char *request_id,
                                       const char *command)
{
    char confirm[32] = {0};
    if (!json_get_string(line, "confirm", confirm, sizeof(confirm)) ||
        strcmp(confirm, "MQ5_BURN_IN_COMPLETE") != 0) {
        send_error(request_id, command,
                   "ESP_ERR_INVALID_ARG: confirm=MQ5_BURN_IN_COMPLETE is required; burn-in not confirmed");
        return;
    }
    esp_err_t err = fridge_sensors_mq5_confirm_burn_in();
    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE
                       ? "ESP_ERR_INVALID_STATE: daily warm-up not complete / no valid sample / burn-in already confirmed"
                       : esp_err_to_name(err));
        return;
    }
    handle_get_sensors(request_id, command);
}

// MQ-5 采集洁净空气基线。仅接受精确口令，防止在非洁净环境误存基线。
static void handle_mq5_capture_baseline(const char *line, const char *request_id,
                                        const char *command)
{
    char confirm[32] = {0};
    if (!json_get_string(line, "confirm", confirm, sizeof(confirm)) ||
        strcmp(confirm, "CAPTURE_MQ5_CLEAN_AIR") != 0) {
        send_error(request_id, command,
                   "ESP_ERR_INVALID_ARG: confirm=CAPTURE_MQ5_CLEAN_AIR is required");
        return;
    }
    esp_err_t err = fridge_sensors_mq5_capture_baseline();
    if (err != ESP_OK) {
        send_error(request_id, command,
                   err == ESP_ERR_INVALID_STATE
                       ? "ESP_ERR_INVALID_STATE: warm-up / burn-in / 64-sample window not ready"
                       : esp_err_to_name(err));
        return;
    }
    handle_get_sensors(request_id, command);
}

// MQ-5 重置老化与基线。仅接受精确口令，防止误清校准状态。
static void handle_mq5_reset_calibration(const char *line, const char *request_id,
                                         const char *command)
{
    char confirm[32] = {0};
    if (!json_get_string(line, "confirm", confirm, sizeof(confirm)) ||
        strcmp(confirm, "RESET_MQ5_CALIBRATION") != 0) {
        send_error(request_id, command,
                   "ESP_ERR_INVALID_ARG: confirm=RESET_MQ5_CALIBRATION is required");
        return;
    }
    esp_err_t err = fridge_sensors_mq5_reset_calibration();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_get_sensors(request_id, command);
}

static void handle_get_pins(const char *request_id, const char *command)
{
    // 此表只描述 P4 CB V1.2 当前最小移植，不再暴露 S3 旧引脚。
    static const char live_pins[] =
        "["
        "{\"gpio\":\"GPIO7/8/9\",\"signal\":\"FY_SP703_I2S1_RX\",\"usage\":\"FY-SP703 noise-cancelling microphone\",\"level\":\"caution\",\"note\":\"LRCK=7,BCLK=8,SDIO=9；FY-SP703 为 I2S 主机，P4 配置 I2S1 从机，当前烧写固件为 16kHz/32-bit/Philips/双时隙；Pin15 接 3.3V，Pin14 5V 悬空。旧 TR230S QSPI 排障路径不可同时启用。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO15/18\",\"signal\":\"TOUCH_GT911_CTRL\",\"usage\":\"WKS70 MIPI panel GT911\",\"level\":\"caution\",\"note\":\"INT=15、RST=18；仅 GT911 使用。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO16/17\",\"signal\":\"I2C0_SHARED\",\"usage\":\"GT911 + SHT40 + MPU6050\",\"level\":\"caution\",\"note\":\"SCL=16、SDA=17；GT911 创建唯一总线，MPU6050 为 0x68/100kHz/INT悬空/AD0接地。全部上拉只能到3.3V；MPU模块自带2.2k上拉，接入后检查并联等效阻值。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO2/3/4/5/6/11/28/29\",\"signal\":\"ES8311_AUDIO\",\"usage\":\"onboard microphone codec\",\"level\":\"caution\",\"note\":\"MCLK=2,DSDIN(P4 DOUT)=3,BCLK=4,ASDOUT(P4 DIN)=5,WS=6,PA_CTRL=11,SDA=28,SCL=29；GPIO28/29 与摄像头 SCCB 分时复用。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO39/40/41/42/43/44/45\",\"signal\":\"TF_SDMMC\",\"usage\":\"onboard TF 4-bit\",\"level\":\"caution\",\"note\":\"D0-D3=39-42,CLK=43,CMD=44,PWR_EN=45 低有效；64GB 卡需显式确认后才可格式化 FAT32。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO47-54\",\"signal\":\"C6_SDIO\",\"usage\":\"onboard ESP32-C6 Wi-Fi\",\"level\":\"danger\",\"note\":\"板载专用，不接外设；Wi-Fi 必须经 esp_hosted/esp_wifi_remote，并启用板级 SDIO 电源管理。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO37/38\",\"signal\":\"UART0\",\"usage\":\"CH343P flash/monitor/JSON Lines\",\"level\":\"safe\",\"note\":\"115200 baud；烧录、monitor 与 Web Serial 不可同时占用同一 COM 口。\",\"readonly\":true},"
        "{\"gpio\":\"P6-29/GPIO20\",\"signal\":\"MQ5_AO_ADC1_CH4\",\"usage\":\"MQ-5 燃气模块 AO 模拟输入\",\"level\":\"caution\",\"note\":\"VCC=P6 SYS_5V（待实测电流/压降/温升）；AO 经 22k/33k 分压 + 100nF 到 GPIO20/ADC1_CH4；GND 共地；DO=N/C。禁止 5V 直入 P4 GPIO；当前仅相对 ADC，非 ppm，不触发正式燃气报警。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO0/1/35/36\",\"signal\":\"RESERVED_STRAP_XTAL\",\"usage\":\"reserved/boot-sensitive\",\"level\":\"danger\",\"note\":\"GPIO0/1 被 32.768kHz 晶振占用，GPIO35 为 BOOT，GPIO36 带上拉；禁止用于临时外设接线。\",\"readonly\":true}"
        "]";
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fputs(live_pins, stdout);
    response_end();
}

static void handle_device_binding_status(const char *request_id,
                                         const char *command)
{
    fridge_device_binding_status_t status = {0};
    esp_err_t err = fridge_device_binding_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"initialized\":", stdout);
    fputs(status.initialized ? "true" : "false", stdout);
    fputs(",\"provisioned\":", stdout);
    fputs(status.provisioned ? "true" : "false", stdout);
    fputs(",\"bound\":", stdout);
    fputs(status.bound ? "true" : "false", stdout);
    fputs(",\"qrReady\":", stdout);
    fputs(status.qr_ready ? "true" : "false", stdout);
    printf(",\"expiresInSeconds\":%lu,\"bindingEpoch\":%lu,\"httpStatus\":%d,\"lastError\":%d",
           (unsigned long)status.expires_in_seconds,
           (unsigned long)status.binding_epoch,
           status.last_http_status,
           status.last_error);
    fputs(",\"apiBaseUrl\":", stdout);
    json_print_escaped(status.api_base_url);
    fputs(",\"deviceSn\":", stdout);
    json_print_escaped(status.device_sn);
    fputs(",\"sessionId\":", stdout);
    json_print_escaped(status.session_id);
    fputs(",\"status\":", stdout);
    json_print_escaped(status.status);
    fputs(",\"detail\":", stdout);
    json_print_escaped(status.detail);
    fputs("}", stdout);
    response_end();
}

static void handle_device_provision_set(const char *line,
                                        const char *request_id,
                                        const char *command)
{
    fridge_device_provisioning_t provisioning = {0};
    bool ok = json_get_string(line, "apiBaseUrl",
                              provisioning.api_base_url,
                              sizeof(provisioning.api_base_url)) &&
              json_get_string(line, "deviceSn",
                              provisioning.device_sn,
                              sizeof(provisioning.device_sn)) &&
              json_get_string(line, "keyId",
                              provisioning.bootstrap_key_id,
                              sizeof(provisioning.bootstrap_key_id)) &&
              json_get_string(line, "bootstrapSecret",
                              provisioning.bootstrap_secret,
                              sizeof(provisioning.bootstrap_secret));
    if (!ok) {
        usb_secure_zero(&provisioning, sizeof(provisioning));
        send_error(request_id, command,
                   "apiBaseUrl, deviceSn, keyId and bootstrapSecret are required");
        return;
    }
    esp_err_t err = fridge_device_binding_set_provisioning(&provisioning);
    usb_secure_zero(provisioning.bootstrap_secret,
                    sizeof(provisioning.bootstrap_secret));
    if (err != ESP_OK) {
        usb_secure_zero(&provisioning, sizeof(provisioning));
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"saved\":true,\"apiBaseUrl\":", stdout);
    json_print_escaped(provisioning.api_base_url);
    fputs(",\"deviceSn\":", stdout);
    json_print_escaped(provisioning.device_sn);
    fputs(",\"keyId\":", stdout);
    json_print_escaped(provisioning.bootstrap_key_id);
    fputs(",\"hasBootstrapSecret\":true}", stdout);
    response_end();
    usb_secure_zero(&provisioning, sizeof(provisioning));
}

static void handle_mqtt_status(const char *request_id, const char *command)
{
    fridge_mqtt_status_t status = {0};
    esp_err_t err = fridge_mqtt_get_status(&status);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"initialized\":", stdout);
    fputs(status.initialized ? "true" : "false", stdout);
    fputs(",\"enabled\":", stdout);
    fputs(status.enabled ? "true" : "false", stdout);
    fputs(",\"configured\":", stdout);
    fputs(status.configured ? "true" : "false", stdout);
    fputs(",\"connected\":", stdout);
    fputs(status.connected ? "true" : "false", stdout);
    fputs(",\"hasPassword\":", stdout);
    fputs(status.has_password ? "true" : "false", stdout);
    fridge_mqtt_config_t config = {0};
    if (fridge_mqtt_get_config(&config) == ESP_OK) {
        printf(",\"keepaliveSeconds\":%u", (unsigned)config.keepalive_seconds);
    }
    printf(",\"protocolVersion\":%u,\"bindingEpoch\":%lu,"
           "\"reconnectCount\":%lu,\"publishedCount\":%lu,"
           "\"receivedCount\":%lu,\"rejectedCount\":%lu,"
           "\"queueDroppedCount\":%lu,\"lastError\":%d",
           (unsigned)status.protocol_version,
           (unsigned long)status.binding_epoch,
           (unsigned long)status.reconnect_count,
           (unsigned long)status.published_count,
           (unsigned long)status.received_count,
           (unsigned long)status.rejected_count,
           (unsigned long)status.queue_dropped_count,
           status.last_error);
    fputs(",\"brokerUri\":", stdout);
    json_print_escaped(status.broker_uri);
    fputs(",\"homeId\":", stdout);
    json_print_escaped(status.home_id);
    fputs(",\"deviceId\":", stdout);
    json_print_escaped(status.device_id);
    fputs(",\"username\":", stdout);
    json_print_escaped(status.username);
    fputs(",\"status\":", stdout);
    json_print_escaped(status.status_text);
    fputs(",\"statusText\":", stdout);
    json_print_escaped(status.status_text);
    fputs("}", stdout);
    response_end();
    // get_config 会带出内部密码副本；响应完成后立即清零，绝不经 USB 回显。
    usb_secure_zero(&config, sizeof(config));
}

static void handle_set_mqtt_config(const char *line,
                                   const char *request_id,
                                   const char *command)
{
    fridge_mqtt_config_t config = {0};
    esp_err_t err = fridge_mqtt_get_config(&config);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    (void)json_get_string(line, "brokerUri", config.broker_uri,
                          sizeof(config.broker_uri));
    (void)json_get_string(line, "mqttHost", config.broker_uri,
                          sizeof(config.broker_uri));
    (void)json_get_string(line, "homeId", config.home_id,
                          sizeof(config.home_id));
    (void)json_get_string(line, "deviceId", config.device_id,
                          sizeof(config.device_id));
    (void)json_get_string(line, "username", config.username,
                          sizeof(config.username));
    const bool has_password = find_json_key(line, "password") != NULL;
    const bool has_token = find_json_key(line, "token") != NULL;
    const bool update_password = has_password || has_token;
    if (has_password) {
        (void)json_get_string(line, "password", config.password,
                              sizeof(config.password));
    } else if (has_token) {
        (void)json_get_string(line, "token", config.password,
                              sizeof(config.password));
    }
    config.enabled = json_get_bool(line, "enabled", true);
    uint32_t keepalive = config.keepalive_seconds
                             ? config.keepalive_seconds
                             : FRIDGE_MQTT_DEFAULT_KEEPALIVE_SECONDS;
    (void)json_get_u32(line, "keepaliveSeconds", &keepalive);
    config.keepalive_seconds =
        keepalive <= UINT16_MAX ? (uint16_t)keepalive : 0;

    err = fridge_mqtt_set_config(&config, update_password);
    if (err == ESP_OK && config.enabled) {
        const esp_err_t start_err = fridge_mqtt_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_INVALID_STATE) {
            err = start_err;
        }
    }
    usb_secure_zero(&config, sizeof(config));
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_mqtt_status(request_id, command);
}

static void handle_clear_mqtt_secret(const char *request_id,
                                     const char *command)
{
    esp_err_t err = fridge_mqtt_clear_secret();
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_mqtt_status(request_id, command);
}

static void handle_mqtt_publish_state(const char *request_id,
                                      const char *command)
{
    esp_err_t err = fridge_mqtt_start();
    if (err == ESP_OK) {
        err = fridge_mqtt_publish_state(true);
    }
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }
    handle_mqtt_status(request_id, command);
}

static void handle_get_logs(const char *request_id, const char *command)
{
    response_begin(request_id, command, true);
    fputs(",\"payload\":[{\"id\":\"boot\",\"at\":\"runtime\",\"level\":\"info\","
          "\"source\":\"firmware\",\"message\":\"P4 minimal JSON protocol running\"}]",
          stdout);
    response_end();
}

static void handle_line(const char *line)
{
    char request_id[USB_MIN_VALUE_BUFFER_SIZE] = "unknown";
    char command[USB_MIN_VALUE_BUFFER_SIZE] = {0};

    // 日志和 JSON 共用 UART；只处理以“{”开头的行，其他文本按普通调试输出忽略。
    if (!line || line[0] != '{') {
        return;
    }
    (void)json_get_string(line, "request_id", request_id, sizeof(request_id));
    if (!json_get_string(line, "command", command, sizeof(command))) {
        (void)json_get_string(line, "cmd", command, sizeof(command));
    }
    if (command[0] == '\0') {
        send_error(request_id, "unknown", "command is required");
        return;
    }

    // get_pins/get_logs 只访问静态文本，可在 booting 阶段用于核线；其余命令等待初始化屏障。
    const bool boot_safe = strcmp(command, "get_pins") == 0 || strcmp(command, "get_logs") == 0 ||
                           strcmp(command, "c6_ota_status") == 0 || strcmp(command, "retry_hosted") == 0;
    if (!atomic_load_explicit(&s_hardware_ready, memory_order_acquire) && !boot_safe) {
        send_error(request_id, command, "system is booting; wait for ready event");
        return;
    }

    if (strcmp(command, "get_status") == 0) {
        handle_get_status(request_id, command);
    } else if (strcmp(command, "get_network") == 0) {
        handle_get_network(request_id, command);
    } else if (strcmp(command, "scan_wifi") == 0) {
        handle_scan_wifi(request_id, command);
    } else if (strcmp(command, "set_network") == 0) {
        handle_set_network(line, request_id, command);
    } else if (strcmp(command, "retry_hosted") == 0) {
        handle_retry_hosted(request_id, command);
    } else if (strcmp(command, "c6_ota_status") == 0) {
        handle_c6_ota_status(request_id, command);
    } else if (strcmp(command, "c6_ota_begin") == 0) {
        handle_c6_ota_begin(line, request_id, command);
    } else if (strcmp(command, "c6_ota_chunk") == 0) {
        handle_c6_ota_chunk(line, request_id, command);
    } else if (strcmp(command, "c6_ota_end") == 0) {
        handle_c6_ota_end(line, request_id, command);
    } else if (strcmp(command, "c6_ota_activate") == 0) {
        handle_c6_ota_activate(line, request_id, command);
    } else if (strcmp(command, "get_asr_config") == 0) {
        handle_get_asr_config(request_id, command);
    } else if (strcmp(command, "set_asr_config") == 0) {
        handle_set_asr_config(line, request_id, command);
    } else if (strcmp(command, "clear_asr_key") == 0) {
        handle_clear_asr_key(request_id, command);
    } else if (strcmp(command, "asr_transcribe") == 0) {
        handle_asr_transcribe(request_id, command);
    } else if (strcmp(command, "wake_status") == 0) {
        handle_wake_status(request_id, command);
    } else if (strcmp(command, "wake_set_enabled") == 0) {
        handle_wake_set_enabled(line, request_id, command, -1);
    } else if (strcmp(command, "wake_start") == 0) {
        handle_wake_set_enabled(line, request_id, command, 1);
    } else if (strcmp(command, "wake_stop") == 0) {
        handle_wake_set_enabled(line, request_id, command, 0);
    } else if (strcmp(command, "wake_reset_stats") == 0) {
        handle_wake_reset_stats(request_id, command);
    } else if (strcmp(command, "wake_maintenance_hold") == 0) {
        handle_wake_maintenance_hold(line, request_id, command);
    } else if (strcmp(command, "mic_record_start") == 0) {
        handle_audio_start(line, request_id, command);
    } else if (strcmp(command, "mic_record_status") == 0) {
        handle_audio_status(request_id, command);
    } else if (strcmp(command, "mic_record_stop") == 0) {
        handle_audio_stop(request_id, command);
    } else if (strcmp(command, "mic_record_wav") == 0) {
        handle_audio_wav(request_id, command);
    } else if (strcmp(command, "mic_record_wav_info") == 0) {
        handle_audio_wav_info(request_id, command);
    } else if (strcmp(command, "mic_record_wav_chunk") == 0) {
        handle_audio_wav_chunk(line, request_id, command);
    } else if (strcmp(command, "mic_record_wav_end") == 0) {
        handle_audio_wav_end(line, request_id, command);
    } else if (strcmp(command, "speaker_beep") == 0) {
        handle_speaker_beep(request_id, command);
    } else if (strcmp(command, "get_tfcard_status") == 0) {
        handle_tfcard_status(request_id, command);
    } else if (strcmp(command, "agent_storage_status") == 0) {
        handle_agent_storage_status(request_id, command);
    } else if (strcmp(command, "task_scheduler_status") == 0) {
        handle_task_scheduler_status(request_id, command);
    } else if (strcmp(command, "tool_list") == 0) {
        handle_tool_list(request_id, command);
    } else if (strcmp(command, "tool_call") == 0) {
        handle_tool_call(line, request_id, command);
    } else if (strcmp(command, "tool_permission_status") == 0) {
        handle_tool_permission_status(request_id, command);
    } else if (strcmp(command, "tool_permission_set") == 0) {
        handle_tool_permission_set(line, request_id, command);
    } else if (strcmp(command, "skill_status") == 0) {
        handle_skill_status(request_id, command);
    } else if (strcmp(command, "skill_list") == 0) {
        handle_skill_list(request_id, command);
    } else if (strcmp(command, "skill_read") == 0) {
        handle_skill_read(line, request_id, command);
    } else if (strcmp(command, "skill_match") == 0) {
        handle_skill_match(line, request_id, command);
    } else if (strcmp(command, "workflow_status") == 0) {
        handle_workflow_status(request_id, command);
    } else if (strcmp(command, "workflow_start") == 0) {
        handle_workflow_start(line, request_id, command);
    } else if (strcmp(command, "device_provision_set") == 0) {
        handle_device_provision_set(line, request_id, command);
    } else if (strcmp(command, "device_binding_status") == 0) {
        handle_device_binding_status(request_id, command);
    } else if (strcmp(command, "device_binding_refresh") == 0) {
        esp_err_t err = fridge_device_binding_refresh_session();
        if (err == ESP_OK) {
            handle_device_binding_status(request_id, command);
        } else {
            send_error(request_id, command, esp_err_to_name(err));
        }
    } else if (strcmp(command, "mqtt_status") == 0 ||
               strcmp(command, "get_mqtt_config") == 0 ||
               strcmp(command, "get_mqtt_status") == 0) {
        handle_mqtt_status(request_id, command);
    } else if (strcmp(command, "set_mqtt_config") == 0) {
        handle_set_mqtt_config(line, request_id, command);
    } else if (strcmp(command, "clear_mqtt_secret") == 0) {
        handle_clear_mqtt_secret(request_id, command);
    } else if (strcmp(command, "mqtt_publish_state") == 0) {
        handle_mqtt_publish_state(request_id, command);
    } else if (strcmp(command, "agent_status") == 0) {
        handle_agent_status(request_id, command);
    } else if (strcmp(command, "agent_run") == 0) {
        handle_agent_run(line, request_id, command);
    } else if (strcmp(command, "agent_submit") == 0) {
        handle_agent_submit(line, request_id, command);
    } else if (strcmp(command, "agent_run_status") == 0) {
        handle_agent_run_status(request_id, command);
    } else if (strcmp(command, "agent_cancel") == 0) {
        handle_agent_cancel(request_id, command);
    } else if (strcmp(command, "agent_confirm") == 0) {
        handle_agent_confirmation(line, request_id, command, true);
    } else if (strcmp(command, "agent_reject") == 0) {
        handle_agent_confirmation(line, request_id, command, false);
    } else if (strcmp(command, "agent_session_read") == 0) {
        handle_agent_session_read(request_id, command);
    } else if (strcmp(command, "agent_session_clear") == 0) {
        handle_agent_session_clear(request_id, command);
    } else if (strcmp(command, "get_ai_config") == 0) {
        handle_get_ai_config(request_id, command);
    } else if (strcmp(command, "get_ai_profiles") == 0) {
        handle_get_ai_profiles(request_id, command);
    } else if (strcmp(command, "set_ai_config") == 0) {
        handle_set_ai_config(line, request_id, command);
    } else if (strcmp(command, "clear_ai_key") == 0) {
        handle_clear_ai_key(request_id, command);
    } else if (strcmp(command, "get_tts_config") == 0) {
        handle_get_tts_config(request_id, command);
    } else if (strcmp(command, "set_tts_config") == 0) {
        handle_set_tts_config(line, request_id, command);
    } else if (strcmp(command, "clear_tts_key") == 0) {
        handle_clear_tts_key(request_id, command);
    } else if (strcmp(command, "tts_play") == 0) {
        handle_tts_play(line, request_id, command);
    } else if (strcmp(command, "tts_status") == 0) {
        handle_tts_status(request_id, command);
    } else if (strcmp(command, "tts_stop") == 0) {
        handle_tts_stop(request_id, command);
    } else if (strcmp(command, "create_ai_profile") == 0) {
        handle_create_ai_profile(line, request_id, command);
    } else if (strcmp(command, "select_ai_profile") == 0) {
        handle_select_ai_profile(line, request_id, command);
    } else if (strcmp(command, "delete_ai_profile") == 0) {
        handle_delete_ai_profile(line, request_id, command);
    } else if (strcmp(command, "test_ai_chat") == 0) {
        handle_test_ai_chat(line, request_id, command);
    } else if (strcmp(command, "agent_trace_status") == 0) {
        handle_agent_trace_status(request_id, command);
    } else if (strcmp(command, "agent_trace_read") == 0) {
        handle_agent_trace_read(request_id, command);
    } else if (strcmp(command, "get_realtime_config") == 0) {
        handle_get_realtime_config(request_id, command);
    } else if (strcmp(command, "set_realtime_config") == 0) {
        handle_set_realtime_config(line, request_id, command);
    } else if (strcmp(command, "clear_realtime_key") == 0) {
        handle_clear_realtime_key(request_id, command);
    } else if (strcmp(command, "realtime_status") == 0) {
        handle_realtime_status(request_id, command);
    } else if (strcmp(command, "realtime_start") == 0) {
        handle_realtime_start(request_id, command);
    } else if (strcmp(command, "realtime_stop") == 0) {
        handle_realtime_stop(request_id, command);
    } else if (strcmp(command, "realtime_cancel") == 0) {
        handle_realtime_cancel(request_id, command);
    } else if (strcmp(command, "realtime_send_text") == 0) {
        handle_realtime_send_text(line, request_id, command);
    } else if (strcmp(command, "format_tfcard") == 0) {
        handle_tfcard_format(line, request_id, command);
    } else if (strcmp(command, "tfcard_selftest") == 0) {
        handle_tfcard_selftest(request_id, command);
    } else if (strcmp(command, "tf_list") == 0) {
        handle_tf_list(line, request_id, command);
    } else if (strcmp(command, "tf_stat") == 0) {
        handle_tf_stat(line, request_id, command);
    } else if (strcmp(command, "tf_mkdir") == 0) {
        handle_tf_mkdir(line, request_id, command);
    } else if (strcmp(command, "tf_move") == 0) {
        handle_tf_move(line, request_id, command);
    } else if (strcmp(command, "tf_delete") == 0) {
        handle_tf_delete(line, request_id, command);
    } else if (strcmp(command, "tf_restore") == 0) {
        handle_tf_restore(line, request_id, command);
    } else if (strcmp(command, "tf_trash_empty") == 0) {
        handle_tf_trash_empty(line, request_id, command);
    } else if (strcmp(command, "tf_text_read") == 0) {
        handle_tf_text_read(line, request_id, command);
    } else if (strcmp(command, "tf_text_write") == 0) {
        handle_tf_text_write(line, request_id, command);
    } else if (strcmp(command, "tf_transfer_begin") == 0) {
        handle_tf_transfer_begin(line, request_id, command);
    } else if (strcmp(command, "tf_transfer_status") == 0) {
        handle_tf_transfer_status(request_id, command);
    } else if (strcmp(command, "tf_transfer_chunk") == 0) {
        handle_tf_transfer_chunk(line, request_id, command);
    } else if (strcmp(command, "tf_transfer_end") == 0) {
        handle_tf_transfer_finish(line, request_id, command, false);
    } else if (strcmp(command, "tf_transfer_abort") == 0) {
        handle_tf_transfer_finish(line, request_id, command, true);
    } else if (strcmp(command, "tf_mount") == 0) {
        handle_tf_mount(request_id, command, true);
    } else if (strcmp(command, "tf_unmount") == 0) {
        handle_tf_mount(request_id, command, false);
    } else if (strcmp(command, "tf_health_test") == 0) {
        handle_tf_health(request_id, command);
    } else if (strcmp(command, "tf_benchmark") == 0) {
        handle_tf_benchmark(line, request_id, command);
    } else if (strcmp(command, "tf_get_access_mode") == 0) {
        handle_tf_access_mode(line, request_id, command, false);
    } else if (strcmp(command, "tf_set_access_mode") == 0) {
        handle_tf_access_mode(line, request_id, command, true);
    } else if (strcmp(command, "tf_http_enable") == 0) {
        handle_tf_http(request_id, command, 1);
    } else if (strcmp(command, "tf_http_status") == 0) {
        handle_tf_http(request_id, command, 0);
    } else if (strcmp(command, "tf_http_disable") == 0) {
        handle_tf_http(request_id, command, -1);
    } else if (strcmp(command, "tf_export_diagnostics") == 0) {
        handle_tf_export_diagnostics(request_id, command);
    } else if (strcmp(command, "get_diagnostics") == 0) {
        handle_get_diagnostics(request_id, command);
    } else if (strcmp(command, "touch_status") == 0) {
        handle_touch_status(request_id, command);
    } else if (strcmp(command, "touch_scan") == 0) {
        handle_touch_scan(request_id, command);
    } else if (strcmp(command, "get_sensors") == 0) {
        handle_get_sensors(request_id, command);
    } else if (strcmp(command, "mq5_confirm_burn_in") == 0) {
        handle_mq5_confirm_burn_in(line, request_id, command);
    } else if (strcmp(command, "mq5_capture_baseline") == 0) {
        handle_mq5_capture_baseline(line, request_id, command);
    } else if (strcmp(command, "mq5_reset_calibration") == 0) {
        handle_mq5_reset_calibration(line, request_id, command);
    } else if (strcmp(command, "ui_status") == 0) {
        handle_ui_status(request_id, command);
    } else if (strcmp(command, "get_camera_status") == 0) {
        handle_camera_status(request_id, command);
    } else if (strcmp(command, "camera_capture") == 0) {
        handle_camera_capture(request_id, command);
    } else if (strcmp(command, "screenshot_capture") == 0) {
        handle_screenshot_capture(request_id, command);
    } else if (strcmp(command, "clear_camera_frame") == 0) {
        handle_camera_clear(request_id, command);
    } else if (strcmp(command, "camera_preview_start") == 0) {
        handle_camera_preview(request_id, command, true);
    } else if (strcmp(command, "camera_preview_stop") == 0) {
        handle_camera_preview(request_id, command, false);
    } else if (strcmp(command, "get_pins") == 0) {
        handle_get_pins(request_id, command);
    } else if (strcmp(command, "get_logs") == 0) {
        handle_get_logs(request_id, command);
    } else {
        send_error(request_id, command, "unsupported in p4_minimal profile");
    }
}

static void usb_protocol_task(void *arg)
{
    (void)arg;
    size_t line_len = 0;
    bool dropping_oversized_line = false;
    char oversized_request_id[64] = {0};
    char oversized_command[64] = {0};

    ESP_LOGI(TAG, "P4 minimal JSON Lines protocol started (115200 baud)");
    if (atomic_load_explicit(&s_hardware_ready, memory_order_acquire)) {
        send_ready_event();
    } else {
        send_booting_event();
    }
    while (true) {
        // stdin 可能分段返回，逐字节累计到换行后再解析，避免把半条请求当成无效命令。
        const int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (dropping_oversized_line) {
                send_error(oversized_request_id[0] ? oversized_request_id : "unknown",
                           oversized_command[0] ? oversized_command : "unknown",
                           "JSON line exceeds 16383 bytes");
                dropping_oversized_line = false;
                line_len = 0;
                usb_secure_zero(oversized_request_id, sizeof(oversized_request_id));
                usb_secure_zero(oversized_command, sizeof(oversized_command));
                continue;
            }
            if (line_len > 0) {
                s_line_buffer[line_len] = '\0';
                handle_line(s_line_buffer);
                // 请求可能含 API Key/Wi-Fi 密码，处理完成后不得让明文继续驻留在静态缓冲区。
                usb_secure_zero(s_line_buffer, line_len + 1);
                s_command_count++;
                if ((s_command_count % 16U) == 0U) {
                    const UBaseType_t remaining = uxTaskGetStackHighWaterMark(NULL);
                    if (remaining < 1536U) {
                        ESP_LOGW(TAG, "USB task stack margin low: %u bytes", (unsigned)remaining);
                    }
                }
                line_len = 0;
            }
            continue;
        }
        if (dropping_oversized_line) {
            continue;
        }
        if (line_len + 1 >= USB_MIN_LINE_BUFFER_SIZE) {
            s_line_buffer[line_len] = '\0';
            (void)json_get_string(s_line_buffer,
                                  "request_id",
                                  oversized_request_id,
                                  sizeof(oversized_request_id));
            (void)json_get_string(s_line_buffer,
                                  "command",
                                  oversized_command,
                                  sizeof(oversized_command));
            usb_secure_zero(s_line_buffer, line_len + 1);
            ESP_LOGW(TAG, "JSON line exceeds %u bytes; dropping until next newline",
                     (unsigned)(USB_MIN_LINE_BUFFER_SIZE - 1));
            line_len = 0;
            dropping_oversized_line = true;
            continue;
        }
        s_line_buffer[line_len++] = (char)ch;
    }
}

esp_err_t fridge_usb_protocol_start(void)
{
    // 启动函数保持幂等，避免主流程重复调用后出现两个任务争抢同一 UART stdin。
    if (s_protocol_task) {
        return ESP_OK;
    }
    if (!s_line_buffer) {
        s_line_buffer = usb_large_alloc(USB_MIN_LINE_BUFFER_SIZE);
        if (!s_line_buffer) {
            ESP_LOGE(TAG, "allocate USB JSON line buffer failed");
            return ESP_ERR_NO_MEM;
        }
        memset(s_line_buffer, 0, USB_MIN_LINE_BUFFER_SIZE);
    }
    fridge_task_scheduler_set_event_callback(emit_task_due_event, NULL);
    esp_err_t voice_err = fridge_voice_asr_init();
    if (voice_err == ESP_OK) {
        (void)fridge_wake_word_set_event_callback(emit_wake_detected_event, NULL);
        (void)fridge_voice_asr_set_event_callback(emit_wake_asr_event, NULL);
    } else {
        ESP_LOGW(TAG, "voice ASR coordinator init failed: %s", esp_err_to_name(voice_err));
    }
    // Realtime 事件 sink：组件初始化可能稍早/稍晚，注册失败不阻断 USB。
    esp_err_t rt_cb_err = fridge_glm_realtime_set_event_callback(emit_realtime_event, NULL);
    if (rt_cb_err != ESP_OK) {
        ESP_LOGW(TAG, "glm realtime event callback not ready yet: %s", esp_err_to_name(rt_cb_err));
    }

    const BaseType_t ok = xTaskCreate(usb_protocol_task,
                                      "usb_protocol",
                                      USB_MIN_TASK_STACK_BYTES,
                                      NULL,
                                      5,
                                      &s_protocol_task);
    if (ok != pdPASS) {
        heap_caps_free(s_line_buffer);
        s_line_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void fridge_usb_protocol_set_hardware_ready(void)
{
    atomic_store_explicit(&s_hardware_ready, true, memory_order_release);
    fridge_voice_asr_set_hardware_ready();
    send_ready_event();
}
