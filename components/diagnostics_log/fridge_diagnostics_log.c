// 冰箱小精灵运行日志与 Flash coredump 导出实现。
//
// esp_log_set_vprintf() 回调运行在产生日志的任务上下文，不能直接访问
// FATFS；这里只做短暂格式化和环形缓冲复制，由低优先级任务串行调用 TF
// API。TF 日志路径按板级宏固定在受控日志目录，普通浏览仍由 tfcard 组件
// 执行路径校验；coredump 另走受保护的 diagnostic stream API。

#include "fridge_diagnostics_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fridge_tfcard.h"

#define LOG_LINE_BUFFER_SIZE 1024U
#define LOG_BATCH_SIZE 4096U
#define LOG_WRITER_STACK_SIZE 4096U
#define LOG_WRITER_PRIORITY (tskIDLE_PRIORITY + 1)
#define LOG_RETRY_DELAY_MS 1000U

typedef struct {
    uint8_t data[FRIDGE_DIAGNOSTICS_LOG_RING_SIZE];
    size_t head;
    size_t tail;
    size_t used;
    uint32_t dropped_records;
    uint64_t dropped_bytes;
} log_ring_t;

typedef struct {
    const esp_partition_t *partition;
    size_t image_size;
} coredump_stream_context_t;

static log_ring_t s_ring;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_writer_task;
static SemaphoreHandle_t s_writer_exit_sem;
static vprintf_like_t s_previous_vprintf;
static bool s_installed;
static bool s_stop_requested;
static bool s_tf_ready;
static bool s_coredump_pending;
static bool s_coredump_exported;
static uint8_t s_current_file;
static size_t s_current_file_size;
static bool s_current_file_valid;
static uint64_t s_written_bytes;
static uint32_t s_write_errors;
static char s_current_path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
static char s_last_error[FRIDGE_DIAGNOSTICS_LOG_ERROR_MAX];
static uint32_t s_coredump_size;
static char s_coredump_path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
static char s_coredump_reason[FRIDGE_DIAGNOSTICS_LOG_REASON_MAX];
static char s_coredump_error[FRIDGE_DIAGNOSTICS_LOG_ERROR_MAX];

// 设置内部状态错误文本。状态锁只保护短字符串复制，不持有 TF 锁。
static void set_last_error(const char *message)
{
    taskENTER_CRITICAL(&s_state_mux);
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
    taskEXIT_CRITICAL(&s_state_mux);
}

// 日志格式首字符为 IDF 的级别标记（I/W/E/C/F）；D/V 行不写入 TF。
// 其它格式（例如 panic 原始文本）保留，便于卡死/重启后定位上下文。
static bool log_line_is_info_or_above(const char *line, size_t length)
{
    if (!line || length == 0) {
        return false;
    }
    switch (line[0]) {
        case 'D':
        case 'V':
            return false;
        case 'I':
        case 'W':
        case 'E':
        case 'C':
        case 'F':
            return true;
        default:
            return true;
    }
}

// 在环形缓冲中保留最新记录；空间不足时丢弃最旧字节并累计计数。
static void ring_push(const uint8_t *data, size_t length)
{
    if (!data || length == 0) {
        return;
    }

    taskENTER_CRITICAL(&s_ring_mux);
    if (length >= sizeof(s_ring.data)) {
        data += length - (sizeof(s_ring.data) - 1U);
        length = sizeof(s_ring.data) - 1U;
        s_ring.dropped_records++;
    }

    const size_t free_space = sizeof(s_ring.data) - s_ring.used;
    if (length > free_space) {
        const size_t discard = length - free_space;
        s_ring.tail = (s_ring.tail + discard) % sizeof(s_ring.data);
        s_ring.used -= discard;
        s_ring.dropped_records++;
        s_ring.dropped_bytes += discard;
    }

    const size_t first = (sizeof(s_ring.data) - s_ring.head) < length
                       ? (sizeof(s_ring.data) - s_ring.head) : length;
    memcpy(&s_ring.data[s_ring.head], data, first);
    if (length > first) {
        memcpy(&s_ring.data[0], data + first, length - first);
    }
    s_ring.head = (s_ring.head + length) % sizeof(s_ring.data);
    s_ring.used += length;
    taskEXIT_CRITICAL(&s_ring_mux);

    if (s_writer_task) {
        xTaskNotifyGive(s_writer_task);
    }
}

