// 冰箱小精灵 AI 语音对话页。
// 屏幕端只做轻量语音入口：录音和 HTTPS/AI/TTS 都放到后台任务，避免阻塞 LVGL 刷新和触摸响应。

#include "fridge_ui_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "fridge_asr.h"
#include "fridge_audio.h"
#include "fridge_speaker.h"
#include "fridge_storage.h"
#include "fridge_voice_session.h"
#include "fridge_wake_word.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AI_VOICE_TASK_STACK 8192
#define AI_VOICE_TASK_STACK_WORDS ((AI_VOICE_TASK_STACK + sizeof(StackType_t) - 1) / sizeof(StackType_t))
#define AI_CHAT_SAFE_BOTTOM 132

static const char *TAG = "fridge_ui_ai";

static lv_obj_t *s_status;
static lv_obj_t *s_chat_list;
static lv_obj_t *s_voice_btn;
static lv_obj_t *s_voice_label;
static lv_obj_t *s_voice_hint;
static bool s_recording;
static bool s_busy;
static bool s_wake_was_listening;
static bool s_history_loaded;
static uint32_t s_generation;
static char *s_latest_transcript;
static char *s_latest_reply;
static StaticTask_t s_voice_task_tcb;
static StackType_t s_voice_task_stack[AI_VOICE_TASK_STACK_WORDS];
static TaskHandle_t s_voice_task_handle;

typedef struct {
    esp_err_t err;
    uint32_t generation;
    bool heap_allocated;
    bool tts_played;
    char *transcript;
    char *reply;
    char error[FRIDGE_VOICE_SESSION_MAX_ERROR_LEN + 1];
} ai_voice_done_t;

static ai_voice_done_t s_emergency_done;

static void free_ai_voice_done(ai_voice_done_t *done)
{
    if (!done || !done->heap_allocated) {
        return;
    }
    free(done->transcript);
    free(done->reply);
    free(done);
}

static char *dup_text_for_ui(const char *text, size_t max_len)
{
    if (!text || text[0] == '\0') {
        return NULL;
    }
    size_t len = strlen(text);
    if (len > max_len) {
        len = max_len;
    }
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

static void set_status(const char *text)
{
    fridge_ui_label_set_text_if_changed(s_status, text ? text : "");
}

static void set_button_state(const char *label, const char *hint)
{
    fridge_ui_label_set_text_if_changed(s_voice_label, label ? label : "开始说话");
    fridge_ui_label_set_text_if_changed(s_voice_hint, hint ? hint : "");
}

static bool text_prefix_matches(const char *stored, const char *full)
{
    if (!stored || !full || stored[0] == '\0' || full[0] == '\0') {
        return false;
    }
    size_t stored_len = strlen(stored);
    size_t full_len = strlen(full);
    size_t cmp_len = stored_len < full_len ? stored_len : full_len;
    return strncmp(stored, full, cmp_len) == 0;
}

static void scroll_chat_to_bottom(void)
{
    if (!s_chat_list) {
        return;
    }
    uint32_t child_count = lv_obj_get_child_count(s_chat_list);
    if (child_count > 0) {
        lv_obj_scroll_to_view(lv_obj_get_child(s_chat_list, child_count - 1), LV_ANIM_OFF);
    }
}

static void cache_latest_texts(const char *transcript, const char *reply)
{
    if (transcript && transcript[0] != '\0') {
        size_t len = strlen(transcript);
        if (len > FRIDGE_VOICE_SESSION_TRANSCRIPT_LEN) {
            len = FRIDGE_VOICE_SESSION_TRANSCRIPT_LEN;
        }
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, transcript, len);
            copy[len] = '\0';
            free(s_latest_transcript);
            s_latest_transcript = copy;
        }
    }
    if (reply && reply[0] != '\0') {
        size_t len = strlen(reply);
        if (len > FRIDGE_VOICE_SESSION_REPLY_LEN) {
            len = FRIDGE_VOICE_SESSION_REPLY_LEN;
        }
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, reply, len);
            copy[len] = '\0';
            free(s_latest_reply);
            s_latest_reply = copy;
        }
    }
}

