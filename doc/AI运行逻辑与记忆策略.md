# AI 运行逻辑与记忆策略

本文记录 P4 主线唯一有效的 Agent 记忆与短期会话策略。旧 LittleFS
`/cache/memory_summary.json`、NVS `fridge_store/memory` 和回复尾部
`MEMORY_OP` 协议已经停用，不得再作为新功能的数据源。

## 1. 两类长期数据

- 用户长期记忆：TF 卡 `/sdcard/agent/data/memory.json`，保存用户偏好、家庭事实等结构化数据，也是默认上下文的唯一长期记忆来源。
- 人工记忆笔记：TF 卡 `/sdcard/agent/MEMORY.md`，只保存人工维护的家庭笔记，不承载系统规则，也不默认注入模型。
- 写入统一通过 Tool Broker 的 `memory.add` / `memory_md.append`；结构化记忆可通过 `memory.delete` 按 id 删除。
- 检索分别使用 `memory.search` / `memory_md.read`，两类结果不得混为一谈。
- 旧 LittleFS 和 NVS 数据不自动迁移，因为旧默认文件包含演示偏好；确需保留时必须先导出并由用户逐条确认。

Agent Storage 对结构化 JSON 使用递归事务锁、`.part/.bak` 可恢复写入、写后读回解析和字节校验。API Key 与 Wi-Fi 凭据仍保存在 NVS，不能写入 TF 记忆或日志。

## 2. 默认上下文注入

每次云模型调用前，Runtime 都构造 `user_memory`：

- 最近最多 16 条结构化记忆；
- 不读取或注入 `MEMORY.md`；
- 超出缓冲时优先移除最旧结构化项；
- `truncated=true` 表示还有未注入的结构化记忆，模型需要时调用 `memory.search`。

默认注入是只读行为，不生成 Tool 卡，也不改变用户设置的 Tool Permission Mode。读取失败时注入带 `unavailable=true` 的空上下文，不能阻断普通对话。

## 3. 短期会话

- 不再使用 `session_ttl_hours`。
- 用户问题算一条消息，Agent 最终回答算一条消息。
- 最多保留 24 条 user/assistant 消息，通常相当于 12 轮问答。
- Tool、Tool Result 和状态卡不占 24 条对话额度。
- 淘汰一条旧对话时，同一 `run_id` 的旧 Tool/Result 一并移除，避免孤立工具卡。
- 云模型历史只注入这 24 条 user/assistant 消息，不注入 Tool/Result。

会话仍持久化在 `/sdcard/agent/data/sessions.json`；活动 Run 和待确认 Tool 使用独立检查点，设备重启后不会自动重放有副作用的请求。

## 4. 用户询问记忆

普通“记忆/偏好”意图直接进入模型，根据本次已经注入的 `user_memory.items` 回答，不再重复读取 TF 文件。`truncated=false` 时模型不得调用 `memory.search`；只有 `truncated=true` 且所需事实不在已注入项中，或用户明确要求全部、更早、历史、搜索、查找、最早或指定第几条记忆时才检索结构化文件。用户明确提到 `MEMORY.md`、“记忆笔记”或“家庭笔记”时仍执行 `memory_md.read`。工具的真实 `data.items` / `data.content` 会进入当前 Run 的可信 Tool Result，再由模型组织用户可见回答；不得把工具摘要或内部文件路径当作答案。

## 5. 兼容边界

- `components/storage` 继续负责库存、提醒、旧短期历史等非长期记忆数据，不能整个删除。
- 旧 `/cache/memory_summary.json` 和 NVS key 本次不物理删除，只是不再读取或写入，避免升级时误删用户数据。
- Web Mock 和旧 USB 源码不再暴露 `get/set/clear_memory_summary`。
- 未经用户修改、且逐字等于旧内置模板的 `MEMORY.md` 会迁移为中性笔记模板；任何被用户编辑过的文件都原样保留。
- `doc/progress.md` 中的旧 `MEMORY_OP` 条目属于历史记录，不代表当前实现。
