# P4 Agent MQTT 公网多端同步与二维码绑定技术文档

> 文档状态：P4 v2 实施契约  
> 更新时间：2026-07-26  
> 适用范围：ESP32-P4 固件、Backend、小程序  
> 兼容性：**只支持当前 P4 Agent 架构，不兼容、不迁移、不订阅任何 S3 旧协议**

本文是小程序修改和三端联调的权威接口文档。字段名、Topic、状态值和错误码均区分大小写。除明确标为“可选”的字段外，各端不得自行增删或改名。

## 1. 当前阶段边界

### 1.1 联调地址

| 用途 | 地址 | 使用方 |
| --- | --- | --- |
| REST API Base URL | `http://165.154.23.36:6005` | 小程序、P4 |
| REST API v2 | `http://165.154.23.36:6005/api/v2` | 小程序、P4 |
| WebSocket | `ws://165.154.23.36:6005/api/v2/ws` | 小程序 |
| MQTT Broker | `mqtt://165.154.23.36:1883` | P4、Backend |

当前是比赛开发联调模式，明确使用明文 HTTP、WebSocket 和 MQTT。其后果包括：

- 同网段或链路上的第三方可能读取或篡改业务数据、JWT、设备 token、MQTT 密码和 AI Key。
- 不能将当前配置用于公开发布、真实用户数据或长期运行。
- 微信正式版小程序通常要求已备案合法域名和 HTTPS/WSS；IP + HTTP 只适用于开发者工具中关闭域名校验后的联调。
- 正式部署必须切换为 HTTPS、WSS、MQTTS，关闭匿名 MQTT，并轮换开发阶段所有凭据。

### 1.2 数据和传输边界

- P4 的 `/sdcard/agent`、Agent Storage、Tool Broker、Agent Runtime 是设备端唯一业务事实来源。
- 不读取 S3 legacy 文件，不发布旧 `fridge/{home}/{device}/...` Topic，不兼容 schema v1。
- 小程序不直连 MQTT。小程序写操作走 REST，实时更新走 WebSocket，断线补偿走 REST。
- MQTT 只承载命令、增量事件、ACK、状态和错误。
- 全量/大型快照走 HTTP；照片、录音、固件、字体、UI 资源等二进制媒体不走 MQTT。
- AI API Key 在当前测试模式允许由 Backend 通过非 retained MQTT 命令明文下发给设备，但不得出现在日志、ACK、普通 WebSocket 广播和 GET 响应中。

## 2. 总体架构

```text
┌──────────────────┐          REST + WebSocket          ┌──────────────────┐
│ 微信小程序        │◄──────────────────────────────────►│ Backend / DB     │
│ JWT 用户身份      │                                    │ 家庭与版本权威    │
└──────────────────┘                                    └────────┬─────────┘
                                                                  │
                                                  MQTT v2 + HTTP  │
                                                                  │
                                                        ┌─────────▼────────┐
                                                        │ ESP32-P4         │
                                                        │ P4 Agent / TF卡  │
                                                        │ Tool Broker      │
                                                        └──────────────────┘
```

数据权威规则：

1. 未绑定且云端家庭 `serverRevision=0` 时，允许 P4 上传一次初始种子。
2. 初始种子成功后，Backend 是跨设备同步版本权威；P4 仍是设备本地执行和离线可用权威。
3. P4 和小程序的修改都必须转化为带 `eventId`、`entityId`、`baseEntityRevision` 的同步事件。
4. Backend 事务性检查版本、持久化、提升 `serverRevision`，再向 MQTT 和 WebSocket 广播。
5. P4 应用远程事件时必须经过 Tool Broker 或对应的 P4 Agent 数据服务，禁止 MQTT 回调直接覆盖 TF 文件。

## 3. 通用约定

### 3.1 标识符与时间

- `homeId`、`messageId`、`requestId`、`sessionId`：UUID 字符串。
- `eventId`、`entityId`：长度不超过 128 的稳定字符串；小程序新建事件建议使用 UUID v4，设备首次种子会使用 `seed:...` 形式的稳定 ID。
- `deviceSn`：设备稳定序列号，例如 `FS-P4-000001`，同时是 MQTT Topic 段。
- MQTT 的 `sentAt` 使用 Unix 毫秒 UTC；REST 的 `expiresAt`、`generatedAt`、`createdAt` 使用 ISO 8601 UTC 字符串。
- 时区：IANA 名称，例如 `Asia/Shanghai`，禁止只保存 `UTC+8`。
- 日期：`YYYY-MM-DD`。
- 可空字段使用 `null`，不要用空字符串表达“无值”。

### 3.2 REST 通用响应

成功：

```json
{
  "ok": true,
  "data": {},
  "message": "ok",
  "requestId": null
}
```

业务代码主动返回的失败通常使用 FastAPI HTTP 状态和 `detail`：

```json
{
  "detail": "cloud already has P4 Agent data"
}
```

请求字段校验失败为 HTTP 422，`detail` 是 FastAPI 的校验错误数组。小程序请求封装必须同时兼容成功壳的 `message` 与失败响应的 `detail`，不能依赖不存在的 `serverTime/error.code`。

小程序用户接口使用：

```http
Authorization: Bearer <miniappJwt>
Content-Type: application/json
```

已绑定 P4 使用：

```http
Authorization: Device <deviceHttpToken>
Content-Type: application/json
```

未绑定 P4 创建二维码会话时把 Bootstrap 凭据放在 JSON body；轮询会话时使用 `X-Bootstrap-Key-Id`、`X-Bootstrap-Secret` 请求头。

### 3.3 增量游标

```json
{
  "serverRevision": 15,
  "latestServerRevision": 18,
  "hasMore": false
}
```

`GET /api/v2/sync/events?sinceRevision=N` 最多按 revision 升序返回 200 个事件。客户端把响应 `serverRevision` 作为下一页的 `sinceRevision`，直到 `hasMore=false`；完整快照使用独立的 `/sync/snapshot`。

