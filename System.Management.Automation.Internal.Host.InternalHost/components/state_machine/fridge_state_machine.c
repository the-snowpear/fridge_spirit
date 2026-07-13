// 冰箱小精灵独立状态机实现。
// 该任务周期读取传感器快照，不直接访问 GPIO/UART/ADC；夜间“关闭雷达”首版仅做软件暂停策略，不卸载 UART、不改供电。

#include "fridge_state_machine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fridge_audio.h"
#include "fridge_network.h"
#include "fridge_sensors.h"
#include "fridge_voice_session.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SM_TASK_STACK 8192
#define SM_TASK_PRIORITY 4
#define SM_PERIOD_MS 300
#define SM_NVS_NAMESPACE "fridge_state"
// 关门稳定窗口改用可配置的 close_stable_ms（见 fridge_sm_config_t），不再使用固定时长。
#define SM_POST_CLOSE_HOLD_MS 8000      // 进入 POST_CLOSE（已关门+触发语音）后保持的展示窗口，到点回到 CLOSED
// 运动事件检测阈值：以陀螺仪角速度为主、去重力动态加速度为辅，全部需按真机标定调整。
#define SM_GYRO_MOTION_ON_DPS 60.0f     // 三轴角速度绝对值之和高于此值视为“正在运动”
#define SM_GYRO_MOTION_OFF_DPS 25.0f    // 低于此值才视为“静止”，on/off 不同形成迟滞，避免抖动反复触发
#define SM_GYRO_STRONG_DPS 120.0f       // 明确的门摆动角速度，单独即可确认是开关门动作
#define SM_VIB_MOTION_ON_G 0.10f        // 去重力动态加速度高于此值视为运动（典型为关门撞击）
#define SM_VIB_MOTION_OFF_G 0.04f       // 去重力动态加速度低于此值视为静止
#define SM_LIGHT_DOOR_DELTA 60          // 光照相对基线突变达到此幅度，作为开关门确认信号之一
#define SM_MOTION_QUIET_HOLD_MS 500     // 运动后需持续静止这么久，才允许下一次运动再触发事件
#define SM_MOTION_EVENT_DEBOUNCE_MS 700 // 两次运动事件最小间隔，过滤一次开关门的连续抖动
#define SM_VOICE_TASK_STACK 24576
#define SM_NIGHT_DEBOUNCE_SAMPLES 5

static const char *TAG = "state_machine";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static TaskHandle_t s_auto_voice_task;
static fridge_sm_config_t s_config;
static fridge_sm_snapshot_t s_snapshot;
static fridge_sm_door_state_t s_door_state = FRIDGE_SM_DOOR_UNKNOWN;
static bool s_is_night;
static uint8_t s_night_candidate_count;
static uint8_t s_day_candidate_count;
static int64_t s_state_since_ms;
static int64_t s_post_close_enter_ms;
static uint32_t s_post_close_generation;
// 运动事件检测的迟滞状态：
// s_motion_active 表示当前是否处于“正在运动”电平；只有从静止跨入运动（上升沿）才算一次运动事件。
// s_motion_quiet_since_ms 记录最近一次进入静止的时间，用于要求“运动前必须先静止够久”。
// s_last_motion_event_ms 记录最近一次已采纳的运动事件时间，用于事件去抖。
static bool s_motion_active;
static int64_t s_motion_quiet_since_ms;
static int64_t s_last_motion_event_ms;

static fridge_sm_config_t default_config(void)
{
    return (fridge_sm_config_t) {
        .night_light_threshold = 250,
        .day_light_threshold = 450,
        .radar_two_meter_raw = 200,
        .radar_two_meter_gate = 8,
        .sleep_enabled = false,
        .auto_voice_after_close = true,
        .auto_voice_record_seconds = 6,
        .close_stable_ms = 2500,
    };
}

