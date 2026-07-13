# TF 卡存储工作台使用说明

## 入口与前置条件

1. 使用 Chrome 或 Edge 打开 `web/` 构建出的本地页面。
2. 选择“USB”并连接 P4 板的 CH343P 串口（当前真机为 COM15，115200）。
3. 打开左侧“TF 卡”。大于 512 KiB 的文件默认尝试 Wi-Fi HTTP；设备尚未连接 Wi-Fi 时会明确降级为串口。

TF 卡固定挂载于 `/sdcard`。Web 和串口 API 中的路径均使用 TF 根相对路径，例如 `照片/早餐.jpg`，不要添加 `/sdcard` 前缀。

## 文件与传输

- 文件页支持分页浏览、名称筛选、排序、多选、拖放/选择上传、下载、新建目录、移动、重命名和删除。
- 删除默认移动到 `.fridge_spirit/trash`；回收站支持恢复和使用 `EMPTY_TF_TRASH` 精确口令永久清空。
- 文本、JSON 和日志不超过 256 KiB 时可在线编辑；写入采用 `.part`、同步落盘和同卷原子替换。
- 图片、音频、视频由浏览器预览；未知文件最多读取前 64 KiB 十六进制内容。
- 串口分块为 1024 字节，包含 session、offset 和 CRC32；传输队列支持暂停、继续和取消。
- FAT32 单文件必须小于 4 GiB。

## Wi-Fi HTTP 大文件通道

HTTP 服务只在 C6 Hosted、Wi-Fi、IP 和 TF 都就绪时开放。Web 先经 COM15 创建固定路径/方向的传输会话，再取得一次性 Bearer token。服务端口为 8080，token 十分钟无活动失效；只接受 localhost/127.0.0.1 Origin，不提供匿名目录浏览。

下载支持单区间 `Range`，上传以最多 1 MiB 的 PUT 请求续传，并通过 `X-Upload-Offset` 校验偏移。固件始终以 1024 字节块流式读写，不把整个文件载入 P4 RAM/PSRAM。

## 保护与维护

- 默认保护 `.fridge_spirit`。输入 `ENABLE_TF_EXPERT_MODE` 可启用专家模式，状态保存到 NVS；关闭专家模式不需要破坏性确认。
- 格式化必须输入 `ERASE_TF_CARD`。安全卸载只释放 SDMMC Slot0，不释放 C6 Hosted 使用的 Slot1 或共享 LDO4。
- “一键导出”将非敏感设备、Hosted、TF 状态和基础日志写到 `.fridge_spirit/exports`；不会写入 Wi-Fi 密码或 API Key。
- 健康和测速结果只代表挂载及 I/O，不代表存储卡寿命或 SMART。

## 主要 JSON Lines 命令

文件命令：`tf_list`、`tf_stat`、`tf_mkdir`、`tf_move`、`tf_delete`、`tf_restore`、`tf_trash_empty`、`tf_text_read`、`tf_text_write`。

传输命令：`tf_transfer_begin`、`tf_transfer_status`、`tf_transfer_chunk`、`tf_transfer_end`、`tf_transfer_abort`。

维护命令：`tf_mount`、`tf_unmount`、`tf_health_test`、`tf_benchmark`、`tf_get_access_mode`、`tf_set_access_mode`、`tf_http_enable/status/disable`、`tf_export_diagnostics`。

## 当前验证边界

- COM15 已真机验证中文 UTF-8 路径、分页、文本与二进制分块、CRC32、移动、回收/恢复、专家模式、健康测试、测速、安全卸载/挂载和诊断导出。
- 当前设备没有保存 Wi-Fi 凭据，因此 HTTP 服务的“无网络安全拒绝”已验证，真实 Wi-Fi HTTP 的 1/10/100 MiB 吞吐与断网续传仍需先在网络页完成配网后实测。
- 同一次开机内可按 session/offset 继续传输；设备重启后保留 `.part` 供人工恢复或清理，但当前版本不自动重建旧 session。
- 格式化、清空回收站和测速目前由串口命令同步执行，Web 已使用长超时保护；后台 job 进度事件仍属于后续长任务体验优化。
