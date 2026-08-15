// 冰箱小精灵 ESP32-P4 唤醒后 ASR 编排。
// WakeNet 回调仅投递信号；内部 SRAM 协调任务负责 NVS/状态，PSRAM 大栈任务负责 HTTPS。

#include "fridge_voice_asr.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_audio.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define VOICE_ASR_POLICY_TICK_MS 500
#define VOICE_ASR_AUTO_TASK_STACK 16384
#define VOICE_ASR_POLICY_TASK_STACK 4096
#define VOICE_ASR_HTTP_TASK_STACK 20480
#define VOICE_DIALOG_QUEUE_LEN 8
#define VOICE_DIALOG_WAKE_STOP_WAIT_MS 600
#define VOICE_DIALOG_IDLE_WAIT_MS 10000
#define VOICE_DIALOG_SILENCE_END_MS 1500
#define VOICE_DIALOG_MIN_SPEECH_MS 180
#define VOICE_DIALOG_POLL_MS 40
#define VOICE_DIALOG_VAD_RMS 80
#define VOICE_DIALOG_VAD_PEAK 400

static const char *TAG = "voice_asr";

typedef struct {
    const fridge_asr_config_update_t *config;
    fridge_asr_result_t *result;
    SemaphoreHandle_t done;
    esp_err_t err;
} voice_asr_http_job_t;

static SemaphoreHandle_t s_run_lock;
static SemaphoreHandle_t s_state_lock;
static TaskHandle_t s_auto_task;
#if !CONFIG_FRIDGE_P4_UI_ENABLE
static TaskHandle_t s_policy_task;
#endif
static QueueHandle_t s_dialog_queue;
static bool s_initialized;
static bool s_hardware_ready;
static bool s_manual_session;
static uint32_t s_trigger_id;
static fridge_voice_asr_event_cb_t s_event_cb;
static void *s_event_ctx;
static fridge_voice_dialog_status_t s_dialog_status;
static bool s_dialog_start_pending;
static bool s_dialog_pending_confirmation;
static bool s_dialog_auto_complete;
static char s_dialog_pending_run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN];
static char s_dialog_pending_call_id[FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN];

typedef enum {
    VOICE_DIALOG_CMD_START = 0,
    VOICE_DIALOG_CMD_STOP,
    VOICE_DIALOG_CMD_AGENT_EVENT,
} voice_dialog_command_type_t;

// Agent 事件在投递前压缩为连续会话真正需要的字段，避免内部 RAM 队列复制 512B 正文。
typedef struct {
    voice_dialog_command_type_t type;
    fridge_agent_runtime_event_kind_t agent_kind;
    bool working;
    bool requires_confirmation;
    bool dialog_complete;
    uint8_t tool_status;
    char run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN];
    char call_id[FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN];
    char state[32];
    char detail[96];
} voice_dialog_command_t;

typedef enum {
    VOICE_DIALOG_CAPTURE_UTTERANCE = 0,
    VOICE_DIALOG_CAPTURE_TIMEOUT,
    VOICE_DIALOG_CAPTURE_STOPPED,
    VOICE_DIALOG_CAPTURE_ERROR,
} voice_dialog_capture_result_t;

typedef enum {
    VOICE_DIALOG_AGENT_NEXT_TURN = 0,
    VOICE_DIALOG_AGENT_FINISH,
} voice_dialog_agent_result_t;

const char *fridge_voice_asr_state_text(fridge_voice_asr_state_t state)
{
    switch (state) {
    case FRIDGE_VOICE_ASR_STATE_RECORDING:
        return "recording";
    case FRIDGE_VOICE_ASR_STATE_TRANSCRIBING:
        return "transcribing";
    case FRIDGE_VOICE_ASR_STATE_DONE:
        return "done";
    case FRIDGE_VOICE_ASR_STATE_ERROR:
        return "error";
    case FRIDGE_VOICE_ASR_STATE_IDLE:
    default:
        return "idle";
    }
}

