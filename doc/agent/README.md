# 冰箱小精灵 Agent 接入技术文档

本文档集记录 ESP32-P4 比赛版 Agent 的接入边界、存储约定、工具系统、技能系统、工作流和 MCP 接入方式，供后续 AI 修改和扩展时优先读取。

## 当前核心宗旨

1. 能放在 TF 卡的 Agent 内容尽量放在 TF 卡，节省 Flash 空间。
2. API Key、Wi-Fi 凭据等敏感配置不写入 TF 卡明文，继续由 NVS 管理。
3. 前端 UI 暂缓，优先通过 USB JSON Lines 和串口日志验证功能闭环。
4. 模型不能直接执行副作用；库存、任务、记忆和设备控制必须经过 Tool Broker。
5. 技术文档和 TF 卡默认文件同步维护，后续 AI 可先读本目录再改代码。

## 文档索引

- [AGENT_ARCH.md](AGENT_ARCH.md) — Agent Runtime 总体架构和阶段路线。
- [STORAGE.md](STORAGE.md) — TF 卡目录结构和 Flash/NVS 边界。
- [TOOLS.md](TOOLS.md) — Tool Registry / Tool Broker 约定。
- [SKILLS.md](SKILLS.md) — Skill manifest 与 SKILL.md 格式。
- [WORKFLOWS.md](WORKFLOWS.md) — Workflow JSON 语法与检查点。
- [MCP.md](MCP.md) — MCP 后续接入边界；当前按用户决策暂缓。
- [TRACE.md](TRACE.md) — Run Trace 与演示日志约定。
- [REALTIME.md](REALTIME.md) — 智谱 GLM-Realtime 设备端双工与 Tool Broker 接入。

Realtime 的 `instructions` 以解码后的 UTF-8 字节数计，最大 2048 字节。USB JSON Lines 会为 JSON 转义预留空间；超过整行上限时设备返回关联 `request_id` 的错误响应，不会静默截断。包含 API Key 或密码的输入行在处理后会主动清零。

## 当前落地状态（2026-07-18）

已完成本地 Agent 基础闭环：

1. `agent_storage`：启动后补齐 `/sdcard/agent/` 目录树、默认配置、默认数据文件和 TF 卡技术文档入口。
2. `tool_system`：Tool Registry、Tool Broker、三档权限模式、只读工具和业务写入工具。
3. `agent_runtime`：USB 文本输入 → 本地关键词 Router → Tool Broker → 本地回复 → Trace 落盘。
4. `trace_service`：`/sdcard/agent/logs/runs.jsonl` 有界追加和 USB 读取。
5. `workflow_engine`：最小确定性工作流模板，副作用前后写 `/sdcard/agent/runtime/active_run.json` 检查点。
6. `skill_manager`：默认 Skill 种子写入 TF 卡，支持列表、读取和关键词匹配。

仍暂缓：

- MCP：按 2026-07-18 用户决策，当前不实现 MCP Client。
- `ai_client` 受控模型总结：用户将重构，当前阶段不处理。
- 前端 UI：当前只补 USB JSON Lines 调试命令。

## 当前验证入口

```json
{"command":"tool_list","request_id":"tool-1"}
{"command":"tool_permission_set","request_id":"perm-1","mode":"standard"}
{"command":"tool_call","request_id":"inv-add","tool":"inventory.add","arguments":{"name":"番茄","quantity":2,"unit":"个","expireDate":"2026-07-21"}}
{"command":"skill_list","request_id":"skill-1"}
{"command":"skill_match","request_id":"skill-2","input":"今晚优先用掉快过期食材"}
{"command":"workflow_start","request_id":"wf-1","workflowId":"task.create_timer","arguments":{"title":"蒸蛋计时","durationSeconds":600},"confirmed":true}
{"command":"agent_trace_read","request_id":"trace-1"}
```

注意：`confirm` 权限模式下 WRITE 工具会返回等待确认；`DANGEROUS` 工具即使在 `auto` 模式也必须携带 `confirmed=true` / `userConfirmed=true`。