static lv_obj_t *make_bubble(lv_obj_t *parent, const char *title, const char *body, bool me)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    const int32_t bubble_width = 560;
    const int32_t content_width = bubble_width - 28;
    lv_obj_t *bubble = lv_obj_create(parent);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bubble, bubble_width, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(bubble, 0, 0);
    lv_obj_set_style_margin_left(bubble, me ? 64 : 0, 0);
    lv_obj_set_style_margin_right(bubble, me ? 0 : 64, 0);
    lv_obj_set_style_bg_color(bubble, me ? theme->accent : lv_color_hex(0xFFF6E5), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bubble, me ? theme->accent : lv_color_hex(0xF4C987), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_style_pad_left(bubble, 14, 0);
    lv_obj_set_style_pad_right(bubble, 14, 0);
    lv_obj_set_style_pad_top(bubble, 12, 0);
    lv_obj_set_style_pad_bottom(bubble, 12, 0);
    lv_obj_set_style_pad_row(bubble, 7, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title_label = lv_label_create(bubble);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, content_width);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title_label, me ? lv_color_white() : theme->accent, 0);
    lv_obj_set_style_text_font(title_label, fridge_ui_font_small(), 0);

    lv_obj_t *body_label = lv_label_create(bubble);
    lv_label_set_text(body_label, body && body[0] ? body : "（空）");
    lv_obj_set_width(body_label, content_width);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body_label, me ? lv_color_white() : theme->text, 0);
    lv_obj_set_style_text_font(body_label, fridge_ui_font_for_text(fridge_ui_font_ai_body(), body), 0);
    return bubble;
}

static void add_chat_message(const char *role, const char *content)
{
    if (!s_chat_list || !content || content[0] == '\0') {
        return;
    }
    bool me = role && strcmp(role, "user") == 0;
    make_bubble(s_chat_list, me ? "我" : "小精灵", content, me);
}

static bool should_replace_tail_with_latest(const fridge_storage_chat_history_t *history,
                                            const char *transcript,
                                            const char *reply,
                                            size_t *history_end)
{
    if (!history || !history_end || !transcript || transcript[0] == '\0') {
        return false;
    }
    if (reply && reply[0] != '\0' && history->count >= 2) {
        const fridge_storage_chat_message_t *user = &history->messages[history->count - 2];
        const fridge_storage_chat_message_t *assistant = &history->messages[history->count - 1];
        if (strcmp(user->role, "user") == 0 &&
            strcmp(assistant->role, "assistant") == 0 &&
            text_prefix_matches(user->content, transcript) &&
            text_prefix_matches(assistant->content, reply)) {
            *history_end = history->count - 2;
            return true;
        }
    }
    if ((!reply || reply[0] == '\0') && history->count >= 1) {
        const fridge_storage_chat_message_t *last = &history->messages[history->count - 1];
        if (strcmp(last->role, "user") == 0 && text_prefix_matches(last->content, transcript)) {
            *history_end = history->count - 1;
            return true;
        }
    }
    return false;
}