static void emit_event(fridge_voice_asr_state_t state,
                       uint32_t trigger_id,
                       const fridge_asr_result_t *result,
                       const char *error)
{
    fridge_voice_asr_event_cb_t cb = NULL;
    void *ctx = NULL;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    cb = s_event_cb;
    ctx = s_event_ctx;
    xSemaphoreGive(s_state_lock);
    if (!cb) {
        return;
    }

    fridge_voice_asr_event_t event = {
        .state = state,
        .trigger_id = trigger_id,
    };
    if (result) {
        event.result = *result;
    }
    strlcpy(event.error,
            error ? error : (result ? result->error : ""),
            sizeof(event.error));
    cb(&event, ctx);
}

// HTTPS 深调用栈放在 PSRAM；任务只使用调用方已加载好的配置，不在 PSRAM 栈上访问 NVS。
static void voice_asr_http_task(void *arg)
{
    voice_asr_http_job_t *job = (voice_asr_http_job_t *)arg;
    job->err = fridge_asr_transcribe_latest_recording_with_config(job->config, job->result);
    ESP_LOGI(TAG,
             "ASR HTTP done: err=%s http=%d stack_hwm=%u words",
             esp_err_to_name(job->err),
             job->result->http_status,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(job->done);
    // HTTPS worker 由 xTaskCreateWithCaps() 创建，必须配套释放 PSRAM 栈。
    vTaskDeleteWithCaps(NULL);
}

esp_err_t fridge_voice_asr_transcribe_latest(fridge_asr_result_t *out)
{
    if (!out || !s_run_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (xSemaphoreTake(s_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        strlcpy(out->error, "ASR busy", sizeof(out->error));
        return ESP_ERR_INVALID_STATE;
    }

    // 配置读取在当前内部 SRAM 栈完成；API Key 只存在于设备内存，不写日志或串口。
    fridge_asr_config_update_t *config = heap_caps_calloc(1,
                                                          sizeof(*config),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!config) {
        config = calloc(1, sizeof(*config));
    }
    esp_err_t err = config ? fridge_asr_load_active_config(config) : ESP_ERR_NO_MEM;
    SemaphoreHandle_t done = NULL;
    if (err == ESP_OK) {
        done = xSemaphoreCreateBinary();
        if (!done) {
            err = ESP_ERR_NO_MEM;
        }
    }

    voice_asr_http_job_t job = {
        .config = config,
        .result = out,
        .done = done,
        .err = ESP_FAIL,
    };
    if (err == ESP_OK) {
        BaseType_t ok = xTaskCreateWithCaps(voice_asr_http_task,
                                            "voice_asr_http",
                                            VOICE_ASR_HTTP_TASK_STACK,
                                            &job,
                                            4,
                                            NULL,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            err = ESP_ERR_NO_MEM;
            strlcpy(out->error, "ASR HTTPS worker create failed", sizeof(out->error));
        } else {
            // HTTP 客户端自身有配置超时；必须等 worker 结束，避免栈上 job 生命周期提前结束。
            xSemaphoreTake(done, portMAX_DELAY);
            err = job.err;
        }
    }
    if (err != ESP_OK && out->error[0] == '\0') {
        strlcpy(out->error, esp_err_to_name(err), sizeof(out->error));
    }

    if (done) {
        vSemaphoreDelete(done);
    }
    free(config);
    xSemaphoreGive(s_run_lock);
    return err;
}

const char *fridge_voice_dialog_state_text(fridge_voice_dialog_state_t state)
{
    switch (state) {
    case FRIDGE_VOICE_DIALOG_STATE_PROMPTING: return "提示中";
    case FRIDGE_VOICE_DIALOG_STATE_LISTENING: return "正在听";
    case FRIDGE_VOICE_DIALOG_STATE_TRANSCRIBING: return "识别中";
    case FRIDGE_VOICE_DIALOG_STATE_THINKING: return "思考中";
    case FRIDGE_VOICE_DIALOG_STATE_WAITING_CONFIRMATION: return "等待确认";
    case FRIDGE_VOICE_DIALOG_STATE_SPEAKING: return "正在播报";
    case FRIDGE_VOICE_DIALOG_STATE_ERROR: return "语音错误";
    case FRIDGE_VOICE_DIALOG_STATE_IDLE:
    default: return "已结束";
    }
}

// 连续对话状态只在协调任务下更新；UI 通过 get_status 复制快照，不直接持有 LVGL 对象。
static void dialog_set_status(fridge_voice_dialog_state_t state, bool active, const char *detail)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_dialog_status.active = active;
    s_dialog_status.state = state;
    strlcpy(s_dialog_status.state_text, fridge_voice_dialog_state_text(state),
            sizeof(s_dialog_status.state_text));
    strlcpy(s_dialog_status.detail, detail ? detail : "", sizeof(s_dialog_status.detail));
    xSemaphoreGive(s_state_lock);
}

static bool dialog_is_active(void)
{
    bool active = false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    active = s_dialog_status.active;
    xSemaphoreGive(s_state_lock);
    return active;
}

static bool dialog_voice_detected(const fridge_audio_status_t *audio)
{
    return audio && audio->live_rms >= VOICE_DIALOG_VAD_RMS &&
           audio->live_peak_abs >= VOICE_DIALOG_VAD_PEAK;
}

static void dialog_stop_recording_if_needed(void)
{
    fridge_audio_status_t audio = {0};
    if (fridge_audio_get_status(&audio) == ESP_OK && audio.state == FRIDGE_AUDIO_STATE_RECORDING) {
        (void)fridge_audio_stop_recording();
    }
}

// 结束时先让 Agent/TTS 停止，再关闭录音；WakeNet 的恢复始终由既有策略任务决定。
static void dialog_finish(bool cancel_agent, const char *detail)
{
    if (cancel_agent) {
        esp_err_t cancel = fridge_agent_runtime_cancel_active();
        if (cancel != ESP_OK && cancel != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "cancel active Agent on dialog finish: %s", esp_err_to_name(cancel));
        }
    }
    dialog_stop_recording_if_needed();
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_dialog_pending_confirmation = false;
    s_dialog_auto_complete = false;
    s_dialog_pending_run_id[0] = '\0';
    s_dialog_pending_call_id[0] = '\0';
    xSemaphoreGive(s_state_lock);
    fridge_wake_word_notify_dialog_finished();
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    fridge_voice_dialog_state_t terminal = s_dialog_status.state == FRIDGE_VOICE_DIALOG_STATE_ERROR ?
                                                FRIDGE_VOICE_DIALOG_STATE_ERROR :
                                                FRIDGE_VOICE_DIALOG_STATE_IDLE;
    xSemaphoreGive(s_state_lock);
    dialog_set_status(terminal, false, detail);
}

static bool dialog_drain_stop_command(void)
{
    voice_dialog_command_t command = {0};
    bool stopped = false;
    while (xQueueReceive(s_dialog_queue, &command, 0) == pdTRUE) {
        if (command.type == VOICE_DIALOG_CMD_STOP) {
            stopped = true;
        } else if (command.type == VOICE_DIALOG_CMD_AGENT_EVENT) {
            // 录音期间理论上没有活跃 Agent；保留事件到状态机只会造成队列重排，直接忽略旧事件。
            ESP_LOGD(TAG, "drop stale Agent event while listening: %d", (int)command.agent_kind);
        }
    }
    return stopped;
}

// 在不保存十秒完整 PCM 的前提下候听：PCM 缓冲仍最多六秒；前六秒无声时重开一次录音，
// 已检测到语音则优先保留当前缓冲并在 1.5 秒静音后提交 ASR。
static voice_dialog_capture_result_t dialog_capture_utterance(void)
{
    const int64_t wait_started_us = esp_timer_get_time();
    int64_t speech_started_us = 0;
    int64_t last_voice_us = 0;
    bool heard_speech = false;

    dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_LISTENING, true, "10秒内请说话");
    while (dialog_is_active()) {
        esp_err_t err = fridge_audio_start_recording();
        if (err != ESP_OK) {
            dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false, "无法启动麦克风");
            return VOICE_DIALOG_CAPTURE_ERROR;
        }
        while (dialog_is_active()) {
            if (dialog_drain_stop_command()) return VOICE_DIALOG_CAPTURE_STOPPED;
            fridge_audio_status_t audio = {0};
            if (fridge_audio_get_status(&audio) != ESP_OK || audio.state == FRIDGE_AUDIO_STATE_ERROR) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false,
                                  audio.error[0] ? audio.error : "麦克风录音失败");
                return VOICE_DIALOG_CAPTURE_ERROR;
            }
            const int64_t now_us = esp_timer_get_time();
            if (audio.state == FRIDGE_AUDIO_STATE_RECORDING && dialog_voice_detected(&audio)) {
                if (!heard_speech) {
                    heard_speech = true;
                    speech_started_us = now_us;
                }
                last_voice_us = now_us;
            }
            if (heard_speech && now_us - speech_started_us >= VOICE_DIALOG_MIN_SPEECH_MS * 1000LL &&
                now_us - last_voice_us >= VOICE_DIALOG_SILENCE_END_MS * 1000LL) {
                dialog_stop_recording_if_needed();
                return VOICE_DIALOG_CAPTURE_UTTERANCE;
            }
            if (!heard_speech && now_us - wait_started_us >= VOICE_DIALOG_IDLE_WAIT_MS * 1000LL) {
                dialog_stop_recording_if_needed();
                return VOICE_DIALOG_CAPTURE_TIMEOUT;
            }
            // 缓冲已到六秒：无声时继续剩余候听时间；有声时不丢本轮，立即交给 ASR。
            if (audio.state == FRIDGE_AUDIO_STATE_READY) {
                if (heard_speech) return VOICE_DIALOG_CAPTURE_UTTERANCE;
                if (now_us - wait_started_us >= VOICE_DIALOG_IDLE_WAIT_MS * 1000LL) {
                    return VOICE_DIALOG_CAPTURE_TIMEOUT;
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(VOICE_DIALOG_POLL_MS));
        }
    }
    return VOICE_DIALOG_CAPTURE_STOPPED;
}

