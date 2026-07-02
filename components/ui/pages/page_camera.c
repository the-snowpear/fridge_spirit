// 冰箱小精灵拍照登记页。
// 进入页面后启动 OV3660 低分辨率 RGB565 取景，快门触发高清 JPEG 抓拍并提交 AI 识别。

#include "fridge_ui_internal.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fridge_ai_client.h"
#include "fridge_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define CAMERA_PREVIEW_TASK_STACK 6144
#define CAMERA_ANALYZE_TASK_STACK 20480
#define CAMERA_PREVIEW_INTERVAL_MS 650
#define CAMERA_PREVIEW_FINDER_W 656
#define CAMERA_PREVIEW_FINDER_H 392
#define CAMERA_AI_FALLBACK_NAME "水杯"

static const char *TAG = "fridge_ui_camera";

static lv_obj_t *s_finder;
static lv_obj_t *s_preview_img;
static lv_obj_t *s_hint;
static lv_obj_t *s_status;
static lv_obj_t *s_shutter;
static lv_obj_t *s_shutter_label;

static TaskHandle_t s_preview_task;
static bool s_preview_stop;
static bool s_preview_running;
static bool s_preview_attempted;
static bool s_preview_failed;
static bool s_analyzing;
static uint32_t s_generation;
static lv_image_dsc_t s_preview_dsc;
static uint8_t *s_preview_pixels;

typedef struct {
    esp_err_t err;
    uint32_t generation;
    fridge_camera_preview_frame_t frame;
    char error[128];
} camera_preview_done_t;

typedef struct {
    esp_err_t err;
    uint32_t generation;
    fridge_ui_camera_result_t result;
    bool heap_allocated;
} camera_analyze_done_t;

typedef struct {
    uint32_t generation;
    fridge_ai_config_update_t *config;
} camera_analyze_job_arg_t;

static camera_analyze_done_t s_emergency_done;