## 4. 二维码绑定

### 4.1 设备预置

每台设备首次联网前通过 USB 管理端写入：

- `deviceSn`
- `bootstrapKeyId`
- 独立随机 `bootstrapSecret`
- `apiBaseUrl`

`bootstrapSecret` 每台设备必须不同，Backend 只保存其安全摘要。USB 写入响应只确认 `hasBootstrapSecret=true`；后续状态接口不回显 secret。

固件 USB JSON Lines 写入示例：

```json
{"command":"device_provision_set","request_id":"prov-001","apiBaseUrl":"http://165.154.23.36:6005","deviceSn":"FS-P4-000001","keyId":"bootstrap-FS-P4-000001","bootstrapSecret":"<provision 脚本输出的一次性明文>"}
```

可用 `device_binding_status`、`device_binding_refresh`、`mqtt_status` 查询脱敏状态。响应只返回 `hasBootstrapSecret/hasPassword`，不回显任何 secret。

### 4.2 二维码 grammar

二维码只包含短期一次性 claim token：

```text
fridgespirit://bind?v=2&sessionId=<uuid>&claimToken=<base64url-token>
```

固定规则：

- scheme 必须为 `fridgespirit`。
- host 必须为 `bind`。
- `v` 必须为字符串 `"2"`。
- `sessionId` 是本次会话 UUID。
- `claimToken` 是 Backend 生成的随机、不透明、Base64URL token。
- token 有效期 5 分钟，只能成功消费一次。
- 同一设备同一时刻只保留一个 pending 会话；创建新会话会使旧二维码失效。
- 二维码不得包含 MQTT 用户名、密码、设备 HTTP token、bootstrap secret、AI Key 或 Wi-Fi 密码。

### 4.3 绑定状态

```ts
type BindingSessionStatus =
  | "pending"
  | "claimed"
  | "acked"
  | "expired";
```

流程：

```text
P4 创建 session
  → P4 显示二维码并轮询 session
  → 已登录小程序扫码
  → 小程序 claim
  → Backend 创建/加入家庭并生成设备凭据
  → P4 轮询得到 claimed 和凭据
  → P4 原子写入 NVS
  → P4 启动 MQTT
  → P4 ack
  → Backend 标记 acked
```

首次绑定：

- 扫码用户有当前家庭：设备绑定到该家庭。
- 扫码用户没有家庭：Backend 创建家庭，再绑定设备。

已绑定设备生成的二维码用于“加入当前家庭”：

- 扫码用户加入设备现有家庭。
- 不转移设备，不删除已有成员，不改变设备凭据。
- 当前阶段不区分 Owner/Admin/Member；家庭成员业务读写权限相同。

### 4.4 创建绑定会话

```http
POST /api/v2/device-binding/sessions
```

未绑定设备使用 Bootstrap 鉴权；已绑定设备使用 Device 鉴权。

未绑定设备请求：

```json
{
  "deviceSn": "FS-P4-000001",
  "keyId": "competition-key-01",
  "bootstrapSecret": "设备预置密钥"
}
```

已绑定设备改传 `deviceToken`，也可使用 `Authorization: Device <deviceToken>`。

响应：

```json
{
  "ok": true,
  "data": {
    "sessionId": "uuid",
    "status": "pending",
    "qrPayload": "fridgespirit://bind?v=2&sessionId=uuid&claimToken=...",
    "expiresAt": "2026-07-26T12:05:00Z",
    "expiresInSeconds": 300,
    "pollAfterMs": 1500
  }
}
```

### 4.5 P4 查询会话

```http
GET /api/v2/device-binding/sessions/{sessionId}
  ?deviceSn=FS-P4-000001
X-Bootstrap-Key-Id: competition-key-01
X-Bootstrap-Secret: <设备预置密钥>
```

pending：

```json
{
  "ok": true,
  "data": {
    "sessionId": "uuid",
    "status": "pending",
    "expiresAt": "2026-07-26T12:05:00Z",
    "expiresInSeconds": 284,
    "pollAfterMs": 1500
  }
}
```

首次绑定 claimed：

```json
{
  "ok": true,
  "data": {
    "sessionId": "uuid",
    "status": "claimed",
    "homeId": "uuid",
    "bindingEpoch": 1,
    "deviceToken": "opaque-token",
    "mqtt": {
      "brokerUri": "mqtt://165.154.23.36:1883",
      "clientId": "p4-FS-P4-000001",
      "username": "p4-FS-P4-000001",
      "password": "opaque-password"
    }
  }
}
```

凭据只会返回给通过 Bootstrap 鉴权的目标设备，不会返回给小程序。

### 4.6 P4 确认凭据落盘

```http
POST /api/v2/device-binding/sessions/{sessionId}/ack
Authorization: Device <deviceToken>
```

请求：

```json
{
  "committed": true
}
```

只有 NVS 原子写入、首次 seed/云端快照落盘成功后才能发送。响应 `status=acked`；
设备在 ACK 前已可启动 MQTT，以免错过绑定期间产生的家庭事件。

### 4.7 小程序 claim

```http
POST /api/v2/bindings/claim
Authorization: Bearer <miniappJwt>
```

请求：

```json
{
  "sessionId": "<二维码 sessionId 参数>",
  "claimToken": "<二维码 claimToken 参数>"
}
```

响应不包含设备凭据：

```json
{
  "ok": true,
  "data": {
    "sessionId": "uuid",
    "status": "claimed",
    "homeId": "uuid",
    "bindingEpoch": 1,
    "expiresAt": "2026-07-26T12:05:00Z",
    "expiresInSeconds": 260,
    "qrPayload": null,
    "mqtt": null,
    "deviceToken": null
  }
}
```

幂等规则：

- 同一用户使用同一个已成功消费 token 重试，返回原成功结果。
- 不同用户使用已消费 token，返回 HTTP 409。
- token 过期返回 HTTP 410。
- 已绑定设备的 join token 被已有家庭成员扫描时，返回幂等成功。

