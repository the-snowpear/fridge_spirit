// 冰箱小精灵运行日志与 Flash coredump 导出组件。
//
// 日志 tee 尽早安装以保留串口输出，同时把 INFO/WARN/ERROR/FATAL 行放入
// 固定 8KB RAM 环形缓冲；TF 卡挂载成功后由低优先级任务批量写入 4 个
// 256KB 轮转文件。Flash coredump 只在下一次成功启动、完整写入受保护
// diagnostics 目录后擦除，避免断电或 TF 卡故障导致证据丢失。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_DIAGNOSTICS_LOG_RING_SIZE (8U * 1024U)
#define FRIDGE_DIAGNOSTICS_LOG_FILE_COUNT 4U
#define FRIDGE_DIAGNOSTICS_LOG_FILE_SIZE (256U * 1024U)
#define FRIDGE_DIAGNOSTICS_LOG_PATH_MAX 96U
#define FRIDGE_DIAGNOSTICS_LOG_ERROR_MAX 96U
#define FRIDGE_DIAGNOSTICS_LOG_REASON_MAX 160U

// 日志和 coredump 导出状态；只包含状态/计数，不回传敏感 coredump 内容。
typedef struct {
    bool installed;                 // esp_log tee 是否已安装。
    bool running;                   // TF writer 任务是否运行。
    bool tf_ready;                  // 最近一次 TF 写入是否可用。
    bool coredump_pending;          // Flash 中是否仍有待导出的 coredump。
    bool coredump_exported;         // 本次启动是否完整导出并擦除 coredump。
    uint8_t current_file;           // 当前轮转文件索引（0..3）。
    uint32_t dropped_records;       // 环形缓冲因空间不足丢弃的记录数。
    uint64_t dropped_bytes;         // 环形缓冲或 TF 写失败丢弃的字节数。
    uint64_t written_bytes;         // 已成功追加到 TF 的日志字节数。
    uint32_t write_errors;          // TF 写入失败次数。
    uint32_t coredump_size;         // 待处理或最近导出的 coredump 字节数。
    char current_path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
    char last_error[FRIDGE_DIAGNOSTICS_LOG_ERROR_MAX];
    char coredump_path[FRIDGE_DIAGNOSTICS_LOG_PATH_MAX];
    char coredump_reason[FRIDGE_DIAGNOSTICS_LOG_REASON_MAX];
    char coredump_error[FRIDGE_DIAGNOSTICS_LOG_ERROR_MAX];
} fridge_diagnostics_log_status_t;

// 尽早安装全局日志 tee；保留原有串口 vprintf 输出。可在 TF 未挂载时调用。
esp_err_t fridge_diagnostics_log_install(void);

// TF 卡初始化成功后调用，启动低优先级 writer 任务并排空早期 RAM 日志。
esp_err_t fridge_diagnostics_log_start(void);

// 停止 writer 任务；通常只在测试或卸载 TF 前调用。
esp_err_t fridge_diagnostics_log_stop(void);

// 读取日志和 coredump 状态；不会读取或返回 coredump 内容。
esp_err_t fridge_diagnostics_log_get_status(fridge_diagnostics_log_status_t *out);

// 在成功启动且 TF 已挂载后导出上一次启动遗留的 Flash coredump。
// boot_sequence 用于生成唯一文件名，完整写入 ELF 与 metadata 后才擦除 Flash。
esp_err_t fridge_diagnostics_log_process_coredump(uint32_t boot_sequence);

#ifdef __cplusplus
}
#endif