const char *fridge_state_machine_state_to_string(fridge_sm_state_t state)
{
    switch (state) {
    case FRIDGE_SM_STATE_SLEEP:
        return "SLEEP";
    case FRIDGE_SM_STATE_NIGHT_SAVE:
        return "NIGHT_SAVE";
    case FRIDGE_SM_STATE_APPROACH:
        return "APPROACH";
    case FRIDGE_SM_STATE_INTERACTIVE:
        return "INTERACTIVE";
    case FRIDGE_SM_STATE_DOOR_MOVING:
        return "DOOR_MOVING";
    case FRIDGE_SM_STATE_DOOR_OPEN:
        return "DOOR_OPEN";
    case FRIDGE_SM_STATE_POST_CLOSE:
        return "POST_CLOSE";
    case FRIDGE_SM_STATE_OFFLINE:
        return "OFFLINE";
    default:
        return "UNKNOWN";
    }
}

const char *fridge_state_machine_door_to_string(fridge_sm_door_state_t state)
{
    switch (state) {
    case FRIDGE_SM_DOOR_CLOSED:
        return "CLOSED";
    case FRIDGE_SM_DOOR_MOVING:
        return "MOVING";
    case FRIDGE_SM_DOOR_OPEN:
        return "OPEN";
    case FRIDGE_SM_DOOR_POST_CLOSE:
        return "POST_CLOSE";
    case FRIDGE_SM_DOOR_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

const char *fridge_state_machine_auto_voice_to_string(fridge_sm_auto_voice_state_t state)
{
    switch (state) {
    case FRIDGE_SM_AUTO_VOICE_IDLE:
        return "IDLE";
    case FRIDGE_SM_AUTO_VOICE_RECORDING:
        return "RECORDING";
    case FRIDGE_SM_AUTO_VOICE_PROCESSING:
        return "PROCESSING";
    case FRIDGE_SM_AUTO_VOICE_DONE:
        return "DONE";
    case FRIDGE_SM_AUTO_VOICE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void set_auto_voice_state(fridge_sm_auto_voice_state_t state, const char *error)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snapshot.auto_voice_state = state;
    if (error) {
        strlcpy(s_snapshot.auto_voice_error, error, sizeof(s_snapshot.auto_voice_error));
    } else if (state != FRIDGE_SM_AUTO_VOICE_ERROR) {
        s_snapshot.auto_voice_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);
}

static void auto_voice_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    fridge_sm_config_t cfg = {0};
    (void)fridge_state_machine_get_config(&cfg);
    uint32_t seconds = cfg.auto_voice_record_seconds;
    if (seconds == 0 || seconds > FRIDGE_AUDIO_MAX_RECORD_SECONDS) {
        seconds = FRIDGE_AUDIO_MAX_RECORD_SECONDS;
    }

    // 关门后的自动语音在独立任务中运行，避免 ASR/AI 的 HTTPS 阻塞状态机周期判断。
    set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_RECORDING, NULL);
    esp_err_t err = fridge_audio_start_recording();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
        err = fridge_audio_stop_recording();
    }
    if (err == ESP_OK) {
        set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_PROCESSING, NULL);
        fridge_voice_session_result_t *result = heap_caps_calloc(1, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!result) {
            result = calloc(1, sizeof(*result));
        }
        if (!result) {
            err = ESP_ERR_NO_MEM;
            set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_ERROR, "auto voice result allocation failed");
        } else {
            err = fridge_voice_session_run_latest_recording(result);
            set_auto_voice_state(err == ESP_OK ? FRIDGE_SM_AUTO_VOICE_DONE : FRIDGE_SM_AUTO_VOICE_ERROR,
                                 err == ESP_OK ? NULL : result->error);
            free(result);
        }
    } else {
        set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_ERROR, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "auto voice generation=%lu done err=%s", (unsigned long)generation, esp_err_to_name(err));
    s_auto_voice_task = NULL;
    vTaskDelete(NULL);
}

static void maybe_start_auto_voice(void)
{
    if (!s_config.auto_voice_after_close || s_auto_voice_task) {
        return;
    }
    s_post_close_generation++;
    // 自动语音链路会读 ASR/AI NVS，并写入本地聊天历史。
    // Flash 事务会关闭 cache，PSRAM 栈在该窗口不可访问，必须使用普通内部 SRAM 栈。
    BaseType_t ok = xTaskCreate(auto_voice_task,
                                "sm_auto_voice",
                                SM_VOICE_TASK_STACK,
                                (void *)(uintptr_t)s_post_close_generation,
                                4,
                                &s_auto_voice_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG,
                 "create auto voice task failed, stack=%lu, largest_internal=%u bytes",
                 (unsigned long)SM_VOICE_TASK_STACK,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        set_auto_voice_state(FRIDGE_SM_AUTO_VOICE_ERROR, "auto voice task create failed");
    }
}

