// 板载 ESP32-C6 Hosted Slave 串口维护升级组件。
// 数据由 P4 的 COM15 JSON Lines 分块送入，再通过 Hosted OTA RPC 写入 C6 非活动分区。

#include "fridge_c6_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

static const char *TAG = "fridge_c6_ota";

static SemaphoreHandle_t s_mutex;
static fridge_c6_ota_status_t s_status;
static psa_hash_operation_t s_sha_ctx = PSA_HASH_OPERATION_INIT;
static uint8_t s_expected_sha256[32];
static bool s_sha_started;

static esp_err_t ensure_mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static void set_error_locked(esp_err_t err, const char *operation)
{
    if (s_sha_started) {
        (void)psa_hash_abort(&s_sha_ctx);
        memset(&s_sha_ctx, 0, sizeof(s_sha_ctx));
        s_sha_started = false;
    }
    s_status.state = FRIDGE_C6_OTA_FAILED;
    snprintf(s_status.last_error,
             sizeof(s_status.last_error),
             "%s failed: %s",
             operation ? operation : "C6 OTA",
             esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", s_status.last_error);
}

static int hex_nibble(char ch)
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

static bool parse_sha256(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != FRIDGE_C6_OTA_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        const int hi = hex_nibble(hex[i * 2]);
        const int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void format_sha256(const uint8_t hash[32], char out[FRIDGE_C6_OTA_SHA256_HEX_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0x0F];
    }
    out[FRIDGE_C6_OTA_SHA256_HEX_LEN] = '\0';
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

const char *fridge_c6_ota_state_name(fridge_c6_ota_state_t state)
{
    switch (state) {
    case FRIDGE_C6_OTA_WRITING:
        return "writing";
    case FRIDGE_C6_OTA_WAIT_ACTIVATE:
        return "wait_activate";
    case FRIDGE_C6_OTA_COMPLETE:
        return "complete";
    case FRIDGE_C6_OTA_FAILED:
        return "failed";
    case FRIDGE_C6_OTA_IDLE:
    default:
        return "idle";
    }
}

esp_err_t fridge_c6_ota_begin(uint32_t image_size,
                              const char *sha256_hex,
                              fridge_c6_ota_activation_t activation,
                              uint32_t *out_session)
{
    if (!out_session || image_size == 0 || image_size > FRIDGE_C6_OTA_MAX_IMAGE_SIZE ||
        !parse_sha256(sha256_hex, s_expected_sha256) ||
        (activation != FRIDGE_C6_OTA_LEGACY_END_REBOOTS &&
         activation != FRIDGE_C6_OTA_EXPLICIT_ACTIVATE)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_status.state == FRIDGE_C6_OTA_WRITING || s_status.state == FRIDGE_C6_OTA_WAIT_ACTIVATE) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_hosted_coprocessor_fwver_t version = {0};
    if (activation == FRIDGE_C6_OTA_EXPLICIT_ACTIVATE) {
        err = esp_hosted_get_coprocessor_fwversion(&version);
        if (err != ESP_OK) {
            memset(&s_status, 0, sizeof(s_status));
            set_error_locked(err, "read C6 version");
            xSemaphoreGive(s_mutex);
            return err;
        }
    } else {
        // 厂商随板镜像已静态确认是 Hosted 0.0.6；该版本没有查询版本的 RPC。
        version.patch1 = 6;
    }

    err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        memset(&s_status, 0, sizeof(s_status));
        set_error_locked(err, "C6 OTA begin");
        xSemaphoreGive(s_mutex);
        return err;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = FRIDGE_C6_OTA_WRITING;
    s_status.session = esp_random();
    if (s_status.session == 0) {
        s_status.session = 1;
    }
    s_status.expected_size = image_size;
    s_status.slave_major = version.major1;
    s_status.slave_minor = version.minor1;
    s_status.slave_patch = version.patch1;
    s_status.requires_activation = activation == FRIDGE_C6_OTA_EXPLICIT_ACTIVATE;
    strlcpy(s_status.expected_sha256, sha256_hex, sizeof(s_status.expected_sha256));

    psa_status_t psa_ret = psa_crypto_init();
    if (psa_ret == PSA_SUCCESS) {
        memset(&s_sha_ctx, 0, sizeof(s_sha_ctx));
        psa_ret = psa_hash_setup(&s_sha_ctx, PSA_ALG_SHA_256);
    }
    err = psa_ret == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        set_error_locked(err, "SHA-256 init");
        xSemaphoreGive(s_mutex);
        return err;
    }
    s_sha_started = true;
    *out_session = s_status.session;
    ESP_LOGI(TAG,
             "C6 OTA started: slave=%lu.%lu.%lu size=%lu session=%08lx",
             (unsigned long)version.major1,
             (unsigned long)version.minor1,
             (unsigned long)version.patch1,
             (unsigned long)image_size,
             (unsigned long)s_status.session);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t fridge_c6_ota_write(uint32_t session,
                              uint32_t offset,
                              const uint8_t *data,
                              size_t data_len,
                              uint32_t expected_crc32,
                              uint32_t *out_next_offset,
                              bool *out_duplicate)
{
    if (!data || data_len == 0 || data_len > FRIDGE_C6_OTA_MAX_CHUNK_SIZE ||
        !out_next_offset || !out_duplicate) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_duplicate = false;
    if (s_status.state != FRIDGE_C6_OTA_WRITING || session != s_status.session) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t crc = crc32_ieee(data, data_len);
    if (crc != expected_crc32) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_CRC;
    }

    if (offset == s_status.last_offset && data_len == s_status.last_length &&
        crc == s_status.last_crc32 && offset + data_len == s_status.received_size) {
        s_status.last_chunk_duplicate = true;
        *out_duplicate = true;
        *out_next_offset = s_status.received_size;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (offset != s_status.received_size || offset + data_len > s_status.expected_size) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    if (offset == 0 && data[0] != 0xE9) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = esp_hosted_slave_ota_write((uint8_t *)data, (uint32_t)data_len);
    if (err != ESP_OK) {
        set_error_locked(err, "C6 OTA write");
        xSemaphoreGive(s_mutex);
        return err;
    }
    if (psa_hash_update(&s_sha_ctx, data, data_len) != PSA_SUCCESS) {
        err = ESP_FAIL;
        set_error_locked(err, "SHA-256 update");
        xSemaphoreGive(s_mutex);
        return err;
    }

    s_status.last_offset = offset;
    s_status.last_length = (uint32_t)data_len;
    s_status.last_crc32 = crc;
    s_status.received_size += (uint32_t)data_len;
    s_status.last_chunk_duplicate = false;
    *out_next_offset = s_status.received_size;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t fridge_c6_ota_end(uint32_t session)
{
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_status.state != FRIDGE_C6_OTA_WRITING || session != s_status.session ||
        s_status.received_size != s_status.expected_size || !s_sha_started) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t actual_sha256[32];
    size_t actual_sha256_len = 0;
    if (psa_hash_finish(&s_sha_ctx,
                        actual_sha256,
                        sizeof(actual_sha256),
                        &actual_sha256_len) != PSA_SUCCESS ||
        actual_sha256_len != sizeof(actual_sha256)) {
        err = ESP_FAIL;
        set_error_locked(err, "SHA-256 finish");
        xSemaphoreGive(s_mutex);
        return err;
    }
    memset(&s_sha_ctx, 0, sizeof(s_sha_ctx));
    s_sha_started = false;
    format_sha256(actual_sha256, s_status.actual_sha256);
    if (memcmp(actual_sha256, s_expected_sha256, sizeof(actual_sha256)) != 0) {
        err = ESP_ERR_INVALID_CRC;
        set_error_locked(err, "C6 image SHA-256");
        xSemaphoreGive(s_mutex);
        return err;
    }

    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        set_error_locked(err, "C6 OTA end");
        xSemaphoreGive(s_mutex);
        return err;
    }
    s_status.state = s_status.requires_activation ? FRIDGE_C6_OTA_WAIT_ACTIVATE : FRIDGE_C6_OTA_COMPLETE;
    ESP_LOGI(TAG, "C6 OTA image validated; activation=%s", s_status.requires_activation ? "required" : "legacy-auto");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t fridge_c6_ota_activate(uint32_t session)
{
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_status.state != FRIDGE_C6_OTA_WAIT_ACTIVATE || session != s_status.session ||
        !s_status.requires_activation) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) {
        set_error_locked(err, "C6 OTA activate");
        xSemaphoreGive(s_mutex);
        return err;
    }
    s_status.state = FRIDGE_C6_OTA_COMPLETE;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void fridge_c6_ota_get_status(fridge_c6_ota_status_t *status)
{
    if (!status) {
        return;
    }
    if (ensure_mutex() != ESP_OK || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        memset(status, 0, sizeof(*status));
        status->state = FRIDGE_C6_OTA_FAILED;
        strlcpy(status->last_error, "C6 OTA state lock unavailable", sizeof(status->last_error));
        return;
    }
    *status = s_status;
    xSemaphoreGive(s_mutex);
}