// 从环形缓冲批量取出连续数据，避免 writer 每条日志都访问 FATFS。
static size_t ring_pop(uint8_t *out, size_t capacity)
{
    if (!out || capacity == 0) {
        return 0;
    }

    taskENTER_CRITICAL(&s_ring_mux);
    const size_t length = s_ring.used < capacity ? s_ring.used : capacity;
    const size_t first = (sizeof(s_ring.data) - s_ring.tail) < length
                       ? (sizeof(s_ring.data) - s_ring.tail) : length;
    memcpy(out, &s_ring.data[s_ring.tail], first);
    if (length > first) {
        memcpy(out + first, &s_ring.data[0], length - first);
    }
    s_ring.tail = (s_ring.tail + length) % sizeof(s_ring.data);
    s_ring.used -= length;
    taskEXIT_CRITICAL(&s_ring_mux);
    return length;
}

// 生成固定 4 个轮转文件名；路径长度远小于 tfcard 的最大相对路径。
static void make_log_path(uint8_t index, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s/diag_%u.log", FRIDGE_TFCARD_LOG_DIR,
             (unsigned)index);
}

// 挂载后选择最近修改的文件；若该文件已满，则下一个文件先原子清空。
static esp_err_t select_log_file(void)
{
    bool found = false;
    uint8_t selected = 0;
    int64_t selected_mtime = -1;
    uint64_t selected_size = 0;

    for (uint8_t i = 0; i < FRIDGE_DIAGNOSTICS_LOG_FILE_COUNT; ++i) {
        char path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
        make_log_path(i, path, sizeof(path));
        fridge_tfcard_entry_t entry = {0};
        if (fridge_tfcard_stat(path, &entry) == ESP_OK && !entry.is_dir) {
            if (!found || entry.mtime > selected_mtime ||
                (entry.mtime == selected_mtime && entry.size >= selected_size)) {
                found = true;
                selected = i;
                selected_mtime = entry.mtime;
                selected_size = entry.size;
            }
        }
    }

    if (!found) {
        selected = 0;
        selected_size = 0;
    } else if (selected_size >= FRIDGE_DIAGNOSTICS_LOG_FILE_SIZE) {
        selected = (selected + 1U) % FRIDGE_DIAGNOSTICS_LOG_FILE_COUNT;
        selected_size = 0;
    }

    make_log_path(selected, s_current_path, sizeof(s_current_path));
    s_current_file = selected;
    s_current_file_size = (size_t)selected_size;
    s_current_file_valid = true;

    // text_write 的零长度原子写入只用于轮转清空，不承担日志正文写入。
    if (found && selected_size == 0) {
        esp_err_t err = fridge_tfcard_text_write(s_current_path, "", 0, true);
        if (err != ESP_OK) {
            s_current_file_valid = false;
            return err;
        }
    }
    return ESP_OK;
}

// 文件达到 256KB 后切换到下一个槽位，并先清空旧内容避免 append 无限增长。
static esp_err_t rotate_log_file(void)
{
    const uint8_t next = (s_current_file + 1U) % FRIDGE_DIAGNOSTICS_LOG_FILE_COUNT;
    char path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
    make_log_path(next, path, sizeof(path));
    esp_err_t err = fridge_tfcard_text_write(path, "", 0, true);
    if (err != ESP_OK) {
        return err;
    }
    strlcpy(s_current_path, path, sizeof(s_current_path));
    s_current_file = next;
    s_current_file_size = 0;
    s_current_file_valid = true;
    return ESP_OK;
}

