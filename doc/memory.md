# 项目记忆与踩坑记录

本文记录“冰箱小精灵”项目中已经验证过的环境、硬件、串口、编码和联调经验。后续 AI 或协作者在开始工作前应先阅读本文件，避免重复踩坑。

----

## 2026-05-21 光照传感器与 INMP441 麦克风接入

### 接线与硬件安全

- 光照传感器当前按模拟量接入：`VCC -> 3V3`，`GND -> GND`，`IO/AO -> GPIO1`。
- `GPIO1` 对应 `ADC1_CH0`，固件用 ADC oneshot 读取 12-bit 原始值，再换算成 Web 面板展示的 `0-1023`。
- 当前光照模块实测为反向模拟量：读值高表示暗，读值低表示亮；固件保留 `lightRaw12bit` 原始值，同时把 `lightValue10bit`、`lightPercent` 和 `lightDelta` 换算成“数值越高越亮”的业务语义。
- 光照模块必须使用 `3.3V` 供电，避免 `IO/AO` 输出高于 ESP32-S3 ADC 输入范围。
- INMP441 当前按 I2S 接入：`VDD -> 3V3`，`GND -> GND`，`SCK -> GPIO40`，`WS -> GPIO41`，`SD -> GPIO42`，`L/R -> GND`。
- `L/R -> GND` 表示先固定左声道；如果后续 I2S 数据异常或转写为空，可尝试改接 `3V3` 测试右声道。
- 录音测试前先确认共地、线尽量短、杜邦线接触可靠；I2S 对接触不良比普通 GPIO 更敏感。

### 固件实现记忆

- 已新增 `components/sensors`，对外提供 `fridge_sensors_get_snapshot()`，不要在 USB 协议或业务逻辑里直接读 ADC。
- 已新增 `components/audio`，INMP441 采样参数为 `16 kHz / 16-bit PCM / mono`，录音缓存优先放 PSRAM。
- INMP441 常见输出是 24-bit 数据左对齐到 32-bit I2S slot，当前实现会把 32-bit 原始采样右移压成 16-bit PCM。
- 已新增 `components/asr`，ASR 配置独立于 AI 配置，NVS namespace 为 `fridge_asr`。
- 默认 ASR 配置为硅基流动：`https://api.siliconflow.cn/v1/audio/transcriptions`，模型 `TeleAI/TeleSpeechASR`。
- ASR 上传体包含 WAV 头和 PCM 数据，6 秒音频接近 192KB，multipart body 必须优先放 PSRAM，避免与 TLS/HTTP 内部缓冲争抢内部 SRAM。
- `voice_chat_stop` 会执行录音停止、WAV 封装、ASR HTTPS 请求和 AI HTTPS 请求，Web Serial 超时必须按长请求处理，不能使用普通短命令超时。

### USB/Web Serial 调试记忆

- 固件 USB 协议仍是 `115200` 波特率、JSON Lines，一行一个请求，一行一个响应。
- `get_sensors` 已返回真实光照字段：`lightRaw12bit`、`lightValue10bit`、`lightPercent`、`lightDelta`、`lightPolarity`，同时保留 `pir`、`lux`、`angleDelta`、`vibrationPeak`、`doorState` 等旧字段；其中 `lux` 只是兼容旧 Web 面板的亮度 0-1023，不是真实物理 lux。
- `get_asr_config` 会返回 ASR Base URL、模型、超时、`hasApiKey`、`apiKeyPreview` 和 `ready`，不会回显明文 Key。
- 已新增 USB 命令：`get_asr_config`、`set_asr_config`、`clear_asr_key`、`voice_chat_start`、`voice_chat_stop`、`voice_chat_status`。
- ASR Key 缺失时，`voice_chat_stop` 的预期错误是 `missing ASR API Key`，这是正常可读错误，不代表录音任务失败。
- COM16 可能被 `idf.py monitor`、Web Serial、VS Code Serial Monitor 或其他串口助手占用。如果出现 `Access to the port 'COM16' is denied`，先查是否有残留 `idf_monitor` 进程。
- `idf.py monitor` 在被外部超时强杀时可能留下多层 Python 子进程继续占用 COM16；只应停止命令行明确包含 `COM16` 且为 `idf_monitor` 或 `idf.py -p COM16 monitor` 的进程，不要乱杀其它 Python 服务。

### 2026-05-21 真机验证结果

- `idf.py build` 通过，固件镜像约 `0x116140`，最小 app 分区 `0x240000`，剩余约 52%。
- `npm run build` 通过。
- `idf.py -p COM16 flash` 成功，烧录设备识别为 ESP32-S3 rev v0.2，8MB Flash，8MB PSRAM。
- 启动日志确认 PSRAM 初始化和 memory test OK。
- 启动日志确认 `fridge_sensors` 初始化 GPIO1/ADC1_CH0 成功。
- 启动日志确认 `fridge_audio` 初始化 INMP441 I2S 成功。
- 串口验证 `get_sensors` 可返回实时光照值，遮挡/照射应继续用 Web 面板做实物复测。
- 串口验证 `voice_chat_start` 后 `pcmBytes` 和 `rms` 会增长，说明录音任务在跑。
- 测试中出现过连续 `i2s read failed: ESP_ERR_TIMEOUT`，已改为限频日志 `i2s read timeout, count=N`，避免刷爆 USB 日志。
- I2S timeout 如果持续出现，优先检查麦克风是否真实接线、`SD/WS/SCK` 是否接反、`L/R` 声道选择、GND 是否可靠、线是否太长。

### 文档与编码记忆