static BaseType_t create_camera_task(TaskFunction_t task,
                                     const char *name,
                                     uint32_t stack_bytes,
                                     void *arg,
                                     TaskHandle_t *handle,
                                     bool may_touch_flash)
{
    if (may_touch_flash) {
        // 只有确实需要在 worker 内访问 NVS/LittleFS 时才使用内部 SRAM 栈。
        // 图片 AI 路径会先在 UI 任务中预加载配置，因此可走 PSRAM 栈，避免完整 UI 场景下内部 SRAM 碎片导致任务创建失败。
        BaseType_t ok = xTaskCreate(task, name, stack_bytes, arg, 4, handle);
        if (ok != pdPASS) {
            ESP_LOGW(TAG,
                     "%s internal stack create failed, stack=%lu, largest_internal=%u bytes, free_psram=%u KB",
                     name,
                     (unsigned long)stack_bytes,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) / 1024));
        }
        return ok;
    }

    // 预览任务只做相机采集和 UI 通知，暂不访问 NVS/Flash；保留 PSRAM 栈以减轻取景页内部 SRAM 压力。
    // 注意：xTaskCreateWithCaps 创建的任务结束时必须使用 vTaskDeleteWithCaps。
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        4,
                                        handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) {
        ESP_LOGI(TAG,
                 "%s task created with PSRAM stack=%lu, free_heap=%u KB, free_psram=%u KB",
                 name,
                 (unsigned long)stack_bytes,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return ok;
    }

    ESP_LOGW(TAG,
             "%s PSRAM stack create failed, stack=%lu, free_heap=%u KB, largest_internal=%u bytes, free_psram=%u KB",
             name,
             (unsigned long)stack_bytes,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return pdFAIL;
}

static void set_status_text(const char *text)
{
    fridge_ui_label_set_text_if_changed(s_status, text ? text : "");
}

static void set_shutter_enabled(bool enabled, const char *label)
{
    fridge_ui_label_set_text_if_changed(s_shutter_label, label ? label : "拍照");
    if (enabled) {
        lv_obj_remove_state(s_shutter, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_shutter, LV_STATE_DISABLED);
    }
}

static void release_preview_pixels(void)
{
    if (s_preview_img) {
        lv_image_set_src(s_preview_img, NULL);
        lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    free(s_preview_pixels);
    s_preview_pixels = NULL;
    memset(&s_preview_dsc, 0, sizeof(s_preview_dsc));
}

static void update_preview_image(fridge_camera_preview_frame_t *frame)
{
    if (!frame || !frame->rgb565 || frame->len == 0 || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    release_preview_pixels();
    s_preview_pixels = frame->rgb565;
    frame->rgb565 = NULL;

    memset(&s_preview_dsc, 0, sizeof(s_preview_dsc));
    s_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_preview_dsc.header.w = (uint32_t)frame->width;
    s_preview_dsc.header.h = (uint32_t)frame->height;
    s_preview_dsc.header.stride = (uint32_t)(frame->width * 2);
    s_preview_dsc.data_size = (uint32_t)frame->len;
    s_preview_dsc.data = s_preview_pixels;

    lv_image_set_src(s_preview_img, &s_preview_dsc);
    lv_image_set_inner_align(s_preview_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_clear_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    fridge_ui_label_set_text_fmt_if_changed(s_status, "实时预览 %dx%d · %lu ms",
                                            frame->width,
                                            frame->height,
                                            (unsigned long)frame->capture_ms);
}

static void preview_done_async(void *user_data)
{
    camera_preview_done_t *done = (camera_preview_done_t *)user_data;
    if (!done) {
        return;
    }

    if (done->generation == s_generation && g_ui_page == FRIDGE_UI_PAGE_CAMERA && !s_analyzing) {
        if (done->err == ESP_OK) {
            update_preview_image(&done->frame);
            set_shutter_enabled(true, "拍照");
        } else {
            s_preview_failed = true;
            lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
            fridge_ui_label_set_text_if_changed(s_hint, done->error[0] ? done->error : "摄像头预览失败");
            set_status_text("预览失败，可点取景框重试或直接拍照");
            set_shutter_enabled(true, "拍照");
        }
    }

    fridge_camera_free_preview_frame(&done->frame);
    free(done);
}

static void preview_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    while (!s_preview_stop && generation == s_generation) {
        camera_preview_done_t *done = calloc(1, sizeof(*done));
        if (!done) {
            ESP_LOGW(TAG, "preview done allocation failed");
            vTaskDelay(pdMS_TO_TICKS(CAMERA_PREVIEW_INTERVAL_MS));
            continue;
        }
        done->generation = generation;
        done->err = fridge_camera_capture_preview_rgb565(&done->frame);
        if (done->err != ESP_OK) {
            fridge_camera_status_t status = {0};
            fridge_camera_get_status(&status);
            strlcpy(done->error,
                    status.last_error[0] ? status.last_error : esp_err_to_name(done->err),
                    sizeof(done->error));
        }
        bool preview_failed = done->err != ESP_OK;
        if (lv_async_call(preview_done_async, done) != LV_RESULT_OK) {
            fridge_camera_free_preview_frame(&done->frame);
            free(done);
        }
        if (preview_failed) {
            // 预览初始化失败时不要循环重试；否则会反复 reset/init esp-camera，导致页面切换明显卡顿。
            (void)fridge_camera_reset();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CAMERA_PREVIEW_INTERVAL_MS));
    }
    s_preview_running = false;
    s_preview_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void stop_preview(void)
{
    s_preview_stop = true;
}

static void start_preview(void)
{
    if (s_preview_running || s_preview_task || s_analyzing) {
        return;
    }
    s_preview_stop = false;
    s_preview_running = true;
    s_preview_attempted = true;
    s_preview_failed = false;
    set_status_text("正在启动摄像头预览...");
    set_shutter_enabled(true, "拍照");
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    fridge_ui_label_set_text_if_changed(s_hint, "正在打开摄像头...");

    BaseType_t ok = create_camera_task(preview_task,
                                       "ui_cam_preview",
                                       CAMERA_PREVIEW_TASK_STACK,
                                       (void *)(uintptr_t)s_generation,
                                       &s_preview_task,
                                       false);
    if (ok != pdPASS) {
        s_preview_running = false;
        s_preview_task = NULL;
        s_preview_failed = true;
        set_status_text("预览任务创建失败，可点取景框重试");
        fridge_ui_label_set_text_if_changed(s_hint, "预览任务创建失败\n请稍后点这里重试，或直接拍照");
        set_shutter_enabled(true, "拍照");
    }
}

static void retry_preview_cb(lv_event_t *event)
{
    (void)event;
    if (g_ui_page != FRIDGE_UI_PAGE_CAMERA || s_preview_running || s_analyzing) {
        return;
    }
    if (!s_preview_failed && s_preview_attempted) {
        return;
    }

    // 用户手动点取景框才重新尝试预览，避免硬件未接好时后台不停初始化摄像头。
    s_generation++;
    s_preview_attempted = false;
    start_preview();
}

static esp_err_t wait_preview_stopped_before_capture(void)
{
    // 快门会切换到高清抓拍模式；先等预览任务归还 esp-camera，避免并发反初始化/抓帧。
    for (uint8_t i = 0; i < 50 && s_preview_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_preview_task) {
        ESP_LOGW(TAG, "preview task still stopping before AI capture");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static char *extract_json_object_copy(const char *text)
{
    // 兼容 Gemini/OpenAI 常见的 ```json ... ``` 包裹；只抽取第一段完整 JSON 对象。
    // 这里不手写 JSON 解析，只做括号边界定位，真正字段校验仍交给 cJSON。
    if (!text) {
        return NULL;
    }
    const char *start = strchr(text, '{');
    if (!start) {
        return NULL;
    }

    bool in_string = false;
    bool escape = false;
    int depth = 0;
    for (const char *p = start; *p; p++) {
        char ch = *p;
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escape = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                char *json = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!json) {
                    json = malloc(len + 1);
                }
                if (!json) {
                    return NULL;
                }
                memcpy(json, start, len);
                json[len] = '\0';
                return json;
            }
        }
    }
    return NULL;
}

static void make_reply_preview(const char *reply, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!reply || reply[0] == '\0') {
        strlcpy(out, "空返回", out_size);
        return;
    }

    size_t written = 0;
    bool last_space = false;
    for (const unsigned char *p = (const unsigned char *)reply; *p && written + 1 < out_size; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t' || *p == ' ') {
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

static bool is_uncertain_object_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return true;
    }
    return strstr(name, "待确认") ||
           strstr(name, "未知") ||
           strstr(name, "无法识别") ||
           strstr(name, "不能识别") ||
           strstr(name, "不确定") ||
           strstr(name, "包装物") ||
           strstr(name, "瓶罐") ||
           strstr(name, "袋装物") ||
           strstr(name, "盒装物") ||
           strstr(name, "电子设备") ||
           strstr(name, "反光物体") ||
           strstr(name, "模糊物体");
}

static const char *best_guess_name_from_text(const char *reply, const char *doubt)
{
    const char *texts[] = {reply ? reply : "", doubt ? doubt : ""};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        const char *text = texts[i];
        if (strstr(text, "包装")) {
            return "零食袋";
        }
        if (strstr(text, "瓶") || strstr(text, "罐")) {
            return "饮料瓶";
        }
        if (strstr(text, "袋")) {
            return "零食袋";
        }
        if (strstr(text, "盒") || strstr(text, "箱")) {
            return "纸盒";
        }
        if (strstr(text, "纸")) {
            return "纸巾";
        }
        if (strstr(text, "屏") || strstr(text, "电子")) {
            return "手机";
        }
        if (strstr(text, "反光") || strstr(text, "过曝") || strstr(text, "高光")) {
            return "玻璃杯";
        }
    }
    return CAMERA_AI_FALLBACK_NAME;
}

