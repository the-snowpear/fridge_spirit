# Workflow 设计约定

Workflow 是本地确定性流程，不负责自由推理，不执行任意脚本。它适合拍照登记、库存编辑确认、计时器创建、记忆确认等必须稳定复现的链路。

## 比赛版步骤类型

长期目标支持：

- `tool`：调用 Tool Broker。
- `set`：设置局部变量。
- `if` / `switch`：条件分支。
- `wait`：等待用户确认或取消事件。
- `delay`：短延迟。
- `retry`：有界重试。
- `return`：成功结束。
- `fail`：失败结束。

## 禁止内容

- 任意 C/Python/Lua/JavaScript 脚本。
- 递归和无限循环。
- 绕过 Tool Broker 直接写文件或控制硬件。

## 检查点

以下节点必须写入 `/sdcard/agent/runtime/active_run.json`：

1. Workflow 开始。
2. 副作用工具执行前。
3. 副作用工具执行后。
4. 等待用户确认。
5. Workflow 完成或失败。

## 当前最小实现（2026-07-18）

已新增 `components/workflow_engine`。当前实现不是完整 JSON Workflow 解释器，而是确定性内置模板，目的是先跑通：

```text
USB workflow_start
  → 查找内置 workflow_id
  → 必要时等待 confirmed=true
  → 写 active_run.json 检查点
  → 通过 Tool Broker 执行工具
  → 写完成/失败检查点
  → 追加 runs.jsonl Trace
```

当前模板：

| Workflow ID | Tool | 确认策略 |
| --- | --- | --- |
| `inventory.edit_confirm` | `inventory.update` | 需要 `confirmed=true` |
| `task.create_timer` | `task.create_timer` | 遵循 Tool Broker 权限；建议确认后执行 |
| `memory.preference_confirm` | `memory.add` | 需要 `confirmed=true` |
| `memory_md.append_confirm` | `memory_md.append` | 需要 `confirmed=true` |
| `device.self_check` | `device.state.get` | 只读，可直接执行 |

检查点文件：

```text
/sdcard/agent/runtime/active_run.json
```

该文件只保存最近一次 Workflow 的运行状态，不作为长期日志；长期演示时间线仍看 `/sdcard/agent/logs/runs.jsonl`。

## USB 调试命令

```json
{"command":"workflow_status","request_id":"wf-1"}
{"command":"workflow_start","request_id":"wf-2","workflowId":"device.self_check","arguments":{}}
{"command":"workflow_start","request_id":"wf-3","workflowId":"task.create_timer","arguments":{"title":"蒸蛋计时","durationSeconds":600},"confirmed":true}
{"command":"workflow_start","request_id":"wf-4","workflowId":"memory.preference_confirm","arguments":{"text":"家里晚餐偏好少盐。","type":"preference"},"confirmed":true}
```

## 后续扩展

后续若实现 TF 卡 JSON Workflow 解释器，应先支持有限步骤数、无递归、无任意脚本，并把所有副作用步骤继续交给 Tool Broker。JSON Workflow 文件建议放：

```text
/sdcard/agent/workflows/kitchen/*.json
/sdcard/agent/workflows/task/*.json
/sdcard/agent/workflows/memory/*.json
```