- 本项目涉及中文文档，编辑前必须确认目标文件是否为有效 UTF-8。
- `doc/progress.md` 在 2026-05-21 检查时存在非 UTF-8 字节，`apply_patch` 无法安全编辑；不要用二进制方式强行追加中文，避免扩大乱码。
- 如果需要更新 `doc/progress.md`，应先单独处理文件编码或让用户确认是否允许做一次编码修复。
- 编辑前按项目要求保留 `.bak-YYYYMMDD-HHMMSS` 备份；本次相关备份已生成在对应文件旁。

----

## 2026-05-21 麦克风采集优化参考结论

- 已参考 `example/microphone_reference/atomic14-esp32-i2s-mic-test`、`atomic14-esp32_audio` 和 Espressif 官方 `examples/peripherals/i2s`。
- 当前项目继续使用 ESP-IDF 新 I2S STD 驱动，不引入 Arduino/PlatformIO 依赖。
- I2S 通道应在初始化时配置，在录音开始时 enable，在录音任务结束时 disable，避免空闲时 DMA 长时间运行。
- INMP441 仍按 `16 kHz / 32-bit slot / mono / left slot` 采集，`L/R -> GND` 对应左声道。
- 当前 32-bit 原始采样转 16-bit PCM 的默认位移为 `raw >> 14`；如果声音过小可尝试 `>> 12` 或 `>> 11`，如果削顶严重可尝试 `>> 16`。
- `voice_chat_status` 新增诊断字段：`sampleCount`、`peakAbs`、`minSample`、`maxSample`、`meanSample`、`clipCount`、`timeoutCount`、`qualityHint`。
- `qualityHint=ok` 表示当前采样质量基本可用；`silent` 优先检查麦克风供电、SD、L/R 声道和说话距离；`clipping` 表示 PCM 可能削顶；`i2s_timeout` 优先检查 SCK/WS/SD/GND、线长和接触；`too_short` 表示录音太短或无样本。

----

## 2026-05-22 麦克风测试独立页面

- Web 面板已新增侧边栏页面“麦克风测试”，用于单独验证 INMP441 硬件采集、音量、I2S 稳定性和 ASR 识别效果。
- 新增 USB 命令：`mic_record_start`、`mic_record_status`、`mic_record_stop`。这三条命令只做麦克风采集诊断，其中 `mic_record_stop` 不会上传 ASR，也不会调用 AI。
- `voice_chat_start`、`voice_chat_status`、`voice_chat_stop` 保持原有 AI 助手语音对话链路；`voice_chat_stop` 仍会执行“停止录音 -> ASR -> AI 回复”。
- Web 的“硬件录音测试”会每 `500 ms` 轮询一次 `mic_record_status`，显示 `rms`、`peakAbs`、`min/max`、`clipCount`、`timeoutCount`、`qualityHint` 和最近 30 次轻量音量柱状图。
- Web 的“ASR 识别测试”复用 `voice_chat_start/voice_chat_stop`，用于完整验证“录音 -> 转文字 -> AI 回复”，并显示转写文本、AI 回复、ASR/AI 耗时和 HTTP 状态。
- AI 助手页仍保留语音按钮，但硬件录音测试进行中时不会允许它误触发 `voice_chat_stop`；如需停止硬件录音，应回到“麦克风测试”页点击停止。
- `qualityHint` 排查建议：
  - `ok`：采样基本可用，可以继续测试 ASR。
  - `silent`：检查 VDD/GND/SD、L/R 声道选择和说话距离。
  - `clipping`：PCM 可能削顶，后续可调大 `AUDIO_PCM_SHIFT` 或降低输入增益。
  - `i2s_timeout`：优先检查 SCK/WS/SD/GND、线长和杜邦线接触。
  - `too_short`：至少录 1-2 秒再判断，确认 `pcmBytes` 是否按约 `32000 bytes/s` 增长。

----

## 2026-05-22 语音链路 AI HTTP 400 排查

- 用户实测麦克风硬件测试、ASR 转写和 AI 回复整体可用，但连续说几次后偶发 `AI HTTP 状态异常：400`。
- `example/INMP441麦克风模块` 官方例程使用 Arduino 旧 I2S 驱动，配置为 `16-bit / ONLY_LEFT / I2S_MSB / 44100Hz`；当前工程继续使用 ESP-IDF 新 I2S STD 驱动和 `32-bit slot -> 16-bit PCM`，因为现有麦克风与 ASR 已验证正常，暂不因该例程改采样链路。
- 已增强 AI HTTP 400 错误显示：如果服务端响应中没有 `message/error_msg/detail`，固件会把响应 JSON 摘要带回 Web，避免只看到“状态异常：400”。
- 已给语音链路增加 ASR 空文本保护：ASR 返回空文本或极短文本时不再继续请求 AI，而是提示重新录音，避免兼容 API 因空 user content 返回 400。
- 语音链路和文字对话应保持一致：ASR 转写文本只作为另一种输入方式，进入 AI 后同样注入项目上下文、结构化记忆和设备侧短期会话历史。
- 语音链路已复用文字对话的设备历史裁剪逻辑 `convert_storage_history_to_ai_history()`，避免 user/assistant 顺序异常导致兼容 API 返回 400。
- 已确认一次真实 400 根因为：`messages[0].content: invalid unicode code point`，通常是固定长度缓冲或 `strlcpy` 按字节截断中文 UTF-8，导致半个汉字进入 AI 请求 JSON。
- AI 客户端 `json_escape_alloc()` 已增加 UTF-8 校验与清洗，所有 system/context/history/user 文本在出网前都会把非法 UTF-8 字节替换为 `?`，避免 OpenAI-compatible 服务拒绝解析请求体。
- 如果后续仍出现 400，优先查看 Web/串口返回的服务端错误摘要；常见方向是模型名不兼容、Base URL 不匹配、服务不支持 `max_tokens/temperature` 参数、或单次上下文仍然过长。