## 5. MQTT v2 契约

### 5.1 固定 Topic

```text
fridge/v2/{homeId}/{deviceSn}/down/command
fridge/v2/{homeId}/{deviceSn}/up/event
fridge/v2/{homeId}/{deviceSn}/up/ack
fridge/v2/{homeId}/{deviceSn}/up/state
fridge/v2/{homeId}/{deviceSn}/up/error
```

| Topic | 方向 | QoS | Retained |
| --- | --- | --- | --- |
| `down/command` | Backend → P4 | 1 | 否 |
| `up/event` | P4 → Backend | 1 | 否 |
| `up/ack` | P4 → Backend | 1 | 否 |
| `up/state` | P4 → Backend | 1 | 是 |
| `up/error` | P4 → Backend | 1 | 否 |

P4 的 Last Will 发布到 `up/state`：

```json
{
  "schemaVersion": 2,
  "messageId": "uuid",
  "requestId": null,
  "homeId": "uuid",
  "deviceId": "FS-P4-000001",
  "bindingEpoch": 1,
  "sentAt": 1785000000000,
  "type": "device.state",
  "payload": {
    "online": false,
    "reason": "connection_lost"
  }
}
```

### 5.2 MQTT envelope

```ts
interface MqttEnvelope<T = unknown> {
  schemaVersion: 2;
  messageId: string;
  requestId: string | null;
  homeId: string;
  deviceId: string; // 当前实现中等于设备稳定序列号 deviceSn
  bindingEpoch: number;
  sentAt: number;
  type: MqttMessageType;
  payload: T;
}

type MqttMessageType =
  | "sync.apply"
  | "sync.events"
  | "sync.event"
  | "sync.ack"
  | "sync.request_snapshot"
  | "agent.tool_call"
  | "device.ping"
  | "device.state"
  | "command.ack"
  | "device.error";
```

验证顺序：

1. MQTT Topic 中的 `homeId/deviceSn` 必须分别与 envelope 的 `homeId/deviceId` 完全相同。
2. `schemaVersion` 必须等于 2。
3. `bindingEpoch` 必须等于当前有效绑定版本。
4. JSON UTF-8 总长度不得超过 65536 bytes。
5. `messageId` 必须有效且通过幂等检查。
6. 只有校验通过后才能入 worker 队列。

任何失败都不得执行 Tool。可报告的失败发布 `device.error`。

### 5.3 同步事件

```ts
type SyncDomain =
  | "inventory"
  | "reminders"
  | "timers"
  | "alarms"
  | "stopwatch"
  | "shopping"
  | "recipes"
  | "nutrition"
  | "agent_sessions"
  | "agent_memory"
  | "device_settings"
  | "ai_config";

type SyncOperation = "upsert" | "delete" | "append";

interface SyncEvent<T = unknown> {
  eventId: string;
  domain: SyncDomain;
  entityId: string;
  operation: SyncOperation;
  baseEntityRevision: number;
  entityRevision?: number;
  serverRevision?: number;
  data: T | null;
}
```

P4 上报：

```json
{
  "schemaVersion": 2,
  "messageId": "uuid",
  "requestId": null,
  "homeId": "uuid",
  "deviceId": "FS-P4-000001",
  "bindingEpoch": 1,
  "sentAt": 1785000000000,
  "type": "sync.event",
  "payload": {
    "eventId": "uuid",
    "domain": "inventory",
    "entityId": "uuid",
    "operation": "upsert",
    "baseEntityRevision": 3,
    "data": {}
  }
}
```

Backend 下发使用相同 `SyncEvent`，但补齐 `entityRevision` 和 `serverRevision`，envelope `type=sync.apply`。

当前固件的本地上行接点：

- Tool Broker 成功执行库存、提醒/计时任务、结构化记忆和秒表写工具后，自动读取已提交状态并发布 `sync.event`。
- Agent Runtime 每次成功持久化 foreground 会话后，自动以 `agent_sessions/foreground` 发布当前结构化对话历史。
- 首次种子成功后设备立即回拉权威快照，补齐本地 `entityRevision`，避免下一次本地修改仍以 revision 0 上报。
- `shopping/recipes/nutrition/device_settings/ai_config` 当前 P4 端没有独立本地编辑器；小程序/Backend 下发已接通。以后新增本地写入口时必须调用 `fridge_mqtt_publish_sync_event()`，不能只改 TF 文件。

### 5.4 命令 ACK

收到合法命令并完成设备端持久化或 Tool 执行后发送 ACK；`stage="completed"`，
`ok=false` 表示已处理但执行失败。QoS 1 的 PUBACK 不能替代业务 ACK。

```ts
interface CommandAck {
  command: string;
  stage: "accepted" | "rejected" | "completed";
  ok: boolean;
  status?: string;
  code?: string;       // Tool ACK
  errorCode?: string;  // 通用/同步 ACK
  message: string;
  localRevision?: number;
  serverRevision?: number;
  applied?: boolean;
}
```

ACK 的 envelope `requestId` 必须等于命令 envelope 的 `requestId`，Backend 必须同时校验 Topic、`homeId`、`deviceId`、`bindingEpoch` 和 `command`，不能只按 `requestId` 唤醒等待者。

## 6. 业务域 TypeScript 模型

### 6.1 公共实体

```ts
interface VersionedEntity {
  id: string;
  entityRevision: number;
  createdAt: number;
  updatedAt: number;
  deletedAt: number | null;
}

type EntitySource = "device" | "miniapp" | "agent" | "backend";
```

Backend 快照使用本节模型。P4 现有 snake_case、旧单调时钟字段由设备适配层转换，不直接暴露给小程序。

### 6.2 库存

```ts
interface InventoryItem extends VersionedEntity {
  name: string;
  quantity: number;
  unit: string;
  category: string | null;
  zone: string | null;
  slot: string | null;
  expireDate: string | null; // YYYY-MM-DD
  note: string | null;
  source: EntitySource;
  confidence: number | null; // 0..1
}
```

