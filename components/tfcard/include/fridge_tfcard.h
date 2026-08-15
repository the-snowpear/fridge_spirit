// 冰箱小精灵 TF 卡存储公共接口。
//
// 硬件固定为 ESP32-P4 CB V1.2 板载 SDMMC Slot0；C6 Hosted 占用 Slot1。
// 本组件只挂载/释放 Slot0，不申请或释放共享 LDO4。所有路径均为相对 /sdcard 的 UTF-8 路径。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_TFCARD_MOUNT_POINT "/sdcard"
#define FRIDGE_TFCARD_SYSTEM_DIR ".fridge_spirit"
#define FRIDGE_TFCARD_LOG_DIR ".fridge_spirit/logs"
#define FRIDGE_TFCARD_DIAGNOSTICS_DIR ".fridge_spirit/diagnostics"
#define FRIDGE_TFCARD_TRASH_DIR ".fridge_spirit/trash"
#define FRIDGE_TFCARD_UPLOAD_DIR ".fridge_spirit/uploads"
#define FRIDGE_TFCARD_EXPORT_DIR ".fridge_spirit/exports"
#define FRIDGE_TFCARD_MAX_ERROR_LEN 96
#define FRIDGE_TFCARD_MAX_PATH 255
#define FRIDGE_TFCARD_MAX_NAME 255
#define FRIDGE_TFCARD_LIST_PAGE_MAX 32
#define FRIDGE_TFCARD_TRANSFER_CHUNK 1024
#define FRIDGE_TFCARD_TEXT_MAX (256U * 1024U)
#define FRIDGE_TFCARD_FILE_MAX 0xFFFFFFFFULL

// TF 卡状态快照；健康字段只表示本机 I/O 结果，不代表卡片寿命或 SMART 数据。
typedef struct {
    bool mounted;
    bool format_required;
    bool formatted_this_boot;
    bool expert_mode;
    bool transfer_active;
    uint64_t capacity_mb;
    uint64_t free_mb;
    char card_name[8];
    char last_error[FRIDGE_TFCARD_MAX_ERROR_LEN + 1];
} fridge_tfcard_status_t;

// 目录项/文件属性；mtime 为 Unix 秒，FAT 时间精度由卡上文件系统决定。
typedef struct {
    char name[FRIDGE_TFCARD_MAX_NAME + 1];
    char path[FRIDGE_TFCARD_MAX_PATH + 1];
    uint64_t size;
    int64_t mtime;
    bool is_dir;
    bool is_hidden;
    bool is_protected;
} fridge_tfcard_entry_t;

// 分页目录列表。cursor 是从 0 开始的目录项偏移，next_cursor 可直接用于下一页。
typedef struct {
    fridge_tfcard_entry_t entries[FRIDGE_TFCARD_LIST_PAGE_MAX];
    size_t count;
    uint32_t next_cursor;
    bool has_more;
} fridge_tfcard_list_result_t;

typedef enum {
    FRIDGE_TFCARD_TRANSFER_READ = 0,
    FRIDGE_TFCARD_TRANSFER_WRITE = 1,
} fridge_tfcard_transfer_direction_t;

// 串口/后续 HTTP 共用的单传输会话状态；同一时间只允许一个会话。
typedef struct {
    bool active;
    uint32_t session_id;
    fridge_tfcard_transfer_direction_t direction;
    uint64_t total_size;
    uint64_t offset;
    char path[FRIDGE_TFCARD_MAX_PATH + 1];
} fridge_tfcard_transfer_status_t;

typedef struct {
    uint64_t bytes;
    uint32_t write_ms;
    uint32_t read_ms;
    uint32_t write_kib_s;
    uint32_t read_kib_s;
} fridge_tfcard_benchmark_result_t;

// 诊断二进制流读取回调。回调不能访问 TF 卡，只负责从 Flash/其它只读源提供数据。
typedef esp_err_t (*fridge_tfcard_diagnostic_read_cb_t)(void *context,
                                                        uint64_t offset,
                                                        uint8_t *buffer,
                                                        size_t capacity,
                                                        size_t *out_size);

// 初始化/挂载接口。默认挂载失败绝不自动格式化。
esp_err_t fridge_tfcard_init(void);
esp_err_t fridge_tfcard_mount(void);
esp_err_t fridge_tfcard_unmount(void);
void fridge_tfcard_mark_unavailable(const char *reason);
esp_err_t fridge_tfcard_format_fat32(void);
void fridge_tfcard_get_status(fridge_tfcard_status_t *out);

