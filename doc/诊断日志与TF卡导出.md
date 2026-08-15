# 诊断日志与 TF 卡导出

本文说明 P4 固件如何把运行日志、复位统计和 Flash coredump 保留下来，方便排查卡死、看门狗复位、panic 和 brownout。当前实现面向 `ESP32-P4 CB V1.2`、ESP-IDF 5.5.1 和板载 TF 卡；尚未替代实机长稳、故障注入或断电测试。

## 运行日志

- 固件在最早期通过 `fridge_diagnostics_log_install()` 安装 `esp_log_set_vprintf()` tee，原有串口输出保持不变。
- 只有 INFO/WARN/ERROR/FATAL 及未带级别前缀的文本进入日志缓冲；DEBUG/VERBOSE 不写 TF，避免高频调试输出挤占存储。
- 日志回调不访问 FATFS，只把文本复制到 8KiB RAM 环形缓冲。TF 卡挂载后由低优先级 `diag_log` 任务批量写入。
- 日志目录固定为 `.fridge_spirit/logs/`，文件为 `diag_0.log` 到 `diag_3.log`，每个文件上限 256KiB。达到上限后切换到下一个文件并原子清空，因此总日志空间约 1MiB。
- TF 卡暂时不可用时，早期日志最多保留在 8KiB RAM 中；空间不足会丢弃最旧记录，并在 `get_diagnostics` 的 `logger.droppedRecords/droppedBytes` 中累计。

## 复位与重启统计

`fridge_diagnostics_init()` 在 NVS 初始化后记录：

- 当前复位原因：`power_on`、`panic`、`task_wdt`、`interrupt_wdt`、`brownout` 等稳定 ASCII 名称；
- 持久化启动序号；
- brownout、panic、Task-WDT、Interrupt-WDT 和其它 watchdog 累计次数；
- OTA/恢复流程调用 `fridge_diagnostics_set_planned_restart("原因")` 后的计划重启原因。

计划重启原因只允许短文本，不得写入 API Key、Wi-Fi 密码或其它凭据。下次启动读取后会清除 NVS 中的 pending 标志，但仍在本次运行的状态快照中显示。

## Flash coredump 导出

启用的是 Flash ELF coredump，分区为 `partitions_recovery_p4.csv` 中的 `coredump`（768KiB）。成功启动后，主固件按以下顺序处理上一次 panic 遗留数据：

1. 查询 Flash coredump 大小；空分区与损坏/异常尺寸分开记录，异常不会静默当成“无数据”。
2. 调用 `esp_core_dump_image_check()` 校验完整性。
3. 通过 4KiB 流式缓冲把 ELF 原子写入 `.fridge_spirit/diagnostics/coredump-<boot_sequence>.elf`。
4. 写入同名 `.txt` metadata，包含启动序号、大小、panic reason、异常任务和 PC。
5. 只有 ELF 与 metadata 都同步落盘成功后，才调用 `esp_core_dump_image_erase()` 清除 Flash；任一步失败都会保留 Flash coredump，等待下次重试。

`.fridge_spirit/diagnostics/` 是受保护目录。普通 `tf_list`、`tf_stat`、`tf_text_read`、`tf_transfer_*`、HTTP 浏览和专家模式都不能读取或传输其中内容；这样可以避免把异常转储误暴露到通用文件接口。`get_diagnostics` 只返回 pending/exported、大小、路径、原因和错误状态，不返回 ELF 正文。

## USB 调试用法

协议仍为 115200 波特率 JSON Lines。设备发出 `ready` 后可使用：

```json
{"type":"request","request_id":"diag-1","command":"get_diagnostics"}
```

重点查看：

- `payload.resetReason`、`bootSequence`、各类计数和 `plannedRestartReason`；
- `payload.logger.running/tfReady/currentPath/writtenBytes/writeErrors/droppedRecords`；
- `payload.logger.coredump.pending/exported/size/path/reason/error`。

日志正文可通过受控的 `tf_text_read` 分别读取 `.fridge_spirit/logs/diag_0.log` 至 `diag_3.log`。诊断 ELF 不走普通 USB/HTTP 接口；如需分析，必须在受信任的固件内部导出或使用专用维护流程。

## 分区与构建说明

- 主固件 `ota_0` 为 6MiB；`ota_1` 保留 2MiB 本地 recovery，recovery 必须使用 `FRIDGE_RECOVERY_BUILD=ON` 和 recovery 配置单独构建。
- 主固件镜像大于 2MiB 时，ESP-IDF 的通用分区大小检查会提示 `ota_1 overflow`。这不是把主固件写入 recovery 的许可；应继续单独构建并检查 recovery 镜像大小。
- 当前 coredump 导出依赖 TF 卡已挂载。TF 卡未挂载时不会擦除 Flash coredump，网络/USB 初始化失败也不会阻断启动诊断。

## 尚未完成的实机验收

尚未在 P4 实机上完成：TF 卡持续写入、真实 Task-WDT/Interrupt-WDT/panic 注入、brownout 复位、coredump ELF 读回与 `espcoredump.py info_corefile` 解码、写入期间断电和长稳测试。完成这些测试前，不应把“构建通过”描述为“硬件诊断链路已验证”。
