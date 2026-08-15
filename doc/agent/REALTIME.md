# GLM Realtime 接入说明

本文件记录 ESP32-P4 设备端智谱 GLM-Realtime 与 Agent Tool Broker 的接入边界。实现细节以代码和 `doc/plan/GLM-Realtime_P4_双工音频与Agent接入计划.md` 为准。

## 边界

1. **全部运行在设备端**：WebSocket、会话、麦克风上行、扬声器下行、工具循环都在 P4 上。
2. **WebUI 仅调试**：配置、启停、状态、事件日志；浏览器不连接智谱，不保存 API Key。
3. **API Key** 存放 NVS 命名空间 `fridge_rt`；USB/Web 响应只返回 `hasApiKey` 与脱敏预览。
4. **工具** 必须经 `fridge_tool_broker_call()`；导出 READ + WRITE，**不导出 DANGEROUS**（如 `inventory.delete`）。
5. **VAD**：MVP 固定 `server_vad`。
6. **每轮最多一个 Function Call**；多调用会被拒绝并回传明确状态。
7. Realtime 会话期间占用 ES8311 双工与共享 I2C gate，**摄像头不得并发**；启动前暂停 WakeNet。

## 组件

- `components/glm_realtime/`：配置、WebSocket、状态机、工具循环、音频上下行调度。
- `components/codec_es8311/`：双工 I2S RX/TX；`PA_CTRL=GPIO11` 仅在写 PCM 时拉高。
- `components/audio/`：`fridge_audio_realtime_stream_*` / `play_pcm` / `playback_stop`。
- `components/usb_protocol/fridge_usb_protocol_minimal.c`：USB 命令与事件 sink。

## USB 命令

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

异步命令（start/stop/send_text/cancel）立即返回 `accepted`，结果走事件。

## 事件

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

## 默认协议

- WSS：`wss://open.bigmodel.cn/api/paas/v4/realtime`
- 模型：`glm-realtime-flash`
- 音色：`tongtong`
- 输入：`pcm16` 16 kHz mono
- 输出：`pcm` 24 kHz mono（设备重采样到 16 kHz 播放）
- `beta_fields.chat_mode`：`audio`

## 真机验收（COM15）

1. 配网并确认 `internet_ready`。
2. `set_realtime_config` 写入 Key → `get_realtime_config` 见 preview。
3. `realtime_start` → `session_ready` / `listening`。
4. 对着板载 MIC 说话，或 `realtime_send_text` 诊断工具调用。
5. 观察 `realtime_tool_call` / `realtime_tool_result` 与喇叭回放。
6. `realtime_cancel` 打断；`realtime_stop` 后 WakeNet 可恢复。
