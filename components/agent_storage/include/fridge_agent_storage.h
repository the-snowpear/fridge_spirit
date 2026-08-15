// 冰箱小精灵 Agent TF 卡存储初始化接口。
//
// 本组件只负责在 /sdcard/agent 下建立 Agent Runtime 需要的目录、默认配置和技术文档入口。
// 设计原则：能放在 TF 卡的 Agent 内容尽量放在 TF 卡，Flash/NVS 只保存 Wi-Fi 凭据和 API Key 等敏感配置。

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRIDGE_AGENT_STORAGE_ROOT "agent"
#define FRIDGE_AGENT_STORAGE_MAX_ERROR_LEN 128
#define FRIDGE_AGENT_STORAGE_MAX_PATH_LEN 160
#define FRIDGE_AGENT_STORAGE_MEMORY_CONTEXT_MAX 4096

typedef struct {
    bool ready;             // Agent 目录和默认文件是否已准备完成。
    bool created_defaults;  // 本次启动是否创建过任一默认文件。
    uint32_t created_dirs;  // 本次启动创建的目录数量。
    uint32_t created_files; // 本次启动创建的文件数量。
    uint32_t recovered_files; // 本次启动从 .bak/.part 恢复的文件数量。
    char root[FRIDGE_AGENT_STORAGE_MAX_PATH_LEN];
    char last_error[FRIDGE_AGENT_STORAGE_MAX_ERROR_LEN + 1];
} fridge_agent_storage_status_t;

// 初始化 TF 卡上的 Agent 数据根目录。
// 调用前必须确保 fridge_tfcard_init() 已成功挂载 TF 卡；失败时不会格式化 TF 卡。
esp_err_t fridge_agent_storage_init(void);

// 读取最近一次 Agent 存储初始化状态，供 USB/诊断命令展示。
void fridge_agent_storage_get_status(fridge_agent_storage_status_t *out);

// 获取 Agent Storage 跨组件事务锁。需要“读 JSON→修改→写回”的调用方必须在整个
// 读改写期间持有该锁，防止本地 Tool 与 MQTT 同步互相覆盖。锁为递归锁，同一任务内
// 可继续调用下方 json_read/json_write；timeout_ms=UINT32_MAX 表示永久等待。
esp_err_t fridge_agent_storage_transaction_begin(uint32_t timeout_ms);
void fridge_agent_storage_transaction_end(void);

// 读取并校验 schema_version=1 的 Agent JSON。正式文件损坏/缺失时依次尝试 .bak、.part；
// 只有三者都不可用且提供 default_json 时才创建默认文件。
esp_err_t fridge_agent_storage_json_read(const char *path, char *buffer, size_t buffer_size,
                                        size_t *out_size, const char *default_json);

// 先在 RAM 校验 JSON，再通过 TF 可恢复写入保留上一版 .bak，并从盘读回复验。
esp_err_t fridge_agent_storage_json_write(const char *path, const char *json, size_t json_size);

// MEMORY.md 等受管文本使用同一可恢复替换策略，但不做 JSON Schema 校验。
esp_err_t fridge_agent_storage_text_write(const char *path, const char *text, size_t text_size);

// 生成供模型默认注入的紧凑用户记忆 JSON。
// 只包含最近的结构化用户事实，不注入 MEMORY.md 人工笔记；完整检索仍使用 memory.search。
esp_err_t fridge_agent_storage_build_memory_context(char *buffer, size_t buffer_size,
                                                    size_t *out_size);

#ifdef __cplusplus
}
#endif
