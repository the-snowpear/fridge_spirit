// 冰箱小精灵 ESP32-P4 最小移植串口协议。
// 提供 Wi-Fi、板载麦克风、WakeNet/云端 ASR、TF 卡和基础诊断命令，
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
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_c6_ota.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "fridge_tfcard.h"
#include "fridge_tf_http.h"
#include "fridge_voice_asr.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define USB_MIN_LINE_BUFFER_SIZE 2048
#define USB_MIN_VALUE_BUFFER_SIZE 128
#define USB_MIN_SCAN_MAX_AP 50
#define USB_MIN_TASK_STACK_BYTES 6144
#define USB_C6_OTA_B64_BUFFER_SIZE 1400
#define USB_TF_B64_BUFFER_SIZE 1400
#define USB_TF_TEXT_INPUT_SIZE 1536

static const char *TAG = "usb_p4_min";
static char s_line_buffer[USB_MIN_LINE_BUFFER_SIZE];
static fridge_wifi_ap_t s_scan_aps[USB_MIN_SCAN_MAX_AP];
static char s_c6_ota_b64[USB_C6_OTA_B64_BUFFER_SIZE];
static uint8_t s_c6_ota_chunk[FRIDGE_C6_OTA_MAX_CHUNK_SIZE];
static char s_tf_b64[USB_TF_B64_BUFFER_SIZE];
static uint8_t s_tf_chunk[FRIDGE_TFCARD_TRANSFER_CHUNK];
static char s_tf_text_input[USB_TF_TEXT_INPUT_SIZE];
static TaskHandle_t s_protocol_task;
static atomic_bool s_hardware_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_ready_event_sent = ATOMIC_VAR_INIT(false);

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

// 生成 16 kHz、单声道、16-bit little-endian PCM 的标准 44 字节 WAV 头。
static void write_wav_header(uint8_t *header, size_t pcm_bytes)
{
    const uint32_t sample_rate = FRIDGE_AUDIO_SAMPLE_RATE;
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
                    (void)append_utf8(out, out_size, &written, codepoint);
                    continue;
                }
                ch = '?';
            } else {
                ch = escaped;
            }
        }
        if (written + 1 < out_size) {
            out[written++] = ch;
        }
    }
    out[written] = '\0';
    return *pos == '"';
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
          "\"get_tfcard_status\",\"format_tfcard\",\"tfcard_selftest\","
          "\"tf_list\",\"tf_stat\",\"tf_mkdir\",\"tf_move\",\"tf_delete\",\"tf_restore\",\"tf_trash_empty\","
          "\"tf_text_read\",\"tf_text_write\",\"tf_transfer_begin\",\"tf_transfer_status\","
          "\"tf_transfer_chunk\",\"tf_transfer_end\",\"tf_transfer_abort\","
          "\"tf_mount\",\"tf_unmount\",\"tf_health_test\",\"tf_benchmark\","
          "\"tf_get_access_mode\",\"tf_set_access_mode\","
          "\"tf_http_enable\",\"tf_http_status\",\"tf_http_disable\",\"tf_export_diagnostics\"," 
          "\"get_diagnostics\",\"get_pins\",\"get_logs\"]}}\n",
          stdout);
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
    esp_restart();
}

