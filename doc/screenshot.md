# P4 截屏链路说明

更新时间：2026-07-25

本文是固件、USB JSON Lines、Web 首页和 Codex 本地读图之间的唯一截屏链路说明。当前有效命令为 `screenshot_capture`；`capture_screenshot` 不是设备命令，只是项目 PowerShell 入口名称。

## 一条命令完成 Codex 读图

先断开 Web Serial、ESP-IDF monitor 和其他串口助手，确保同一时间只有一个串口读者，然后从项目根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1 -Port COM17
```

指定目标页面时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1 `
  -Port COM17 -Page more -Output tmp\cache\screenshot_test\codex_latest
```

根入口内部转发到 `tmp\tools\test_screenshot.py`，自动使用项目 IDF Python 环境，并完成：

```text
打开 COM17（115200，DTR=false，RTS=true）
  → 等待本次会话 event=ready（超时硬失败）
  → 可选顶层 tool_call：ui.switch_page
  → 轮询 ui_status，四项状态连续两次满足
  → 只发送一次 screenshot_capture
  → tf_transfer_begin(direction=read)
  → 循环 tf_transfer_chunk，逐块校验 CRC32
  → tf_transfer_end
  → 保存本地 JPEG/BMP
```

脚本最后一行是机器 JSON。成功必须同时满足：`ok=true`、`local_path` 存在、`format` 为 `jpeg` 或 `bmp`、`bytes>0`。Codex 取得 `local_path` 后直接用本地图像读取能力打开，不要只看串口日志，也不要复用旧图片。

## 为什么不能固定等待几秒

`event=ready` 只表示 USB 协议和基础启动编排可以响应，不表示 TF 图标、字典、FreeType 字体和双缓冲已经稳定。资源加载耗时会随 TF 卡、CPU 调度和启动状态变化；固定等待 8 秒曾经导致约 80% 截到 `TF 资源加载中` 或首页旧帧。

截屏前必须轮询 `ui_status`：

| 字段 | 含义 |
| --- | --- |
| `ready` | UI 任务已经完成基础首帧并可提供状态 |
| `resourceReady` | TF 资源和 FreeType 字体完成绑定 |
| `framebuffersSynced` | 资源整页已提交，并同步到另一块显示缓冲 |
| `pageFrameReady` | 最近一次页面切换至少成功提交一帧 |
| `page` | 当前页面稳定键，例如 `home`、`more`、`timer` |

四项布尔值必须都为 `true`；指定 `-Page` 时还要满足 `page` 等于目标键，并连续两次查询通过。状态门未通过时，脚本不会发送 `screenshot_capture`。

## Web 首页流程

总览页的“截取屏幕”按钮与 Codex 使用同一状态门：

1. 设置忙碌态并清除旧预览。
2. 轮询 `ui_status`，等待 `ready/resourceReady/framebuffersSynced/pageFrameReady` 全部满足两次。
3. 只发送一次 `screenshot_capture`。
4. 使用返回的 TF 路径启动 `tf_transfer_begin`。
5. 循环读取 1024 字节分块，校验 `offset`、`length`、`crc32`，成功后发送 `tf_transfer_end`。
6. 生成浏览器 Blob 预览和下载链接；异常时发送 `tf_transfer_abort`。

WebUI、Codex 和 monitor 不能并发打开 COM17。WebUI 只要求查看当前页，不会擅自切换页面；如果需要指定页面，应使用 Codex 脚本的 `-Page` 参数或先按下节手动调用 `ui.switch_page`。

## 页面切换协议

页面切换是异步 Tool 调用，`arguments` 必须位于顶层：

```json
{"type":"request","request_id":"page-001","command":"tool_call","tool":"ui.switch_page","arguments":{"page":"more"}}
```

只有以下条件表示请求已入队：

```text
response.ok == true
response.payload.status == "ok"
response.payload.data.queued == true
```

这三个条件不代表页面已经显示。之后仍必须查询 `ui_status`，等待 `pageFrameReady=true` 和目标 `page`。

## 固件和 TF 文件

- `components/screenshot/` 读取当前 DPI 正在扫描的 1024×600 RGB565 显示帧，不访问摄像头 SCCB。
- 固件先在 PSRAM 申请 RGB565 暂存区，持 UI 渲染互斥复制可见缓冲，随后立即释放互斥。
- 摄像头空闲且内部 DMA 连续空间足够时优先使用 P4 硬件 JPEG；摄像头忙、DMA 碎片化或 JPEG 驱动失败时使用 BMP16。
- JPEG 编码器的内部 DMA 不足时主动 BMP 降级，避免 IDF 5.5.1 JPEG 驱动异常路径；BMP 是安全成功路径，不应当被当作错误。
- 文件先写 `.part`，再 `fflush`、`rename` 到 `.fridge_spirit/exports/shot_<启动秒数>.jpg|bmp`。
- USB 响应只返回 `path` 和 `format`，图片内容始终通过 TF 分块读回，不塞入单条 115200 JSON。

## 手动读回（仅用于定位脚本故障）

手动操作必须保持这个顺序：

```text
ready
  → 可选 tool_call(ui.switch_page)
  → ui_status 稳定门
  → screenshot_capture
  → tf_transfer_begin(direction=read)
  → 多次 tf_transfer_chunk(session, offset)
  → tf_transfer_end
```

每个分块都必须检查 `offset`、`length`、`crc32` 和 Base64 解码长度。任意异常发送 `tf_transfer_abort`，不要重复发送截图命令碰运气。

## 端口和串口控制线

- 当前实机端口：`COM17`；Windows 重新枚举后以实际端口为准，不要盲试多个 COM 口。
- 波特率：`115200`。
- 打开端口必须是 `DTR=false、RTS=true`。RTS=false 虽可能打开端口，但实测固件收不到命令。
- 设备复位、掉线、串口重开或重新出现启动日志后，必须重新等待本次会话的 `ready`。

## 排查顺序

| 现象 | 处理 |
| --- | --- |
| 端口打开失败 | 关闭 WebUI/monitor/串口助手，确认实际端口；不要杀不明确的进程 |
| ready 超时 | 检查 `DTR=false、RTS=true`、单串口读者和设备供电；脚本会硬失败，不要继续截图 |
| UI 稳定超时 | 读取日志和 `ui_status`，确认资源加载、双缓冲同步和页面首帧，不要改回固定 8 秒 |
| `ESP_ERR_INVALID_STATE` | 检查显示初始化、TF 挂载、UI 四项状态和 `.fridge_spirit/exports/` |
| `ESP_ERR_NO_MEM` | 停止摄像头预览或释放大块 PSRAM；先接受 BMP 降级 |
| 总是 BMP | 可能是摄像头忙或内部 DMA 连续空间不足；仍是有效截屏 |
| CRC32 失败 | 保留日志，确认没有第二个串口读者；脚本/Web 会尝试 abort 会话 |
| 画面仍是加载页 | 确认使用当前固件，检查 `ui_status` 是否四项连续满足；不要只看 `ready` |

## 相关文件

- `tools/capture_screenshot.ps1`：Codex/PowerShell 根入口
- `tmp/tools/capture_screenshot.ps1`：参数转发和 IDF Python 选择
- `tmp/tools/test_screenshot.py`：串口、状态门、截屏和 TF 读回实现
- `components/screenshot/fridge_screenshot.c`
- `components/usb_protocol/fridge_usb_protocol_minimal.c`
- `components/ui_p4/fridge_ui_p4.c`
- `web/src/App.tsx`
- `web/src/types.ts`
- `web/src/transports/MockTransport.ts`
