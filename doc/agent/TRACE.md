# Trace 与演示日志约定

Trace 用于比赛演示和调试，证明设备不是简单聊天转发，而是经历了 Router、Skill、Tool、Workflow 和本地数据读取。

## 当前 Trace 落盘（2026-07-18）

已新增 `components/trace_service`，`components/agent_runtime` 每次 Run 完成后会把内联 `trace` JSON 追加到 `/sdcard/agent/logs/runs.jsonl`。Trace 写入失败只记录 warning，不会把已经完成的业务 Run 判定为失败。

`components/workflow_engine` 当前也会把 Workflow 结果摘要追加到同一个 `runs.jsonl`，便于串口演示看到工作流执行结果。

日志采用有界策略：

- 当前文件：`/sdcard/agent/logs/runs.jsonl`
- 上一份轮转文件：`/sdcard/agent/logs/runs.prev.jsonl`
- 当前上限：64KB
- 超限后覆盖上一份 `runs.prev.jsonl`，避免 TF 卡日志无限增长。

USB 调试命令：

```json
{"command":"agent_trace_status","request_id":"trace-1"}
{"command":"agent_trace_read","request_id":"trace-2"}
```

`agent_trace_read` 返回当前 `runs.jsonl` 尾部最多 4096 字节，便于 Web/串口查看最近 Run。

## Workflow 检查点

Workflow 的运行中状态不写入 `runs.jsonl` 的每一步，而是覆盖写入：

```text
/sdcard/agent/runtime/active_run.json
```

当前会记录：

- `run_id`
- `workflow_id`
- `step`
- `state`
- `tool`
- `summary`
- `at_ms`

该文件用于断电/失败后查看最后一步；长期演示日志仍看 `runs.jsonl`。

## 当前最小 Agent Trace 示例

```json
{
  "run_id": "run_000001",
  "state": "COMPLETED",
  "input_preview": "还有什么食材？",
  "intent": "inventory_query",
  "tool": "inventory.list",
  "tool_status": "ok",
  "tool_code": "SUCCESS",
  "latency_ms": 4
}
```

## 日志文件规划

```text
/sdcard/agent/logs/runs.jsonl
/sdcard/agent/logs/tools.jsonl
/sdcard/agent/logs/mcp.jsonl
/sdcard/agent/logs/system.log
```

当前只实现 `runs.jsonl`；`tools.jsonl` 和 `mcp.jsonl` 后续再扩展。MCP 当前按用户决策暂缓。

## 不记录内容

- API Key 明文。
- Wi-Fi 密码。
- 完整图片原图。
- 用户隐私长文本的无界复制。

## 演示重点

- Agent 使用了本地真实库存。
- Agent 读取了家庭记忆并处理偏好冲突。
- Agent 通过 Tool Broker 执行副作用。
- Workflow 在副作用前后写入检查点。
- 断网时仍能执行本地固定命令和查看库存。
