# Tool System 设计约定

Tool 是 Agent 能调用的最小原子能力，命名采用 `namespace.action`。模型只能提出 Tool Call，不能直接改 TF 卡、NVS 或硬件状态。

## 权限等级

工具自身权限分三类：

- `READ`：只读工具，默认可自动执行。
- `WRITE`：修改本地数据，是否自动执行由 Agent 权限模式决定。
- `DANGEROUS`：删除、清空、恢复出厂、修改密钥、外部写入型工具等，必须明确确认。

Agent 权限模式三档都需要实现并长期保留：

| 模式 | READ | WRITE | DANGEROUS |
| --- | --- | --- | --- |
| `confirm` | 自动执行 | 等待确认 | 等待确认 |
| `standard` | 自动执行 | 自动执行 | 等待确认 |
| `auto` | 自动执行 | 自动执行 | 仍需显式确认 |

即使在 `auto` 模式，`DANGEROUS` 也不自动执行，避免误删库存、清空记忆、恢复出厂或修改密钥。若调用方已经完成确认，可以在 ToolCall 或 USB 命令中携带 `confirmed=true` / `userConfirmed=true`。

MCP 已按当前决策暂缓，不进入近期实现范围。

## Tool Broker 流程

```text
模型或 USB 提出 Tool Call
  → 工具存在性检查
  → 参数 Schema 说明与基础 JSON 校验
  → 权限检查
  → 必要时返回 pending_confirmation
  → 执行工具
  → 生成 ToolResult
  → 返回模型/USB/Workflow
```

## 当前组件

- `fridge_tool_types.h`：Tool 定义、ToolCall、ToolResult、权限枚举。
- `fridge_tool_registry.h`：固定数组注册表，避免长期运行时碎片化。
- `fridge_tool_broker.h`：统一调用入口，执行权限判断。
- `fridge_builtin_tools.h` / `builtin_tools.c`：内置只读工具。
- `fridge_business_tools.h` / `business_tools.c`：库存、任务、记忆写入工具。

## 已注册工具（2026-07-24）

### 只读工具

- `inventory.list`：读取 `/sdcard/agent/data/inventory.json`。
- `inventory.list_expiring`：读取指定天数内的临期库存，参数 `days` 默认 7。
- `task.list`：读取 `/sdcard/agent/data/tasks.json`。
- `memory.search`：搜索 `/sdcard/agent/data/memory.json`。
- `memory_md.read`：读取 `/sdcard/agent/MEMORY.md`。
- `device.state.get`：读取诊断组件的设备状态和 Agent 存储 ready 状态。
- `stopwatch.status`：读取秒表状态、实时毫秒数和最多 20 条计次记录。
- `ui.switch_page`：异步切换 P4 UI 页面；当前支持 `home`、`home_detail`、`more`、`timer`、`alarm`、`stopwatch`、`camera`、`plan`、`chat`。其中 `home_detail` 对应 HOME-02 今日摘要详情。
- `ui.set_orientation`：异步切换 `landscape` 横屏标准或 `portrait_privacy` 竖屏隐私；当前只有 HOME-02 完成竖屏布局，进入竖屏前必须先切到 `home_detail`。竖屏隐私态会拒绝摄像头入口。竖屏物理输出按设备实际安装方向逆时针旋转 90°，触摸坐标同步反变换。

### 写入工具

- `inventory.add`：新增库存条目，写入 `/sdcard/agent/data/inventory.json`。
- `inventory.update`：按 `id` 更新库存条目。
- `task.create_timer`：创建本地计时任务记录。
- `task.create_reminder`：创建本地提醒任务记录。
- `task.cancel`：把任务状态改为 `canceled`。
- `memory.add`：写入结构化家庭记忆 `/sdcard/agent/data/memory.json`。
- `memory_md.append`：追加长期记忆到 `/sdcard/agent/MEMORY.md`。
- `stopwatch.start`：开始或继续运行态秒表，不清空已有记录。
- `stopwatch.pause`：暂停秒表，保留毫秒数和计次记录。
- `stopwatch.lap`：记录当前计次；通过幂等键避免 Agent/USB 重试重复插入。

### 危险工具

- `inventory.delete`：按 `id` 删除库存条目，必须显式确认。
- `stopwatch.reset`：清空秒表毫秒数和全部计次记录，必须显式确认。

### 秒表工具契约

秒表工具直接复用 `kitchen_tools` 的运行态 API，Tool 执行层不访问 LVGL；页面由 `ui_p4` 任务读取同一份快照后刷新。秒表计时不是 TF 卡持久化任务，设备重启后回到 `idle`，不能把 `task.create_timer` 当作秒表状态恢复接口。

`stopwatch.status` 及所有操作结果使用以下毫秒字段：