AI 识别结果不能直接创建该实体，必须经过规则校验和用户确认。

### 6.3 提醒

```ts
interface Reminder extends VersionedEntity {
  title: string;
  note: string | null;
  dueAt: number;
  timezone: string;
  status: "pending" | "triggered" | "completed" | "cancelled";
  source: EntitySource;
}
```

### 6.4 计时器

```ts
interface Timer extends VersionedEntity {
  label: string;
  durationMs: number;
  accumulatedMs: number;
  startedAt: number | null;
  pausedAt: number | null;
  dueAt: number | null;
  state: "running" | "paused" | "done" | "cancelled";
  sound: "chime" | "voice" | "silent";
  source: EntitySource;
}
```

计时器不逐秒同步。显示端计算：

```ts
const elapsed =
  timer.accumulatedMs +
  (timer.state === "running" && timer.startedAt
    ? Math.max(0, Date.now() - timer.startedAt)
    : 0);
const remainingMs = Math.max(0, timer.durationMs - elapsed);
```

### 6.5 闹钟

```ts
interface Alarm extends VersionedEntity {
  label: string;
  enabled: boolean;
  hour: number;       // 0..23
  minute: number;     // 0..59
  timezone: string;
  weekdayMask: number; // bit0=周一 ... bit6=周日；0 表示仅一次
  onceAt: number | null;
  sound: "chime" | "voice" | "silent";
  speechText: string | null;
  source: EntitySource;
}
```

### 6.6 秒表

每个家庭只有一个秒表，固定 `id="household-stopwatch"`：

```ts
interface Stopwatch extends VersionedEntity {
  state: "idle" | "running" | "paused";
  accumulatedMs: number;
  startedAt: number | null;
  lapsMs: number[];
}
```

秒表不逐 10 ms 或逐秒同步，只在开始、暂停、计次和重置时产生事件。

### 6.7 购物清单

```ts
interface ShoppingItem extends VersionedEntity {
  name: string;
  quantity: number | null;
  unit: string | null;
  checked: boolean;
  category: string | null;
  note: string | null;
  source: EntitySource;
}
```

### 6.8 菜谱

```ts
interface RecipeIngredient {
  name: string;
  quantity: number | null;
  unit: string | null;
}

interface Recipe extends VersionedEntity {
  title: string;
  description: string | null;
  ingredients: RecipeIngredient[];
  steps: string[];
  servings: number | null;
  tags: string[];
  favorite: boolean;
  source: EntitySource;
}
```

菜谱只同步文字和结构化数据；封面图只保存 HTTPS URL 或媒体 ID。

### 6.9 营养记录

```ts
interface NutritionRecord extends VersionedEntity {
  title: string;
  consumedAt: number;
  caloriesKcal: number | null;
  proteinG: number | null;
  fatG: number | null;
  carbohydrateG: number | null;
  sodiumMg: number | null;
  note: string | null;
  source: EntitySource;
}
```

### 6.10 Agent 会话

```ts
interface AgentSession extends VersionedEntity {
  title: string;
  status: "active" | "archived";
  lastMessageAt: number;
}

interface AgentChatMessage extends VersionedEntity {
  sessionId: string;
  sequence: number;
  role: "user" | "assistant" | "tool";
  text: string;
  toolName: string | null;
  toolCallId: string | null;
  status: "completed" | "failed";
  source: EntitySource;
}
```

聊天消息使用 `operation=append`，`entityId` 是消息 ID。`eventId` 重投不得生成第二条消息。全量历史通过 REST 快照分页获取，不在 MQTT 中反复发送整个会话。

### 6.11 Agent Memory

```ts
interface AgentMemory extends VersionedEntity {
  kind: "structured" | "markdown";
  text: string;
  tags: string[];
  source: EntitySource;
}
```

- `/sdcard/agent/data/memory.json` 条目映射为 `kind=structured`。
- `/sdcard/agent/MEMORY.md` 映射为固定实体 `id="household-memory-md"`、`kind=markdown`。
- Memory 属于家庭共享敏感数据；当前所有家庭成员均可读写。

### 6.12 设备设置

```ts
interface DeviceSettings extends VersionedEntity {
  deviceSn: string;
  timezone: string;
  locale: "zh-CN";
  volumePercent: number;     // 0..100
  brightnessPercent: number; // 0..100
  reminderEnabled: boolean;
  agentVoiceEnabled: boolean;
}
```

### 6.13 AI 配置

写请求：

```ts
interface AgentConfigData {
  profileId: number;
  profileName: string;
  apiBaseUrl: string;
  model: string;
  systemPrompt: string;
  timeoutMs: number;
  apiFormat: number;
  apiKey?: string;
}
```

`GET /agent-config` 返回标准版本包装：

```ts
interface AgentConfigView {
  entityId: "active-profile";
  entityRevision: number;
  serverRevision: number;
  data: Omit<AgentConfigData, "apiKey"> & {
    apiKey?: "***";
  };
}
```

固件当前把 `apiBaseUrl`、`model`、`apiKey` 应用到 AI 客户端；其余字段保留在同步实体中。P4 的 TF 镜像删除 `apiKey` 并只写 `hasApiKey`，明文 Key 由 AI 客户端 NVS 保存。

## 7. REST API

### 7.1 P4 v2 微信登录

```http
POST /api/v2/auth/wechat-login
Content-Type: application/json
```

```json
{
  "code": "<wx.login 临时 code>",
  "nickname": "可选昵称",
  "avatarUrl": "可选头像 URL"
}
```

成功响应 `data.accessToken` 是后续 Bearer JWT，同时返回 `expiresIn` 和 `hasBoundDevice`。该 v2 登录不会执行旧 DEMO 家庭自动归并；家庭成员关系只能通过本章二维码 claim 建立。

公网多用户联调必须在 `.env` 配置真实 `WX_APPID/WX_SECRET`。留空时后端会使用固定 demo openid，仅适合单用户接口联调，不能验证两个微信账号加入同一家庭。

