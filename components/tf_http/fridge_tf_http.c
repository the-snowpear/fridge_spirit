// 冰箱小精灵 TF 卡 Wi-Fi HTTP 流式传输。
// 安全模型：路径和方向由已存在的串口 TF transfer session 固定；HTTP 只凭一次性 token
// 访问该会话。服务不提供目录浏览，也不记录 token。TCP 任务只使用 4KiB 工作缓冲，
// 再拆成 TF 核心允许的 1024B 块，避免把大文件放入 P4 RAM/PSRAM。

#include "fridge_tf_http.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fridge_network.h"
#include "fridge_tfcard.h"

#define TF_HTTP_URI "/api/tf/v1/transfer"
#define TF_HTTP_TOKEN_TTL_US (10LL * 60LL * 1000000LL)
#define TF_HTTP_MAX_PUT_BYTES (1024U * 1024U)

static const char *TAG = "tf_http";
static httpd_handle_t s_server;
static uint32_t s_session_id;
static int64_t s_expires_at_us;
static char s_token[FRIDGE_TF_HTTP_TOKEN_HEX_LEN + 1];
static char s_url[FRIDGE_TF_HTTP_URL_LEN];
static char s_last_error[FRIDGE_TF_HTTP_ERROR_LEN + 1];

static void set_error(const char *error)
{
    strlcpy(s_last_error, error ? error : "", sizeof(s_last_error));
}

static void invalidate_authorization(void)
{
    s_session_id = 0;
    s_expires_at_us = 0;
    memset(s_token, 0, sizeof(s_token));
    memset(s_url, 0, sizeof(s_url));
}

static bool origin_allowed(httpd_req_t *req)
{
    const size_t len = httpd_req_get_hdr_value_len(req, "Origin");
    if (len == 0) {
        return true; // curl/脚本没有 Origin，仍必须通过 Bearer token。
    }
    if (len >= 128) return false;
    char origin[128] = "";
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    static const char *allowed[] = {
        "http://localhost", "http://127.0.0.1", "https://localhost", "https://127.0.0.1"
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        const size_t prefix = strlen(allowed[i]);
        if (strncmp(origin, allowed[i], prefix) != 0) continue;
        const char *suffix = origin + prefix;
        if (*suffix == '\0') return true;
        if (*suffix++ != ':') continue;

        // 浏览器 Origin 的可选端口必须是 1..65535 的纯数字，拒绝 localhost:evil。
        unsigned port = 0;
        bool has_digit = false;
        while (*suffix) {
            if (*suffix < '0' || *suffix > '9') {
                has_digit = false;
                break;
            }
            has_digit = true;
            port = port * 10U + (unsigned)(*suffix++ - '0');
            if (port > 65535U) {
                has_digit = false;
                break;
            }
        }
        if (has_digit && port > 0) return true;
    }
    return false;
}

static void add_cors_headers(httpd_req_t *req)
{
    char origin[128] = "http://localhost";
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        !origin_allowed(req)) {
        strlcpy(origin, "http://localhost", sizeof(origin));
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", origin);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, PUT, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type, Range, X-Upload-Offset");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "Content-Length, Content-Range, X-Next-Offset");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Vary", "Origin");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[224];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message ? message : "error");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool authorize(httpd_req_t *req, fridge_tfcard_transfer_status_t *transfer)
{
    if (!origin_allowed(req) || !s_token[0] || esp_timer_get_time() >= s_expires_at_us) return false;
    char auth[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) return false;
    const size_t expected_len = 7U + FRIDGE_TF_HTTP_TOKEN_HEX_LEN;
    if (strlen(auth) != expected_len || memcmp(auth, "Bearer ", 7) != 0) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < FRIDGE_TF_HTTP_TOKEN_HEX_LEN; i++) {
        diff |= (unsigned)((uint8_t)auth[7 + i] ^ (uint8_t)s_token[i]);
    }
    if (diff != 0) return false;
    if (fridge_tfcard_transfer_status(transfer) != ESP_OK || !transfer->active ||
        transfer->session_id != s_session_id) return false;
    s_expires_at_us = esp_timer_get_time() + TF_HTTP_TOKEN_TTL_US;
    return true;
}