static bool dialog_text_is_confirm(const char *text, bool *confirmed)
{
    if (!text || !confirmed) return false;
    if (strstr(text, "取消") || strstr(text, "不要") || strstr(text, "不执行") ||
        strcmp(text, "否") == 0 || strcmp(text, "不") == 0) {
        *confirmed = false;
        return true;
    }
    if (strstr(text, "确认") || strstr(text, "执行") || strstr(text, "同意") ||
        strstr(text, "可以") || strstr(text, "好的") || strcmp(text, "是") == 0) {
        *confirmed = true;
        return true;
    }
    return false;
}

static voice_dialog_agent_result_t dialog_wait_for_agent(void)
{
    for (;;) {
        voice_dialog_command_t command = {0};
        if (xQueueReceive(s_dialog_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command.type == VOICE_DIALOG_CMD_STOP) {
            dialog_finish(true, "已结束连续对话");
            return VOICE_DIALOG_AGENT_FINISH;
        }
        if (command.type != VOICE_DIALOG_CMD_AGENT_EVENT) continue;

        if (command.agent_kind == FRIDGE_AGENT_RUNTIME_EVENT_TOOL &&
            command.requires_confirmation && command.tool_status == 0U) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_dialog_pending_confirmation = true;
            s_dialog_auto_complete = false;
            strlcpy(s_dialog_pending_run_id, command.run_id, sizeof(s_dialog_pending_run_id));
            strlcpy(s_dialog_pending_call_id, command.call_id, sizeof(s_dialog_pending_call_id));
            xSemaphoreGive(s_state_lock);
            dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_WAITING_CONFIRMATION, true,
                              "请说确认或取消");
            continue;
        }
        if (command.agent_kind == FRIDGE_AGENT_RUNTIME_EVENT_STATUS) {
            if (strcmp(command.state, "FAILED") == 0 || strcmp(command.state, "CANCELLED") == 0 ||
                strcmp(command.state, "failed") == 0 || strcmp(command.state, "cancelled") == 0) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false,
                                  command.detail[0] ? command.detail : "Agent 对话失败");
                dialog_finish(false, command.detail[0] ? command.detail : "Agent 对话失败");
                return VOICE_DIALOG_AGENT_FINISH;
            }
            if (strcmp(command.state, "COMPLETED") == 0 || strcmp(command.state, "completed") == 0) {
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                s_dialog_auto_complete = command.dialog_complete && !s_dialog_pending_confirmation;
                xSemaphoreGive(s_state_lock);
            } else if (command.working) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_THINKING, true,
                                  command.detail[0] ? command.detail : "正在处理请求");
            }
            continue;
        }
        if (command.agent_kind == FRIDGE_AGENT_RUNTIME_EVENT_TTS) {
            if (strcmp(command.state, "synthesizing") == 0 || strcmp(command.state, "playing") == 0) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_SPEAKING, true,
                                  command.detail[0] ? command.detail : "正在播报");
                continue;
            }
            if (strcmp(command.state, "done") == 0) {
                xSemaphoreTake(s_state_lock, portMAX_DELAY);
                bool waiting_confirmation = s_dialog_pending_confirmation;
                bool auto_complete = s_dialog_auto_complete;
                xSemaphoreGive(s_state_lock);
                if (waiting_confirmation || !auto_complete) return VOICE_DIALOG_AGENT_NEXT_TURN;
                dialog_finish(false, "对话已完成");
                return VOICE_DIALOG_AGENT_FINISH;
            }
            if (strcmp(command.state, "error") == 0 || strcmp(command.state, "stopped") == 0) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false,
                                  command.detail[0] ? command.detail : "语音播报失败");
                dialog_finish(false, command.detail[0] ? command.detail : "语音播报失败");
                return VOICE_DIALOG_AGENT_FINISH;
            }
        }
    }
}