// 将一批日志按文件剩余空间拆分写入；每批最后一次 append 要求同步到卡。
static esp_err_t write_log_batch(const uint8_t *data, size_t length)
{
    while (length > 0) {
        if (!s_current_file_valid || s_current_file_size >= FRIDGE_DIAGNOSTICS_LOG_FILE_SIZE) {
            esp_err_t err = rotate_log_file();
            if (err != ESP_OK) {
                return err;
            }
        }

        size_t remaining = FRIDGE_DIAGNOSTICS_LOG_FILE_SIZE - s_current_file_size;
        size_t part = length < remaining ? length : remaining;
        const bool sync = (part == length);
        esp_err_t err = fridge_tfcard_append_log(s_current_path, data, part, sync);
        if (err != ESP_OK) {
            return err;
        }
        s_current_file_size += part;
        s_written_bytes += part;
        data += part;
        length -= part;
    }
    return ESP_OK;
}

// TF writer 低优先级运行，避免阻塞 UI/网络；TF 失败时保留后续记录并累计丢失。
static void log_writer_task(void *context)
{
    (void)context;
    // 批处理缓冲由单一 writer 任务独占；放在静态区，避免 4KB 缓冲直接耗尽任务栈。
    static uint8_t batch[LOG_BATCH_SIZE];

    while (!s_stop_requested) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOG_RETRY_DELAY_MS));
        if (!s_tf_ready) {
            fridge_tfcard_status_t tf_status = {0};
            fridge_tfcard_get_status(&tf_status);
            s_tf_ready = tf_status.mounted;
            if (!s_tf_ready) {
                continue;
            }
        }

        if (!s_current_file_valid) {
            if (select_log_file() != ESP_OK) {
                s_tf_ready = false;
                s_write_errors++;
                set_last_error("select log file failed");
                continue;
            }
        }

        size_t length;
        while ((length = ring_pop(batch, sizeof(batch))) > 0) {
            esp_err_t err = write_log_batch(batch, length);
            if (err != ESP_OK) {
                s_tf_ready = false;
                s_current_file_valid = false;
                s_write_errors++;
                taskENTER_CRITICAL(&s_ring_mux);
                s_ring.dropped_bytes += length;
                taskEXIT_CRITICAL(&s_ring_mux);
                set_last_error(esp_err_to_name(err));
                break;
            }
        }
    }

    // 先清空句柄，避免日志回调在任务删除后向悬空 TaskHandle 发通知。
    s_writer_task = NULL;
    if (s_writer_exit_sem) {
        xSemaphoreGive(s_writer_exit_sem);
    }
    vTaskDelete(NULL);
}

// 将一条 INFO+ 日志复制到环形缓冲。
// 单独拆成函数，避免 diag_log 自己输出 SDMMC/FATFS 错误时仍在深调用栈上
// 固定保留 1KB 格式化缓冲，导致 4KB writer 栈发生保护故障。
static void capture_log_line(const char *format, va_list args)
{
    char line[LOG_LINE_BUFFER_SIZE];
    va_list capture_args;
    va_copy(capture_args, args);
    const int needed = vsnprintf(line, sizeof(line), format, capture_args);
    va_end(capture_args);
    if (needed <= 0) {
        return;
    }
    size_t length = (size_t)needed;
    if (length >= sizeof(line)) {
        length = sizeof(line) - 1U;
    }
    if (log_line_is_info_or_above(line, length)) {
        ring_push((const uint8_t *)line, length);
    }
}

