// 冰箱小精灵公共语音会话实现。
// 负责把 audio 组件保留的最近一段 PCM 录音送入 ASR，再携带项目上下文调用 AI，并写回本地会话历史。

#include "fridge_voice_session.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_ai_actions.h"
#include "fridge_ai_client.h"
#include "fridge_ai_context.h"
#include "fridge_asr.h"
#include "fridge_mqtt_protocol.h"
#include "fridge_speaker.h"
#include "fridge_storage.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define VOICE_HTTP_WORKER_STACK 20480

static const char *TAG = "voice_session";

typedef enum {
    VOICE_HTTP_JOB_ASR = 0,
    VOICE_HTTP_JOB_AI,
} voice_http_job_type_t;

typedef struct {
    fridge_asr_config_update_t *config;
    fridge_asr_result_t *result;
    esp_err_t err;
    SemaphoreHandle_t done;
} voice_asr_job_t;

typedef struct {
    fridge_ai_config_update_t *config;
    fridge_ai_assistant_request_t *request;
    fridge_ai_assistant_result_t *result;
    esp_err_t err;
    SemaphoreHandle_t done;
} voice_ai_job_t;

typedef struct {
    voice_http_job_type_t type;
    void *job;
} voice_http_dispatch_t;

static QueueHandle_t s_http_queue;
static TaskHandle_t s_http_task;

static void *voice_large_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *voice_large_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = voice_large_alloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static size_t convert_storage_history_to_ai_history(const fridge_storage_chat_history_t *storage_history,
                                                    fridge_ai_chat_history_item_t *history,
                                                    size_t max_history)
{
    if (!storage_history || !history || max_history == 0) {
        return 0;
    }

    size_t source_count = storage_history->count;
    if (source_count > FRIDGE_STORAGE_MAX_CHAT_MESSAGES) {
        source_count = FRIDGE_STORAGE_MAX_CHAT_MESSAGES;
    }
    if (source_count > 0 && strcmp(storage_history->messages[source_count - 1].role, "user") == 0) {
        // 历史注入只使用已经完成的 user/assistant 轮次，避免异常中断遗留的 user 单边消息破坏请求格式。
        source_count--;
    }
    if (source_count % 2 != 0) {
        source_count--;
    }

    size_t pair_limit = (max_history / 2) * 2;
    if (pair_limit == 0) {
        return 0;
    }
    size_t start = source_count > pair_limit ? (source_count - pair_limit) : 0;
    if (start % 2 != 0) {
        start++;
    }

    size_t count = 0;
    for (size_t i = start; i < source_count && count < max_history; i++) {
        const fridge_storage_chat_message_t *message = &storage_history->messages[i];
        if (message->content[0] == '\0') {
            continue;
        }
        bool expect_user = (count % 2) == 0;
        if ((expect_user && strcmp(message->role, "user") != 0) ||
            (!expect_user && strcmp(message->role, "assistant") != 0)) {
            // 真实 OpenAI-compatible 服务对 role 顺序更敏感，遇到坏轮次时宁可丢弃前缀。
            count = 0;
            continue;
        }
        strlcpy(history[count].role,
                strcmp(message->role, "assistant") == 0 ? "assistant" : "user",
                sizeof(history[count].role));
        strlcpy(history[count].content, message->content, sizeof(history[count].content));
        count++;
    }
    return (count / 2) * 2;
}

static void voice_session_restore_wake(bool was_listening)
{
    // 仅在进入会话前确实暂停过唤醒词时才恢复，避免误启用本就关闭的唤醒监听。
    if (was_listening) {
        esp_err_t err = fridge_wake_word_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restart wake word failed: %s", esp_err_to_name(err));
        }
    }
}