static void load_config_from_nvs(void)
{
    s_config = default_config();
    nvs_handle_t nvs = 0;
    if (nvs_open(SM_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_get_u16(nvs, "night", &s_config.night_light_threshold);
    (void)nvs_get_u16(nvs, "day", &s_config.day_light_threshold);
    (void)nvs_get_u16(nvs, "radar_raw", &s_config.radar_two_meter_raw);
    (void)nvs_get_u8(nvs, "radar_gate", &s_config.radar_two_meter_gate);
    uint8_t sleep_enabled = s_config.sleep_enabled ? 1 : 0;
    (void)nvs_get_u8(nvs, "sleep_en", &sleep_enabled);
    s_config.sleep_enabled = sleep_enabled != 0;
    uint8_t auto_voice = s_config.auto_voice_after_close ? 1 : 0;
    (void)nvs_get_u8(nvs, "auto_voice", &auto_voice);
    s_config.auto_voice_after_close = auto_voice != 0;
    (void)nvs_get_u32(nvs, "voice_sec", &s_config.auto_voice_record_seconds);
    (void)nvs_get_u32(nvs, "close_ms", &s_config.close_stable_ms);
    nvs_close(nvs);
}

static esp_err_t save_config_to_nvs(const fridge_sm_config_t *config)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(SM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, "night", config->night_light_threshold);
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "day", config->day_light_threshold);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "radar_raw", config->radar_two_meter_raw);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "radar_gate", config->radar_two_meter_gate);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "sleep_en", config->sleep_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "auto_voice", config->auto_voice_after_close ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "voice_sec", config->auto_voice_record_seconds);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "close_ms", config->close_stable_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static bool normalize_config(fridge_sm_config_t *config)
{
    if (!config) {
        return false;
    }
    if (config->night_light_threshold >= config->day_light_threshold) {
        return false;
    }
    if (config->day_light_threshold > FRIDGE_LIGHT_VALUE_MAX_10BIT) {
        return false;
    }
    if (config->radar_two_meter_gate >= FRIDGE_RADAR_GATE_COUNT) {
        config->radar_two_meter_gate = FRIDGE_RADAR_GATE_COUNT - 1;
    }
    if (config->auto_voice_record_seconds == 0 || config->auto_voice_record_seconds > FRIDGE_AUDIO_MAX_RECORD_SECONDS) {
        config->auto_voice_record_seconds = FRIDGE_AUDIO_MAX_RECORD_SECONDS;
    }
    if (config->close_stable_ms < 800) {
        config->close_stable_ms = 800;
    }
    return true;
}

