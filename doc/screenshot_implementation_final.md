# 截屏功能当前实现总结

更新时间：2026-07-25

## 结论

截屏链路已统一为：

```text
等待本次会话 ready
  → 可选顶层 tool_call(ui.switch_page)
  → ui_status 四项稳定门连续两次通过
  → 只发送一次 screenshot_capture
  → TF 分块读回并 CRC32 校验
  → Codex/Web 读取本地图片
```

该状态门解决了 `ready` 后继续等待固定 8 秒仍经常截到 `TF 资源加载中` 的问题。Web 首页和 Codex Python 使用同一套条件；固件还会在内部 DMA 连续空间不足时安全降级 BMP，避免 JPEG 驱动异常路径。

## 固件接口

- `ui_status`：返回 `ready`、`resourceReady`、`framebuffersSynced`、`pageFrameReady`、`page`。
- `tool_call` + `ui.switch_page`：异步页面切换，成功判据是 `ok=true`、`payload.status="ok"`、`payload.data.queued=true`。
- `screenshot_capture`：返回 TF `path` 和 `format`，不直接回传图片内容。
- `tf_transfer_begin/chunk/end/abort`：分块读回，逐块带 CRC32。

## 固定入口

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1 -Port COM17 -Page more
```

最后一行机器 JSON 的 `local_path` 是最终图片。端口为 `COM17`、115200 baud，控制线必须 `DTR=false、RTS=true`，且 WebUI/monitor/Codex 不能同时占用串口。

## 格式

- 摄像头空闲且内部 DMA 连续空间充足：P4 硬件 JPEG/YUV422。
- 摄像头忙、JPEG 失败或 DMA 碎片化：BMP16 安全降级。
- 两种格式都通过同一 TF 分块读回链路；BMP 大、读回慢，但不是失败。

## 当前验证状态

状态门、Python 根入口和 Web 流程已完成代码更新；需在当前固件构建/烧录后再次实测 `ready → 切页 → ui_status → 单次截屏 → TF 读回 → 本地图像读取`，并以实际日志确认无新增 panic、brownout 或 watchdog。实测结果应追加到 `doc/progress.md`，不要把旧的固定等待 8 秒记录当作当前验收依据。