static void voice_asr_http_job(void *arg)
{
    voice_asr_job_t *job = (voice_asr_job_t *)arg;
    // PSRAM 大栈 worker 只跑 ASR HTTPS；配置读取和网络校时必须在调用方内部栈完成。
    job->err = fridge_asr_transcribe_latest_recording_with_config(job->config, job->result);
    ESP_LOGI(TAG, "voice ASR HTTP job done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
}

static void voice_ai_http_job(void *arg)
{
    voice_ai_job_t *job = (voice_ai_job_t *)arg;
    // PSRAM 大栈 worker 只跑 AI HTTPS；上下文、历史和配置都已在内部栈任务中准备好。
    job->err = fridge_ai_client_assistant_chat_with_config(job->config, job->request, job->result);
    ESP_LOGI(TAG, "voice AI HTTP job done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
}

static void voice_http_worker_task(void *arg)
{
    (void)arg;
    voice_http_dispatch_t dispatch = {0};
    for (;;) {
        if (xQueueReceive(s_http_queue, &dispatch, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (dispatch.type) {
        case VOICE_HTTP_JOB_ASR:
            voice_asr_http_job(dispatch.job);
            break;
        case VOICE_HTTP_JOB_AI:
            voice_ai_http_job(dispatch.job);
            break;
        default:
            break;
        }
    }
}

static BaseType_t voice_dispatch_http_worker(voice_http_dispatch_t dispatch)
{
    if (!s_http_queue) {
        s_http_queue = xQueueCreate(1, sizeof(voice_http_dispatch_t));
        if (!s_http_queue) {
            ESP_LOGW(TAG, "voice HTTP queue create failed");
            return pdFAIL;
        }
    }

    if (!s_http_task) {
        BaseType_t ok = xTaskCreateWithCaps(voice_http_worker_task,
                                            "voice_http",
                                            VOICE_HTTP_WORKER_STACK,
                                            NULL,
                                            4,
                                            &s_http_task,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            s_http_task = NULL;
            ESP_LOGW(TAG,
                     "voice_http PSRAM stack create failed, stack=%lu, largest_internal=%u bytes, free_psram=%u KB",
                     (unsigned long)VOICE_HTTP_WORKER_STACK,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));
            return pdFAIL;
        }
    }

    if (uxQueueMessagesWaiting(s_http_queue) > 0) {
        ESP_LOGW(TAG, "voice HTTP worker is busy");
        return pdFAIL;
    }
    return xQueueSend(s_http_queue, &dispatch, pdMS_TO_TICKS(1000)) == pdTRUE ? pdPASS : pdFAIL;
}

static void strip_known_draft_prefix(char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }

    // 少数 OpenAI-compatible 服务会把内部草稿串混入正文，例如：
    // "Drafting the Response (Iterative refinement):\n你好..."。这不是用户可见内容，
    // 也不应进入 TTS/本地历史；只匹配明确英文前缀，避免误伤正常中文。
    static const char *prefixes[] = {
        "Drafting the Response",
        "Drafting response",
        "Iterative refinement",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        const char *hit = strstr(text, prefixes[i]);
        if (hit != text) {
            continue;
        }
        const char *colon = strchr(text, ':');
        if (!colon) {
            return;
        }
        const char *body = colon + 1;
        while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') {
            body++;
        }
        if (*body != '\0') {
            memmove(text, body, strlen(body) + 1);
        }
        return;
    }
}

esp_err_t fridge_voice_session_run_latest_recording_ex(const fridge_voice_session_options_t *options,
                                                       fridge_voice_session_result_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    // 唤醒词与麦克风/扬声器共用 I2S 时钟，录音/识别期间先暂停监听，结束后按原状态恢复。
    // 注意：当前比赛固件已在 main.c 禁用唤醒词，这里的暂停/恢复实际是 no-op，保留逻辑为后续启用留口。
    bool wake_was_listening = false;
    if (options && options->manage_wake_word) {
        fridge_wake_word_status_t wake = {0};
        if (fridge_wake_word_get_status(&wake) == ESP_OK && wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING) {
            if (fridge_wake_word_stop() == ESP_OK) {
                wake_was_listening = true;
            }
        }
    }

    fridge_asr_config_update_t *asr_config = voice_large_calloc(1, sizeof(*asr_config));
    fridge_ai_config_update_t *ai_config = voice_large_calloc(1, sizeof(*ai_config));
    fridge_asr_result_t *asr = voice_large_calloc(1, sizeof(*asr));
    fridge_ai_assistant_result_t *ai = voice_large_calloc(1, sizeof(*ai));
    fridge_ai_assistant_request_t *assistant_request = voice_large_calloc(1, sizeof(*assistant_request));
    fridge_ai_chat_history_item_t *history = voice_large_calloc(FRIDGE_AI_MAX_CHAT_HISTORY, sizeof(*history));
    fridge_storage_chat_history_t *storage_history = voice_large_calloc(1, sizeof(*storage_history));
    fridge_storage_chat_message_t *persisted_messages = voice_large_calloc(2, sizeof(*persisted_messages));
    fridge_ai_context_preview_t *preview = voice_large_calloc(1, sizeof(*preview));
    char *clean_reply = voice_large_calloc(1, FRIDGE_VOICE_SESSION_REPLY_LEN + 1);
    char *visible_reply = voice_large_calloc(1, FRIDGE_VOICE_SESSION_REPLY_LEN + 1);
    if (!asr_config || !ai_config || !asr || !ai || !assistant_request || !history || !storage_history ||
        !persisted_messages || !preview || !clean_reply || !visible_reply) {
        free(asr_config);
        free(ai_config);
        free(asr);
        free(ai);
        free(assistant_request);
        free(history);
        free(storage_history);
        free(persisted_messages);
        free(preview);
        free(clean_reply);
        free(visible_reply);
        strlcpy(out->error, "voice session allocation failed", sizeof(out->error));
        voice_session_restore_wake(wake_was_listening);
        return ESP_ERR_NO_MEM;
    }

    // NVS 配置读取和网络校时留在调用方的内部 SRAM 栈中；HTTPS 深调用栈交给 PSRAM worker。
    esp_err_t err = fridge_asr_load_active_config(asr_config);
    if (err == ESP_OK) {
        err = fridge_ai_client_load_active_config(ai_config);
    }
    if (err == ESP_OK) {
        err = fridge_ai_client_ensure_network_ready(out->error, sizeof(out->error));
    }
    SemaphoreHandle_t done = NULL;
    if (err == ESP_OK) {
        done = xSemaphoreCreateBinary();
        if (!done) {
            err = ESP_ERR_NO_MEM;
            strlcpy(out->error, "voice session sync object failed", sizeof(out->error));
        }
    }
    if (err == ESP_OK) {
        voice_asr_job_t asr_job = {
            .config = asr_config,
            .result = asr,
            .err = ESP_FAIL,
            .done = done,
        };
        BaseType_t ok = voice_dispatch_http_worker((voice_http_dispatch_t) {
            .type = VOICE_HTTP_JOB_ASR,
            .job = &asr_job,
        });
        if (ok != pdPASS) {
            err = ESP_ERR_NO_MEM;
            strlcpy(out->error, "voice ASR worker create failed", sizeof(out->error));
        } else {
            xSemaphoreTake(done, portMAX_DELAY);
            err = asr_job.err;
        }
    }
    if (err == ESP_OK && (asr->text[0] == '\0' || strlen(asr->text) < 2)) {
        // ASR 空文本不继续请求 AI，避免兼容 API 因空 user content 返回 400。
        strlcpy(asr->error, "ASR 转写为空或过短，请重新录音", sizeof(asr->error));
        err = ESP_ERR_INVALID_RESPONSE;
    }

    size_t history_pruned_count = 0;
    if (err == ESP_OK) {
        fridge_ai_task_request_t context_request = {
            .include_inventory = true,
            .include_memory = true,
            .include_reminders = true,
            .include_preferences = true,
        };
        strlcpy(context_request.task_type, "voice_intent_parse", sizeof(context_request.task_type));
        strlcpy(context_request.user_text, asr->text, sizeof(context_request.user_text));

        err = fridge_storage_get_chat_history(storage_history, &history_pruned_count);
        if (err == ESP_OK) {
            err = fridge_ai_context_build_preview(&context_request, preview);
        }
        if (err == ESP_OK) {
            strlcpy(assistant_request->message, asr->text, sizeof(assistant_request->message));
            strlcpy(assistant_request->task_type, "voice_intent_parse", sizeof(assistant_request->task_type));
            strlcpy(assistant_request->context_json, preview->preview_json, sizeof(assistant_request->context_json));
            assistant_request->history = history;
            assistant_request->history_count = convert_storage_history_to_ai_history(storage_history, history, FRIDGE_AI_MAX_CHAT_HISTORY);
            assistant_request->context_injected = true;
            assistant_request->needs_confirmation = preview->needs_confirmation;
            assistant_request->local_snapshot_version = preview->local_snapshot_version;
            voice_ai_job_t ai_job = {
                .config = ai_config,
                .request = assistant_request,
                .result = ai,
                .err = ESP_FAIL,
                .done = done,
            };
            BaseType_t ok = voice_dispatch_http_worker((voice_http_dispatch_t) {
                .type = VOICE_HTTP_JOB_AI,
                .job = &ai_job,
            });
            if (ok != pdPASS) {
                err = ESP_ERR_NO_MEM;
                strlcpy(ai->chat.error, "voice AI worker create failed", sizeof(ai->chat.error));
            } else {
                xSemaphoreTake(done, portMAX_DELAY);
                err = ai_job.err;
            }
        }
    }

    if (err == ESP_OK) {
        bool memory_updated = false;
        (void)fridge_storage_apply_memory_directive(ai->chat.reply,
                                                    clean_reply,
                                                    FRIDGE_VOICE_SESSION_REPLY_LEN + 1,
                                                    &memory_updated);
        if (clean_reply[0] != '\0') {
            strlcpy(ai->chat.reply, clean_reply, sizeof(ai->chat.reply));
        }
        fridge_ai_action_result_t action_result = {0};
        esp_err_t action_err = fridge_ai_actions_strip_directives(ai->chat.reply,
                                                                  visible_reply,
                                                                  FRIDGE_VOICE_SESSION_REPLY_LEN + 1,
                                                                  &action_result);
        bool action_feedback = action_result.tool[0] != '\0' && action_result.message[0] != '\0';
        if ((action_err == ESP_OK && action_result.executed) || action_feedback) {
            strlcpy(ai->chat.reply, action_result.message, sizeof(ai->chat.reply));
        } else if (visible_reply[0] != '\0') {
            strlcpy(ai->chat.reply, visible_reply, sizeof(ai->chat.reply));
        }
        strip_known_draft_prefix(ai->chat.reply);

        snprintf(persisted_messages[0].id, sizeof(persisted_messages[0].id), "msg_%" PRIu32 "_u", (uint32_t)(xTaskGetTickCount() & 0xFFFFFF));
        strlcpy(persisted_messages[0].role, "user", sizeof(persisted_messages[0].role));
        strlcpy(persisted_messages[0].content, asr->text, sizeof(persisted_messages[0].content));
        strlcpy(persisted_messages[0].task_type, "voice_intent_parse", sizeof(persisted_messages[0].task_type));
        snprintf(persisted_messages[1].id, sizeof(persisted_messages[1].id), "msg_%" PRIu32 "_a", (uint32_t)((xTaskGetTickCount() + 1) & 0xFFFFFF));
        strlcpy(persisted_messages[1].role, "assistant", sizeof(persisted_messages[1].role));
        strlcpy(persisted_messages[1].content, ai->chat.reply, sizeof(persisted_messages[1].content));
        strlcpy(persisted_messages[1].task_type, "voice_intent_parse", sizeof(persisted_messages[1].task_type));
        esp_err_t persist_err = fridge_storage_append_chat_messages(persisted_messages, 2, &history_pruned_count);

        // 本轮已写入本地历史，立即把新增对话增量上报云端 ai_history 域，让小程序也能看到。
        // 离线/未连接时这里返回非 OK 很正常，靠 MQTT 重连补发，不影响本地对话和 TTS。
        if (persist_err == ESP_OK) {
            esp_err_t sync_err = fridge_mqtt_publish_chat_history_delta();
            if (sync_err != ESP_OK) {
                ESP_LOGW(TAG, "publish chat history delta failed: %s", esp_err_to_name(sync_err));
            }
        }
    }

    // TTS 播报放在历史写入和上报之后；此时录音早已结束，避免与麦克风 RX 争用同一组 BCLK/WS。
    // ai->chat.reply 此时已经剥离 MEMORY_OP/工具指令，可直接作为屏幕展示和 TTS 文本。
    if (err == ESP_OK && options && options->play_tts && ai->chat.reply[0] != '\0' &&
        fridge_speaker_get_tts_enabled()) {
        fridge_speaker_status_t speaker_status = {0};
        esp_err_t tts_err = fridge_speaker_synthesize_and_play(ai->chat.reply, &speaker_status);
        if (tts_err == ESP_OK) {
            out->tts_played = true;
        } else {
            ESP_LOGW(TAG, "TTS play failed after AI reply: %s",
                     speaker_status.error[0] ? speaker_status.error : esp_err_to_name(tts_err));
        }
    }

    strlcpy(out->transcript, asr->text, sizeof(out->transcript));
    strlcpy(out->reply, ai->chat.reply, sizeof(out->reply));
    strlcpy(out->asr_model, asr->model, sizeof(out->asr_model));
    strlcpy(out->ai_model, ai->chat.model, sizeof(out->ai_model));
    out->asr_http_status = asr->http_status;
    out->ai_http_status = ai->chat.http_status;
    out->asr_latency_ms = asr->latency_ms;
    out->ai_latency_ms = ai->chat.latency_ms;
    out->audio_bytes = asr->audio_bytes;
    out->history_pruned_count = history_pruned_count;
    if (err != ESP_OK) {
        strlcpy(out->error,
                asr->error[0] ? asr->error : (ai->chat.error[0] ? ai->chat.error : esp_err_to_name(err)),
                sizeof(out->error));
    }

    ESP_LOGI(TAG, "voice session done err=%s, asr_http=%d, ai_http=%d", esp_err_to_name(err), asr->http_status, ai->chat.http_status);
    if (done) {
        vSemaphoreDelete(done);
    }
    free(asr_config);
    free(ai_config);
    free(asr);
    free(ai);
    free(assistant_request);
    free(history);
    free(storage_history);
    free(persisted_messages);
    free(preview);
    free(clean_reply);
    free(visible_reply);
    voice_session_restore_wake(wake_was_listening);
    return err;
}

esp_err_t fridge_voice_session_run_latest_recording(fridge_voice_session_result_t *out)
{
    // 兼容入口：USB 测试与状态机自动语音不需要组件内播报或管理唤醒词，options 全为 false。
    const fridge_voice_session_options_t options = {0};
    return fridge_voice_session_run_latest_recording_ex(&options, out);
}