// 检测“一次开关门运动事件”，并返回 0~1 的运动强度供快照展示。
// 设计要点（让开关门检测更可靠）：
// 1. 以陀螺仪三轴角速度之和为主信号：开关门一定带门体转动，纯压缩机震动几乎没有角速度，可天然过滤。
// 2. 去重力动态加速度为辅信号：捕捉关门那一下的撞击冲量。
// 3. 迟滞：进入运动用高阈值 ON，退出运动用低阈值 OFF，避免在阈值附近反复抖动。
// 4. 只在“静止→运动”的上升沿才算事件，且要求事件前已静止足够久（SM_MOTION_QUIET_HOLD_MS），
//    再叠加两次事件最小间隔（close_stable_ms），保证一次物理开门或关门只产生一次事件。
// 5. 多信号校正：弱转动若没有强角速度/撞击/光照突变/雷达有人任一佐证，按噪声丢弃；雷达只作佐证之一，
//    不单独作为开关门依据，符合“雷达只提供上下文”的硬约束。
static float detect_motion_event(const fridge_sensor_snapshot_t *sensors,
                                 bool radar_present,
                                 int64_t now_ms,
                                 bool *door_event)
{
    *door_event = false;

    float gyro_sum = fabsf(sensors->gyro_x_dps) + fabsf(sensors->gyro_y_dps) + fabsf(sensors->gyro_z_dps);
    float vib = sensors->vibration_peak; // 已是去重力动态加速度，静止时约等于 0

    // 运动强度：陀螺仪与去重力加速度归一化后取较大者，仅用于快照展示和调试。
    float gyro_score = gyro_sum / 240.0f;
    float vib_score = vib / 0.5f;
    float strength = fmaxf(gyro_score, vib_score);
    if (strength > 1.0f) {
        strength = 1.0f;
    }

    // 运动电平（带迟滞）：进入运动要求“明显转动”或“转动+撞击”，退出运动用更低阈值。
    bool above_on = (gyro_sum >= SM_GYRO_MOTION_ON_DPS) ||
                    (gyro_sum >= SM_GYRO_MOTION_OFF_DPS && vib >= SM_VIB_MOTION_ON_G);
    bool above_off = (gyro_sum >= SM_GYRO_MOTION_OFF_DPS) || (vib >= SM_VIB_MOTION_OFF_G);

    if (!s_motion_active) {
        if (above_on) {
            // 上升沿：检查“事件前已静止够久”和“与上次事件间隔够大”，两者都满足才采纳为门事件。
            bool quiet_long_enough = (now_ms - s_motion_quiet_since_ms) >= SM_MOTION_QUIET_HOLD_MS;
            bool spacing_ok = (now_ms - s_last_motion_event_ms) >= (int64_t)s_config.close_stable_ms;

            // 多信号佐证：强角速度（明确门摆动）、明显撞击、光照突变或雷达有人，任一成立即认为是真实开关门。
            bool strong_gyro = gyro_sum >= SM_GYRO_STRONG_DPS;
            bool vib_strong = vib >= SM_VIB_MOTION_ON_G;
            bool light_changed = abs((int)sensors->light_delta) >= SM_LIGHT_DOOR_DELTA;
            bool corroborated = strong_gyro || vib_strong || light_changed || radar_present;

            s_motion_active = true;
            if (quiet_long_enough && spacing_ok && corroborated) {
                *door_event = true;
                s_last_motion_event_ms = now_ms;
            }
        } else {
            // 仍处于静止，持续刷新“最近一次静止时刻”，供下一次上升沿计算静止时长。
            s_motion_quiet_since_ms = now_ms;
        }
    } else if (!above_off) {
        // 运动结束，回到静止，记录静止起点。
        s_motion_active = false;
        s_motion_quiet_since_ms = now_ms;
    }

    return strength;
}

// 门状态事件翻转模型：
// 关门(CLOSED) --运动事件--> 开门(OPEN) --运动事件--> 已关门(POST_CLOSE，触发自动语音) --超时--> 关门(CLOSED)
// 该模型与用户预期一致：每检测到一次开关门运动就翻转一次，第二次翻转（回到关门）时自动开启语音对话。
static void update_door_state(bool door_event, int64_t now_ms)
{
    // 开机首次先归位到关门态，作为事件翻转的起点。
    if (s_door_state == FRIDGE_SM_DOOR_UNKNOWN) {
        s_door_state = FRIDGE_SM_DOOR_CLOSED;
    }

    if (door_event) {
        switch (s_door_state) {
        case FRIDGE_SM_DOOR_OPEN:
            // 第二次运动事件：开门 -> 已关门，进入 POST_CLOSE 并触发关门后自动语音。
            s_door_state = FRIDGE_SM_DOOR_POST_CLOSE;
            s_post_close_enter_ms = now_ms;
            maybe_start_auto_voice();
            break;
        case FRIDGE_SM_DOOR_CLOSED:
        case FRIDGE_SM_DOOR_POST_CLOSE:
        default:
            // 第一次运动事件：关门/刚关门 -> 开门。
            s_door_state = FRIDGE_SM_DOOR_OPEN;
            break;
        }
        return;
    }

    // POST_CLOSE 是“刚关门并已触发语音”的展示态，保持固定窗口后回到稳定关门态。
    if (s_door_state == FRIDGE_SM_DOOR_POST_CLOSE && now_ms - s_post_close_enter_ms >= SM_POST_CLOSE_HOLD_MS) {
        s_door_state = FRIDGE_SM_DOOR_CLOSED;
    }
}