```json
{
  "state": "idle|running|paused",
  "running": true,
  "elapsedMs": 12345,
  "lapCount": 2,
  "maxLaps": 20,
  "lapsMs": [1200, 12345]
}
```

`stopwatch.start` 是状态感知操作：`idle` 时开始、`paused` 时继续、`running` 时返回当前状态。`stopwatch.pause` 在已经暂停时返回成功的当前状态；未开始时返回 `STOPWATCH_NOT_RUNNING`。`stopwatch.lap` 允许在运行或暂停且已有毫秒数时执行，最新计次位于 `lapsMs[0]`；达到 20 条后会丢弃最旧记录并返回 `droppedOldest=true`。

`stopwatch.lap` 的 `idempotencyKey` 应对每一次逻辑计次保持唯一且在重试时保持不变；如果未显式传入，Tool Broker 会使用稳定的 `call_id` 作为兜底。通过 `stopwatch.reset` 复位会清理本次运行的计次幂等缓存；如果用户从页面手动复位，Tool 层会在发现计次数量回退时使旧条目失效，但新会话仍应使用新的幂等键。

现有 `fridge_kitchen_tools_execute_ai_json()` 的 `tool=stopwatch` JSON 入口继续作为旧 AI 动作兼容层；Agent Tool Broker 的规范入口是上述 `stopwatch.*` 工具。当前本地 `agent_run` 关键词 Router 仍是库存/记忆/设备状态示例，尚未自动把自然语言路由到秒表工具。

## 幂等要求

`inventory.add`、`task.create_timer`、`task.create_reminder` 等创建类工具支持 `idempotency_key` 或 `idempotencyKey`，避免重启或模型重试造成重复写入。USB 调试也可在顶层携带 `idempotencyKey`。

## USB 调试命令

```json
{"command":"tool_list","request_id":"tool-1"}
{"command":"tool_permission_status","request_id":"perm-1"}
{"command":"tool_permission_set","request_id":"perm-2","mode":"standard"}
{"command":"tool_call","request_id":"tool-2","tool":"inventory.list"}
{"command":"tool_call","request_id":"tool-3","tool":"inventory.add","arguments":{"name":"番茄","quantity":2,"unit":"个","expireDate":"2026-07-21"},"idempotencyKey":"demo-tomato-1"}
{"command":"tool_call","request_id":"tool-4","tool":"inventory.delete","arguments":{"id":"food_xxx"},"confirmed":true}
{"command":"tool_call","request_id":"tool-5","tool":"memory_md.append","arguments":{"title":"口味偏好","text":"家里晚餐偏好少盐。"}}
{"command":"tool_call","request_id":"sw-status-1","tool":"stopwatch.status","arguments":{}}
{"command":"tool_call","request_id":"sw-start-1","tool":"stopwatch.start","arguments":{}}
{"command":"tool_call","request_id":"sw-lap-1","tool":"stopwatch.lap","arguments":{},"idempotencyKey":"cooking-session-1-lap-1"}
{"command":"tool_call","request_id":"sw-reset-1","tool":"stopwatch.reset","arguments":{},"confirmed":true}
{"command":"tool_call","request_id":"ui-page-1","tool":"ui.switch_page","arguments":{"page":"timer"}}
{"command":"tool_call","request_id":"ui-home02-1","tool":"ui.switch_page","arguments":{"page":"home_detail"}}
{"command":"tool_call","request_id":"ui-portrait-1","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
{"command":"tool_call","request_id":"ui-landscape-1","tool":"ui.set_orientation","arguments":{"orientation":"landscape"}}
```

`confirm` 模式下，WRITE 工具会返回 `pending_confirmation`；可切到 `standard` 或在确认后重发调用。删除类 `DANGEROUS` 工具必须携带确认字段。

`ui.switch_page` 是只改变显示页面的 READ 工具，调用成功表示请求已进入 UI 队列，页面对象会由 UI 任务异步更新。秒表的开始、暂停和计次属于 WRITE；复位属于 DANGEROUS，即使 Agent 处于 `auto` 模式也必须显式确认。

`ui.set_orientation` 同样是 READ 工具，`queued=true` 仅表示切换已入队。验收必须继续轮询 `ui_status`，连续两次满足 `ready/resourceReady/framebuffersSynced/pageFrameReady=true`，并核对 `page`、`orientation` 与 `privacyMode`。竖屏 HOME-02 的期望值为：

```json
{
  "page": "home_detail",
  "orientation": "portrait_privacy",
  "privacyMode": true
}
```

竖屏 HOME-02 的 `600×585` 主滚动视口已接入 PPA 快照加速。USB 切换方向后，必须等稳定门通过并在日志中看到 `PPA scroll rebound profile=portrait` 与 `cache ready 600x1668`，才能把该次启动判定为竖屏性能路径就绪；仅看到 `orientation=portrait_privacy` 不代表滚动缓存已经建立。
