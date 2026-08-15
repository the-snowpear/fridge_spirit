# P4 内部 RAM / PSRAM 栈压力治理执行提示

## 目标

降低频繁蓝屏、`Stack protection fault`、SDMMC `not enough mem`、相机 ISP/JPEG 初始化失败风险，同时在 ESP32-P4 上安全使用 32MB PSRAM。

## 已确认约束

- 目标芯片为 ESP32-P4，ESP-IDF 为 5.5.1，设备带 32MB PSRAM。
- Flash Cache 关闭时 PSRAM 不可访问。PSRAM 栈任务不能直接执行 NVS、LittleFS、分区读写、OTA 或其他 Flash API。
- 不得把 UI、GLM、Agent、调度器等常驻任务整体、直接迁为 PSRAM 栈；此前此类改动已经触发 `esp_task_stack_is_sane_cache_disabled()`。
- `xTaskCreateWithCaps()` / `xTaskCreatePinnedToCoreWithCaps()` 创建的任务必须使用 `vTaskDeleteWithCaps()` 删除。检查并修正相机预览、抓拍、ASR 等已有 WithCaps 任务的配对释放。
- 当前配置基础：
  - `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`
  - `CONFIG_SPIRAM_USE_MALLOC=y`
  - `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
  - `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`
  - `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`
- UI 栈已计划由 48KB 缩至 32KB；开始前必须确认当前工作区状态，避免覆盖并行修改。

## 执行要求

1. 先读 `doc/memory.md`、当前 `sdkconfig`、近期启动/崩溃日志；确认没有未结束的构建、烧录或串口进程。
2. 不直接迁移常驻任务整栈到 PSRAM。建立或完善内部 RAM 的 Flash I/O 边界：
   - NVS、LittleFS、分区读写必须从内部 RAM 栈执行；
   - Agent/GLM 的 PSRAM worker 只做 HTTP、JSON、TF 非 Flash I/O 与算法计算；
   - Flash 操作期间，传入参数和接收结果均不能直接引用 PSRAM 指针，必须使用内部 RAM 副本。
3. 先补足可观测性，在关键初始化阶段、相机页启动、拍摄前后记录：
   - `heap_caps_get_free_size()`；
   - `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)`；
   - PSRAM 可用量；
   - UI、Agent、GLM、预览、抓拍、SDMMC 与字体加载路径的 stack high-water mark。
4. 做安全迁移：
   - 大型非 DMA 缓冲、JSON、TLS 工作区、音频环形队列、图像缩放/JPEG 非 DMA 缓冲显式分配到 PSRAM；
   - CSI、JPEG、SDMMC、I2S 的 DMA 描述符、bounce buffer 和驱动要求的缓冲保持 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`；
   - 热路径避免变长 `malloc/free`，优先固定池、静态缓冲或复用工作区。
5. 可评估 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`，但不能盲开：仅迁移确认不会在 Flash Cache 关闭期间访问的大型 BSS。若启用，必须单独构建、烧录与实机回归。
6. 按高水位数据逐个缩小内部任务栈：先 UI，再 Agent/GLM。每次只变更一个任务或一个缓冲策略，分别构建、烧录与验证。

## 重点回归项

- 冷启动稳定到 `ready`；
- TF 字体包、IME 与 FreeType 延迟加载；
- `ui.switch_page(camera)` 后高清预览；
- 拍摄、JPEG、SDMMC；
- Tool Call、Agent、GLM 配置读取；
- 至少 1 分钟观察 panic、watchdog、`sdmmc_read_sectors not enough mem` 和内部 DMA 最大连续块。

## 交付要求

最终明确说明：

- 哪些 panic/资源错误已消除；
- 哪些问题仍是 DMA 碎片或栈保护问题；
- 日志与实机验证证据；
- 未验证的硬件风险。

## 修改纪律

- 编辑前备份到 `tmp/backups/manual/<timestamp>_*`。
- 构建、烧录前备份到 `tmp/backups/before_run/<timestamp>_*`。
- 使用 `apply_patch` 编辑，不清理或覆盖其他并行修改。
