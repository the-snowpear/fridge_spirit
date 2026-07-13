// 冰箱小精灵 ESP32-P4 最小移植串口协议。
// 仅提供 Wi-Fi、板载麦克风、TF 卡和基础诊断命令，避免联调阶段拉入尚未迁移的完整业务组件。
// 协议使用 115200 baud JSON Lines；密码只传给 network 组件，任何响应和日志都不得回显。

#include "fridge_usb_protocol.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_audio.h"
#include "fridge_diagnostics.h"
#include "fridge_network.h"
#include "fridge_tfcard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define USB_MIN_LINE_BUFFER_SIZE 2048
#define USB_MIN_VALUE_BUFFER_SIZE 128
#define USB_MIN_SCAN_MAX_AP 50
#define USB_MIN_TASK_STACK_BYTES 6144

static const char *TAG = "usb_p4_min";
static char s_line_buffer[USB_MIN_LINE_BUFFER_SIZE];
static fridge_wifi_ap_t s_scan_aps[USB_MIN_SCAN_MAX_AP];
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
          "\"mic_record_start\",\"mic_record_status\",\"mic_record_stop\",\"mic_record_wav\","
          "\"get_tfcard_status\",\"format_tfcard\",\"tfcard_selftest\","
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
    fputs(",\"status\":", stdout);
    json_print_escaped(status.connected ? (status.internet_ready ? "online" : "wifi_connected")
                                        : (status.connecting ? "connecting" : "offline"));
    fputs(",\"ip\":", stdout);
    json_print_escaped(status.ip);
    printf(",\"rssi\":%d,\"lastError\":", status.rssi);
    json_print_escaped(status.last_error);
    fputs("}", stdout);
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

static void handle_audio_start(const char *request_id, const char *command)
{
    const esp_err_t err = fridge_audio_start_recording();
    if (err != ESP_OK) {
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
    const bool boot_safe = strcmp(command, "get_pins") == 0 || strcmp(command, "get_logs") == 0;
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
    } else if (strcmp(command, "mic_record_start") == 0) {
        handle_audio_start(request_id, command);
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
    send_ready_event();
}
