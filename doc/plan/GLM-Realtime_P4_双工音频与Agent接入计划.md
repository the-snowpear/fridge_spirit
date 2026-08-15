# 智谱 GLM-Realtime：ESP32-P4 双工音频与 Agent 接入计划

## 总体目标

将冰箱小精灵接入智谱 GLM-Realtime，首个可验收里程碑为：P4 设备端使用板载 ES8311 完成双工语音、打断和 Agent Tool Broker 闭环。

- WebUI 仅作为 USB 调试面板，不在浏览器中连接智谱，也不保存 API Key。
- WRITE 工具允许 Realtime 自动执行；`inventory.delete` 等 DANGEROUS 工具不导出。
- 保留文本注入作为无声诊断路径，但不作为主要 MVP 路径。
- 摄像头、视频输入、MCP、S3 完整协议和旧 MAX98357A 路径不纳入本轮。

Realtime 协议以智谱官方当前文档为准：<https://docs.bigmodel.cn/cn/guide/models/sound-and-video/glm-realtime>。

## Realtime 会话协议

MVP 固定默认配置：

- WSS：`wss://open.bigmodel.cn/api/paas/v4/realtime`
- 认证：设备端发送 `Authorization: Bearer <api_key>`
- 默认模型：`glm-realtime-flash`
- 默认音色：`tongtong`
- `modalities`：`["text", "audio"]`
- 输入格式：`pcm16`，16 kHz、单声道、16-bit
- 输出格式：`pcm`，24 kHz、单声道、16-bit
- `beta_fields.chat_mode`：`audio`
- VAD：MVP 固定 `server_vad`
- `client_vad` 暂不作为可配置项，待真机协议验证后再开放

会话顺序：

```text
WebSocket connected
  -> session.created
  -> session.update
  -> session.updated
  -> audio streaming / text diagnostic
  -> response.* events
```

必须处理：

- `heartbeat` 每 30 秒到达一次，仅更新活跃时间；
- 超过配置阈值未收到事件时进入 stale/error 状态；
- `response.done`、`response.cancelled`、`error` 更新状态机；
- 不假设不同事件类型之间有严格顺序。

## WebSocket 任务和内存模型

`components/glm_realtime` 内部由一个 `glm_rt` 任务独占 WebSocket：

- USB 命令只向命令队列投递请求；
- WebSocket 回调只复制数据并投递接收队列；
- 禁止在回调中调用 Tool Broker、读 TF 卡或发送新的 WebSocket 消息；
- 使用 `payload_offset/payload_len` 重组分片消息后再交给 cJSON；
- 不保存 WebSocket 回调中的临时指针；
- 阶段一关闭自动重连，避免 `stop` 后后台自动重连；
- 所有发送操作由 `glm_rt` 任务串行完成；
- 大 JSON、工具 Schema、音频 Base64 缓冲优先使用 PSRAM，同时检查内部 SRAM 和 TLS 栈水位。

避免 `usb_protocol` 与 `glm_realtime` 互相依赖。采用 Realtime 事件回调或 event sink 接口，由 USB 协议组件注册事件接收器并输出 JSON Lines。

## Function Call 和 Agent 工具策略

Realtime 工具导出规则：

- 自动导出 READ 工具；
- 自动导出 WRITE 工具，并沿用 Tool Broker 的 `standard/auto` 语义执行；
- 不导出 `inventory.delete` 等 DANGEROUS 工具；
- 不允许模型绕过 `fridge_tool_broker_call()`；
- 本地 `call_id` 由 `response_id + output_index` 派生，不假设服务端提供独立 call ID；
- `function_call_output` 按官方结构发送，只携带合法的 `type` 和 `output` 字段；
- `output` 包含 `status/code/summary/data`，并限制长度；
- MVP 将一轮响应限制为最多一个 Function Call，多调用批处理留作后续版本；
- WRITE 调用使用 `request_id` 或派生 idempotency key，防止重试造成重复写入；
- Schema 超限时拒绝导出并记录错误，不按字节截断非法 JSON。

首个工具验收覆盖：

- `inventory.list`
- `inventory.list_expiring`
- `device.state.get`
- `inventory.add`
- `inventory.update`
- `task.create_timer`
- `task.create_reminder`
- `task.cancel`
- `memory.add`
- `memory_md.append`

## 板载 ES8311 双工音频

固定使用板载路径：

```text
板载驻极体 MIC -> ES8311 RX -> P4 Realtime 音频上传
GLM-Realtime 24 kHz PCM -> ES8311 TX/DAC -> NS4150B -> Speaker
```

需要扩展现有音频/codec 接口：

- 新增 Realtime 专用连续流式 API，不复用现有 6 秒录音缓冲；
- 上行按 100 ms、16 kHz、16-bit、mono 帧读取并 Base64 编码；
- 下行建立 24 kHz PCM 播放队列；
- 先验证 ES8311 原生 24 kHz TX，只有硬件时钟不支持时才加入重采样；
- PA_CTRL=GPIO11 的拉高、播放、清空队列、拉低全部由 codec/speaker 组件控制；
- Realtime 会话期间暂停 WakeNet/ASR，结束或异常后按原策略恢复；
- 录音期间继续持有 GPIO28/29 共享 I2C gate，摄像头不得并发访问；
- 任何音频初始化失败都必须保持 PA_CTRL 为低电平。