### 7.2 设备列表

```http
GET /api/v2/devices
Authorization: Bearer <miniappJwt>
```

返回当前家庭全部设备，不接受任意 SN 自动创建：

```ts
interface DeviceSummary {
  id: string;
  deviceSn: string;
  name: string | null;
  status: string;
  lastSeenAt: string | null;
}
```

### 7.3 获取快照

```http
GET /api/v2/sync/snapshot?homeId=<可选家庭UUID>
Authorization: Bearer <miniappJwt>
```

此接口始终返回当前完整快照。响应：

```ts
interface SyncSnapshot {
  serverRevision: number;
  generatedAt: string;
  domains: Partial<Record<SyncDomain, unknown[]>>;
}
```

增量补偿使用：

```http
GET /api/v2/sync/events?sinceRevision=<lastServerRevision>&homeId=<可选家庭UUID>
Authorization: Bearer <miniappJwt>
```

返回 `serverRevision`（本页游标）、`latestServerRevision`、`hasMore` 和 `events`。普通用户快照和增量中的 `ai_config` 密钥会被替换为掩码；设备使用 `GET /api/v2/device-sync/snapshot` + Device token 可获取当前比赛模式下的完整配置。

### 7.4 提交事件

```http
POST /api/v2/sync/events
```

请求：

```json
{
  "events": [
    {
      "eventId": "uuid",
      "domain": "shopping",
      "entityId": "uuid",
      "operation": "upsert",
      "baseEntityRevision": 2,
      "data": {}
    }
  ]
}
```

成功：

```json
{
  "ok": true,
  "data": {
    "accepted": [
      {
        "eventId": "uuid",
        "entityRevision": 3,
        "serverRevision": 16
      }
    ],
    "duplicates": [],
    "conflicts": [],
    "serverRevision": 16
  },
  "message": "ok",
  "requestId": null
}
```

批量请求最多 50 个事件，单个事件的 `data` JSON 最大 48 KiB。各事件按请求顺序处理；冲突事件放入 `conflicts`，其余事件仍可提交。冲突仍返回 HTTP 200，客户端必须检查 `data.conflicts`，不能只看 HTTP 状态码。

### 7.5 首次设备种子

```http
POST /api/v2/device-sync/seed
Authorization: Device <deviceHttpToken>
```

请求：

```json
{
  "domains": {
    "inventory": [
      {
        "entityId": "local-item-id",
        "data": {}
      }
    ]
  }
}
```

只在家庭 `serverRevision=0` 时接受；最多 1000 个实体。每个种子实体会生成稳定的 `seed:<deviceId>:<domain>:<entityId>` eventId，并逐实体提升 revision。云端已有数据返回 HTTP 409，设备随后拉取 `/api/v2/device-sync/snapshot`，不能覆盖云端。

### 7.6 AI 配置

```http
GET /api/v2/agent-config
PUT /api/v2/agent-config
```

`GET` 返回脱敏后的 `ai_config` 默认实体。`PUT` 使用与同步一致的事件语义：

```json
{
  "eventId": "uuid",
  "baseEntityRevision": 3,
  "data": {
    "profileId": 0,
    "profileName": "default",
    "apiBaseUrl": "https://example.test/v1",
    "model": "model-name",
    "systemPrompt": "",
    "timeoutMs": 30000,
    "apiFormat": 0,
    "apiKey": "plain-test-key"
  }
}
```

后端把成功事件以普通 `sync.apply`、`domain=ai_config`、QoS 1、非 retained 下发。Key 输入必须在线直传，禁止写入小程序离线队列、storage、调试日志或埋点。`apiKey` 省略或传 `"***"` 表示保留，空字符串表示清除，其它非空字符串表示轮换；普通配置字段按当前实体合并。设备 AI Base URL 必须是 HTTPS。

## 8. WebSocket

### 8.1 建连和鉴权

连接：

```text
ws://165.154.23.36:6005/api/v2/ws
```

连接打开后 10 秒内发送首帧：

```json
{
  "type": "auth",
  "token": "<miniappJwt>"
}
```

服务端成功帧：

```json
{
  "type": "auth.ok",
  "homeIds": ["uuid"],
  "heartbeatSeconds": 30
}
```

鉴权失败或超时直接以 WebSocket code `4401` 关闭连接；当前实现不发送 `auth.failed` JSON 帧。

### 8.2 固定服务端事件

```ts
type WsServerEvent =
  | { type: "auth.ok"; homeIds: string[]; heartbeatSeconds: 30 }
  | { type: "sync.event"; homeId: string; event: SyncEvent }
  | { type: "sync.conflict"; homeId: string; conflicts: SyncConflict[] }
  | { type: "device.state"; homeId: string; deviceId: string; state: DeviceState }
  | { type: "command.ack" | "device.error"; homeId: string; deviceId: string; payload: unknown }
  | { type: "binding.changed"; homeId: string; deviceId: string; bindingEpoch: number }
  | { type: "pong" };
```

`sync.event` 已包含 Backend 分配的 `entityRevision/serverRevision`。小程序应用后必须更新本地 `lastKnownServerRevision`。

### 8.3 心跳与重连

- 小程序每 25～30 秒发送 `{"type":"ping"}`，服务端回复 `{"type":"pong"}`。
- 客户端自行设置 60 秒无任何帧的断线判定。
- 重连退避固定为 1、2、5、10、30 秒，之后保持 30 秒。
- `onShow` 时若 Socket 不在线立即重连；`onHide` 可保持连接，系统挂起后按断线流程恢复。
- 每次 `auth.ok` 后调用 `GET /sync/events?sinceRevision=<lastKnownServerRevision>`，循环到 `hasMore=false`；首次启动或本地游标丢失时调用 `GET /sync/snapshot`。

## 9. 小程序接入示例

### 9.1 公共配置