----

## 2026-05-23 Web Serial `get_status` 超时排查

- 现象：Web 面板发起 `get_status` 时偶发显示“请求超时”，但设备端串口并未表现出明显异常。
- 根因：`WebSerialTransport` 之前是先把 JSON 消息分发给前端消息处理器，再清理 `pending` 请求；一旦某个消息处理器抛异常，响应可能来不及完成匹配，看起来就像 `get_status` 超时。
- 修复：调整为先完成 `response -> pending` 的确认，再分发消息；同时给消息处理器加 try/catch，避免单个前端异常拖垮整条串口请求链路。
- 经验：`get_status` 这类基础命令应优先保证“先回包再渲染”，不要让 UI 事件处理路径反过来影响协议时序。
- 二次复测发现另一类根因：`sdkconfig` 里残留 `CONFIG_FRIDGE_SCREEN_TEST=y` 时，`app_main()` 会进入独立屏幕测试分支，只启动 `display_test`，不会初始化 NVS/Wi-Fi/传感器/音频，也不会调用 `fridge_usb_protocol_start()`，因此 Web 面板所有 USB JSON 命令都会超时。
- 修复后 `sdkconfig` 与 `sdkconfig.defaults` 都应为 `CONFIG_FRIDGE_SCREEN_TEST=n`；切换后必须执行 `idf.py reconfigure build`，确认 `build/config/sdkconfig.h` 不再定义 `CONFIG_FRIDGE_SCREEN_TEST`，再烧录。
- 真机验证：2026-05-23 已刷入正常 USB 运维固件，`COM16` 直发 `{"type":"request","request_id":"...","command":"get_status"}` 可收到 `ok=true` 响应；启动日志应出现 `usb_protocol: USB JSON Lines protocol task started`。

----

## 2026-05-23 Web 陀螺仪面板不变化排查

- COM16 直发 `get_sensors` 已验证 MPU6050 工作正常：`imuReady=true`、`imuError=0`、`WHO_AM_I=0x68`，并且 `accelXG/Y/Z`、`gyroXDps/Y/Z`、`pitchDeg`、`rollDeg` 都会返回真实数值。
- 如果 Web 面板看起来“陀螺仪没变化”，先不要直接判断为接线或 I2C 故障；需要确认传感器页是否展示了原始 `Gyro X/Y/Z` 字段，以及页面是否在高频刷新。
- 本次根因是 Web 传感器页之前主要展示 `angleDelta`、`vibrationPeak` 等聚合值，全局刷新间隔较慢，短时间晃动 MPU6050 时不容易看到原始陀螺仪变化。
- 修复：传感器页激活时单独以约 `300 ms` 间隔轮询 `get_sensors`，并增加 `IMU 就绪`、`WHO_AM_I`、`Accel X/Y/Z`、`Gyro X/Y/Z`、`陀螺仪模长` 等字段；使用独立 in-flight 标记避免高频轮询堆积串口请求。
- 验证：`npm run build` 通过；COM16 连续读取 `get_sensors` 通过，gyro 三轴均有读数。实测 Web 时应进入“传感器”页面后再轻晃/转动 MPU6050，观察 `Gyro X/Y/Z` 和 `陀螺仪模长` 快速变化。

----

## 2026-05-23 24G 雷达测试页调试认知

- COM16 实测确认 HMMD 24G 雷达当前可同时返回文本与上报快照：正常模式会回 `Range xx`，上报模式会回二进制帧并在 USB 协议里汇总为 `radar_test_status`。
- 实测上报帧尾不止一种写法，当前设备回包里既可能出现示例尾 `08070605`，也可能出现 `F8F7F6F5`，不能把尾帧写死成单一值。
- 实测 `presence=true` 只是模块原始上报，不应在 Web 上直接翻译成“冰箱前一定有人”，更适合展示为“模块上报有目标”。
- 距离门一共有 16 个原始门，但在调试页面里直接平铺 16 根柱子不够直观；更适合拆成近 / 中 / 远 三段区域，并单独保留原始门值用于排查。
- 2026-05-23 已将 Web 雷达页改为“模块上报 + 目标状态 + 近/中/远区域 + 原始门值 + 时间线”的组合展示，并通过 `npm run build` 验证。

----

## 2026-07-02 迁移 ESP32-P4 CB V1.2：接线红线与联网方式（来自厂商手册核对，未上电验证）

- 项目自本日起迁移到慧勤智远 ESP32-P4 CB V1.2 小系统板；本文件上方 2026-05 的所有条目均为旧 ESP32-S3 板经验，其中 GPIO 号、COM16、INMP441/MAX98357A 接线在新板上不成立，只能当问题模式参考。
- S3 最终代码存档：git 分支 `s3-legacy`、标签 `s3-final-20260702`（提交 `ea8d0e8`）。
- **屏幕直插红线（已对照两份手册核实）**：板上 20pin SPI 屏座 Pin1/2=5V、Pin3/4=GND；原 TR230S 屏 20pin FPC 的 Pin1=GND、Pin2=5V VCC。直插会把 5V 加到屏 GND，烧屏。只能做逐脚核对过的转接排线，且该座只有单根 MOSI/MISO，TR230S 只能跑 SPI 4-wire，QSPI 4SDA 物理不可行。
- **共享 I2C 红线**：GPIO28(SDA)/GPIO29(SCL) 同时接 MIPI-CSI 摄像头 SCCB 和 ES8311 编解码器，用户手册明确要求分时复用，不能同时使用；“拍照+语音”并存的功能必须先做总线串行化。
- **联网方式**：ESP32-P4 芯片无 Wi-Fi，板载 ESP32-C6-MINI-1 经 SDIO（GPIO47-52，`C6_WKUP`=GPIO53，`C6_EN`=GPIO54）提供 Wi-Fi；软件走 `esp_hosted` + `esp_wifi_remote` 管理组件（厂商 WiFi 例程 dependencies.lock 证实），且必须按手册 §2.2 配置 SDIO 电源管理。
- **启动敏感脚**：GPIO35(BOOT)、GPIO36（10K 上拉）是启动绑带脚；GPIO0/GPIO1 被 32.768KHz 晶振占用。
- **串口**：新板 CH343P USB 串口（UART0=GPIO37/38）+ 独立 USB-C JTAG 口，COM 号待首次接板后在设备管理器确认并回填本文件；COM16 属旧 S3 板，不要再用。
- 厂商资料位置：`doc/慧勤智远 ESP32-P4 CB V1.2小系统板/`（约 2.7GB，已 gitignore，禁止删除）；引脚分配以用户手册 V1.4 §1.5 为准。