## NVS、公共 API 和 USB 命令

新增 `fridge_rt` NVS 命名空间：

- `api_key`
- `model`
- `voice`
- `instructions`，较长内容使用 blob 或明确长度上限
- `vad_mode`，MVP 固定 `server_vad`
- `timeout_ms`
- `enabled`

公共 API 至少包括：

```text
fridge_glm_realtime_init()
fridge_glm_realtime_start()
fridge_glm_realtime_stop()
fridge_glm_realtime_cancel()
fridge_glm_realtime_send_text()
fridge_glm_realtime_get_status()
fridge_glm_realtime_get_config()
fridge_glm_realtime_set_config()
fridge_glm_realtime_clear_key()
fridge_glm_realtime_set_event_callback()
```

USB 命令：

```text
get_realtime_config
set_realtime_config
clear_realtime_key
realtime_start
realtime_stop
realtime_status
realtime_send_text
realtime_cancel
```

异步命令立即返回 `accepted`，最终结果通过事件返回。事件至少包括：

```text
realtime_state
realtime_transcript
realtime_text_delta
realtime_audio_state
realtime_tool_call
realtime_tool_result
realtime_error
realtime_done
```

配置响应只返回 `hasApiKey`、脱敏预览、模型、音色、状态和错误摘要，不回显明文 Key。

## WebUI

新增 Realtime 调试页面，包含：

- 配置、Key 写入/清除和脱敏状态；
- Realtime 启停；
- 连接、VAD、上行和下行音频状态；
- 文本诊断输入；
- 助手转写和回复文本；
- 工具调用及结果日志；
- 打断按钮；
- 有界事件日志；
- MockTransport 对应响应和事件模拟。

浏览器不得打开智谱 WebSocket，也不得持有智谱 API Key。

## 工程集成和依赖

- 新增 `components/glm_realtime/`；
- 在 P4 minimal 构建闭包中加入该组件；
- `esp_websocket_client` 使用明确版本范围并更新锁文件；
- WSS 使用系统证书包并显式配置证书回调；
- Realtime 初始化放在 network、audio、Agent Tool Broker 初始化之后；
- 初始化失败不阻断 USB 控制台；
- `realtime_start` 必须检查 `internet_ready`、Tool Broker ready 和音频资源可用；
- 修改前备份到 `tmp/backups/manual/YYYYMMDD-HHMMSS_glm_realtime_revision/`；
- 阶段性完成后更新 `doc/progress.md` 和 `doc/agent/REALTIME.md`。

## 实施顺序

1. 备份原计划和相关工程文件。
2. 更新计划文档中的目标、协议字段、工具策略、音频路径和验收章节。
3. 建立 `glm_realtime`，实现 WSS、session.update、心跳、错误和状态机。
4. 扩展 ES8311 为 Realtime 连续 RX/TX 流。
5. 完成“说话 -> 云端 -> 播放 -> 打断”的设备端闭环。
6. 接入 READ/WRITE Tool Broker，限制一轮一个工具调用。
7. 接入 USB 异步命令和事件 sink。
8. 接入 WebUI Realtime 页面和 MockTransport。
9. 执行真机 COM15 验收并记录结果。
10. 再评估自动重连、Client VAD、多工具并行、摄像头视频输入和危险工具确认。

## 验收标准

### 构建和资源

- P4 minimal 构建通过；
- `esp_websocket_client` 依赖锁定；
- `size-components` 记录新增 Flash、内部 SRAM、PSRAM 和任务栈占用；
- 无编译期循环依赖；
- 无 API Key 出现在日志、USB 响应或仓库。

### 真机双工音频

- `realtime_start` 后收到 `session.updated`/ready；
- 板载 MIC 能触发服务端语音检测并返回转写/响应；
- Speaker 能播放 24 kHz PCM；
- `realtime_cancel` 能停止响应并清空播放队列；
- Realtime 结束后 WakeNet 能恢复；
- 音频期间摄像头访问被正确拒绝或延迟；
- PA_CTRL 无异常脉冲、爆音或持续发热。

### Agent 工具

- READ 工具正常返回；
- WRITE 工具在自动模式下执行；
- 工具结果回传后模型能继续生成自然语言回复；
- `inventory.delete` 不会被导出；
- 重复请求不会产生重复库存/任务记录；
- 多工具调用被限制并返回明确状态。

### 异常场景

- 无 Key、Wi-Fi 未连接、SNTP 未就绪；
- TLS 证书失败；
- WebSocket 分片消息；
- 服务端 error；
- heartbeat 超时；
- 网络断开；
- 停止后的迟到事件；
- Tool Broker 未初始化；
- ES8311/I2C gate 忙；
- 播放队列溢出；
- PSRAM 不可用。

## 明确假设

- 目标为 ESP32-P4 CB V1.2 的 `CONFIG_FRIDGE_P4_MINIMAL` 路径。
- 首版音频只实现板载 ES8311，不同时抽象外接音频源。
- Realtime 运行在设备端，WebUI 仅调试。
- WRITE 工具允许自动执行，但危险工具不导出。
- 设备端不长期保存原始音频和图片。
- 摄像头与 Realtime 音频不并发。
- 阶段一固定 Server VAD、单工具调用和手动启停；自动重连、Client VAD、多工具并行和视频输入不属于首个验收范围。
