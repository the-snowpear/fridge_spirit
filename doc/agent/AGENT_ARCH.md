# Agent Runtime 总体架构

冰箱小精灵比赛版 Agent 运行在 ESP32-P4 上。云端模型、ASR、TTS、视觉和外部 MCP 只作为被设备调用的能力；最终状态、工具执行权、数据写入和日志记录都由开发板掌握。

## 运行链路

```text
输入事件（USB/语音/后续UI）
  → Router（安全命令/固定命令/Workflow/Agent）
  → Skill Manager（关键词匹配与 SKILL.md 读取）
  → Context Composer（Persona + Memory + Skill + Tool Schema + 当前输入，后续实现）
  → Model Adapter（OpenAI-compatible 或后续 Provider，后续接入）
  → Tool Broker（校验、权限、执行、Trace）
  → Workflow Engine（确定性副作用流程与检查点）
  → 最终回复 / 澄清问题 / 等待确认
```

## 组件落地顺序

1. `agent_storage`：TF 卡目录、默认配置、默认文档和状态查询。已完成。
2. `tool_system`：Tool Registry、Tool Broker、ToolResult、权限和业务写入工具。已完成最小版。
3. `agent_runtime`：最小本地 Run Manager、关键词 Router、Tool Broker 调用和本地回复。已完成。
4. `trace_service`：有界 `runs.jsonl` 落盘和 USB Trace 读取。已完成。
5. `skill_manager`：默认 Skill 种子、列表、读取和关键词匹配。已完成最小版。
6. `workflow_engine`：确定性模板、Tool Broker 调用和 `active_run.json` 检查点。已完成最小版。
7. `agent_router`：后续扩展为本地命令、Intent 规则和 Skill 选择。
8. `context_composer`：上下文预算、Persona、Memory、Tool Schema 裁剪。未开始。
9. `mcp_client`：按 2026-07-18 用户决策暂缓，不进入近期实现范围。

## 当前实现边界（2026-07-18）

当前阶段不修改硬件引脚、不改变音频/摄像头/屏幕时钟、不增加网络依赖。新增内容只操作 TF 卡 `/sdcard/agent/` 下的数据、文档、Skill、Workflow 检查点和 Trace。

## 秒表 Agent Tool 接入边界（2026-07-24）

Tool Broker 已注册 `stopwatch.status`、`stopwatch.start`、`stopwatch.pause`、`stopwatch.lap` 和 `stopwatch.reset`，以及已有的 `ui.switch_page`。这些工具调用 `kitchen_tools` 的线程安全 API；Tool 执行路径不直接调用 LVGL，秒表页由 `ui_p4` 任务读取厨房工具快照刷新。

- `stopwatch.status` 为 READ，返回 `elapsedMs`、`lapCount` 和 `lapsMs`，秒表状态仅存在于当前运行周期。
- `stopwatch.start`、`stopwatch.pause`、`stopwatch.lap` 为 WRITE；开始/暂停做状态感知，计次使用幂等键缓存防止请求重试重复计次。
- `stopwatch.reset` 为 DANGEROUS，清空毫秒数和计次，必须显式用户确认。
- `task.create_timer` 仍表示持久化倒计时任务，不与秒表运行态混用。
- 当前 `agent_run` 本地关键词 Router 尚未把自然语言自动路由到秒表工具；后续 Model Adapter/Context Composer 接入时，应优先注入这组 Tool Schema。

## 最小 Agent Runtime

当前本地闭环：

```text
USB agent_run 文本输入
  → 关键词 Router
  → intent 选择本地 Tool
  → Tool Broker 调用 inventory.list / memory_md.read / device.state.get
  → 生成本地回复
  → Trace Service 追加 runs.jsonl
```

当前支持意图：

- `inventory_query`：关键词 `库存`、`食材`、`还有`、`临期`，调用 `inventory.list`。
- `memory_read`：关键词 `记忆`、`偏好`、`MEMORY`、`memory`，调用 `memory_md.read`。
- `device_state`：关键词 `状态`、`设备`、`内存`、`诊断`，调用 `device.state.get`。
- `help`：关键词 `帮助`、`help`、`能做什么`、`命令`，返回本地帮助。

## 新增最小 Workflow Engine

`components/workflow_engine` 当前只执行内置模板，不执行任意脚本、递归或无限循环。所有副作用步骤仍走 Tool Broker。

当前模板：

- `inventory.edit_confirm` → `inventory.update`
- `task.create_timer` → `task.create_timer`
- `memory.preference_confirm` → `memory.add`
- `memory_md.append_confirm` → `memory_md.append`
- `device.self_check` → `device.state.get`

检查点文件：

```text
/sdcard/agent/runtime/active_run.json
```

写入节点：开始、等待确认、副作用工具前、副作用工具后、完成或失败。

## 新增最小 Skill Manager

`components/skill_manager` 会在 TF 卡补齐默认 Skill 文档：

- `kitchen.inventory_query`
- `kitchen.expiring_meal`
- `kitchen.cooking_timer`
- `memory.preference_confirm`
- `system.device_help`

当前只做本地 catalog 和关键词匹配；后续 Context Composer 接入后，可读取 `manifest.json` 与 `SKILL.md` 注入模型上下文。

## USB 调试命令

```json
{"command":"agent_status","request_id":"agent-1"}
{"command":"agent_run","request_id":"agent-2","input":"还有什么食材？"}
{"command":"tool_list","request_id":"tool-1"}
{"command":"skill_status","request_id":"skill-1"}
{"command":"skill_list","request_id":"skill-2"}
{"command":"skill_read","request_id":"skill-3","skillId":"kitchen.expiring_meal"}
{"command":"skill_match","request_id":"skill-4","input":"今晚吃什么，优先用掉临期食材"}
{"command":"workflow_status","request_id":"wf-1"}
{"command":"workflow_start","request_id":"wf-2","workflowId":"device.self_check","arguments":{}}
```