----

## 2026-07-11 P4 第一阶段最小移植记忆（软件已构建，真机未验证）

- 当前 P4 最小固件版本为 `p4-minimal-20260711`，目标是板载 C6 Wi-Fi、板载 ES8311 麦克风、TR230S QSPI 静态色带和 TF 基础读写。完成 OTA 健康确认和 SDMMC 共享控制器修复后，ESP-IDF 6.0.1 从空 `build/` 目录干净构建通过，镜像 0xCB4B0（832,688 字节）；这不是硬件通过结论。
- P4 的 Wi-Fi 固定为 C6 + SDIO Slot1：CMD=47、CLK=48、D0-D3=49-52、C6_EN/复位=54，LDO4=3.3V；当前组件版本与厂商例程一致，为 `esp_hosted 2.2.3` / `esp_wifi_remote 0.15.0`。真机仍需核对 C6 slave 固件握手版本。
- TF 固定使用 SDMMC Slot0：D0-D3=39-42、CLK=43、CMD=44、GPIO45 低电平供电。P4 只有一个 SDMMC 控制器，TF 必须复用 hosted 为 Slot1 建立的 host，只新增/释放 Slot0，不能二次初始化或整体反初始化控制器。hosted 必须先成功持有 LDO4，TF 组件不能再次申请该可调 LDO；hosted 失败时本次启动跳过 TF。64GB 卡不自动格式化，只有 `confirm:"ERASE_TF_CARD"` 才允许清卡。
- 板载 ES8311：I2S0 MCLK=2、P4 DOUT=3、BCLK=4、ES8311 ASDOUT/P4 DIN=5、WS=6；I2C 为 SDA=28/SCL=29，PA_CTRL=11 必须保持低。GPIO28/29 与未来 OV5647 SCCB 共线，录音期间由统一 owner gate 独占，不能同时拍照。
- TR230S QSPI 飞线：CS=GPIO7、D0=8、SCLK=9、D1=10、WAIT=19、D2=20、D3=21、RST=22，先用 10MHz。屏幕原生 2×7 是 2.00mm，不能硬插 2.54mm 杜邦母头；该接口没有 FT6336U 直接 I2C，触摸默认关闭。
- P6 正面编号：板中文字正向、P6 位于下方时，靠板边为奇数排、靠 MCU 为偶数排，左侧方形焊盘是 P6-1；背面视角镜像。显示接线完整表见 `doc/P4第一阶段最小移植与接线方案.md`。
- 最新屏幕方案取代 2026-07-02 的“板载 SPI 座 4-wire”临时想法：为了复用既有 QSPI 驱动，本阶段从 P6 GPIO 飞线到屏幕原生 2×7 或独立 FPC 转接板。TR230S 20-pin FPC 仍严禁直插板载 20-pin SPI 座。
- 串口启动协议先发 `booting`，只允许静态 `get_pins/get_logs`；所有首阶段外设完成一次初始化尝试后再发 `ready` 并开放硬件命令。网络/hosted 首次初始化失败后，本次启动禁止重入，修正原因后必须重启。
- 已启用 bootloader OTA rollback；主固件只有在首阶段初始化走完且 USB 控制台健康门通过后，才调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认 `PENDING_VERIFY` 镜像，防止第二次复位被误回滚到 recovery。
- 当前未识别到 P4 CH343P 新 COM 口；旧 COM16 只属于 S3。没有烧录、没有给屏幕上电、没有格式化 TF，也没有任何 P4 真机“通过”记录。
- 构建命令继续使用：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 build`。烧录前必须先确认实际 COM 号，且 Web Serial/monitor/串口助手不能同时占用。

----

## 2026-07-11 TR230S 改为 SPI-4WIRE 并启用 FT6336U（软件已构建，真机未验证）

- 本节取代上一节的 QSPI/2×7 屏幕方案。当前固定使用 **20-pin FPC 转独立杜邦线**，开发板板载 20-pin SPI 屏座必须保持空置。
- TR230S SPI-4WIRE：CS=GPIO7、MOSI=GPIO8、SCLK=GPIO9、MISO=GPIO10、WAIT#=GPIO19、D/C#=GPIO20、RESET#=GPIO22；GPIO21 已释放。SPI Mode 0，10MHz 起步。
- FT6336U：TP-INT=GPIO15、TP-SCL=GPIO16、TP-SDA=GPIO17、TP-RST=GPIO18，I2C0 100kHz，实物已知地址 0x48。SDA/SCL 外部上拉到 3.3V；FPC pin19 TP-VCC 按规格保持悬空。
- 显示驱动已移除 QSPI 的 0x02/0x12 封包和 QIO 事务，改为 D/C# 区分命令与数据；软件手动控制 CS，确保 0x2C 到最后一块 RGB565 像素期间片选连续。
- `CONFIG_FRIDGE_P4_TOUCH_ENABLE=y` 已写入 defaults 和当前 sdkconfig。P4 最小固件只打印触摸坐标，完整 S3 LVGL UI 尚未迁移。
- `idf.py reconfigure build` 通过；镜像 0xCBEF0（835,312 字节），SHA-256 `0714C46693409AF143CB18FE4367194D3A08CFFD0926990758759E72D76405D9`，最小 2MiB app 分区剩余约 60%。这不是屏幕或触摸真机通过结论。
- 权威接线表：`doc/P4_TR230S_SPI4WIRE_FT6336U接线方案.md`。

----

## 2026-07-11 P4 COM15 首次烧录与真机启动结果

- 新板串口已确认：`COM15`，Windows 设备为 `USB VID_1A86&PID_55D3`（CH343 系列）；esptool 识别芯片为 ESP32-P4 rev v1.3，MAC `80:f1:b2:d2:04:85`。旧 COM16 仍只属于 S3。
- IDF 6 默认生成的 P4 rev v3.1+ 固件与本板 v1.3 不兼容；已在 `sdkconfig.defaults.esp32p4` 固定 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`、最低 rev v1.0，并按板卡标称/厂商例程把 CPU 从不保证稳定的 400MHz 改为 360MHz。
- CH343 在 esptool RAM stub 阶段会出现 `Invalid head of packet (0xE0)`，460800 和 115200 均可复现；改用 ROM loader `--no-stub`、115200 后烧录成功，四个写入区域均通过 Hash 校验。
- 本次固件 `build/fridge_spirit.bin` 大小 833,168 字节，SHA-256 `D9666D9B8CE32BC1533A759FC59FBF726ABF3B75C14EEAF18F56BE521B85F156`。
- 启动验证：CPU 360MHz；32MB HEX PSRAM 200MHz 检出且 memory test OK；16MB Flash 正常；无 brownout、watchdog 或 panic。
- C6/SDIO：Slot1 GPIO47-52、40MHz 初始化成功，识别 ESP32-C6 并启动 Wi-Fi STA；Host 端 esp_hosted 比 C6 协处理器固件新，日志有版本警告，后续需要匹配 C6 slave 固件。
- TR230S：SPI-4WIRE 引脚与 10MHz 配置正确，日志显示 `TR230S display ready` 且静态色带刷新完成；屏幕实际画面仍需用户肉眼确认。
- FT6336U：0x48 读取芯片 ID 返回 `ESP_ERR_INVALID_RESPONSE`，触摸未通过。优先核对 FPC16 SDA→GPIO17、FPC17 SCL→GPIO16、FPC18 RST→GPIO18、FPC20 GND，以及 SDA/SCL 各 4.7k 上拉到 3.3V；FPC19 TP-VCC 继续悬空。
- ES8311：I2C1 GPIO28/29 和 I2S0 初始化成功，板载麦克风进入 16kHz/16-bit/mono ready 状态，PA 保持关闭。
- TF：64GB 卡检测路径运行，但 FAT 挂载失败并返回 `formatRequired=true`；没有执行格式化。只有用户明确确认整卡数据可清空后，才能发送 `ERASE_TF_CARD`。