```ts
export const API_BASE_URL = "http://165.154.23.36:6005";
export const API_V2 = `${API_BASE_URL}/api/v2`;
export const WS_V2 = "ws://165.154.23.36:6005/api/v2/ws";
```

不要复用旧 `/api/v1` 的任意 SN/DEMO 绑定逻辑。

### 9.2 解析二维码

```ts
function parseBindQr(raw: string): { sessionId: string; claimToken: string } {
  const match = raw.trim().match(
    /^fridgespirit:\/\/bind\?v=2&sessionId=([0-9a-f-]{36})&claimToken=([A-Za-z0-9_-]{20,})$/i,
  );
  if (!match) {
    throw new Error("不是有效的冰箱小精灵 P4 绑定二维码");
  }
  return { sessionId: match[1], claimToken: match[2] };
}
```

### 9.3 `wx.scanCode` + claim

```ts
interface ClaimResponse {
  ok: true;
  data: {
    sessionId: string;
    status: "claimed";
    homeId: string;
    bindingEpoch: number;
    expiresAt: string;
    expiresInSeconds: number;
    qrPayload: null;
    mqtt: null;
    deviceToken: null;
  };
  message: string;
  requestId: string | null;
}

function request<T>(
  path: string,
  method: "GET" | "POST" | "PUT",
  token: string,
  data?: unknown,
): Promise<T> {
  return new Promise((resolve, reject) => {
    wx.request({
      url: `${API_V2}${path}`,
      method,
      data,
      header: {
        Authorization: `Bearer ${token}`,
        "Content-Type": "application/json",
      },
      success: (res) => {
        const body = res.data as any;
        if (res.statusCode >= 200 && res.statusCode < 300 && body?.ok) {
          resolve(body as T);
        } else {
          const detail =
            typeof body?.detail === "string"
              ? body.detail
              : JSON.stringify(body?.detail ?? body);
          reject(new Error(detail || `HTTP ${res.statusCode}`));
        }
      },
      fail: reject,
    });
  });
}

export function scanAndClaim(jwt: string): Promise<ClaimResponse> {
  return new Promise((resolve, reject) => {
    wx.scanCode({
      onlyFromCamera: true,
      scanType: ["qrCode"],
      success: async (scan) => {
        try {
          const bind = parseBindQr(scan.result);
          const result = await request<ClaimResponse>(
            "/bindings/claim",
            "POST",
            jwt,
            bind,
          );
          resolve(result);
        } catch (error) {
          reject(error);
        }
      },
      fail: reject,
    });
  });
}
```

绑定成功不代表设备已经 MQTT 在线。页面应展示“等待设备确认”，并监听 `binding.changed` 或 `device.state`。

### 9.4 `wx.connectSocket`

```ts
export function connectRealtime(
  jwt: string,
  onEvent: (event: WsServerEvent) => void,
) {
  const socket = wx.connectSocket({ url: WS_V2 });

  socket.onOpen(() => {
    socket.send({
      data: JSON.stringify({
        type: "auth",
        token: jwt,
      }),
    });
  });

  socket.onMessage((message) => {
    try {
      const event = JSON.parse(String(message.data)) as WsServerEvent;
      onEvent(event);
    } catch {
      console.warn("忽略无法解析的 WebSocket 帧");
    }
  });

  socket.onError((error) => console.warn("WebSocket error", error));
  socket.onClose(() => {
    // 由上层统一按 1/2/5/10/30 秒退避重连；
    // 重连 auth.ok 后由上层按 lastKnownServerRevision 调 /sync/events 补偿。
  });

  return socket;
}
```

部分微信基础库没有 `crypto.randomUUID()`。项目应提供 UUID v4 工具，不能用时间戳代替 `requestId/eventId`。

### 9.5 本地事件 outbox

小程序业务写操作建议先生成稳定 `eventId`，再加入本地 outbox：

```ts
interface PendingSyncEvent {
  event: SyncEvent;
  attempts: number;
  nextRetryAt: number;
}
```

- 相同业务重试必须复用原 `eventId`。
- 只有 Backend 明确接受后才能从 outbox 删除。
- 网络错误按 1、2、5、10、30 秒退避。
- `REVISION_CONFLICT` 不自动覆盖，保留本地草稿并提示用户刷新/合并。
- AI Key 请求禁止进入 outbox。

## 10. Revision、冲突和幂等

### 10.1 两级 revision

- `entityRevision`：单实体版本，从 1 开始，每次成功修改 +1。
- `serverRevision`：家庭全局单调版本，每个成功事件 +1，用于断线补偿。
- `baseEntityRevision=0` 只用于创建从未存在的实体。
- 删除在云端实体表中保留 tombstone；当前实现没有自动清理周期。删除后的实体仍占用该 ID，后续修改必须基于 tombstone 的最新 `entityRevision`。

### 10.2 冲突响应

`POST /sync/events` 的 HTTP 状态仍为 200，冲突位于成功响应的 `data.conflicts`：

```json
{
  "ok": true,
  "data": {
    "serverRevision": 16,
    "accepted": [],
    "duplicates": [],
    "conflicts": [{
      "eventId": "uuid",
      "domain": "shopping",
      "entityId": "uuid",
      "expectedRevision": 2,
      "actualRevision": 3,
      "serverEntity": {}
    }]
  },
  "message": "conflict",
  "requestId": null
}
```

默认不使用“最后写入者覆盖”。小程序必须：

1. 用 `serverEntity` 刷新显示；
2. 保留用户未提交内容；
3. 让用户重试或显式合并；
4. 生成新的 `eventId`，并使用最新 `baseEntityRevision`。

### 10.3 幂等保存期

- Backend 在 `p4_sync_events` 中以 `(homeId,eventId)` 唯一约束持久保存已接受事件；当前实现没有自动清理周期。
- P4 在 NVS 中保存最近 8 个远程 `agent.tool_call` 的 `requestId` 和完成结果，防止 QoS 1 重投重复执行 Tool。
- P4 同步事件由 `entityRevision/serverRevision` 和本地 tombstone 共同判断重复或过期。
- Backend 收到重复 `eventId` 时把 ID 放入 `duplicates`；当前实现不会比较重复 ID 的内容，因此小程序绝不能把同一 `eventId` 用于不同业务内容。
- QoS 1 的 PUBACK 只代表 Broker 收到消息，不代表 Tool 执行成功；业务以 `completed` ACK 或 Backend 接受结果为准。