static void update_night_state(uint16_t light_value_10bit)
{
    // 光敏模块是模拟量，Wi-Fi 发射、杜邦线接触或环境光边界都可能带来瞬时跳变。
    // 夜间省电会影响雷达/UI 行为，所以必须连续多次越过阈值才切换，避免 SLEEP/NIGHT_SAVE 快速振荡。
    if (s_is_night) {
        s_night_candidate_count = 0;
        if (light_value_10bit > s_config.day_light_threshold) {
            if (s_day_candidate_count < SM_NIGHT_DEBOUNCE_SAMPLES) {
                s_day_candidate_count++;
            }
            if (s_day_candidate_count >= SM_NIGHT_DEBOUNCE_SAMPLES) {
                s_is_night = false;
                s_day_candidate_count = 0;
            }
        } else {
            s_day_candidate_count = 0;
        }
    } else {
        s_day_candidate_count = 0;
        if (light_value_10bit < s_config.night_light_threshold) {
            if (s_night_candidate_count < SM_NIGHT_DEBOUNCE_SAMPLES) {
                s_night_candidate_count++;
            }
            if (s_night_candidate_count >= SM_NIGHT_DEBOUNCE_SAMPLES) {
                s_is_night = true;
                s_night_candidate_count = 0;
            }
        } else {
            s_night_candidate_count = 0;
        }
    }
}

static fridge_sm_state_t decide_state(const fridge_sensor_snapshot_t *sensors,
                                      const fridge_network_status_t *net,
                                      bool radar_reliable,
                                      bool radar_within_2m,
                                      char *reason,
                                      size_t reason_size)
{
    if (s_is_night) {
        strlcpy(reason, "light below night threshold; radar software paused", reason_size);
        return FRIDGE_SM_STATE_NIGHT_SAVE;
    }
    if (s_door_state == FRIDGE_SM_DOOR_POST_CLOSE) {
        strlcpy(reason, "second motion event: door closed, auto voice triggered", reason_size);
        return FRIDGE_SM_STATE_POST_CLOSE;
    }
    if (s_door_state == FRIDGE_SM_DOOR_OPEN) {
        strlcpy(reason, "first motion event toggled door to open", reason_size);
        return FRIDGE_SM_STATE_DOOR_OPEN;
    }
    if (s_door_state == FRIDGE_SM_DOOR_MOVING) {
        strlcpy(reason, "door movement in progress", reason_size);
        return FRIDGE_SM_STATE_DOOR_MOVING;
    }
    if (radar_reliable && radar_within_2m) {
        strlcpy(reason, "reliable radar target within configured 2m boundary", reason_size);
        return FRIDGE_SM_STATE_INTERACTIVE;
    }
    if (radar_reliable || sensors->radar.approaching) {
        strlcpy(reason, "radar sees approaching/present target outside interaction boundary", reason_size);
        return FRIDGE_SM_STATE_APPROACH;
    }
    // 离线只作为快照标志上报，不能覆盖本地靠近/开门/首页流程。
    // 这样重启后 Wi-Fi 还没连上时，设备仍保持本地可用，而不是默认进入离线页。
    if (!net->connected) {
        strlcpy(reason, "network offline flag set; local idle state continues", reason_size);
        return FRIDGE_SM_STATE_SLEEP;
    }
    strlcpy(reason, "bright light, no reliable radar target, IMU quiet", reason_size);
    return FRIDGE_SM_STATE_SLEEP;
}