----

## 2026-07-11 TR230S 恢复 S3 已验证的 QSPI-4SDA

- 本节取代同日的 SPI-4WIRE 显示方案。用户确认恢复 S3 点亮方式后，P4 显示驱动重新使用 TR230S 已验证的 `0x02` 寄存器写封包、`0x12` 显存写封包和 `SPI_TRANS_MODE_QIO` 四线像素传输。
- P4 QSPI 接线固定为：CS=GPIO7、D0=GPIO8、SCLK=GPIO9、D1=GPIO10、WAIT#=GPIO19、D2=GPIO20、D3=GPIO21、RESET#=GPIO22；FPC pin14 D/C# 必须悬空。FT6336U 继续为 INT=15、SCL=16、SDA=17、RST=18。
- 屏幕 20-pin FPC 仍严禁直插板载 20-pin SPI 座；只能使用独立 FPC 转杜邦板逐线连接。权威表为 `doc/P4_TR230S_QSPI_FT6336U接线方案.md`，旧 SPI-4WIRE 文档已标记停用。
- QSPI Mode0 继续以 10MHz 起步。ESP-IDF 6.0.1 构建通过，镜像 833,440 字节，SHA-256 `BB6CAD6BA6F4B6AD75497BE09CF626E0467328EAF94F537BE5EDDC2046FE261C`；尚未烧录，因为必须先断电把实物排线由 SPI4 改成 QSPI，避免固件与接线错配。
- 2026-07-11 19:53 补充真机结果：COM15 使用 ROM loader `--no-stub -b 115200` 烧录成功，四个区域均通过 Hash 校验。启动日志确认 QSPI 引脚、10MHz、TR230S 初始化和静态色带刷新完成；FT6336U 也已读到地址 0x48、chip 0x64、vendor 0x11。屏幕画面仍需要用户肉眼确认。
- 同次启动 C6 SDIO 卡枚举成功但 hosted slave 未就绪，随后重试路径重复申请 LDO4 失败，网络初始化最终返回 ESP_FAIL，TF 因依赖 hosted/LDO4 被安全跳过；该问题与屏幕 QSPI、触摸无关，后续单独修复。

----

## 2026-07-11 P4/C6 ESP-Hosted 2.12.11 真机稳定结论