## 11. 离线、重连与恢复

### 11.1 P4

- MQTT 断线时本地 Agent 和本地数据继续工作。
- 当前首期使用 ESP-MQTT QoS 1 内存 outbox，payload 已通过 `CONFIG_MQTT_OUTBOX_DATA_ON_EXTERNAL_MEMORY=y` 放入 PSRAM；它不是断电持久队列。
- 重连退避为 1、2、5、10、30 秒，连接成功后重新订阅命令并发布 retained state。
- 设备 HTTP 首次绑定可拉取 2 MiB 以内完整快照；单条 MQTT envelope 最大 64 KiB。
- 当前没有实现 `/sdcard/agent/data/sync_outbox.jsonl`。MQTT 断线期间若设备产生了尚未进入 Broker 的本地变化，掉电后可能需要完整 HTTP 快照/人工联调对账；“TF 持久 outbox + 原 eventId 断电续传”列为赛后增强，不能作为本期验收前提。

### 11.2 Backend

- Backend 同步事件先提交 PostgreSQL，再尝试 MQTT 发布；发布失败不回滚数据库，设备可在绑定/恢复阶段通过 HTTP 快照补齐。
- 当前没有独立的数据库 MQTT command outbox；非同步的远程 Tool 命令若需要可靠断电重发，应在赛后补充命令表和调度器。
- ACK 状态事件按设备、Topic 身份、`bindingEpoch` 和 envelope 保存，发布成功不等于执行成功。
- 旧 `bindingEpoch` 的消息全部拒绝，避免解绑/重绑后旧消息污染新家庭。

### 11.3 小程序

- 本地持久化 `lastKnownServerRevision` 和业务 outbox。
- WebSocket 仅用于提速，不作为唯一事实来源。
- 每次冷启动、登录用户切换、家庭切换和 Socket 重连后都执行 revision 补偿。
- 收到家庭/绑定变化时清理原家庭缓存，防止跨家庭展示。

## 12. AI Key 明文测试模式

当前阶段按比赛联调需求允许：

```text
小程序 PUT /agent-config
  → Backend 明文数据库字段
  → 非 retained MQTT sync.apply(domain=ai_config)
  → P4 NVS
```

强制限制：

- 不得写入二维码、URL、Topic、日志、错误详情、ACK、WebSocket、普通 GET、TF 卡、截图或小程序 storage。
- Backend 日志只能记录 revision、模型名、HTTP 状态、响应长度和耗时。
- P4 串口和 USB 查询接口只能返回 `hasKey`，不得回显明文。
- `sync.apply` 必须 QoS 1、非 retained；P4 以 `entityRevision/serverRevision` 幂等应用。
- MQTT 断线时配置仍保存在 Backend；设备下次绑定恢复时从设备快照接口获取，当前没有独立的命令投递状态页面。

正式部署改造项：

1. HTTPS/WSS/MQTTS；
2. Backend 使用 KMS 或环境主密钥进行字段级加密；
3. AI Key 不再通过普通同步事件广播；
4. NVS 加密、Secure Boot、Flash Encryption；
5. 轮换所有开发阶段 MQTT、设备和 AI 凭据。

## 13. 错误码

当前 Backend 使用 FastAPI 标准 `{ "detail": ... }` 错误体，尚未实现稳定的机器错误码枚举。小程序首期按 HTTP 状态和 `detail` 处理：

| HTTP | 当前触发场景 | 客户端处理 |
| --- | --- | --- |
| 400 | 缺少 Bootstrap 字段、ACK `committed=false` | 修正请求，不重试 |
| 401 | JWT、Device token、Bootstrap 或 claim token 无效 | 重新鉴权；不得打印凭据 |
| 403 | 用户不属于目标家庭、凭据与设备不匹配 | 清理家庭上下文并停止重试 |
| 404 | 绑定会话不存在 | 让设备刷新二维码 |
| 409 | token 已被其他用户使用、会话状态错误、云端已有种子 | 刷新二维码或拉设备完整快照 |
| 410 | claim token 过期 | 让设备刷新二维码 |
| 413 | 单事件 `data` 超过 48 KiB | 拆分实体或改走 HTTP 快照 |
| 422 | 字段、域名或 operation 校验失败 | 修正客户端数据模型 |
| 500 | 未处理异常 | 保留原 eventId 并退避重试 |

revision 冲突不是 HTTP 错误，必须检查 HTTP 200 响应里的 `data.conflicts`。

设备 `device.error` 常用 code：

- `INVALID_PAYLOAD`
- `UNSUPPORTED_COMMAND`
- `INVALID_TOOL_CALL`
- `TOOL_ARGUMENTS_TOO_LARGE`
- `NO_MEMORY`
- `DEDUPE_PERSIST_FAILED`
- `DUPLICATE_EXECUTION_UNKNOWN`

同步命令的 `command.ack.errorCode` 还可能是 `SYNC_BUSY`、`SYNC_STORAGE_BUSY`、`REVISION_CONFLICT` 或 ESP-IDF 错误名。

## 14. 公网 IP 测试部署

仓库现有部署入口：

```text
backend/docker-compose.yml
backend/deploy/nginx/default.conf
backend/deploy/emqx/emqx.conf
```

服务器步骤：

```bash
cd backend
cp .env.example .env
# 编辑数据库、JWT、微信登录、MQTT 和联调地址配置
docker compose pull
docker compose build
docker compose up -d
docker compose ps
docker compose logs --tail=200 api emqx nginx
```

`api` 容器启动命令会先执行 `python scripts/migrate_p4_v2.py` 再启动 Uvicorn，不使用 Alembic。首次部署后预置设备：

