# Skill 设计约定

Skill 负责告诉 Agent 在什么场景下、为什么、怎样使用 Tool 和 Workflow。比赛版重点实现厨房场景 Domain Skill。

## TF 卡目录

```text
/sdcard/agent/skills/
├── system/
├── kitchen/
├── user/
└── drafts/
```

每个 Skill 一个目录：

```text
manifest.json
SKILL.md
```

## manifest.json 建议字段

```json
{
  "skill_id": "kitchen.expiring_meal",
  "name": "临期食材晚饭推荐",
  "version": "1.0.0",
  "description": "根据临期库存和家庭饮食约束推荐晚饭。",
  "triggers": {"keywords": ["快过期", "今晚吃什么", "优先用掉"]},
  "required_tools": ["inventory.list_expiring", "memory.search"],
  "optional_tools": ["external.recipe.search"],
  "execution_mode": "agent_guided",
  "max_steps": 6
}
```

## SKILL.md 章节

- Goal
- Use When
- Do Not Use When
- Required Information
- Procedure
- Conflict Handling
- Failure Handling
- Completion Criteria

## 当前最小实现（2026-07-18）

已新增 `components/skill_manager`，当前能力：

1. 启动时在 TF 卡补齐默认 Skill 的 `manifest.json` 与 `SKILL.md`，不覆盖用户已修改文件。
2. 保持一个固件内置轻量 catalog，用于 USB 列表和关键词匹配。
3. 支持按 `skillId` 读取对应 `SKILL.md`，后续可给 Context Composer 使用。
4. 不调用模型、不联网、不直接执行 Tool；Tool 执行仍由 Agent Runtime 或 Workflow Engine 发起。

默认 Skill：

| Skill ID | 作用 | 典型工具/工作流 |
| --- | --- | --- |
| `kitchen.inventory_query` | 查询库存 | `inventory.list` / `inventory.list_expiring` |
| `kitchen.expiring_meal` | 临期食材晚饭推荐 | `inventory.list_expiring` + `memory.search` |
| `kitchen.cooking_timer` | 厨房计时 | `workflow task.create_timer` |
| `memory.preference_confirm` | 偏好记忆确认 | `memory.add` / `memory_md.append` |
| `system.device_help` | 设备状态和帮助 | `device.state.get` |

## USB 调试命令

```json
{"command":"skill_status","request_id":"skill-1"}
{"command":"skill_list","request_id":"skill-2"}
{"command":"skill_read","request_id":"skill-3","skillId":"kitchen.expiring_meal"}
{"command":"skill_match","request_id":"skill-4","input":"今晚优先用掉快过期食材"}
```

## 后续扩展原则

- Skill 文档应尽量放 TF 卡，避免把长提示词烧进 Flash。
- `manifest.json` 只放可裁剪的元信息；大段流程说明放 `SKILL.md`。
- Skill 只能指导 Agent 选择 Tool/Workflow，不能绕过 Tool Broker 直接写文件或控制硬件。
- 涉及库存、记忆、提醒等副作用时，必须说明确认条件和失败回滚策略。