- 本节取代同日早先的 C6 版本不匹配和握手失败待办。P4 Host 与板载 C6 Slave 已统一为官方 `esp_hosted 2.12.11`，COM15 的 `get_network` 当前应返回 `hostedReady=true`、`c6OtaGuard=false`、两端版本均为 `2.12.11`。
- SDIO LDO4 只允许申请一次并在整板生命周期内保留，因为 C6 Slot1 与 TF Slot0 共用该电源域；禁止在 Hosted read task 存活时原地清空或重枚举 card。现场恢复统一发送 `retry_hosted`，由固件延迟 500ms 整板重启。
- C6 可经 P4 JSON Lines 执行 APP-only 流式 OTA。工具为 `tools/c6_hosted_ota.py`，固定单块 1024 字节，先用 esptool 校验 C6 镜像，再校验总长度、SHA-256、逐块 CRC32；支持匹配会话断点续传。旧 0.0.6 使用 `legacy_end_reboots`，升级后 2.12.11 使用显式激活能力。
- C6 2.12.11 镜像已真机写入并启动：大小 0x130EC0，SHA-256 `30FC480E297917F319AAFEA5533EAAE848E9173656608C9717269F9697B3B502`，可装入旧分区表 0x180000 OTA slot。不要把 bootloader/partition table 当 APP-only 镜像发送。
- 稳定性实测为连续 3 轮整板重启 + Hosted 重新握手 + Wi-Fi 扫描，均成功，热点数分别 11/14/8。若后续再次出现 Hosted 故障，先看 `hostedLastError`、brownout 和 5V 供电，再使用 `retry_hosted`，不要恢复旧的热重试路径。
- Web 管理面板现在可以通过 COM15 配置 Wi-Fi：USB 连接后进入网络页，点击“扫描”，选 2.4GHz SSID、输入密码并点“连接”。真实 AP 扫描已验证；当前设备 NVS 无保存凭据，实际认证、DHCP 和外网状态要在用户提供正确热点密码后验证。
- 2026-07-11 23:48，64GB TF 已经用户明确授权后格式化成功并挂载为 FAT32：`/sdcard`，容量 59,638 MB、空闲 59,623 MB；写入—读回自检文件 `/sdcard/fridge_selftest.txt` 校验通过。后续不要再把该卡标记为 `formatRequired`，也不要无确认口令重复格式化。

----

## 2026-07-12 TF 卡存储工作台真机结论

- COM15 当前固件已具备完整 TF 文件管理命令族、回收站、UTF-8/CP936 长文件名、1024 字节 CRC32 分块传输、NVS 专家模式、Slot0 安全卸载/挂载和一次性 token HTTP 服务。
- 真机已验证中文目录/文件、4096 字节二进制上传下载及 Range 跳读、移动、回收/恢复、精确确认保护、自检、64 KiB 测速和诊断包导出；测速结果约写 1954 KiB/s、读 6458 KiB/s。
- 安全卸载/重新挂载 TF 后 C6 Hosted 仍为 2.12.11 ready，没有重复申请 LDO4。当前 TF 容量仍为 59,638 MB，未重复格式化。
- HTTP 只允许 localhost/127.0.0.1 Origin、固定 transfer session、固定路径/方向和十分钟一次性 Bearer token；无 Wi-Fi 时真机已验证安全拒绝。设备尚无 Wi-Fi 凭据，真实 HTTP 吞吐待配网后测试。
- 最终烧录镜像为 1,150,480 字节，SHA-256 `457ED52E42C8A7506C87915BA53B329DB65BB41D66A323CF838B97485FEFF035`；COM15 使用 `--no-stub -b 115200` 写入并通过全部 Hash 校验。
- Web 工作台入口为左侧“TF 卡”，支持文件/回收站/维护、拖放、预览、编辑、传输队列暂停/继续/取消和诊断导出。使用说明见 `doc/TF卡存储工作台使用说明.md`。

----

## 2026-07-12 P4 microWakeWord 接入记忆

- P4 rev1.3 的 ESP-SR `esp32p4_less_v3` 核心是预编译 `libwakenet.a/libesp_audio_front_end.a`，在 IDF 6.0.1/GCC 15 下即使绕过官方检查仍会出现 RISC-V relocation 链接失败；不要再次尝试通过删检查或 `--no-relax` 修复二进制 ABI。
- 当前唤醒后端改为完全从源码构建的 microWakeWord：`esp-tflite-micro 1.3.3~1`、`esp-nn 1.1.2`、`esp-micro-speech-features 1.2.3`。输入仍是板载 ES8311 的 16kHz/16-bit/mono PCM，和普通录音/摄像头共享资源约束不变。
- 当前固件模型是英文 `Okay Nabu` v2 启动验证模型，不是“小冰小冰”；Web/串口必须看到 `targetModelReady=false`。只有通过 `tools/microwakeword/install_model.py ... --target-ready` 安装 manifest 中精确写为“小冰小冰”的模型后，才能标记目标模型就绪。
- 模型固定嵌入 `components/wake_word/models/fridge_wake_word.tflite`；不再生成或烧录 `srmodels.bin`。旧 2MiB `model` 分区暂时保留，后续重排 OTA/存储时再单独处理。
- P4 esp-nn 推理任务栈固定 8192 字节；tensor arena 至少 64KiB并放 PSRAM。`wake_status` 的 `inferenceCount/lastInferenceUs/tensorArenaUsed` 是首要真机判据，不能仅凭“listening”判断模型正常。
- 训练与部署权威说明为 `doc/microWakeWord小冰小冰模型训练与部署.md`。板载数据采集用 `tools/microwakeword/collect_samples.py`，模型安装必须用 `install_model.py`，不要手工只替换 `.tflite` 而忘记 cutoff/window/arena。
- 本轮构建通过但未烧录：检查时 COM15/CH343P 不在线，仅有蓝牙虚拟串口。重新接板后先用启动模型说 “Okay Nabu” 完成运行时验证，再投入中文训练和长稳误触发测试。