```bash
docker compose exec api python scripts/provision_p4_device.py \
  --device-sn FS-P4-000001 --name "比赛设备"
```

命令只显示一次 `keyId/bootstrapSecret`，立即通过 USB 写入设备，服务器数据库仅保存摘要。

必须配置或确认：

```dotenv
PUBLIC_API_BASE_URL=http://165.154.23.36:6005
MQTT_BROKER_HOST=emqx
MQTT_BROKER_PORT=1883
MQTT_PUBLIC_BROKER_URI=mqtt://165.154.23.36:1883
MQTT_TOPIC_PREFIX=fridge
MQTT_KEEPALIVE_SECONDS=60
P4_BINDING_TTL_SECONDS=300
WX_APPID=<真实小程序 AppID>
WX_SECRET=<真实小程序 Secret>
```

网络端口：

- 对开发联调开放 TCP `6005` 和 `1883`。
- PostgreSQL、EMQX Dashboard 不向公网开放。
- Nginx `6005` 必须正确代理 `/api/v2/ws` 的 WebSocket Upgrade。

健康检查：

```bash
curl http://165.154.23.36:6005/healthz
```

健康响应：

```json
{
  "status": "ok",
  "service": "fridge-spirit-backend",
  "env": "local",
  "database": true,
  "mqtt": true,
  "p4Schema": true,
  "websocketConnections": 0
}
```

三项依赖未全部就绪时 `status` 为 `degraded`，但 HTTP 健康接口仍可访问。

## 15. 联调与验收清单

### 15.1 二维码和家庭

- [ ] 未预置设备不能创建绑定 session。
- [ ] 二维码中只有 v2 和一次性 token，没有任何长期凭据。
- [ ] token 5 分钟过期，刷新二维码后旧 token 失效。
- [ ] 同一用户 claim 重试幂等成功。
- [ ] 不同用户重复消费返回 `TOKEN_USED`。
- [ ] 首个用户绑定设备；第二个用户扫描设备新二维码后加入同一家庭。
- [ ] 已绑定设备不会因再次扫码被转移到其他家庭。
- [ ] P4 只有在 NVS commit 后才 ACK 并启动 MQTT。

### 15.2 MQTT

- [ ] P4 只订阅自己的 `fridge/v2/.../down/command`。
- [ ] v1/S3 Topic 和 schema 被拒绝。
- [ ] state retained 与 LWT 能正确显示在线/离线。
- [ ] QoS 1 重投不会重复创建库存、提醒、计时器或聊天消息。
- [ ] 错误 home/device/bindingEpoch 的消息不会执行。
- [ ] 65536 bytes 以上消息返回 `PAYLOAD_TOO_LARGE`。
- [ ] MQTT 回调不做 TF I/O、JSON 大解析或 Tool 执行。

### 15.3 全业务同步

- [ ] `inventory`
- [ ] `reminders`
- [ ] `timers`
- [ ] `alarms`
- [ ] `stopwatch`
- [ ] `shopping`
- [ ] `recipes`
- [ ] `nutrition`
- [ ] `agent_sessions`
- [ ] `agent_memory`
- [ ] `device_settings`
- [ ] `ai_config`

每个域至少验证：P4 新建、小程序新建、修改、删除、两端同时编辑、断网编辑、重启恢复和重复事件。

### 15.4 实时与离线

- [ ] 正常网络下 P4/Backend/两个小程序间更新 2 秒内可见。
- [ ] WebSocket 断线后按 1/2/5/10/30 秒重连。
- [ ] 重连使用 `lastKnownServerRevision` 补齐丢失事件。
- [ ] 首次启动或游标丢失时能够完整拉取快照。
- [ ] 明确验证当前 PSRAM 内存 outbox 的边界；断电持久 outbox 作为赛后增强单独验收。
- [ ] 云端已有数据时拒绝设备旧种子覆盖。
- [ ] revision 冲突显示明确，不静默最后写覆盖。

### 15.5 AI 配置

- [ ] PUT 能更新模型和 Key，设备收到新的 `entityRevision/serverRevision`。
- [ ] 重发相同 revision 不会重复写 NVS。
- [ ] GET/WebSocket/ACK/日志/串口中没有 Key 明文。
- [ ] MQTT retained 中没有 AI 配置。
- [ ] 小程序本地 storage 和离线 outbox 中没有 Key。

### 15.6 媒体和资源

- [ ] 图片、音频、固件和全量会话历史不通过 MQTT 传输。
- [ ] MQTT 大缓冲和 outbox 优先使用 PSRAM。
- [ ] MQTT/TLS/Flash 相关控制栈保留内部 SRAM。
- [ ] 联调期间无 watchdog、PSRAM 初始化失败、TF 数据损坏或截图 DMA 内存明显退化。

## 16. 小程序迁移要点

小程序修改时必须完成：

1. 登录改用 `/api/v2/auth/wechat-login`，全部 P4 Agent 业务接口使用 `/api/v2`。
2. 删除 `DEMO`、手输任意 SN、扫码结果直接当 SN 的绑定方式。
3. 按固定 URI grammar 提取二维码 `sessionId` 和 `claimToken` 参数并调用 `/bindings/claim`。
4. 保留 JWT REST 鉴权，不在小程序内引入 MQTT SDK 或 MQTT 凭据。
5. 用 WebSocket 替换 15 秒全量轮询；REST revision 补偿仍必须保留。
6. 所有写操作生成稳定 `eventId` 和正确 `baseEntityRevision`。
7. 计时器和秒表根据 epoch 状态本地渲染，不逐秒向服务器写入。
8. 冲突必须可见，不能直接用本地快照覆盖云端。
9. AI Key 只在在线 PUT 请求中短暂存在，不写缓存和日志。
10. 用户/家庭切换时关闭旧 Socket、清空旧家庭缓存并重新拉取快照。

此文档未定义的旧 S3 字段、Topic、命令或绑定行为一律视为不支持。