static void parse_ai_candidate(const char *reply, fridge_ui_camera_result_t *result)
{
    if (!result) {
        return;
    }
    result->zone = 1;
    result->cell = 4;
    strlcpy(result->name, CAMERA_AI_FALLBACK_NAME, sizeof(result->name));
    strlcpy(result->quantity, "1件", sizeof(result->quantity));
    result->doubt[0] = '\0';

    char *json_text = extract_json_object_copy(reply);
    cJSON *root = cJSON_Parse(json_text ? json_text : (reply ? reply : ""));
    if (!root) {
        char preview[128] = {0};
        make_reply_preview(reply, preview, sizeof(preview));
        strlcpy(result->raw_reply, reply ? reply : "", sizeof(result->raw_reply));
        strlcpy(result->name, best_guess_name_from_text(reply, NULL), sizeof(result->name));
        strlcpy(result->meta, "AI 返回内容无法解析，请在结果页确认后再入库。", sizeof(result->meta));
        strlcpy(result->doubt, "诊断：图片已上传且 AI 已返回，但返回内容不是可解析 JSON。AI 原文摘要：", sizeof(result->doubt));
        strlcat(result->doubt, preview, sizeof(result->doubt));
        free(json_text);
        return;
    }

    const cJSON *candidates = cJSON_GetObjectItemCaseSensitive(root, "candidates");
    if (!cJSON_IsArray(candidates)) {
        candidates = cJSON_GetObjectItemCaseSensitive(root, "ingredients");
    }
    if (!cJSON_IsArray(candidates)) {
        candidates = cJSON_GetObjectItemCaseSensitive(root, "items");
    }
    if (!cJSON_IsArray(candidates)) {
        candidates = cJSON_GetObjectItemCaseSensitive(root, "foods");
    }
    const cJSON *first = cJSON_IsArray(candidates) ? cJSON_GetArrayItem(candidates, 0) : NULL;
    if (!first && cJSON_IsObject(root)) {
        first = root;
    }

    const cJSON *name = first ? cJSON_GetObjectItemCaseSensitive(first, "name") : NULL;
    if (!cJSON_IsString(name)) {
        name = first ? cJSON_GetObjectItemCaseSensitive(first, "food") : NULL;
    }
    if (!cJSON_IsString(name)) {
        name = first ? cJSON_GetObjectItemCaseSensitive(first, "ingredient") : NULL;
    }
    if (!cJSON_IsString(name)) {
        name = first ? cJSON_GetObjectItemCaseSensitive(first, "label") : NULL;
    }

    const cJSON *quantity = first ? cJSON_GetObjectItemCaseSensitive(first, "quantity") : NULL;
    if (!cJSON_IsString(quantity)) {
        quantity = first ? cJSON_GetObjectItemCaseSensitive(first, "amount") : NULL;
    }
    if (!cJSON_IsString(quantity)) {
        quantity = first ? cJSON_GetObjectItemCaseSensitive(first, "count") : NULL;
    }

    const cJSON *confidence = first ? cJSON_GetObjectItemCaseSensitive(first, "confidence") : NULL;
    if (!cJSON_IsNumber(confidence)) {
        confidence = first ? cJSON_GetObjectItemCaseSensitive(first, "score") : NULL;
    }
    if (!cJSON_IsNumber(confidence)) {
        confidence = first ? cJSON_GetObjectItemCaseSensitive(first, "probability") : NULL;
    }

    const cJSON *doubt = first ? cJSON_GetObjectItemCaseSensitive(first, "doubt") : NULL;
    if (!cJSON_IsString(doubt)) {
        doubt = first ? cJSON_GetObjectItemCaseSensitive(first, "note") : NULL;
    }
    if (!cJSON_IsString(doubt)) {
        doubt = first ? cJSON_GetObjectItemCaseSensitive(first, "reason") : NULL;
    }
    const cJSON *safety = cJSON_GetObjectItemCaseSensitive(root, "safety_note");

    if (cJSON_IsString(name) && name->valuestring && name->valuestring[0]) {
        strlcpy(result->name, name->valuestring, sizeof(result->name));
    }
    if (cJSON_IsString(quantity) && quantity->valuestring && quantity->valuestring[0]) {
        strlcpy(result->quantity, quantity->valuestring, sizeof(result->quantity));
    }
    if (cJSON_IsNumber(confidence)) {
        snprintf(result->meta, sizeof(result->meta), "置信度 %.0f%%", confidence->valuedouble * 100.0);
    } else {
        strlcpy(result->meta, "AI 已生成候选，请确认后入库。", sizeof(result->meta));
    }
    if (cJSON_IsString(doubt) && doubt->valuestring && doubt->valuestring[0]) {
        strlcpy(result->doubt, "疑点：", sizeof(result->doubt));
        strlcat(result->doubt, doubt->valuestring, sizeof(result->doubt));
    }
    if (cJSON_IsString(safety) && safety->valuestring && safety->valuestring[0]) {
        if (result->doubt[0]) {
            strlcat(result->doubt, "\n", sizeof(result->doubt));
        }
        strlcat(result->doubt, safety->valuestring, sizeof(result->doubt));
    }
    if (is_uncertain_object_name(result->name)) {
        char preview[128] = {0};
        make_reply_preview(reply, preview, sizeof(preview));
        strlcpy(result->name, best_guess_name_from_text(reply, result->doubt), sizeof(result->name));
        if (result->doubt[0]) {
            strlcat(result->doubt, "\n", sizeof(result->doubt));
        }
        strlcat(result->doubt, "诊断：AI 返回了不确定名称，已自动改为具体物品猜测。AI 原文摘要：", sizeof(result->doubt));
        strlcat(result->doubt, preview, sizeof(result->doubt));
    }
    strlcpy(result->raw_reply, reply ? reply : "", sizeof(result->raw_reply));
    cJSON_Delete(root);
    free(json_text);
}

