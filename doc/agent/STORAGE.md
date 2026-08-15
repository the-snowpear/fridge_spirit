# Agent TF 卡存储约定

Agent 相关的长文本、配置、技能、工作流、运行日志和本地业务数据优先放 TF 卡，减少 Flash 压力。API Key、Wi-Fi 凭据等敏感信息仍保存在 NVS，不写入 TF 卡明文。

## 根目录

```text
/sdcard/agent/
├── config/
├── data/
├── MEMORY.md
├── skills/
├── workflows/
├── runtime/
├── cache/
├── logs/
└── docs/
```

## 当前默认文件

```text
agent/config/agent.json
agent/config/persona.json
agent/config/providers.json
agent/config/mcp_servers.json
agent/data/inventory.json
agent/data/tasks.json
agent/data/memory.json
agent/data/sessions.json
agent/MEMORY.md
agent/docs/*.md
```

新增阶段继续使用：

```text
agent/skills/**/manifest.json
agent/skills/**/SKILL.md
agent/runtime/active_run.json
agent/logs/runs.jsonl
```

## 写入边界

- 库存数据：`agent/data/inventory.json`，由 `inventory.*` 工具维护。
- 本地任务：`agent/data/tasks.json`，由 `task.*` 工具维护。
- 结构化记忆：`agent/data/memory.json`，由 `memory.*` 工具维护。
- 长期 Markdown 记忆：`agent/MEMORY.md`，由 `memory_md.*` 工具维护。
- Skill 文档：`agent/skills/`，由 `skill_manager` 种子补齐；用户可在 TF 卡上编辑。
- Workflow 检查点：`agent/runtime/active_run.json`，由 `workflow_engine` 覆盖写入最近一次状态。
- Run Trace：`agent/logs/runs.jsonl`，由 `trace_service` 有界追加。

## 不应放 TF 卡明文的内容

- Wi-Fi 密码。
- AI / ASR / TTS API Key。
- 未来外部 MCP Server 的 bearer token。
- 未经用户确认的完整隐私长文本。

## 启动顺序依赖

```text
fridge_tfcard_init()
  → fridge_agent_storage_init()
  → fridge_trace_service_init()
  → fridge_tool_broker_init()
  → fridge_skill_manager_init()
  → fridge_workflow_engine_init()
  → fridge_agent_runtime_init()
```

TF 卡未挂载时，Agent 存储、Skill、Workflow 和业务写入工具都不应被视为可用。