static void dialog_run_session(void)
{
    fridge_wake_word_notify_dialog_started();
    dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_PROMPTING, true, "正在准备语音");
    (void)fridge_wake_word_stop();
    int64_t deadline_us = esp_timer_get_time() + VOICE_DIALOG_WAKE_STOP_WAIT_MS * 1000LL;
    while (esp_timer_get_time() < deadline_us) {
        fridge_wake_word_status_t wake = {0};
        if (fridge_wake_word_get_status(&wake) != ESP_OK ||
            wake.state != FRIDGE_WAKE_WORD_STATE_LISTENING) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 本地提示音不依赖云端 TTS；失败不阻断后续对话，详细原因留在日志中。
    esp_err_t beep = fridge_audio_speaker_beep_test();
    if (beep != ESP_OK) ESP_LOGW(TAG, "dialog wake beep failed: %s", esp_err_to_name(beep));

    // 连续会话会在同一任务中进入 NVS 配置读取和 ASR 同步等待；结果对象约 1KB，
    // 放到 PSRAM 可避免它长期占用内部 SRAM 任务栈，并为 HTTPS 前后的深调用链留出余量。
    fridge_asr_result_t *result = heap_caps_calloc(1,
                                                   sizeof(*result),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) {
        result = calloc(1, sizeof(*result));
    }
    if (!result) {
        dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false, "语音识别内存不足");
        dialog_finish(false, "语音识别内存不足");
        return;
    }

    while (dialog_is_active()) {
        voice_dialog_capture_result_t capture = dialog_capture_utterance();
        if (capture == VOICE_DIALOG_CAPTURE_TIMEOUT) {
            dialog_finish(false, "10秒未检测到语音");
            break;
        }
        if (capture == VOICE_DIALOG_CAPTURE_STOPPED) {
            dialog_finish(true, "已结束连续对话");
            break;
        }
        if (capture == VOICE_DIALOG_CAPTURE_ERROR) {
            dialog_finish(false, "语音录音失败");
            break;
        }

        memset(result, 0, sizeof(*result));
        dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_TRANSCRIBING, true, "正在识别语音");
        ESP_LOGI(TAG, "continuous ASR begin: stack_hwm=%u free_internal=%u free_psram=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        esp_err_t asr = fridge_voice_asr_transcribe_latest(result);
        ESP_LOGI(TAG, "continuous ASR returned: err=%s stack_hwm=%u",
                 esp_err_to_name(asr),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (asr != ESP_OK || result->text[0] == '\0') {
            dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false,
                              result->error[0] ? result->error : "语音识别失败");
            dialog_finish(false, result->error[0] ? result->error : "语音识别失败");
            break;
        }
        emit_event(FRIDGE_VOICE_ASR_STATE_DONE, ++s_trigger_id, result, NULL);

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool pending_confirmation = s_dialog_pending_confirmation;
        char pending_run_id[FRIDGE_AGENT_RUNTIME_MAX_RUN_ID_LEN] = {0};
        char pending_call_id[FRIDGE_AGENT_RUNTIME_MAX_CALL_ID_LEN] = {0};
        strlcpy(pending_run_id, s_dialog_pending_run_id, sizeof(pending_run_id));
        strlcpy(pending_call_id, s_dialog_pending_call_id, sizeof(pending_call_id));
        xSemaphoreGive(s_state_lock);
        if (pending_confirmation) {
            bool confirmed = false;
            if (!dialog_text_is_confirm(result->text, &confirmed)) {
                // 不把非确认句子误解释为批准；继续候听直到静默退出或明确确认。
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_WAITING_CONFIRMATION, true,
                                  "请说确认或取消");
                continue;
            }
            esp_err_t resolve = fridge_agent_runtime_resolve_confirmation(pending_run_id,
                                                                            pending_call_id, confirmed);
            if (resolve != ESP_OK) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false, "确认请求已失效");
                dialog_finish(false, "确认请求已失效");
                break;
            }
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_dialog_pending_confirmation = false;
            xSemaphoreGive(s_state_lock);
        } else {
            esp_err_t submit = fridge_agent_runtime_submit_text(result->text);
            if (submit != ESP_OK) {
                dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_ERROR, false,
                                  "Agent 正在忙，请稍后再试");
                dialog_finish(false, "Agent 正在忙，请稍后再试");
                break;
            }
        }
        dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_THINKING, true, "正在处理请求");
        if (dialog_wait_for_agent() == VOICE_DIALOG_AGENT_FINISH) break;
    }
    free(result);
}

