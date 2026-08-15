# 截屏快速指南

## Codex 一键读取

先断开 WebUI 或其他串口工具，然后从项目根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1 -Port COM17
```

指定页面：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1 -Port COM17 -Page more
```

命令会自动执行：等待 `event=ready` → 可选 `ui.switch_page` → 轮询 `ui_status` 稳定门 → 只截一次 → TF 分块读回 → CRC32 校验 → 保存本地图片。

最后一行 JSON 的 `local_path` 是 Codex 应读取的图片路径。`format=bmp` 是摄像头忙、JPEG 失败或内部 DMA 连续空间不足时的安全降级，仍然是成功结果。

## Web 面板

1. 连接 P4 USB 串口。
2. 打开总览首页。
3. 点击“截取屏幕”。
4. 等待按钮完成资源稳定、截屏和 TF 读回。
5. 在“最近截屏”查看预览，或下载文件。

Web 端与 Codex 使用相同的四项 `ui_status` 稳定门，不使用固定 8 秒延时。截屏完成前不要在 TF 页面发起其他传输。

## 必须记住

- 当前实机串口为 `COM17`，115200 baud。
- 脚本打开串口时必须 `DTR=false、RTS=true`。
- 同一时间只能有一个串口读者。
- `ready` 不等于资源稳定；四项状态必须连续两次通过。
- 页面切换响应的 `queued=true` 不等于画面已显示，仍需等待 `ui_status.pageFrameReady=true`。
- 失败时不要再次发送 `screenshot_capture`；先处理串口占用、启动状态或资源状态。

详细说明见 [`screenshot.md`](screenshot.md)。
