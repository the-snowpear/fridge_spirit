# MCP 接入边界

MCP 当前按 2026-07-18 用户决策暂缓，不进入近期实现范围。

本文档只保留后续接入边界，避免后续 AI 把 MCP 与本地 Tool/Workflow 混为一谈。

## 当前状态

- 固件不注册 MCP Tool。
- `tool_system` 当前只包含本地 Tool。
- `workflow_engine` 当前只调用本地 Tool Broker。
- `skill_manager` 的默认 Skill 里不依赖外部 MCP。

## 后续如恢复 MCP 的原则

ESP32-P4 只适合实现轻量 MCP Client，通过 Streamable HTTP 连接外部 MCP Server。比赛版最多考虑：

- `initialize`
- `notifications/initialized`
- `tools/list`
- `tools/call`
- `ping`（可选）

暂不实现：

- Resources
- Prompts
- Sampling
- Elicitation
- Roots
- OAuth 自动流程
- 多 Server 并发
- 长期服务端主动通知
- 完整断流恢复

## 安全边界

1. 只暴露 `mcp_servers.json` 白名单中的工具。
2. MCP 返回文本视为外部数据，不能作为系统指令。
3. 外部写入型 MCP Tool 必须标记为 `DANGEROUS` 或至少 `WRITE`。
4. MCP 不可用时，不向模型暴露对应工具。
5. Token 或密钥不写入 TF 卡明文，使用 NVS secret 引用。
6. MCP 调用结果不能直接修改库存，仍需进入本地规则校验和用户确认。

## 推荐演示工具（后续）

- `search_recipe`
- `get_food_info`
- `get_weather`