----

## 2026-07-13 ESP-IDF 降级到 5.5.1 恢复 WakeNet“小冰小冰”（真机已验证核心链路）

- 主固件已从 IDF 6.0.1 降到 **5.5.1**，撤销 microWakeWord，恢复 **ESP-SR WakeNet less-v3**。IDF6 保留可回退。详见 `doc/IDF5.5迁移与已验证版本.md`。
- 精确版本：**ESP-IDF 5.5.1（riscv GCC 14.2.0）+ esp-sr 2.4.5**，WakeNet 库路径 **esp32p4_less_v3**（esp-sr CMake 在 IDF 5.x 且 PATCH<3 时自动选中；5.5.1 的 PATCH=1 恰好锁定，5.5.3+ 会误选 rev3.0 库，不要升 5.5.3+）。esp_hosted 2.12.11 / esp_wifi_remote 0.15.0 不变。依据：厂商 `小智AI` 例程 `wks_esp32p4_cb_xiaozhi` 锁 IDF 5.5.1。
- **不要再在 IDF6 下试图链接 WakeNet less-v3**：预编译库是 GCC14.2 编译的，IDF6/GCC15 必现 `R_RISCV_RVC_LUI relocation truncated`，删检查/`--no-relax` 无效。
- 工具链：`D:\esp-IDF\v5.5.1\esp-idf`（clone v5.5.1）；公共工具目录仍 `D:\esp-IDF\.espressif`；`tools/idf.ps1` 已指向 5.5.1。构建/烧录仍先设 `IDF_TOOLS_PATH=D:\esp-IDF\.espressif` 再 dot-source export.ps1。
- 迁移必须的 4 个配置坑（都不动硬件/分区/C6 Slave）：
  1. `main/CMakeLists.txt` 显式加 `esp_psram`——否则 `set(COMPONENTS main)` 最小闭包下 esp_psram 是可选依赖不纳入，CONFIG_SPIRAM 整段未注册、32MB PSRAM 关闭。
  2. `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`——5.5.1 把 P4 PSRAM `SPIRAM_SPEED_200M` 归为实验特性，不开则 200M 选项不可见、回落 20MHz。
  3. `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`——修复 hosted 启动崩溃 `assert sdio_mempool_create sdio_drv.c:255 (buf_mp_g)` / `mempool create failed: no mem`。根因：SDIO 缓冲池需约 48KB 片内 DMA 内存，IDF6 给 hosted 挂了约 120KB RETENT_RAM 堆、5.5.1 不再暴露，启动早期片内 DMA 不足；改从 DMA 可达的 PSRAM 分配即可。
  4. `codec_es8311/idf_component.yml` 的 idf 依赖从 `>=6.0.0,<7.0.0` 放宽到 `>=5.5`。
- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` 在 5.5.1 里是未知符号、仅一条无害告警（该硅片选择符号是 IDF6 才引入的；less_v3 已由 esp-sr 的 PATCH<3 逻辑自动选中）。
- AFE 唤醒监听会持续打印 `Ringbuffer of AFE is empty`（AFE 增强线程比 32ms 喂帧节拍快，两帧间的正常提示，不影响检测）。已在 wake init 用 `esp_log_level_set("AFE", ESP_LOG_ERROR)` 抑制，避免刷爆 USB JSON。
- 真机（COM15，rev1.3，MAC 80:f1:b2:d2:04:85）已验证：PSRAM 32MB@200MHz OK、CPU360、C6 Host/Slave 2.12.11 hostedReady、Wi-Fi 连 eric_2G 拿到 192.168.31.228、TF /sdcard 59638MB 未格式化、FT6336U 0x48 读到、ES8311 麦克风实时喂帧、WakeNet 加载 wn9_xiaobinxiaobin_tts 且 `wake word ready: 小冰小冰`、NVS 持久唤醒重启后自动 listening、唤醒开关幂等。无 brownout/wdt/panic。
- 仍未验证：真人说“小冰小冰”触发、5 秒录音→SiliconFlow ASR（需 ASR API Key）、20 次长稳/误触发。
- **屏幕本次上电失败**：`fridge_display: WAIT# stayed low ... ESP_ERR_TIMEOUT`。属 TR230S QSPI 飞线/WAIT#/供电/FPC 方向的硬件问题，与 IDF 迁移无关（显示代码未改，2026-07-11 IDF6 下同代码曾 `TR230S display ready`）。复测前先断电核对屏线。
- 最终镜像：`fridge_spirit.bin` 1,873,728B SHA-256 `496c8179657694e8b08a81b1397d93b457e6ac167f672e9b8354c65ab6bfd29e`；`srmodels.bin` 579,120B SHA-256 `baab09bb3822c45bb25eb7cd4889fdb526d033b957b4610a226569557fe96443`。烧录仍用 ROM loader `--no-stub -b 115200`，五区 Hash 通过。

----

## 2026-07-13 WKS70WSV114-WCT MIPI-DSI 真机结论

