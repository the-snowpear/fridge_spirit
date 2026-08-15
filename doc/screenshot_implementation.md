# 截屏功能实现记录

更新时间：2026-07-25

## 代码落点

| 层级 | 文件 | 职责 |
| --- | --- | --- |
| UI 状态门 | `components/ui_p4/fridge_ui_p4.c` | 原子发布 `ready`、资源完成、双缓冲同步、页面首帧和当前页面 |
| UI 公共接口 | `components/ui_p4/include/fridge_ui_p4.h` | `fridge_ui_p4_get_status()`、页面键转换和异步切页队列 |
| 截屏组件 | `components/screenshot/fridge_screenshot.c` | 复制可见帧，JPEG/BMP 编码，TF 原子写入 |
| USB 协议 | `components/usb_protocol/fridge_usb_protocol_minimal.c` | `ui_status`、`screenshot_capture` 和 TF 传输命令 |
| Web 首页 | `web/src/App.tsx` | 状态门、截屏按钮、TF 读回、CRC32、预览和下载 |
| Web 类型 | `web/src/types.ts` | `UiStatus`、`ScreenshotResponse` 和 `DeviceCommand` |
| Mock | `web/src/transports/MockTransport.ts` | 稳定状态和小型 BMP 的完整演示链路 |
| Codex 入口 | `tools/capture_screenshot.ps1` | 根目录固定入口，转发到 `tmp/tools` |
| Python 实现 | `tmp/tools/test_screenshot.py` | 串口会话、页面切换、稳定轮询和 TF 读回 |

## UI 稳定门

`ready` 事件和 UI 截屏状态是两个不同层次：

```text
USB event=ready
  ≠ 资源/字体完成
  ≠ 两块显示缓冲同步
  ≠ 目标页面首帧已经提交
```

固件通过 `fridge_ui_p4_status_t` 发布：

- `ready`：UI 基础首帧和任务可用。
- `resource_ready`：TF 资源和 FreeType 字体已绑定。
- `framebuffers_synced`：资源整页经过真实 flush 后同步到另一块显示缓冲。
- `page_frame_ready`：最近一次页面切换至少有一帧成功提交。
- `page`：当前稳定页面键。

USB 映射为 camelCase 的 `ui_status` 响应。Python 和 Web 都要求四项为 true，并连续查询两次；指定页面时再比较 `page`。因此不会因为固定等待时间过短而把 `TF 资源加载中` 或切页前旧帧交给 AI。

## 页面切换

页面请求必须走 Tool Broker 的顶层 `tool_call`：

```json
{"type":"request","request_id":"page-001","command":"tool_call","tool":"ui.switch_page","arguments":{"page":"more"}}
```

Tool 只向 UI FreeRTOS 队列投递，不在 USB/Agent 任务访问 LVGL。`ok/status=ok/data.queued=true` 只表示入队；状态门负责确认真正首帧。

## 截屏资源和内存策略

1. 校验显示、TF、UI 四项状态。
2. 在 PSRAM 申请 1024×600×2 字节 RGB565 暂存区。
3. 持 display UI 渲染互斥，复制当前 DPI 正在扫描的可见缓冲，立即释放互斥。
4. 摄像头空闲且内部 DMA 最大连续块达到 32KiB 时使用硬件 JPEG/YUV422/质量 85。
5. 摄像头忙、DMA 碎片化或 JPEG 失败时使用 BMP16；这是安全降级，避免 IDF 5.5.1 JPEG 驱动异常路径。
6. 写 `.part`，`fflush` 后 `rename` 到 `.fridge_spirit/exports/`。

截屏只读显示帧，不访问 GPIO28/29 摄像头 SCCB 与 ES8311 共享 I2C。JPEG 编码器是单资源，摄像头占用期间不抢占。

## 验证要求

- Web/Mock：确认首页按钮会等待 `ui_status`，只发送一次截图，TF 分块每块 CRC32 通过，并显示预览。
- Codex：确认最后 JSON 为 `ok=true` 且 `local_path` 可读；用 `view_image` 检查实际页面，不只检查文件头。
- 真机：确认 `ready`、目标页面 `ui_status` 四项稳定、JPEG 或 BMP 文件头正确、TF CRC32 全通过；同时检查无 brownout、watchdog、Guru Meditation 和反复重启。
- JPEG 失败或内部 DMA 不足时，BMP 读回成功仍算截屏链路成功，但应记录降级原因。

## 备份

本轮编辑前备份：

```text
tmp/backups/manual/20260725-221736_screenshot_chain_final/
```