static void analyze_done_async(void *user_data)
{
    camera_analyze_done_t *done = (camera_analyze_done_t *)user_data;
    if (!done) {
        return;
    }

    if (done->generation == s_generation) {
        s_analyzing = false;
        fridge_ui_model_set_camera_result(&done->result);
        if (done->err == ESP_OK) {
            fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA_RESULT);
        } else {
            // AI/上传失败也进入结果页展示原因，避免用户看到“自动回预览”误以为快门没反应。
            fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA_RESULT);
        }
    }

    if (done->heap_allocated) {
        free(done);
    }
}

static void show_analyze_start_failed(const char *error)
{
    // 任务创建失败发生在 AI worker 启动前，也要走结果页反馈，避免用户误以为快门没反应。
    fridge_ui_camera_result_t failed = {
        .valid = true,
        .analyzing = false,
        .ok = false,
        .zone = 1,
        .cell = 4,
    };
    strlcpy(failed.name, "识别失败", sizeof(failed.name));
    strlcpy(failed.quantity, "-", sizeof(failed.quantity));
    strlcpy(failed.error,
            error && error[0] ? error : "识别任务创建失败，请稍后重拍",
            sizeof(failed.error));
    fridge_ui_model_set_camera_result(&failed);
    fridge_ui_show_page(FRIDGE_UI_PAGE_CAMERA_RESULT);
}