- 当前 7 寸屏已在 COM15 真机确认正常循环显示红、绿、蓝、白、黑和 RGB565 彩条。屏参数固定为 EK79007、1024x600、RGB565、2-lane、700Mbps/lane、DPI 50MHz，HBP/HFP/HSW=160/160/20、VBP/VFP/VSW=23/12/10；LCD_RST=GPIO22、LCD_BL=GPIO23、DSI PHY LDO3=2.5V。官方 26-pin 排线走线未改，也不应改。
- ESP-IDF 5.5.1 + ESP32-P4 rev1.x 的 DPI/DW-GDMA 首次启动可能只产生一次 `on_refresh_done`，表现为首帧后超时；厂商原始 7 寸驱动在本板上也能复现，因此不能再把该现象直接判定为排线或 EK79007 参数错误。
- 已验证的恢复方法是：背光关闭时调用 `esp_lcd_dpi_panel_set_pattern(..., MIPI_DSI_PATTERN_BAR_VERTICAL)`，等待 40ms，再切回 `MIPI_DSI_PATTERN_NONE`。这会重新打开 DPI bridge，使 DMA 连续刷新恢复；主驱动已在初始化首帧前自动执行，用户看不到诊断图案。
- 排查优先级：若日志没有 `DSI video bridge rearmed`，先查软件初始化；若有 rearm 和 `MIPI-DSI ready` 但屏无图，再查 5V 屏电源、FPC 锁扣/接触和背光；不要改 lane 走线或猜 GPIO。官方 DSI Pattern Generator 能绕过 PSRAM/DMA，可用于区分物理链路与帧缓冲问题。
- 已验证启动日志：`tmp/logs/20260713-2008_mipi_bridge_rearm_main_com15.log`。该日志完成两轮图案且无 refresh timeout、panic、brownout、watchdog、PSRAM 错误或 DSI underrun。

----

## 2026-07-21 WKS70WSV114-WCT GT911 触控驱动对齐（软件已构建，真机待烧录验收）

- 目标屏：WKS70WSV114-WCT（EK79007 1024x600 + GT911），26-pin DSI 座触摸线：INT=GPIO15、SCL=GPIO16、SDA=GPIO17、RST=GPIO18，逻辑 3.3V。
- 自研 ridge_touch_gt911_p4.c 已按厂商 comprehensive_routine_70inch_mipilcd 的 gt9xxx 行为对齐：优先 I2C 地址 **0x14**（备选 0x5D）、**400kHz**（失败回退 100kHz）、RST 低/高各 10ms 后再 settle、读 PID 后写 **0x8040** 软复位、触点从 **0x8150** 读 4 字节 XY（不做 EK79007 镜像翻转）。
- USB 最小协议新增 	ouch_status、	ouch_scan；get_pins 触摸说明改为 GT911。UI 前 8 次按下会 INFO 打坐标。
- 构建通过：ridge_spirit.bin 2680960 字节，SHA-256 E931BAA2A14101EC3DC40E054C16F2394C730FA0B1DB45ABD32746B97786F667；既有 ota_1 2MiB overflow 警告不变。
- 本机烧录时 COM15 不存在，COM17（USB-Serial/JTAG）被占用（PermissionError）；**未完成真机烧录**。请释放串口后执行：	ools\idf.ps1 -p COMxx flash 与 monitor，确认日志 GT911 ready addr=0x14 product=911，再用 	ouch_scan/	ouch_status 与软键盘点按验收。
- 备份：	mp/backups/manual/20260721-011918_wks70_gt911_touch/。

----

## 2026-07-23 SHT40 共享 I2C0 接入经验

- SHT40 温湿度模块挂在 GPIO16（SCL）/ GPIO17（SDA），与 GT911 触摸共用 I2C0 总线。
- **红线**：整条 I2C0 只能有一个 bus handle。SHT40 驱动通过 `i2c_master_get_bus_handle(I2C_NUM_0, &bus)` 复用 GT911 已建总线，**不得再调用 `i2c_new_master_bus`**。
- `fridge_sht4x_init()` 必须在 `fridge_touch_gt911_init()` 之后调用，否则 `i2c_master_get_bus_handle` 返回 `ESP_ERR_INVALID_STATE`。
- SHT40 命令集与 SHT30 不同：测量命令 `0xFD`（单字节，无 clock stretching），等待 ≥10ms，读 6 字节，CRC-8 poly=0x31 init=0xFF。
- 地址默认 0x44，ADDR 引脚接 VDD 时为 0x45；驱动按顺序探测两个地址。

----

## 2026-07-23 Codex 截屏调用链验证

- 当前 P4 USB-Serial/JTAG 实机端口为 `COM17`。WebUI 首页的“截取屏幕”按钮已由用户实测成功；Codex 使用 `tools\capture_screenshot.ps1` 可复用同一套 `screenshot_capture` + `tf_transfer_*` 协议。
- **串口控制线经验：** P4 原生 USB-Serial/JTAG 需要 `DTR=false、RTS=true` 才能稳定把主机输入交给固件 `stdin`；`RTS=false` 时端口虽然能打开，但脚本收不到任何响应。脚本已固定该状态，不能简单改成 DTR/RTS 全低。
- Codex 入口默认命令：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1`。默认输出基名为 `tmp\cache\screenshot_test\codex_latest`，按设备实际格式落为 `.jpg` 或 `.bmp`；最后 JSON 的 `local_path` 是图像查看路径。
- 2026-07-23 实机结果：设备返回 `.fridge_spirit/exports/shot_102.jpg`，JPEG；通过 TF 分块读回 101251 bytes，CRC32、JPEG 文件头均通过，`tmp\cache\screenshot_test\codex_latest.jpg` 已用图像查看器确认内容为当前 1024×600 首页，无花屏、截断或明显色彩损坏。
- WebUI、Codex 脚本、ESP-IDF monitor 或其他串口工具不能同时打开 `COM17`；切换调用方前必须先断开上一个串口读者。摄像头忙时 BMP 降级路径尚未单独完成实机验证。