static esp_err_t options_handler(httpd_req_t *req)
{
    if (!origin_allowed(req)) return send_json_error(req, "403 Forbidden", "origin not allowed");
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static bool parse_range(httpd_req_t *req, uint64_t total, uint64_t *start, uint64_t *range_last, bool *partial)
{
    const size_t len = httpd_req_get_hdr_value_len(req, "Range");
    *start = 0;
    *range_last = total ? total - 1 : 0;
    *partial = false;
    if (len == 0) return true;
    if (len >= 64) return false;
    char range[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) != ESP_OK) return false;
    char *cursor = NULL;
    if (strncmp(range, "bytes=", 6) != 0) return false;
    const unsigned long long value = strtoull(range + 6, &cursor, 10);
    if (cursor == range + 6 || *cursor != '-') return false; // suffix、多区间不接受。
    *start = (uint64_t)value;
    cursor++;
    if (*cursor) {
        char *range_end = NULL;
        const unsigned long long parsed_end = strtoull(cursor, &range_end, 10);
        if (range_end == cursor || *range_end != '\0') return false;
        *range_last = (uint64_t)parsed_end;
    }
    *partial = true;
    return total > 0 && *start <= *range_last && *range_last < total;
}

static esp_err_t download_handler(httpd_req_t *req)
{
    fridge_tfcard_transfer_status_t transfer = {0};
    if (!authorize(req, &transfer)) return send_json_error(req, "401 Unauthorized", "invalid or expired transfer token");
    if (transfer.direction != FRIDGE_TFCARD_TRANSFER_READ) return send_json_error(req, "409 Conflict", "transfer is not a download");

    uint64_t offset = 0;
    uint64_t end = 0;
    bool partial = false;
    if (!parse_range(req, transfer.total_size, &offset, &end, &partial)) {
        return send_json_error(req, "416 Range Not Satisfiable", "invalid range");
    }
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    if (partial) {
        char range[80];
        snprintf(range, sizeof(range), "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
                 offset, end, transfer.total_size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", range);
    }

    uint8_t buffer[FRIDGE_TFCARD_TRANSFER_CHUNK];
    while (offset <= end && transfer.total_size > 0) {
        size_t length_read = 0;
        uint32_t crc32 = 0;
        const size_t wanted = (end - offset + 1) < sizeof(buffer) ? (size_t)(end - offset + 1) : sizeof(buffer);
        esp_err_t err = fridge_tfcard_transfer_read(transfer.session_id, offset, buffer,
                                                    wanted, &length_read, &crc32);
        if (err != ESP_OK || length_read == 0) {
            ESP_LOGE(TAG, "HTTP download read failed at %" PRIu64 ": %s", offset, esp_err_to_name(err));
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, (const char *)buffer, length_read) != ESP_OK) return ESP_FAIL;
        offset += length_read;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool get_upload_offset(httpd_req_t *req, uint64_t *offset)
{
    const size_t len = httpd_req_get_hdr_value_len(req, "X-Upload-Offset");
    if (len == 0) return true;
    if (len >= 32) return false;
    char value[32] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Upload-Offset", value, sizeof(value)) != ESP_OK) return false;
    char *end = NULL;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0') return false;
    *offset = (uint64_t)parsed;
    return true;
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    fridge_tfcard_transfer_status_t transfer = {0};
    if (!authorize(req, &transfer)) return send_json_error(req, "401 Unauthorized", "invalid or expired transfer token");
    if (transfer.direction != FRIDGE_TFCARD_TRANSFER_WRITE) return send_json_error(req, "409 Conflict", "transfer is not an upload");

    uint64_t offset = transfer.offset;
    if (!get_upload_offset(req, &offset) || offset != transfer.offset ||
        offset + req->content_len > transfer.total_size) {
        return send_json_error(req, "409 Conflict", "upload offset or size mismatch");
    }

    if (req->content_len == 0 || req->content_len > TF_HTTP_MAX_PUT_BYTES) {
        return send_json_error(req, "413 Payload Too Large", "each PUT must contain 1..1048576 bytes");
    }
    uint8_t buffer[FRIDGE_TFCARD_TRANSFER_CHUNK];
    size_t remaining = req->content_len;
    unsigned timeout_count = 0;
    while (remaining > 0) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int received = httpd_req_recv(req, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_count <= 3) continue;
        if (received <= 0) return send_json_error(req, "400 Bad Request", "upload body interrupted");
        timeout_count = 0;
        size_t consumed = 0;
        while (consumed < (size_t)received) {
            const size_t part = ((size_t)received - consumed) > FRIDGE_TFCARD_TRANSFER_CHUNK
                                    ? FRIDGE_TFCARD_TRANSFER_CHUNK : ((size_t)received - consumed);
            uint32_t crc32 = 0xFFFFFFFFU;
            for (size_t i = 0; i < part; i++) {
                crc32 ^= buffer[consumed + i];
                for (unsigned bit = 0; bit < 8; bit++) {
                    crc32 = (crc32 >> 1) ^ (0xEDB88320U & (0U - (crc32 & 1U)));
                }
            }
            crc32 = ~crc32;
            const esp_err_t err = fridge_tfcard_transfer_write(transfer.session_id, offset,
                                                               buffer + consumed, part, crc32);
            if (err != ESP_OK) return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
            consumed += part;
            offset += part;
        }
        remaining -= (size_t)received;
    }

    add_cors_headers(req);
    char next[32];
    snprintf(next, sizeof(next), "%" PRIu64, offset);
    httpd_resp_set_hdr(req, "X-Next-Offset", next);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"nextOffset\":%" PRIu64 "}", offset);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_server(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = FRIDGE_TF_HTTP_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 3;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t options = {.uri = TF_HTTP_URI, .method = HTTP_OPTIONS, .handler = options_handler};
    const httpd_uri_t get = {.uri = TF_HTTP_URI, .method = HTTP_GET, .handler = download_handler};
    const httpd_uri_t put = {.uri = TF_HTTP_URI, .method = HTTP_PUT, .handler = upload_handler};
    if ((err = httpd_register_uri_handler(s_server, &options)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_server, &get)) != ESP_OK ||
        (err = httpd_register_uri_handler(s_server, &put)) != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }
    return ESP_OK;
}