static void refresh_chat_history_ui(const char *latest_transcript, const char *latest_reply)
{
    if (!s_chat_list) {
        return;
    }

    const char *display_transcript = latest_transcript && latest_transcript[0] ? latest_transcript : s_latest_transcript;
    const char *display_reply = latest_reply && latest_reply[0] ? latest_reply : s_latest_reply;

    // 聊天历史来自 LittleFS，刷新只放在页面创建、清空和单轮对话完成后，避免在 UI update 中高频读写 Flash。
    lv_obj_clean(s_chat_list);
    fridge_storage_chat_history_t *history = calloc(1, sizeof(*history));
    size_t history_end = 0;
    if (history && fridge_storage_get_chat_history(history, NULL) == ESP_OK) {
        history_end = history->count;
        (void)should_replace_tail_with_latest(history, display_transcript, display_reply, &history_end);
        size_t start = history_end > 10 ? history_end - 10 : 0;
        for (size_t i = start; i < history_end && i < FRIDGE_STORAGE_MAX_CHAT_MESSAGES; i++) {
            add_chat_message(history->messages[i].role, history->messages[i].content);
        }
    } else {
        ESP_LOGW(TAG, "load chat history for UI failed");
    }
    free(history);

    if (display_transcript && display_transcript[0] != '\0') {
        add_chat_message("user", display_transcript);
    }
    if (display_reply && display_reply[0] != '\0') {
        add_chat_message("assistant", display_reply);
    }
    if (lv_obj_get_child_count(s_chat_list) == 0) {
        add_chat_message("assistant", "你好，我可以帮你查临期、想晚餐、解释库存。");
    }
    // 底部安全区给固定的语音按钮和提示文字让位，避免最后一条气泡滚到底后被遮住。
    lv_obj_t *safe_area = lv_obj_create(s_chat_list);
    lv_obj_remove_style_all(safe_area);
    lv_obj_set_size(safe_area, 1, AI_CHAT_SAFE_BOTTOM);
    lv_obj_remove_flag(safe_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    scroll_chat_to_bottom();
}

static void show_boot_placeholder(void)
{
    if (!s_chat_list) {
        return;
    }

    // UI 任务启动时会一次性创建多个页面；这里不要读取 LittleFS 历史，避免开机阶段 Flash/堆/LVGL 同时受压导致重启。
    lv_obj_clean(s_chat_list);
    add_chat_message("assistant", "你好，我可以帮你查临期、想晚餐、解释库存。");
    lv_obj_t *safe_area = lv_obj_create(s_chat_list);
    lv_obj_remove_style_all(safe_area);
    lv_obj_set_size(safe_area, 1, AI_CHAT_SAFE_BOTTOM);
    lv_obj_remove_flag(safe_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    scroll_chat_to_bottom();
}

static void show_latest_texts(const char *transcript, const char *reply)
{
    cache_latest_texts(transcript, reply);
    ESP_LOGI(TAG,
             "show latest AI texts: transcript=%u bytes, reply=%u bytes",
             (unsigned)(transcript ? strlen(transcript) : 0),
             (unsigned)(reply ? strlen(reply) : 0));
    refresh_chat_history_ui(transcript, reply);
}

static void clear_chat_cb(lv_event_t *event)
{
    (void)event;
    if (s_recording || s_busy) {
        fridge_ui_toast("请先完成当前语音");
        return;
    }

    // 清空设备侧会话历史，同时避免后台任务改写当前页面。
    esp_err_t err = fridge_storage_clear_chat_history();
    if (err != ESP_OK) {
        set_status("清空对话失败");
        ESP_LOGW(TAG, "clear chat history failed: %s", esp_err_to_name(err));
        return;
    }
    free(s_latest_transcript);
    free(s_latest_reply);
    s_latest_transcript = NULL;
    s_latest_reply = NULL;
    s_history_loaded = true;
    s_generation++;
    refresh_chat_history_ui(NULL, NULL);
    set_status("对话已清空");
    set_button_state("开始说话", "点一下录音，再点一下发送");
    fridge_ui_toast("对话已清空");
}

static void recipe_jump_cb(lv_event_t *event)
{
    (void)event;
    fridge_ui_show_page(FRIDGE_UI_PAGE_RECIPE);
}

static void finish_recording_visual(void)
{
    s_recording = false;
    s_busy = true;
    s_generation++;
    set_status("正在识别并思考...");
    set_button_state("处理中", "语音转文字、AI 回复和播报会自动完成");
}

static void restore_wake_if_needed(void)
{
    if (s_wake_was_listening) {
        esp_err_t err = fridge_wake_word_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restart wake word failed: %s", esp_err_to_name(err));
        }
    }
    s_wake_was_listening = false;
}

static void ai_voice_done_async(void *user_data)
{
    ai_voice_done_t *done = (ai_voice_done_t *)user_data;
    if (!done) {
        return;
    }

    s_busy = false;
    if (done->generation != s_generation) {
        // 期间用户已开始新一轮录音，丢弃这条过期结果；唤醒词恢复交给当前活跃轮次处理。
        free_ai_voice_done(done);
        return;
    }

    if (done->err == ESP_OK) {
        show_latest_texts(done->transcript, done->reply);
        set_status(done->tts_played ? "回复已播报" : "回复已生成");
        set_button_state("继续说话", "再点一次开始新的语音对话");
    } else {
        set_status(done->error[0] ? done->error : esp_err_to_name(done->err));
        if (done->transcript && done->transcript[0] != '\0') {
            show_latest_texts(done->transcript, NULL);
        }
        set_button_state("重新说话", "请靠近麦克风，录 1 到 6 秒");
    }
    restore_wake_if_needed();
    free_ai_voice_done(done);
}

static void ai_voice_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    ai_voice_done_t *done = calloc(1, sizeof(*done));
    fridge_voice_session_result_t *session = calloc(1, sizeof(*session));

    if (!done) {
        memset(&s_emergency_done, 0, sizeof(s_emergency_done));
        done = &s_emergency_done;
    } else {
        done->heap_allocated = true;
    }
    if (!session) {
        done->err = ESP_ERR_NO_MEM;
        strlcpy(done->error, "语音对话内存不足", sizeof(done->error));
        goto finish;
    }

    // 屏幕 AI 页复用公共语音会话：ASR、上下文注入、历史裁剪、保存历史、上行同步和 TTS 播报都在组件内完成。
    // 唤醒词暂停/恢复由本页在录音前后自行管理（覆盖录音窗口），因此这里 manage_wake_word=false。
    fridge_voice_session_options_t options = {
        .play_tts = true,
        .manage_wake_word = false,
    };
    done->err = fridge_voice_session_run_latest_recording_ex(&options, session);
    if (done->heap_allocated && session->transcript[0] != '\0') {
        done->transcript = dup_text_for_ui(session->transcript, FRIDGE_VOICE_SESSION_TRANSCRIPT_LEN);
    }
    if (done->err == ESP_OK) {
        if (done->heap_allocated && session->reply[0] != '\0') {
            done->reply = dup_text_for_ui(session->reply, FRIDGE_VOICE_SESSION_REPLY_LEN);
        }
        done->tts_played = session->tts_played;
    } else {
        strlcpy(done->error, session->error[0] ? session->error : esp_err_to_name(done->err), sizeof(done->error));
    }

finish:
    free(session);
    if (done) {
        done->generation = generation;
        if (lv_async_call(ai_voice_done_async, done) != LV_RESULT_OK) {
            free_ai_voice_done(done);
        }
    }
    ESP_LOGI(TAG, "UI voice task done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_voice_task_handle = NULL;
    vTaskDelete(NULL);
}

static void voice_cb(lv_event_t *event)
{
    (void)event;
    if (s_busy) {
        fridge_ui_toast("正在处理上一句");
        return;
    }

    if (!s_recording) {
        fridge_speaker_status_t speaker = {0};
        if (fridge_speaker_get_status(&speaker) == ESP_OK &&
            (speaker.state == FRIDGE_SPEAKER_STATE_SYNTHESIZING || speaker.state == FRIDGE_SPEAKER_STATE_PLAYING)) {
            (void)fridge_speaker_stop();
        }

        fridge_wake_word_status_t wake = {0};
        if (fridge_wake_word_get_status(&wake) == ESP_OK && wake.state == FRIDGE_WAKE_WORD_STATE_LISTENING) {
            s_wake_was_listening = true;
            esp_err_t wake_err = fridge_wake_word_stop();
            if (wake_err != ESP_OK) {
                set_status("唤醒监听暂停失败");
                return;
            }
        }

        esp_err_t err = fridge_audio_start_recording();
        if (err != ESP_OK) {
            restore_wake_if_needed();
            set_status(err == ESP_ERR_INVALID_STATE ? "麦克风正被占用" : esp_err_to_name(err));
            return;
        }
        s_recording = true;
        set_status("正在听你说话");
        set_button_state("停止并发送", "最多录 6 秒，结束后自动回复并播报");
        return;
    }

    esp_err_t err = fridge_audio_stop_recording();
    if (err != ESP_OK) {
        s_recording = false;
        restore_wake_if_needed();
        set_status(esp_err_to_name(err));
        set_button_state("重新说话", "录音停止失败，请再试一次");
        return;
    }

    finish_recording_visual();
    if (s_voice_task_handle) {
        s_busy = false;
        restore_wake_if_needed();
        set_status("语音任务仍在结束中");
        set_button_state("重新说话", "请稍后再试");
        return;
    }
    s_voice_task_handle = xTaskCreateStatic(ai_voice_task,
                                            "ui_voice_ai",
                                            AI_VOICE_TASK_STACK,
                                            (void *)(uintptr_t)s_generation,
                                            4,
                                            s_voice_task_stack,
                                            &s_voice_task_tcb);
    if (!s_voice_task_handle) {
        s_busy = false;
        restore_wake_if_needed();
        set_status("语音任务创建失败");
        set_button_state("重新说话", "系统繁忙，请稍后再试");
    }
}

void fridge_ui_page_ai_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *kicker = lv_label_create(parent);
    lv_label_set_text(kicker, "设备语音优先");
    lv_obj_set_style_text_color(kicker, theme->muted, 0);
    lv_obj_set_style_text_font(kicker, fridge_ui_font_small(), 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 36, 8);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "AI 助手");
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 36, 36);

    lv_obj_t *clear = lv_button_create(parent);
    lv_obj_set_size(clear, 154, 48);
    lv_obj_align(clear, LV_ALIGN_TOP_RIGHT, -224, 28);
    lv_obj_set_style_bg_color(clear, lv_color_hex(0xFFF8EA), 0);
    lv_obj_set_style_border_color(clear, lv_color_hex(0xE8CBA2), 0);
    lv_obj_set_style_border_width(clear, 1, 0);
    lv_obj_set_style_shadow_width(clear, 0, 0);
    lv_obj_set_style_radius(clear, 24, 0);
    lv_obj_add_event_cb(clear, clear_chat_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_label = lv_label_create(clear);
    lv_label_set_text(clear_label, "清空对话");
    lv_obj_set_style_text_color(clear_label, theme->accent, 0);
    lv_obj_set_style_text_font(clear_label, fridge_ui_font_small(), 0);
    lv_obj_center(clear_label);

    lv_obj_t *recipe = lv_button_create(parent);
    lv_obj_set_size(recipe, 166, 48);
    lv_obj_align(recipe, LV_ALIGN_TOP_RIGHT, -38, 28);
    lv_obj_set_style_bg_color(recipe, lv_color_hex(0xFFF0D0), 0);
    lv_obj_set_style_border_color(recipe, theme->accent_2, 0);
    lv_obj_set_style_border_width(recipe, 1, 0);
    lv_obj_set_style_shadow_width(recipe, 0, 0);
    lv_obj_set_style_radius(recipe, 24, 0);
    lv_obj_add_event_cb(recipe, recipe_jump_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *recipe_label = lv_label_create(recipe);
    lv_label_set_text(recipe_label, "推荐菜谱");
    lv_obj_set_style_text_color(recipe_label, theme->accent_2, 0);
    lv_obj_set_style_text_font(recipe_label, fridge_ui_font_small(), 0);
    lv_obj_center(recipe_label);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "点下方按钮，和小精灵说一句");
    lv_obj_set_width(s_status, 640);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_status, theme->muted, 0);
    lv_obj_set_style_text_font(s_status, fridge_ui_font_small(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 38, 82);

    s_chat_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_chat_list);
    lv_obj_set_pos(s_chat_list, 30, 116);
    lv_obj_set_size(s_chat_list, 660, 450);
    lv_obj_set_scroll_dir(s_chat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_chat_list, LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_left(s_chat_list, 16, 0);
    lv_obj_set_style_pad_right(s_chat_list, 16, 0);
    lv_obj_set_style_pad_top(s_chat_list, 8, 0);
    lv_obj_set_style_pad_bottom(s_chat_list, 10, 0);
    lv_obj_set_style_pad_row(s_chat_list, 12, 0);
    lv_obj_set_flex_flow(s_chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    s_history_loaded = false;
    show_boot_placeholder();

    s_voice_btn = lv_button_create(parent);
    lv_obj_set_size(s_voice_btn, 286, 76);
    lv_obj_align(s_voice_btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(s_voice_btn, theme->accent_2, 0);
    lv_obj_set_style_radius(s_voice_btn, 34, 0);
    lv_obj_set_style_shadow_width(s_voice_btn, 16, 0);
    lv_obj_set_style_shadow_color(s_voice_btn, lv_color_hex(0xD95745), 0);
    lv_obj_add_event_cb(s_voice_btn, voice_cb, LV_EVENT_CLICKED, NULL);
    s_voice_label = lv_label_create(s_voice_btn);
    lv_label_set_text(s_voice_label, "开始说话");
    lv_obj_set_style_text_color(s_voice_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_voice_label, fridge_ui_font_body(), 0);
    lv_obj_center(s_voice_label);

    s_voice_hint = lv_label_create(parent);
    lv_label_set_text(s_voice_hint, "点一下录音，再点一下发送");
    lv_obj_set_style_text_color(s_voice_hint, theme->muted, 0);
    lv_obj_set_style_text_font(s_voice_hint, fridge_ui_font_small(), 0);
    lv_obj_align_to(s_voice_hint, s_voice_btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
}

void fridge_ui_page_ai_update(void)
{
    if (g_ui_page == FRIDGE_UI_PAGE_AI && !s_history_loaded && !s_recording && !s_busy) {
        s_history_loaded = true;
        refresh_chat_history_ui(NULL, NULL);
    }
}