static void analyze_task(void *arg)
{
    camera_analyze_job_arg_t *job = (camera_analyze_job_arg_t *)arg;
    uint32_t generation = job ? job->generation : 0;
    camera_analyze_done_t *done = heap_caps_calloc(1, sizeof(*done), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fridge_ai_image_result_t *ai = heap_caps_calloc(1, sizeof(*ai), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!done) {
        memset(&s_emergency_done, 0, sizeof(s_emergency_done));
        done = &s_emergency_done;
    } else {
        done->heap_allocated = true;
    }
    done->generation = generation;
    done->result.valid = true;
    done->result.analyzing = false;
    done->result.ok = false;
    done->result.zone = 1;
    done->result.cell = 4;

    if (!ai) {
        done->err = ESP_ERR_NO_MEM;
        strlcpy(done->result.error, "图片识别内存不足", sizeof(done->result.error));
        goto finish;
    }

    done->err = wait_preview_stopped_before_capture();
    if (done->err == ESP_OK) {
        done->err = fridge_camera_capture_ai_fullres();
    }
    fridge_camera_frame_view_t frame = {0};
    if (done->err == ESP_OK) {
        done->err = fridge_camera_get_frame(&frame);
    }
    if (done->err == ESP_OK) {
        fridge_ai_image_request_t request = {
            .jpeg = frame.data,
            .jpeg_len = frame.len,
            .width = frame.width,
            .height = frame.height,
        };
        strlcpy(request.task_type, "recognize_ingredients", sizeof(request.task_type));
        if (job && job->config) {
            done->err = fridge_ai_client_analyze_image_with_config(job->config, &request, ai);
        } else {
            done->err = ESP_ERR_INVALID_STATE;
            strlcpy(ai->chat.error, "AI 配置预加载失败，请稍后重拍", sizeof(ai->chat.error));
        }
    }

    if (ai) {
        done->result.width = ai->width;
        done->result.height = ai->height;
        done->result.jpeg_bytes = ai->jpeg_bytes;
        done->result.latency_ms = ai->chat.latency_ms;
        done->result.http_status = ai->chat.http_status;
        strlcpy(done->result.ai_model, ai->chat.model, sizeof(done->result.ai_model));
    }

    if (done->err == ESP_OK) {
        done->result.ok = true;
        parse_ai_candidate(ai->chat.reply, &done->result);
        ESP_LOGI(TAG,
                 "AI image parsed: http=%d, model=%s, name=%s, quantity=%s, reply_len=%u",
                 done->result.http_status,
                 done->result.ai_model,
                 done->result.name,
                 done->result.quantity,
                 (unsigned)strlen(ai->chat.reply));
    } else {
        fridge_camera_status_t status = {0};
        fridge_camera_get_status(&status);
        if (done->err == ESP_ERR_TIMEOUT) {
            strlcpy(done->result.error, "摄像头预览仍在停止中，请稍后重拍", sizeof(done->result.error));
        } else {
            strlcpy(done->result.error,
                    ai->chat.error[0] ? ai->chat.error : (status.last_error[0] ? status.last_error : esp_err_to_name(done->err)),
                    sizeof(done->result.error));
        }
    }

finish:
    (void)fridge_camera_clear_frame();
    free(ai);
    if (job) {
        free(job->config);
        free(job);
    }
    if (lv_async_call(analyze_done_async, done) != LV_RESULT_OK) {
        s_analyzing = false;
        ESP_LOGE(TAG, "post UI camera analyze result failed: %s", esp_err_to_name(done->err));
        if (done->heap_allocated) {
            free(done);
        }
    }
    ESP_LOGI(TAG, "UI camera analyze task done, stack high watermark=%u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDeleteWithCaps(NULL);
}

static camera_analyze_job_arg_t *prepare_analyze_job(uint32_t generation, char *error, size_t error_size)
{
    // 在 LVGL/UI 任务中预加载 AI 配置和网络状态，避免后台图片识别任务因内部 SRAM 不足无法创建。
    // 配置包含 API Key 明文，只在设备内存中传递，禁止写入日志或 UI。
    camera_analyze_job_arg_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fridge_ai_config_update_t *config = heap_caps_calloc(1, sizeof(*config), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job || !config) {
        free(job);
        free(config);
        strlcpy(error, "识别任务内存不足，请稍后重拍", error_size);
        return NULL;
    }

    esp_err_t err = fridge_ai_client_load_active_config(config);
    if (err == ESP_OK) {
        err = fridge_ai_client_ensure_network_ready(error, error_size);
    } else {
        strlcpy(error, esp_err_to_name(err), error_size);
    }
    if (err != ESP_OK) {
        free(config);
        free(job);
        return NULL;
    }

    job->generation = generation;
    job->config = config;
    return job;
}

static void shutter_cb(lv_event_t *event)
{
    (void)event;
    if (s_analyzing) {
        fridge_ui_toast("正在识别上一张照片");
        return;
    }

    stop_preview();
    s_analyzing = true;
    s_generation++;
    fridge_ui_camera_result_t pending = {
        .valid = true,
        .analyzing = true,
        .ok = false,
        .zone = 1,
        .cell = 4,
    };
    strlcpy(pending.name, "识别中", sizeof(pending.name));
    strlcpy(pending.quantity, "-", sizeof(pending.quantity));
    strlcpy(pending.meta, "正在抓拍高清照片并上传 AI。", sizeof(pending.meta));
    fridge_ui_model_set_camera_result(&pending);
    set_status_text("正在拍照并上传 AI...");
    set_shutter_enabled(false, "识别中");

    char start_error[160] = {0};
    camera_analyze_job_arg_t *job = prepare_analyze_job(s_generation, start_error, sizeof(start_error));
    if (!job) {
        s_analyzing = false;
        set_status_text("识别任务创建失败");
        set_shutter_enabled(true, "重拍");
        show_analyze_start_failed(start_error[0] ? start_error : "识别任务创建失败：内存不足或网络未就绪，请稍后重拍");
        return;
    }

    BaseType_t ok = create_camera_task(analyze_task,
                                       "ui_cam_ai",
                                       CAMERA_ANALYZE_TASK_STACK,
                                       job,
                                       NULL,
                                       false);
    if (ok != pdPASS) {
        free(job->config);
        free(job);
        s_analyzing = false;
        set_status_text("识别任务创建失败");
        set_shutter_enabled(true, "重拍");
        show_analyze_start_failed("识别任务创建失败：PSRAM 栈不可用，请稍后重拍");
    }
}

void fridge_ui_page_camera_create(lv_obj_t *parent)
{
    const fridge_ui_theme_t *theme = fridge_ui_theme_get();
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x161A16), 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "拍照登记");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, fridge_ui_font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 32, 8);

    s_status = lv_label_create(parent);
    lv_label_set_text(s_status, "进入页面后自动启动摄像头");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xB9C7B4), 0);
    lv_obj_set_style_text_font(s_status, fridge_ui_font_small(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -32, 18);

    s_finder = lv_obj_create(parent);
    lv_obj_set_size(s_finder, CAMERA_PREVIEW_FINDER_W, CAMERA_PREVIEW_FINDER_H);
    lv_obj_align(s_finder, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(s_finder, lv_color_hex(0x0A0F0B), 0);
    lv_obj_set_style_border_color(s_finder, lv_color_hex(0xBBD6AE), 0);
    lv_obj_set_style_border_width(s_finder, 2, 0);
    lv_obj_set_style_radius(s_finder, 10, 0);
    lv_obj_set_style_pad_all(s_finder, 0, 0);
    lv_obj_set_style_clip_corner(s_finder, true, 0);
    lv_obj_clear_flag(s_finder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_finder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_finder, retry_preview_cb, LV_EVENT_CLICKED, NULL);

    s_preview_img = lv_image_create(s_finder);
    lv_obj_set_size(s_preview_img, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(s_preview_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(s_preview_img, LV_OBJ_FLAG_HIDDEN);

    s_hint = lv_label_create(s_finder);
    lv_label_set_text(s_hint, "正在打开摄像头...");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xE7F3DD), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_hint, CAMERA_PREVIEW_FINDER_W - 64);
    lv_obj_center(s_hint);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hint, retry_preview_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rail = lv_obj_create(parent);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, CAMERA_PREVIEW_FINDER_W, 74);
    lv_obj_align(rail, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(rail, lv_color_hex(0x20271F), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_80, 0);
    lv_obj_set_style_radius(rail, 8, 0);

    lv_obj_t *tip = lv_label_create(rail);
    lv_label_set_text(tip, "对准主体物体后轻触快门");
    lv_obj_set_style_text_color(tip, lv_color_hex(0xD7E7CF), 0);
    lv_obj_set_style_text_font(tip, fridge_ui_font_body(), 0);
    lv_obj_align(tip, LV_ALIGN_LEFT_MID, 24, 0);

    s_shutter = lv_button_create(rail);
    lv_obj_set_size(s_shutter, 132, 58);
    lv_obj_align(s_shutter, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(s_shutter, 29, 0);
    lv_obj_set_style_bg_color(s_shutter, theme->accent_2, 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(0x546154), LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_shutter, shutter_cb, LV_EVENT_CLICKED, NULL);
    s_shutter_label = lv_label_create(s_shutter);
    lv_label_set_text(s_shutter_label, "拍照");
    lv_obj_center(s_shutter_label);
}

void fridge_ui_page_camera_update(void)
{
    if (g_ui_page == FRIDGE_UI_PAGE_CAMERA && !s_preview_attempted && !s_preview_running && !s_analyzing) {
        s_generation++;
        start_preview();
    } else if (g_ui_page != FRIDGE_UI_PAGE_CAMERA && s_preview_running) {
        stop_preview();
    }
}

void fridge_ui_page_camera_stop(void)
{
    stop_preview();
    s_preview_attempted = false;
    s_preview_failed = false;
    if (!s_preview_running && !s_analyzing) {
        release_preview_pixels();
    }
}