static void fill_status(fridge_tf_http_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->server_running = s_server != NULL;
    out->authorized = s_token[0] && esp_timer_get_time() < s_expires_at_us;
    out->session_id = out->authorized ? s_session_id : 0;
    out->port = FRIDGE_TF_HTTP_PORT;
    if (out->authorized) {
        out->expires_in_seconds = (uint32_t)((s_expires_at_us - esp_timer_get_time()) / 1000000LL);
        strlcpy(out->token, s_token, sizeof(out->token));
        strlcpy(out->url, s_url, sizeof(out->url));
    }
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
}

esp_err_t fridge_tf_http_enable(fridge_tf_http_status_t *out)
{
    fridge_network_status_t network = {0};
    fridge_tfcard_transfer_status_t transfer = {0};
    esp_err_t err = fridge_network_get_status(&network);
    if (err != ESP_OK || !network.hosted_ready || !network.connected || !network.ip[0] || network.c6_ota_guard) {
        set_error("Wi-Fi/Hosted未就绪，无法启用TF HTTP传输");
        fill_status(out);
        return ESP_ERR_INVALID_STATE;
    }
    err = fridge_tfcard_transfer_status(&transfer);
    if (err != ESP_OK || !transfer.active) {
        set_error("请先通过COM15创建TF传输会话");
        fill_status(out);
        return ESP_ERR_INVALID_STATE;
    }
    if ((err = start_server()) != ESP_OK) {
        set_error(esp_err_to_name(err));
        fill_status(out);
        return err;
    }
    uint8_t random[FRIDGE_TF_HTTP_TOKEN_HEX_LEN / 2];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) snprintf(s_token + i * 2, 3, "%02x", random[i]);
    s_session_id = transfer.session_id;
    s_expires_at_us = esp_timer_get_time() + TF_HTTP_TOKEN_TTL_US;
    snprintf(s_url, sizeof(s_url), "http://%s:%u%s", network.ip, FRIDGE_TF_HTTP_PORT, TF_HTTP_URI);
    set_error("");
    fill_status(out);
    ESP_LOGI(TAG, "TF HTTP授权已创建: session=%lu direction=%s ttl=600s",
             (unsigned long)s_session_id,
             transfer.direction == FRIDGE_TFCARD_TRANSFER_WRITE ? "upload" : "download");
    return ESP_OK;
}

esp_err_t fridge_tf_http_get_status(fridge_tf_http_status_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (s_token[0] && esp_timer_get_time() >= s_expires_at_us) invalidate_authorization();
    fill_status(out);
    return ESP_OK;
}

esp_err_t fridge_tf_http_disable(void)
{
    invalidate_authorization();
    set_error("");
    if (!s_server) return ESP_OK;
    const esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