static void state_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        fridge_sensor_snapshot_t sensors = {0};
        fridge_network_status_t net = {0};
        (void)fridge_sensors_get_snapshot(&sensors);
        (void)fridge_network_get_status(&net);

        int64_t now_ms = esp_timer_get_time() / 1000;
        update_night_state(sensors.light_value_10bit);

        // 先评估雷达可靠存在，作为运动事件的佐证之一（雷达只作上下文，不单独判开关门）。
        bool radar_reliable = !s_is_night && sensors.radar.ready &&
                              (sensors.radar.stable_presence || sensors.radar.human_candidate || sensors.radar.threshold_presence);
        uint16_t distance = sensors.radar.smoothed_distance_raw ? sensors.radar.smoothed_distance_raw : sensors.radar.distance_raw;
        bool radar_within_2m = radar_reliable &&
                               (sensors.radar.within_1m ||
                                (distance > 0 && distance <= s_config.radar_two_meter_raw) ||
                                (sensors.radar.stable_gate > 0 && sensors.radar.stable_gate <= s_config.radar_two_meter_gate));

        // 运动事件检测 + 门状态事件翻转：检测到一次开关门运动就翻转一次门态。
        bool door_event = false;
        float motion = detect_motion_event(&sensors, radar_reliable, now_ms, &door_event);
        update_door_state(door_event, now_ms);

        fridge_sm_snapshot_t next = {0};
        next.door_state = s_door_state;
        next.offline = !net.connected;
        next.is_night = s_is_night;
        next.radar_software_paused = s_is_night;
        next.radar_presence_reliable = radar_reliable;
        next.radar_within_2m = radar_within_2m;
        next.radar_within_1m = sensors.radar.within_1m;
        next.radar_approaching = sensors.radar.approaching;
        next.imu_motion_strength = motion;
        next.light_value_10bit = sensors.light_value_10bit;
        next.light_delta = sensors.light_delta;
        next.radar_distance_raw = distance;
        next.radar_gate = sensors.radar.stable_gate;
        next.updated_at_ms = now_ms;
        next.auto_voice_state = s_snapshot.auto_voice_state;
        strlcpy(next.auto_voice_error, s_snapshot.auto_voice_error, sizeof(next.auto_voice_error));
        next.state = decide_state(&sensors, &net, radar_reliable, radar_within_2m, next.last_reason, sizeof(next.last_reason));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (next.state != s_snapshot.state) {
            s_state_since_ms = now_ms;
            ESP_LOGI(TAG, "state %s -> %s, door=%s, reason=%s",
                     fridge_state_machine_state_to_string(s_snapshot.state),
                     fridge_state_machine_state_to_string(next.state),
                     fridge_state_machine_door_to_string(next.door_state),
                     next.last_reason);
        }
        next.state_since_ms = s_state_since_ms;
        s_snapshot = next;
        xSemaphoreGive(s_lock);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SM_PERIOD_MS));
    }
}

esp_err_t fridge_state_machine_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    load_config_from_nvs();
    if (!normalize_config(&s_config)) {
        s_config = default_config();
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = FRIDGE_SM_STATE_SLEEP;
    s_snapshot.door_state = FRIDGE_SM_DOOR_UNKNOWN;
    s_snapshot.auto_voice_state = FRIDGE_SM_AUTO_VOICE_IDLE;
    s_state_since_ms = esp_timer_get_time() / 1000;
    // 运动事件计时基线设为当前时刻：要求开机后先有一段静止与间隔，避免上电安装、晃动时误触发一次开关门事件。
    s_motion_active = false;
    s_motion_quiet_since_ms = s_state_since_ms;
    s_last_motion_event_ms = s_state_since_ms;
    // 主状态机运行期只读取组件快照并更新内存状态；NVS 配置已在创建任务前加载。
    // 栈放 PSRAM 可减少常驻任务对内部 SRAM 的占用，不改变传感器/GPIO/I2S 配置。
    BaseType_t ok = xTaskCreateWithCaps(state_task,
                                        "fridge_sm",
                                        SM_TASK_STACK,
                                        NULL,
                                        SM_TASK_PRIORITY,
                                        &s_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fridge_state_machine_get_snapshot(fridge_sm_snapshot_t *out)
{
    if (!out || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_state_machine_get_config(fridge_sm_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        *out = default_config();
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t fridge_state_machine_set_config(const fridge_sm_config_t *config)
{
    if (!config || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    fridge_sm_config_t next = *config;
    if (!normalize_config(&next)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = save_config_to_nvs(&next);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = next;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}