static void on_wake_dialog(void *user_ctx)
{
    (void)user_ctx;
    // WakeNet 已先置位“对话进行中”；投递失败必须回滚该标记，否则监听策略会永久暂停。
    if (fridge_voice_asr_dialog_start() != ESP_OK) {
        fridge_wake_word_notify_dialog_finished();
    }
}

// 唯一连续会话任务复用旧 wake→ASR 协调任务的内部 SRAM 栈；所有 TLS 深栈仍由短生命周期
// PSRAM ASR/Agent/TTS worker 负责，避免 NVS/Flash 操作期间在 PSRAM 栈运行。
static void voice_asr_auto_task(void *arg)
{
    (void)arg;
    for (;;) {
        voice_dialog_command_t command = {0};
        if (xQueueReceive(s_dialog_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command.type != VOICE_DIALOG_CMD_START) continue;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool busy = s_dialog_status.active || s_manual_session;
        s_dialog_start_pending = false;
        if (!busy) {
            s_dialog_status.active = true;
            s_dialog_status.state = FRIDGE_VOICE_DIALOG_STATE_PROMPTING;
            strlcpy(s_dialog_status.state_text, "提示中", sizeof(s_dialog_status.state_text));
            strlcpy(s_dialog_status.detail, "正在准备语音", sizeof(s_dialog_status.detail));
        }
        xSemaphoreGive(s_state_lock);
        if (busy) {
            ESP_LOGW(TAG, "ignore continuous dialog start while audio session is busy");
            continue;
        }
        dialog_run_session();
        ESP_LOGI(TAG, "continuous dialog finished: free_internal=%u largest_dma=%u free_psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
}

#if !CONFIG_FRIDGE_P4_UI_ENABLE
static void voice_asr_policy_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        bool ready = s_hardware_ready;
        xSemaphoreGive(s_state_lock);
        if (ready) {
            // P4 最小模式使用静态常亮屏；用户持久开关和音频忙闲由 wake_word 内部综合判断。
            fridge_wake_word_policy_update(true, true);
        }
        vTaskDelay(pdMS_TO_TICKS(VOICE_ASR_POLICY_TICK_MS));
    }
}
#endif

esp_err_t fridge_voice_asr_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_run_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    s_dialog_queue = xQueueCreate(VOICE_DIALOG_QUEUE_LEN, sizeof(voice_dialog_command_t));
    if (!s_run_lock || !s_state_lock || !s_dialog_queue) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(fridge_asr_init(), TAG, "ASR config init failed");
    ESP_RETURN_ON_ERROR(fridge_wake_word_set_dialog_callback(on_wake_dialog, NULL),
                        TAG,
                        "wake callback registration failed");
    if (xTaskCreate(voice_asr_auto_task,
                    "voice_asr_auto",
                    VOICE_ASR_AUTO_TASK_STACK,
                    NULL,
                    4,
                    &s_auto_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if !CONFIG_FRIDGE_P4_UI_ENABLE
    // 无 P4 LVGL 时由语音组件维持常亮策略；启用 ui_p4 后策略归 UI 单一所有，
    // 避免两个周期任务覆盖“字体资源加载期间暂缓 WakeNet”的内存门禁。
    if (xTaskCreate(voice_asr_policy_task,
                    "voice_asr_policy",
                    VOICE_ASR_POLICY_TASK_STACK,
                    NULL,
                    4,
                    &s_policy_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif

    s_initialized = true;
    dialog_set_status(FRIDGE_VOICE_DIALOG_STATE_IDLE, false, "");
    ESP_LOGI(TAG, "P4 continuous ASR→Agent→TTS coordinator ready");
    return ESP_OK;
}

void fridge_voice_asr_set_hardware_ready(void)
{
    if (!s_state_lock) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_hardware_ready = true;
    xSemaphoreGive(s_state_lock);
}

esp_err_t fridge_voice_asr_set_event_callback(fridge_voice_asr_event_cb_t cb, void *user_ctx)
{
    if (!s_state_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_event_cb = cb;
    s_event_ctx = user_ctx;
    xSemaphoreGive(s_state_lock);
    return ESP_OK;
}

esp_err_t fridge_voice_asr_manual_record_begin(void)
{
    if (!s_state_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_manual_session || s_dialog_status.active || s_dialog_start_pending) {
        xSemaphoreGive(s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_manual_session = true;
    xSemaphoreGive(s_state_lock);

    fridge_wake_word_notify_dialog_started();
    esp_err_t err = fridge_wake_word_stop();
    if (err != ESP_OK) {
        fridge_voice_asr_manual_session_finish();
    }
    return err;
}

void fridge_voice_asr_manual_session_finish(void)
{
    if (!s_state_lock) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool was_active = s_manual_session;
    s_manual_session = false;
    xSemaphoreGive(s_state_lock);
    if (was_active) {
        fridge_wake_word_notify_dialog_finished();
    }
}

esp_err_t fridge_voice_asr_dialog_start(void)
{
    if (!s_dialog_queue || !s_state_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool busy = s_dialog_status.active || s_manual_session || s_dialog_start_pending;
    if (!busy) s_dialog_start_pending = true;
    xSemaphoreGive(s_state_lock);
    if (busy) return ESP_ERR_INVALID_STATE;
    const voice_dialog_command_t command = {.type = VOICE_DIALOG_CMD_START};
    if (xQueueSend(s_dialog_queue, &command, 0) == pdTRUE) return ESP_OK;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_dialog_start_pending = false;
    xSemaphoreGive(s_state_lock);
    return ESP_ERR_TIMEOUT;
}

esp_err_t fridge_voice_asr_dialog_stop(void)
{
    if (!s_dialog_queue) return ESP_ERR_INVALID_STATE;
    if (!dialog_is_active()) return ESP_ERR_INVALID_STATE;
    const voice_dialog_command_t command = {.type = VOICE_DIALOG_CMD_STOP};
    return xQueueSend(s_dialog_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t fridge_voice_asr_dialog_get_status(fridge_voice_dialog_status_t *out)
{
    if (!out || !s_state_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *out = s_dialog_status;
    xSemaphoreGive(s_state_lock);
    return ESP_OK;
}

esp_err_t fridge_voice_asr_dialog_notify_agent_event(const fridge_agent_runtime_ui_event_t *event)
{
    if (!event || !s_dialog_queue || !dialog_is_active()) return ESP_ERR_INVALID_STATE;
    voice_dialog_command_t command = {
        .type = VOICE_DIALOG_CMD_AGENT_EVENT,
        .agent_kind = event->kind,
        .working = event->working,
        .requires_confirmation = event->requires_confirmation,
        .dialog_complete = event->dialog_complete,
        .tool_status = event->tool_status,
    };
    strlcpy(command.run_id, event->run_id, sizeof(command.run_id));
    strlcpy(command.call_id, event->call_id, sizeof(command.call_id));
    strlcpy(command.state, event->state, sizeof(command.state));
    strlcpy(command.detail, event->detail, sizeof(command.detail));
    return xQueueSend(s_dialog_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