// 专家模式持久化于 NVS；启用确认口令必须精确匹配 ENABLE_TF_EXPERT_MODE。
esp_err_t fridge_tfcard_get_expert_mode(bool *enabled);
esp_err_t fridge_tfcard_set_expert_mode(bool enabled, const char *confirmation);

// 路径与文件管理。recursive 仅影响目录删除；普通删除总是移入受保护回收站。
esp_err_t fridge_tfcard_list(const char *relative_dir, uint32_t cursor, size_t limit,
                             fridge_tfcard_list_result_t *out);
esp_err_t fridge_tfcard_stat(const char *relative_path, fridge_tfcard_entry_t *out);
esp_err_t fridge_tfcard_mkdir(const char *relative_path);
esp_err_t fridge_tfcard_move(const char *from_path, const char *to_path, bool overwrite);
esp_err_t fridge_tfcard_delete_to_trash(const char *relative_path, bool recursive,
                                        char *trash_id, size_t trash_id_size);
esp_err_t fridge_tfcard_restore(const char *trash_id, bool overwrite,
                               char *restored_path, size_t restored_path_size);
esp_err_t fridge_tfcard_empty_trash(void);

// 小文本原子读写；写入先落 .part、fflush/fsync，再同卷改名。
esp_err_t fridge_tfcard_text_read(const char *relative_path, char *buffer, size_t buffer_size,
                                 size_t *out_size);
// 分页读取文本文件。即使文件大于 buffer，也只读取指定区间并返回总大小，供 Agent 大文件按页加载。
esp_err_t fridge_tfcard_text_read_range(const char *relative_path, size_t offset,
                                       char *buffer, size_t buffer_size,
                                       size_t *out_size, size_t *out_total_size);
esp_err_t fridge_tfcard_text_write(const char *relative_path, const char *text, size_t text_size,
                                  bool overwrite);
// 可恢复写入会保留上一版为同目录 .bak；Agent 权威数据启动时据此完成损坏/断电恢复。
esp_err_t fridge_tfcard_text_write_recoverable(const char *relative_path, const char *text,
                                              size_t text_size, bool overwrite);

// 追加运行日志。只允许写入 FRIDGE_TFCARD_LOG_DIR，写入期间复用 TF 全局互斥锁。
esp_err_t fridge_tfcard_append_log(const char *relative_path, const void *data,
                                   size_t data_size, bool sync);

// 将敏感诊断文本原子写入 FRIDGE_TFCARD_DIAGNOSTICS_DIR；普通 TF 浏览接口拒绝读取该目录。
esp_err_t fridge_tfcard_write_diagnostic_text(const char *relative_path, const char *text,
                                              size_t text_size, bool overwrite);

// 将只读源流式原子写入 FRIDGE_TFCARD_DIAGNOSTICS_DIR，避免把 coredump 整体搬入 SRAM/PSRAM。
esp_err_t fridge_tfcard_write_diagnostic_stream(const char *relative_path, uint64_t total_size,
                                                fridge_tfcard_diagnostic_read_cb_t read_cb,
                                                void *context, bool overwrite);

// 固件内部诊断导出专用接口；只允许写入 exports 下的单层文件，不受专家模式影响。
esp_err_t fridge_tfcard_write_diagnostic_export(const char *file_name, const char *text,
                                                size_t text_size, char *out_path,
                                                size_t out_path_size);

// 单一 1024B 分块传输。每块必须携带 offset 与 CRC32；write 结束时原子替换目标文件。
esp_err_t fridge_tfcard_transfer_begin(const char *relative_path,
                                      fridge_tfcard_transfer_direction_t direction,
                                      uint64_t total_size, bool overwrite,
                                      fridge_tfcard_transfer_status_t *out);
esp_err_t fridge_tfcard_transfer_status(fridge_tfcard_transfer_status_t *out);
esp_err_t fridge_tfcard_transfer_write(uint32_t session_id, uint64_t offset,
                                      const uint8_t *data, size_t length, uint32_t crc32);
esp_err_t fridge_tfcard_transfer_read(uint32_t session_id, uint64_t offset,
                                     uint8_t *data, size_t capacity, size_t *out_length,
                                     uint32_t *out_crc32);
esp_err_t fridge_tfcard_transfer_end(uint32_t session_id);
esp_err_t fridge_tfcard_transfer_abort(uint32_t session_id);

// 维护接口：自检与基准测试为同步基础 API，耗时操作应由上层后台 job 调用。
esp_err_t fridge_tfcard_selftest(char *detail, size_t detail_size);
esp_err_t fridge_tfcard_benchmark(size_t test_bytes, fridge_tfcard_benchmark_result_t *out);

#ifdef __cplusplus
}
#endif