// 全局日志回调：先转发原 vprintf 保证串口行为，再复制 INFO+ 文本到 RAM。
static int diagnostics_log_vprintf(const char *format, va_list args)
{
    int result = 0;
    if (s_previous_vprintf) {
        va_list serial_args;
        va_copy(serial_args, args);
        result = s_previous_vprintf(format, serial_args);
        va_end(serial_args);
    } else {
        va_list serial_args;
        va_copy(serial_args, args);
        result = vprintf(format, serial_args);
        va_end(serial_args);
    }

    // writer 写 TF 时产生的底层日志已经从串口输出，不再回灌到自己的环形缓冲。
    // 这既避免错误风暴自激，也让 FATFS/stdio 深调用不再叠加 1KB 捕获栈帧。
    if (xTaskGetCurrentTaskHandle() != s_writer_task) {
        capture_log_line(format, args);
    }
    return result;
}

// 在网络、TF 等外设启动前安装 tee；此阶段只使用静态 RAM，不访问文件系统。
esp_err_t fridge_diagnostics_log_install(void)
{
    if (s_installed) {
        return ESP_OK;
    }
    s_previous_vprintf = esp_log_set_vprintf(diagnostics_log_vprintf);
    s_installed = true;
    return ESP_OK;
}

// TF 挂载成功后启动 writer；启动前先扫描轮转文件，确保早期启动日志不会留在 RAM。
esp_err_t fridge_diagnostics_log_start(void)
{
    esp_err_t err = fridge_diagnostics_log_install();
    if (err != ESP_OK) {
        return err;
    }
    if (s_writer_task) {
        return ESP_OK;
    }

    fridge_tfcard_status_t tf_status = {0};
    fridge_tfcard_get_status(&tf_status);
    if (!tf_status.mounted) {
        s_tf_ready = false;
        set_last_error("TF card is not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_writer_exit_sem) {
        s_writer_exit_sem = xSemaphoreCreateBinary();
        if (!s_writer_exit_sem) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_stop_requested = false;
    s_tf_ready = true;
    s_current_file_valid = false;
    if (select_log_file() != ESP_OK) {
        s_tf_ready = false;
        set_last_error("select log file failed");
        return ESP_FAIL;
    }
    // writer 虽只写 TF，但可能与另一 CPU 的 NVS/LittleFS 操作并发；常驻栈保留内部 RAM。
    BaseType_t result = xTaskCreate(log_writer_task, "diag_log", LOG_WRITER_STACK_SIZE,
                                    NULL, LOG_WRITER_PRIORITY, &s_writer_task);
    if (result != pdPASS) {
        s_writer_task = NULL;
        s_tf_ready = false;
        set_last_error("create log writer failed");
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_writer_task);
    return ESP_OK;
}

// 停止后台 writer 并等待其退出，避免卸载 TF 时仍有 FATFS 访问。
esp_err_t fridge_diagnostics_log_stop(void)
{
    TaskHandle_t task = s_writer_task;
    if (!task) {
        return ESP_OK;
    }
    s_stop_requested = true;
    xTaskNotifyGive(task);
    if (s_writer_exit_sem && xSemaphoreTake(s_writer_exit_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_writer_task = NULL;
    s_tf_ready = false;
    return ESP_OK;
}

// 复制日志与 coredump 状态快照；日志环形缓冲计数单独使用 ring 临界区保护。
esp_err_t fridge_diagnostics_log_get_status(fridge_diagnostics_log_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    taskENTER_CRITICAL(&s_state_mux);
    out->installed = s_installed;
    out->running = s_writer_task != NULL;
    out->tf_ready = s_tf_ready;
    out->coredump_pending = s_coredump_pending;
    out->coredump_exported = s_coredump_exported;
    out->current_file = s_current_file;
    out->written_bytes = s_written_bytes;
    out->write_errors = s_write_errors;
    out->coredump_size = s_coredump_size;
    strlcpy(out->current_path, s_current_path, sizeof(out->current_path));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    strlcpy(out->coredump_path, s_coredump_path, sizeof(out->coredump_path));
    strlcpy(out->coredump_reason, s_coredump_reason, sizeof(out->coredump_reason));
    strlcpy(out->coredump_error, s_coredump_error, sizeof(out->coredump_error));
    taskEXIT_CRITICAL(&s_state_mux);

    taskENTER_CRITICAL(&s_ring_mux);
    out->dropped_records = s_ring.dropped_records;
    out->dropped_bytes = s_ring.dropped_bytes;
    taskEXIT_CRITICAL(&s_ring_mux);
    return ESP_OK;
}

// Flash coredump stream 回调：esp_core_dump_image_get 返回分区基地址，
// 因此这里将 offset 解释为分区内偏移并直接读取，不把整份 dump 搬入内存。
static esp_err_t coredump_read_cb(void *context, uint64_t offset, uint8_t *buffer,
                                  size_t capacity, size_t *out_size)
{
    coredump_stream_context_t *stream = (coredump_stream_context_t *)context;
    if (!stream || !stream->partition || !buffer || !out_size ||
        offset >= stream->image_size) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t wanted = stream->image_size - (size_t)offset;
    if (wanted > capacity) {
        wanted = capacity;
    }
    esp_err_t err = esp_partition_read(stream->partition, (size_t)offset, buffer, wanted);
    if (err == ESP_OK) {
        *out_size = wanted;
    }
    return err;
}

// 生成受保护 coredump metadata；摘要用于串口/USB 状态判断，正文只留在 TF。
static size_t format_coredump_metadata(char *buffer, size_t capacity, uint32_t boot_sequence,
                                       size_t image_size, const char *panic_reason,
                                       const void *summary_ptr)
{
    if (!buffer || capacity == 0) {
        return 0;
    }

    const char *task_name = "unknown";
    uint32_t pc = 0;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    const esp_core_dump_summary_t *summary = (const esp_core_dump_summary_t *)summary_ptr;
    char task_name_copy[sizeof(summary->exc_task) + 1U];
    if (summary) {
        memcpy(task_name_copy, summary->exc_task, sizeof(summary->exc_task));
        task_name_copy[sizeof(summary->exc_task)] = '\0';
        task_name = task_name_copy;
        pc = summary->exc_pc;
    }
#else
    (void)summary_ptr;
#endif
    const int written = snprintf(buffer, capacity,
                                 "boot_sequence=%lu\nsize=%lu\nformat=ELF\npanic_reason=%s\nexception_task=%s\nexception_pc=0x%08lx\n",
                                 (unsigned long)boot_sequence, (unsigned long)image_size,
                                 panic_reason && panic_reason[0] ? panic_reason : "unknown",
                                 task_name, (unsigned long)pc);
    // snprintf 在截断时返回“本应写入”的完整长度；不能把这个长度直接
    // 交给 TF 写接口，否则会从 1KiB 栈缓冲区之外继续读取并写入卡片。
    if (written < 0 || (size_t)written >= capacity) {
        buffer[capacity - 1U] = '\0';
        return 0;
    }
    return (size_t)written;
}

// 成功启动后的 coredump 导出流程：先校验 Flash，再原子写 TF，最后擦除 Flash。
esp_err_t fridge_diagnostics_log_process_coredump(uint32_t boot_sequence)
{
    taskENTER_CRITICAL(&s_state_mux);
    s_coredump_pending = false;
    s_coredump_exported = false;
    s_coredump_size = 0;
    s_coredump_path[0] = '\0';
    s_coredump_reason[0] = '\0';
    s_coredump_error[0] = '\0';
    taskEXIT_CRITICAL(&s_state_mux);

    size_t image_addr = 0;
    size_t image_size = 0;
    esp_err_t err = esp_core_dump_image_get(&image_addr, &image_size);
    (void)image_addr;
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        // 空分区的首字为 0xFFFFFFFF；其它 INVALID_SIZE 通常意味着损坏的 coredump，
        // 仍需在状态中标记 pending，避免把故障静默当成“没有 coredump”。
        const esp_partition_t *candidate = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        uint32_t marker = 0xFFFFFFFFU;
        esp_err_t marker_err = candidate ? esp_partition_read(candidate, 0, &marker, sizeof(marker))
                                         : ESP_ERR_NOT_FOUND;
        if (marker_err != ESP_OK || marker != 0xFFFFFFFFU) {
            taskENTER_CRITICAL(&s_state_mux);
            s_coredump_pending = true;
            strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
            taskEXIT_CRITICAL(&s_state_mux);
            return err;
        }
        return ESP_OK;
    }
    if (err != ESP_OK || image_size == 0) {
        taskENTER_CRITICAL(&s_state_mux);
        s_coredump_pending = true;
        strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
        taskEXIT_CRITICAL(&s_state_mux);
        return err;
    }

    taskENTER_CRITICAL(&s_state_mux);
    s_coredump_pending = true;
    s_coredump_size = image_size > UINT32_MAX ? UINT32_MAX : (uint32_t)image_size;
    taskEXIT_CRITICAL(&s_state_mux);

    err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_state_mux);
        strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
        taskEXIT_CRITICAL(&s_state_mux);
        return err;
    }

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!partition) {
        err = ESP_ERR_NOT_FOUND;
        taskENTER_CRITICAL(&s_state_mux);
        strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
        taskEXIT_CRITICAL(&s_state_mux);
        return err;
    }

    char reason[FRIDGE_DIAGNOSTICS_LOG_REASON_MAX] = "unknown";
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t summary = {0};
    const void *summary_ptr = &summary;
#else
    const void *summary_ptr = NULL;
#endif
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    (void)esp_core_dump_get_panic_reason(reason, sizeof(reason));
    (void)esp_core_dump_get_summary(&summary);
#endif

    char dump_path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
    char meta_path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
    const int dump_path_len = snprintf(dump_path, sizeof(dump_path), "%s/coredump-%lu.elf",
                                       FRIDGE_TFCARD_DIAGNOSTICS_DIR, (unsigned long)boot_sequence);
    const int meta_path_len = snprintf(meta_path, sizeof(meta_path), "%s/coredump-%lu.txt",
                                       FRIDGE_TFCARD_DIAGNOSTICS_DIR, (unsigned long)boot_sequence);
    if (dump_path_len <= 0 || (size_t)dump_path_len >= sizeof(dump_path) ||
        meta_path_len <= 0 || (size_t)meta_path_len >= sizeof(meta_path)) {
        err = ESP_ERR_INVALID_SIZE;
        taskENTER_CRITICAL(&s_state_mux);
        strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
        taskEXIT_CRITICAL(&s_state_mux);
        return err;
    }

    coredump_stream_context_t stream = {.partition = partition, .image_size = image_size};
    err = fridge_tfcard_write_diagnostic_stream(dump_path, image_size, coredump_read_cb,
                                                &stream, true);
    if (err == ESP_OK) {
        char metadata[1024];
        size_t metadata_size = format_coredump_metadata(metadata, sizeof(metadata),
                                                        boot_sequence, image_size, reason,
                                                        summary_ptr);
        err = metadata_size > 0
            ? fridge_tfcard_write_diagnostic_text(meta_path, metadata, metadata_size, true)
            : ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_state_mux);
        strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
        taskEXIT_CRITICAL(&s_state_mux);
        return err;
    }

    taskENTER_CRITICAL(&s_state_mux);
    strlcpy(s_coredump_path, dump_path, sizeof(s_coredump_path));
    strlcpy(s_coredump_reason, reason, sizeof(s_coredump_reason));
    taskEXIT_CRITICAL(&s_state_mux);

    // 只有 ELF 与 metadata 都已原子落盘，才擦除 Flash，保证失败可在下次重试。
    err = esp_core_dump_image_erase();
    taskENTER_CRITICAL(&s_state_mux);
    if (err == ESP_OK) {
        s_coredump_pending = false;
        s_coredump_exported = true;
    } else {
        strlcpy(s_coredump_error, esp_err_to_name(err), sizeof(s_coredump_error));
    }
    taskEXIT_CRITICAL(&s_state_mux);
    return err;
}