static void schedule_restart(uint32_t delay_ms)
{
    (void)xTaskCreate(delayed_restart_task,
                      "c6_ota_restart",
                      2048,
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
    printf(",\"durationMs\":%lu,\"pcmBytes\":%u,\"rms\":%ld,"
           "\"sampleCount\":%lu,\"peakAbs\":%ld,\"minSample\":%d,\"maxSample\":%d,"
           "\"meanSample\":%ld,\"clipCount\":%lu,\"timeoutCount\":%lu",
           (unsigned long)status->duration_ms,
           (unsigned)status->pcm_bytes,
           (long)status->rms,
           (unsigned long)status->sample_count,
           (long)status->peak_abs,
           (int)status->min_sample,
           (int)status->max_sample,
           (long)status->mean_sample,
           (unsigned long)status->clip_count,
           (unsigned long)status->timeout_count);
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
    printf(",\"largestInternalDmaBlock\":%u,\"minDmaBlockThreshold\":%u,\"error\":",
           (unsigned)status.largest_internal_dma_block,
           (unsigned)status.min_dma_block_threshold);
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
    esp_err_t err = for_asr ? fridge_voice_asr_manual_record_begin() : ESP_OK;
    if (err == ESP_OK) {
        err = fridge_audio_start_recording();
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
    if (!pcm || pcm_bytes == 0 || pcm_bytes > FRIDGE_AUDIO_MAX_PCM_BYTES) {
        send_error(request_id, command, "recorded PCM size invalid");
        return;
    }

    const size_t wav_bytes = pcm_bytes + 44;
    uint8_t *wav = usb_large_alloc(wav_bytes);
    if (!wav) {
        send_error(request_id, command, "WAV allocation failed");
        return;
    }
    write_wav_header(wav, pcm_bytes);
    memcpy(wav + 44, pcm, pcm_bytes);
    char *wav_b64 = base64_encode_alloc(wav, wav_bytes);
    heap_caps_free(wav);
    if (!wav_b64) {
        send_error(request_id, command, "WAV base64 allocation failed");
        return;
    }

    response_begin(request_id, command, true);
    printf(",\"payload\":{\"sampleRate\":%d,\"channels\":1,\"bitsPerSample\":16,"
           "\"durationMs\":%lu,\"pcmBytes\":%u,\"wavBytes\":%u,\"wavBase64\":\"",
           FRIDGE_AUDIO_SAMPLE_RATE,
           (unsigned long)duration_ms,
           (unsigned)pcm_bytes,
           (unsigned)wav_bytes);
    fputs(wav_b64, stdout);
    fputs("\"}", stdout);
    response_end();
    heap_caps_free(wav_b64);
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
    printf(",\"freeHeapKb\":%lu,\"minHeapKb\":%lu,\"freePsramKb\":%lu,\"temperatureC\":null",
           (unsigned long)status.free_heap_kb,
           (unsigned long)status.min_heap_kb,
           (unsigned long)status.free_psram_kb);
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
    const esp_err_t err = fridge_diagnostics_get_snapshot(&diag);
    if (err != ESP_OK) {
        send_error(request_id, command, esp_err_to_name(err));
        return;
    }

    response_begin(request_id, command, true);
    fputs(",\"payload\":{\"psram\":", stdout);
    json_print_escaped(diag.psram);
    fputs(",\"flashPartition\":", stdout);
    json_print_escaped(diag.flash_partition);
    fputs(",\"littlefs\":", stdout);
    json_print_escaped(diag.littlefs);
    fputs(",\"otaSlot\":", stdout);
    json_print_escaped(diag.ota_slot);
    printf(",\"brownoutCount\":%lu,\"watchdogCount\":%lu,\"lastError\":",
           (unsigned long)diag.brownout_count,
           (unsigned long)diag.watchdog_count);
    json_print_escaped(diag.last_error);
    fputs(",\"riskItems\":["
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

static void handle_get_pins(const char *request_id, const char *command)
{
    // 此表只描述 P4 CB V1.2 当前最小移植，不再暴露 S3 旧引脚。
    static const char live_pins[] =
        "["
        "{\"gpio\":\"GPIO7/8/9/10/19/20/21/22\",\"signal\":\"LCD_QSPI\",\"usage\":\"TR230S via 20pin FPC-to-Dupont\",\"level\":\"caution\",\"note\":\"CS=7,D0=8,SCLK=9,D1=10,WAIT=19,D2=20,D3=21,RST=22；QSPI Mode0/10MHz，D/C 不接，屏 VCC=5V，GPIO 仅 3.3V。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO15/16/17/18\",\"signal\":\"TOUCH_DIRECT\",\"usage\":\"FT6336U direct I2C\",\"level\":\"caution\",\"note\":\"INT=15,SCL=16,SDA=17,RST=18；I2C 100kHz，外部上拉到 3.3V，FPC19 TP-VCC 保持不接。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO2/3/4/5/6/11/28/29\",\"signal\":\"ES8311_AUDIO\",\"usage\":\"onboard microphone codec\",\"level\":\"caution\",\"note\":\"MCLK=2,DSDIN(P4 DOUT)=3,BCLK=4,ASDOUT(P4 DIN)=5,WS=6,PA_CTRL=11,SDA=28,SCL=29；GPIO28/29 与摄像头 SCCB 分时复用。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO39/40/41/42/43/44/45\",\"signal\":\"TF_SDMMC\",\"usage\":\"onboard TF 4-bit\",\"level\":\"caution\",\"note\":\"D0-D3=39-42,CLK=43,CMD=44,PWR_EN=45 低有效；64GB 卡需显式确认后才可格式化 FAT32。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO47-54\",\"signal\":\"C6_SDIO\",\"usage\":\"onboard ESP32-C6 Wi-Fi\",\"level\":\"danger\",\"note\":\"板载专用，不接外设；Wi-Fi 必须经 esp_hosted/esp_wifi_remote，并启用板级 SDIO 电源管理。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO37/38\",\"signal\":\"UART0\",\"usage\":\"CH343P flash/monitor/JSON Lines\",\"level\":\"safe\",\"note\":\"115200 baud；烧录、monitor 与 Web Serial 不可同时占用同一 COM 口。\",\"readonly\":true},"
        "{\"gpio\":\"GPIO0/1/35/36\",\"signal\":\"RESERVED_STRAP_XTAL\",\"usage\":\"reserved/boot-sensitive\",\"level\":\"danger\",\"note\":\"GPIO0/1 被 32.768kHz 晶振占用，GPIO35 为 BOOT，GPIO36 带上拉；禁止用于临时外设接线。\",\"readonly\":true}"
        "]";
    response_begin(request_id, command, true);
    fputs(",\"payload\":", stdout);
    fputs(live_pins, stdout);
    response_end();
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
    } else if (strcmp(command, "mic_record_start") == 0) {
        handle_audio_start(line, request_id, command);
    } else if (strcmp(command, "mic_record_status") == 0) {
        handle_audio_status(request_id, command);
    } else if (strcmp(command, "mic_record_stop") == 0) {
        handle_audio_stop(request_id, command);
    } else if (strcmp(command, "mic_record_wav") == 0) {
        handle_audio_wav(request_id, command);
    } else if (strcmp(command, "get_tfcard_status") == 0) {
        handle_tfcard_status(request_id, command);
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
                dropping_oversized_line = false;
                line_len = 0;
                continue;
            }
            if (line_len > 0) {
                s_line_buffer[line_len] = '\0';
                handle_line(s_line_buffer);
                line_len = 0;
            }
            continue;
        }
        if (dropping_oversized_line) {
            continue;
        }
        if (line_len + 1 >= sizeof(s_line_buffer)) {
            ESP_LOGW(TAG, "JSON line too long; dropping bytes until next newline");
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
    esp_err_t voice_err = fridge_voice_asr_init();
    if (voice_err == ESP_OK) {
        (void)fridge_wake_word_set_event_callback(emit_wake_detected_event, NULL);
        (void)fridge_voice_asr_set_event_callback(emit_wake_asr_event, NULL);
    } else {
        ESP_LOGW(TAG, "voice ASR coordinator init failed: %s", esp_err_to_name(voice_err));
    }

    const BaseType_t ok = xTaskCreate(usb_protocol_task,
                                      "usb_protocol",
                                      USB_MIN_TASK_STACK_BYTES,
                                      NULL,
                                      5,
                                      &s_protocol_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void fridge_usb_protocol_set_hardware_ready(void)
{
    atomic_store_explicit(&s_hardware_ready, true, memory_order_release);
    fridge_voice_asr_set_hardware_ready();
    send_ready_event();
}
