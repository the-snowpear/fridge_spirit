# AGENTS.md
总提示：如果在开发一个新功能，允许先从github上找已经成熟的开源项目，把源码拉到项目example文件夹，以供参考。
允许自动使用插件和skills
允许使用subagent来提升工作效率
已安装 `esp32-firmware-engineer` skill；在本项目中，凡是涉及 `ESP32`、`ESP-IDF`、`FreeRTOS`、外设驱动、分区表、OTA、串口日志、崩溃排查、LVGL 显示集成、构建/烧录/监视器工作流等任务，允许自动调用该 skill。
工作前必须先阅读 `doc/memory.md`，其中记录了本项目已验证的硬件接线、串口占用、编码问题和联调踩坑；如果该文件不存在或无法按 UTF-8 读取，需要先说明风险再继续。

## 备份工作流

- 任何会修改仓库内文件的操作，在编辑前必须先做备份。
- 备份统一放在项目根目录 `tmp/` 下，禁止继续把备份散落在源文件旁边或直接堆在 `tmp/` 根目录。
- 备份目录统一放到 `tmp/backups/` 下，并按用途分层：
  - `tmp/backups/manual/`：人工修改、AI 协作修改前的文件备份。
  - `tmp/backups/before_run/`：构建、烧录、批量脚本、配置切换前的预备份。
  - `tmp/backups/archive/`：历史零散备份归档，不再继续向其中写入新的临时内容。
- 每次备份必须单独创建一个目录，目录名格式统一为 `YYYYMMDD-HHMMSS_事项名`，事项名使用简短英文、数字或下划线，例如 `tmp/backups/manual/20260523-163000_usb_protocol_refactor/`。
- 同一次修改涉及多个文件时，优先放进同一个备份目录，并尽量保持原始相对路径，便于对照和回滚。
- 备份文件命名优先保持原文件名，不再新增无规则的 `.bak-1`、`final2`、`new`、`临时` 这类命名。
- 如果只是为了保留编辑前版本，也优先复制到对应备份目录，不再默认把 `.bak-时间戳` 直接写到工作目录或 `tmp/` 根目录。
- `tmp/` 根目录只保留少量分类目录，例如 `backups/`、`logs/`、`notes/`、`cache/`；新增临时文件前要先判断应该放入哪个分类目录。
- 调试日志统一放在 `tmp/logs/`；过程说明、临时提示词、排查笔记统一放在 `tmp/notes/`；确实可丢弃的中间缓存才放 `tmp/cache/`。
- 涉及中文内容的备份文件、说明文件和目录清单，必须继续按 UTF-8 读写，避免把乱码再次带入备份链路。
- 如果某次任务需要清理或整理 `tmp/`，默认只做“归类移动和重命名”，不要擅自删除历史备份；如确需删除，必须先征求用户确认。

## 项目概览

这是一个基于 ESP-IDF 的智能冰箱贴项目，作品名称为“冰箱小精灵”。项目目标是构建一个可贴附在普通冰箱上的 AIoT 厨房助手，实现屏幕交互、传感器融合、拍照识别食材、本地库存缓存、临期提醒、菜谱推荐、购物清单和 OTA 运维。

**⚠️ 2026-07-02 起项目正在从 ESP32-S3-DevKitC-1 N8R8 迁移到“慧勤智远 ESP32-P4 CB V1.2”小系统板。**
迁移决定和分工见 `CLAUDE.md` 的 “Board migration in progress” 章节。S3 最终代码已在 git 存档：分支 `s3-legacy`、标签 `s3-final-20260702`；`main` 分支此后只做 P4。旧文档、旧注释、`doc/memory.md` 2026-05 条目里的 GPIO、COM16、INMP441/MAX98357A 接线都属于 S3 旧板经验，在新板上不能照抄，只能当“问题模式”参考。

当前仓库状态：

- ESP-IDF 工程目标芯片：`esp32p4`（迁移中，`idf.py set-target esp32p4` 与 sdkconfig 重建尚未完成前，仓库内可能仍残留 `esp32s3` 生成配置，动手前先核对）。
- 当前主线基于已验证的 `ESP-IDF 5.5.1`（riscv GCC 14.2.0）+ `esp-sr 2.4.5`；P4 rev1.3 的 WakeNet less-v3 预编译库与 IDF 6/GCC15 不兼容。IDF 6.0.1 仅保留作回退，不得作为主线构建环境。
- 当前主固件以 `C` 语言为主，使用 `ESP-IDF`、`FreeRTOS`、`CMake` 和 `Kconfig`。
- 仓库同时包含一个本地运维前端 `web/`，使用 `TypeScript + React 19 + Vite`，用于 USB/Web Serial 调试面板。
- 当前源码已经不再是空白工程：`main/main.c` 已有主控启动编排，`components/` 下已有完整组件集（network、camera、audio、speaker、asr、wake_word、sensors、radar、ui、storage、usb_protocol 等），这些组件的硬件层都需要按 P4 板逐个移植。
- 新板的用户手册、原理图和厂商例程在 `doc/慧勤智远 ESP32-P4 CB V1.2小系统板/`（约 2.7GB，已 gitignore，只存本地，禁止删除）。
- 已新增架构文档：`doc/项目架构与工作流设计.md`。
- `partitions_recovery.csv` 与 `sdkconfig.defaults` 目前仍是 S3 N8R8（8MB Flash）时代的内容，必须按 P4 板重建：`16MB Flash（W25Q128）+ 32MB 片内 PSRAM + 大主固件 ota_0 + 小 recovery ota_1 + 本地数据分区`；分区角色和命名保持不变，不能回退到单 app 或无本地 recovery 的纯云端恢复思路。

## 工程技术栈与编程约定

- 主控固件主语言：`C`。
- 构建系统：`CMake + ESP-IDF`。
- 实时系统：`FreeRTOS`。
- 配置方式：`sdkconfig`、`sdkconfig.defaults`、组件级 `Kconfig`。
- 推荐目录：`main/` 只放启动编排，功能实现优先放到 `components/`。
- 前端工具面板语言：`TypeScript`。
- 前端框架：`React 19`。
- 前端构建工具：`Vite`。
- 文档与设计说明：优先使用 `Markdown`。
- 代码注释语言：默认中文，尤其是硬件、时序、电源、GPIO、总线和存储相关部分必须写中文注释。
- 代码风格：在满足功能和硬件安全的前提下，优先保持实现简单、短小、易维护；避免为了“看起来完整”提前堆入复杂抽象。
- 依赖控制：新增第三方库前要先说明用途、资源占用和对 Flash/PSRAM/启动时间的影响；能用 ESP-IDF 原生能力解决时优先不用额外依赖。
- 硬件相关提交原则：凡是涉及 GPIO、供电、时钟、Flash、PSRAM、分区表、OTA、I2C、SPI/QSPI、I2S、背光和蜂鸣器的改动，都必须同步检查硬件风险和测试项。

## 进度记录规则

- 项目进度统一记录在 `doc/progress.md`。
- 当出现“阶段性完成”的较大进展时，新增一个以 `----` 分隔的区块，并写明完成时间。
- 大进展应记录：完成内容、涉及模块、当前结果、必要的风险或下一步。
- 小修改、小修复、命名调整、注释补充这类低影响改动，不单独新开 `----` 分隔块。
- 小修改默认追加到最近一个相关大进展区块下，作为补充条目记录即可。
- 如果某次改动同时包含“大进展 + 若干小修复”，只为大进展新开分隔块，小修复写在该块下面。
- 后续 AI 或协作者在完成明显里程碑后，应自动追加 `doc/progress.md`，并保持时间顺序一致。

## 重要文档

- `doc/慧勤智远 ESP32-P4 CB V1.2小系统板/1. 用户手册/ESP32-P4 CB V1.2_用户手册_V1.4.pdf`
  - **当前目标板的权威硬件资料**：§1.5 引脚分配表、§2 硬件电路（含 C6 SDIO 电源管理配置、共享 I2C 说明）。改任何引脚前必读。
- `doc/慧勤智远 ESP32-P4 CB V1.2小系统板/3. 程序源码/`
  - 厂商例程（基础例程/WIFI例程/综合实验/LVGL，均为 zip，基于 IDF 5.5.x）；移植 P4 外设时优先解压对应例程参考，API 以本工程锁定的 5.5.1 为准。整个目录约 2.7GB，已 gitignore，禁止删除。
- `doc/冰箱小精灵_本科物联网竞赛完整方案书_V1.1.docx`
  - 项目完整方案书，包含需求、硬件、端侧固件、云端、移动端、数据模型、测试和风险（写于 S3 时期，硬件细节以本文件和 CLAUDE.md 的 P4 章节为准）。
- `doc/屏幕技术文档Specification_TS040HDS02CP-B1620A_V1.2(1).pdf` 与 `doc/屏幕模组_TS040HDS02CP-B1620A_规格整理.md`
  - 原 TR230S 屏技术手册；做转接排线时必须对照其 20pin FPC 引脚定义逐脚核对。
- `doc/项目架构与工作流设计.md`
  - 当前推荐架构和工作流，后续实现应优先参考。
- `doc/progress.md`
  - 项目进度记录。后续较大里程碑完成后需要自动追加，格式遵循本文“进度记录规则”。
- `components/radar/README.md`
  - 24G 人体雷达组件快速说明，优先看这里理解 UART 接线、上报模式、字段语义和当前判定链，再进入 `components/radar/fridge_radar.c`。
- `components/speaker/README.md`
  - MAX98357A 扬声器与云端 TTS 测试记录（S3 时期接线已过时，TTS 结论仍有效），包含 SiliconFlow TTS 400 根因、音色格式、开头杂音判断和后续排查顺序；修改扬声器、TTS 或 Web 扬声器测试页前必须先读。

## 主要硬件设备

### 主控

- 设备：慧勤智远 `ESP32-P4 CB V1.2` 小系统板（主控芯片 `ESP32-P4NRW32`，双核 RISC-V 360MHz）
- 板载资源：16 MB W25Q128 QSPI Flash + 32 MB 片内 PSRAM；板载 `ESP32-C6-MINI-1` 模组（SDIO 连接主控，提供 Wi-Fi 6 / BT5）；ES8311 音频编解码器 + 驻极体 MIC + NS4150B 功放；MIPI-DSI 26pin 屏座、20pin SPI 屏座、MIPI-CSI 15pin 摄像头座；TF 卡、CAN、RS485、USB-C 串口（CH343P）、USB-C JTAG、USB OTG。
- 职责：屏幕 UI、触摸、摄像头采集、传感器融合、本地库存缓存、MQTT、OTA、状态机、诊断日志。
- **注意：ESP32-P4 芯片本身没有 Wi-Fi**，联网必须通过板载 ESP32-C6，走 `esp_hosted` + `esp_wifi_remote` 组件（SDIO），并按用户手册配置 SDIO 电源管理。
- 权威接线依据：`doc/慧勤智远 ESP32-P4 CB V1.2小系统板/1. 用户手册/ESP32-P4 CB V1.2_用户手册_V1.4.pdf` §1.5 引脚分配表和 §2 硬件电路；写任何引脚号之前先查这份表。

### 屏幕

屏幕方案**尚未最终确定**（2026-07-02 用户决定）：

- 近期验证方案：沿用原 `TS040HDS02CP-B1620A`（TR230S 驱动，4.0 寸 720x720，FT6336U 触摸），通过**转接排线**接到板上 20pin SPI 屏座，只能跑 SPI 4-wire 模式（该座只有单根 MOSI/MISO，物理上走不了 QSPI 4SDA）。
- **严禁把 TR230S 的 20pin FPC 直接插进板上 20pin SPI 座**：板座 Pin1/2 是 5V、Pin3/4 是 GND，而 TR230S FPC 的 Pin1 是 GND、Pin2 是 5V VCC，直插会把 5V 加到屏的 GND 上烧屏。转接排线必须逐脚核对并经用户确认后才能上电。
- 后续候选方案：慧勤智远 MIPI-DSI 屏（26pin 座，支持 5.0/5.5/8.0/10.1 寸；DSI 座的复位信号被板上电阻分压到 1.8V）。
- 屏幕供电 VCC 5V、逻辑电平 3.3V 的约束继续有效。LVGL 必须使用局部刷新和行缓冲，避免大分辨率全帧频繁刷新导致内存和总线压力过大。

### 摄像头

- 设备：`OV5647` MIPI-CSI 摄像头模组（板上 15pin CSI 座；原 OV3660 DVP 方案退役，P4 板没有 DVP 接口）。
- 职责：食材拍照、JPEG/帧缓冲管理、HTTPS 上传任务、拍照状态和错误上报。
- 引脚与总线约束：CSI 数据/时钟走专用 MIPI 引脚；`CSI_RST`=GPIO26、`CSI_PWDN`=GPIO27；SCCB 走 GPIO28/GPIO29 的共享 I2C。**该 I2C 总线同时接 ES8311 音频编解码器，用户手册明确要求两者分时复用，不能同时使用**——拍照与语音并存的业务必须对这条总线做串行化（单一持有者/互斥锁），并把该约束写进相关组件头文件注释。
- 参考例程：厂商 `basic__routines` 中的 `17_mipicamera`、`22_photograph`。

### 传感和交互外设

- 光照传感器：BH1750 或光敏电阻模块，用于辅助判断开门光照突变。**P4 的 ADC 通道与 S3 不同，光敏模块的 ADC 引脚必须查 P4 数据手册重新指定，不得沿用 S3 的 GPIO1。**
- IMU：MPU6050 或 ICM-42688，用于检测门体姿态变化和震动；I2C 引脚在 P4 板上重新排（注意避开 GPIO28/29 共享总线的摄像头/编解码器冲突，必要时用 GPIO 矩阵另开一路 I2C）。
- 人体存在毫米波雷达：HLK 24GHz UART 模块，UART 引脚在 P4 板引出 IO 中重新排。
- 音频输入/输出（2026-07-02 用户决定）：**优先使用板载方案**——驻极体 MIC + ES8311（采集）和 ES8311 + NS4150B 功放 + Speaker 端子（播放）；ES8311 的 I2S 在 GPIO2-6，功放使能 `PA_CTRL`=GPIO11。同时**保留外接 INMP441 / MAX98357A 的能力**（走引出 IO），作为板载方案异常时的回退。
- 原 MAX98357A 的 TTS 实测结论仍然有效：不要再使用 `alloy` 作为 SiliconFlow `fnlp/MOSS-TTSD-v0.5` 的音色，见 `components/speaker/README.md`。

## 架构原则

- ESP32-P4 是交互、状态与媒体采集中心，负责确定性、低延迟、本地可用的功能；联网能力由板载 ESP32-C6 经 SDIO + `esp_hosted`/`esp_wifi_remote` 提供，业务代码继续通过 `network` 组件的接口使用 Wi-Fi，不直接感知 C6。
- OV5647 摄像头经 MIPI-CSI 直接接入主控，主控负责拍照、帧缓冲/JPEG 管理、上传任务和状态机联动。
- 首版 AI 能力采用设备直连 OpenAI-compatible API：主控通过 HTTPS 直接访问用户配置的 API Base URL、模型和 API Key。
- API Key 在开发/Demo 阶段允许保存在设备 NVS 中，串口响应和日志不得回显明文；后续如进入公开部署或量产，应评估 NVS 加密和设备端 Key 轮换。
- 远程服务在首版中不是 AI 必需链路；如后续需要跨设备同步、远程库存管理、日志分析和 OTA 运维，应作为独立扩展，不改变 AI 直连主链路。
- 库存、提醒和位置数据要支持本地缓存，断网时仍能展示基础信息。
- 首版不把 TF/SD 卡作为图片缓存主路径（P4 板虽有 TF 卡座，扩展前先评估必要性），主控不长期保存图片原图；离线时只缓存拍照任务状态、错误码和必要元数据，网络恢复后重试上传或提示用户重新拍照。
- 人体存在毫米波雷达只作为靠近/有人上下文，不得单独作为开门判断依据；开门判断仍以光照突变和 IMU 姿态变化为主。

## 硬件安全规则，必须严格遵守

这个项目连接真实硬件。任何代码、配置、接线建议和调试步骤都必须先审查硬件风险，避免烧坏开发板、屏幕、传感器或摄像头。

执行任何硬件相关改动前，必须检查：

- 电压是否匹配：屏幕 VCC 是 5 V，但 ESP32-P4 GPIO 逻辑是 3.3 V，不得把 5 V 信号直接接入 GPIO；DSI 座的复位网络在板上被分压到 1.8 V，不要绕过。
- GPIO 是否安全：P4 板上避开启动绑带脚 `GPIO35(BOOT)`、`GPIO36`（带 10K 上拉），以及 32.768KHz 晶振占用的 `GPIO0/GPIO1`。改引脚前先查用户手册 §1.5 引脚表，确认目标脚没有被板载外设占用（ES8311 I2S 在 GPIO2-6、SPI 屏座 GPIO7-10/15-20/22/23、`PA_CTRL`=GPIO11、按键/LED GPIO12-14、USB GPIO24/25、CSI 控制 GPIO26/27、共享 I2C GPIO28/29、RS485 GPIO30-32、CAN GPIO33/34、UART0 GPIO37/38、TF 卡 GPIO39-45、`LCD_ID`=GPIO46、C6 SDIO GPIO47-54）。
- 共享 I2C 是否冲突：GPIO28/29 同时接 CSI 摄像头 SCCB 和 ES8311，手册要求分时复用；任何同时用到摄像头和音频的改动都必须先说明总线串行化方案。
- 电源能力是否足够：屏幕背光、C6 Wi-Fi 峰值、OV5647 摄像头、毫米波雷达、麦克风和传感器同时工作时建议使用稳定 5 V/2 A 电源；板上保险丝为 1.5 A，注意总电流余量。
- GND 是否共地：主控、摄像头、传感器、屏幕调试时需要确认共地关系。
- I2C 是否有合适上拉：通常 SDA/SCL 上拉到 3.3 V，不能上拉到 5 V。
- 背光和蜂鸣器是否需要限流或驱动：不得直接让 GPIO 承担超额电流。
- SPI 引脚方向是否正确：尤其是屏幕转接排线的 CS、SCLK、MOSI/MISO、RESET、DC。
- 触摸芯片供电和复位是否符合手册。
- OV5647 的供电、CSI 时钟/数据、`CSI_RST`/`CSI_PWDN`、SCCB 和 PSRAM 帧缓冲是否满足时序与内存约束。
- C6 联网链路是否按手册配置了 SDIO 电源管理；未配置时 Wi-Fi 异常不要先怀疑业务代码。
- 毫米波雷达模块的 VCC、UART/GPIO 逻辑电平、波特率、安装朝向和金属遮挡是否明确；若模块可 5 V 供电，也必须确认 IO 不会输出 5 V 到主控。

禁止事项：

- 禁止在未核对电压和引脚定义前给硬件上电。
- **禁止把 TR230S 屏的 20pin FPC 直接插入板上 20pin SPI 屏座**：两者引脚定义不同（板座 Pin1/2=5V、Pin3/4=GND；屏 FPC Pin1=GND、Pin2=5V），直插会烧屏。必须使用逐脚核对过、并经用户确认的转接排线。
- 禁止把 5 V 输出或背光高压直接接主控 GPIO。
- 禁止在不确认启动绑带脚状态的情况下把外设接到启动敏感 GPIO（GPIO35/GPIO36）。
- 禁止为了“试一下”随意提高 SPI 时钟、摄像头时钟、背光占空比或 GPIO 驱动强度。
- 禁止在同一时刻同时占用 GPIO28/29 共享 I2C 上的摄像头和 ES8311，必须分时复用。
- 禁止把毫米波雷达的“有人存在”结果直接等同于“冰箱门已打开”。
- 禁止忽略烧录日志中的 brownout、watchdog、heap overflow、PSRAM 初始化失败等硬件相关异常。

如果硬件资料、接线图或电源条件不明确，必须先暂停实现并说明风险。

## 代码规范

### 中文注释要求

每一个代码功能区必须有中文注释。注释用于说明“这块代码负责什么、为什么这样做、有什么硬件或时序注意事项”。

要求：

- 文件顶部写中文模块说明。
- 每个初始化函数、任务函数、状态机分支、硬件驱动关键步骤都要有中文注释。
- 涉及 GPIO、电压、时钟、DMA、PSRAM、I2C、SPI/QSPI、I2S、OTA、Flash 写入的位置必须说明硬件约束。
- 注释要简洁准确，不写无意义注释。
- 公共接口、消息结构和状态枚举要用中文说明字段含义。

示例：

```c
// 初始化屏幕 QSPI 总线。
// 注意：屏幕逻辑电平为 3.3V，VCC 为 5V；这里只配置数据总线，不控制背光电源。
static esp_err_t display_bus_init(void)
{
    ...
}
```

```c
// 开门状态机：融合光照突变、IMU 姿态变化和毫米波雷达靠近事件。
// 该路径必须本地快速完成，不能等待 AI API，避免用户开门时 UI 卡顿。
static void fridge_state_update(const sensor_event_t *event)
{
    ...
}
```

### 代码组织建议

后续固件实现推荐按 ESP-IDF 组件拆分，把不同功能放在不同组件和文件中。这样做是推荐方案，原因是硬件驱动、UI、网络、存储和状态机的生命周期、依赖和测试方式都不同，拆分后更容易控制硬件风险、复用代码和定位问题。

目录规则：

- 优先使用 ESP-IDF 默认目录名 `components/`，每个功能组件一个子目录，例如 `components/display/`、`components/sensors/`。
- 如果仓库使用单数目录 `component/`，必须在根 `CMakeLists.txt` 里通过 `EXTRA_COMPONENT_DIRS` 显式加入，否则 ESP-IDF 默认不会自动扫描该目录。
- 每个组件目录应包含自己的 `CMakeLists.txt`、`include/` 和源码文件，公共头文件放在该组件的 `include/` 下。
- `main/main.c` 只作为启动编排入口，负责初始化顺序、任务启动和高层状态串联，不堆放具体驱动、协议解析或业务细节。
- 组件之间通过清晰的头文件接口和事件/消息结构通信，避免跨组件直接访问内部静态变量。
- 硬件相关组件必须在接口或初始化代码注释中写明 GPIO、电压、时钟、DMA、PSRAM、电源和时序约束。

后续固件实现建议按以下组件拆分：

- `bsp`：板级硬件、GPIO、I2C、SPI/QSPI、I2S、LEDC。
- `display`：TR230S 屏幕驱动和背光。
- `touch`：FT6336U 触摸。
- `camera`：OV3660 初始化、帧缓冲/JPEG 管理、拍照任务和上传状态。
- `ui`：LVGL 页面和 UI 事件。
- `sensors`：光照、IMU、人体存在毫米波雷达。
- `state_machine`：靠近、开门、关门、拍照、离线、夜间状态。
- `storage`：NVS、LittleFS、库存快照、断网队列。
- `network`：Wi-Fi、SNTP、HTTP、TLS。
- `mqtt_protocol`：MQTT 主题、消息编解码和重连。
- `audio`：I2S 麦克风、命令词、蜂鸣器。
- `ota`：OTA 检查、下载、回滚。
- `diagnostics`：heap/PSRAM、任务看门狗、错误码和日志。

## 实现顺序建议

1. 修正目标硬件配置：8 MB Flash、PSRAM、自定义分区表、大主固件 + 小 recovery、LittleFS。
2. 建立组件化目录结构，保留 `main.c` 作为启动编排入口。
3. 先做硬件安全验证：电源、电压、GPIO、共地、I2C 上拉、屏幕复位和背光。
4. 点亮屏幕和触摸，使用低时钟起步，再逐步提升。
5. 接入 LVGL 局部刷新和行缓冲。
6. 接入传感器并实现开门状态机。
7. 接入 Wi-Fi、HTTPS AI API、本地缓存和离线队列。
8. 接入 OV3660 摄像头，从低分辨率和低时钟起步，完成拍照、帧缓冲/JPEG 管理和 HTTPS 上传。
9. 接入设备直连 AI API、库存确认、菜谱推荐和 OTA。
10. 最后做长稳测试和硬件温升/供电检查。

## 测试要求

每次涉及硬件或底层配置的改动，都要至少验证：

- 上电无异常发热、异味、反复重启。
- 串口日志无 brownout、PSRAM 初始化失败、watchdog 反复触发。
- GPIO 输出不会导致启动失败。
- I2C 设备可扫描到，地址符合预期。
- 屏幕刷新正常，无花屏、白屏、闪屏。
- Wi-Fi 峰值和背光开启时电源稳定。
- Flash/LittleFS 写入后断电重启数据不损坏。

建议记录关键测试结果到后续测试文档，尤其是接线版本、GPIO 表、供电方式和异常日志。

## 本项目 ESP-IDF 构建、烧录与串口调试经验

当前主线使用 ESP-IDF 5.5.1，源码在 `D:\esp-IDF\v5.5.1\esp-idf`，公共工具链目录为 `D:\esp-IDF\.espressif`。优先使用项目包装脚本，它会固定版本、清理 MSYS 环境变量、校验烧录 profile 并转发 `idf.py` 参数；不要直接运行未激活环境中的 `idf.py`。

推荐构建命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 build
```

推荐烧录命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM15 flash
```

目标芯片与串口：

- 目标芯片为 `esp32p4`；首次移植时需 `tools\\idf.ps1 set-target esp32p4` 并按新 `sdkconfig.defaults` 重建配置，不要沿用 S3 生成的 `sdkconfig`。
- **P4 板串口已确认是 `COM15`**（USB VID `1A86:55D3` 的 CH343P）；旧的 `COM16` 只属于 S3。COM15 用于烧录、monitor 和 JSON Lines 调试，另一个 USB-C 口是 JTAG。烧录默认配置已固定 `ESPTOOLPY_NO_STUB=y`，不要改回 RAM stub。
- 打开串口或烧录后开发板可能复位一次，这是正常现象；调试脚本应等待 Wi-Fi/SNTP 稳定后再发网络或 AI 请求。
- 串口可能被浏览器 Web Serial、ESP-IDF monitor、VS Code Serial Monitor 或其他串口助手占用；如果打开失败，先关闭占用端口的程序，不要反复重试烧录。杀残留进程时只杀命令行明确包含目标 COM 口且为 `idf_monitor` / `idf.py monitor` 的进程。

串口协议调试经验：

- 固件 USB 协议使用 `115200` 波特率、JSON Lines，一行一个请求，一行一个响应。
- 日志和 JSON 响应会混在同一串口输出中；调试脚本必须按行读取，只把以 `{` 开头并包含目标 `request_id` 的行当作响应。
- Web 面板和调试脚本发送中文时优先发送 UTF-8；如果工具把中文转成 `\uXXXX`，固件应能正确解码，但调试时仍要检查模型实际看到的用户问题是否完整。
- AI 请求可能耗时 10 秒以上，`ai_assistant_chat`、`test_ai_chat`、`scan_wifi`、`set_network` 等命令需要较长超时，建议至少 `90-130` 秒。
- API Key 不得写入日志、串口响应或调试输出；调试 AI 只记录 HTTP 状态、响应长度、耗时、任务类型和 history 条数。
- 真实 API 二轮对话回归至少验证：第一轮无 history 返回 `HTTP 200`，第二轮带最近 user/assistant history 仍返回 `HTTP 200`，并确认模型没有看到被截断或乱码的问题。

## 给后续 AI/协作者的提醒

- 先读 `doc/项目架构与工作流设计.md`，再写代码。
- 这是硬件项目，不是纯软件项目。任何看似简单的 GPIO、供电、时钟、背光或外设初始化都可能造成硬件风险。
- 项目正在从 ESP32-S3 迁移到 ESP32-P4 CB V1.2：不要假设当前 `sdkconfig`、分区表或任何旧引脚号在 P4 上有效；S3 存档在 git 分支 `s3-legacy` / 标签 `s3-final-20260702`。
- ESP32-P4 芯片本身没有 Wi-Fi，联网必须走板载 ESP32-C6（SDIO + `esp_hosted` + `esp_wifi_remote`），不要在 P4 工程里直接找 `esp_wifi` 的片内实现。
- 摄像头是 OV5647 MIPI-CSI 直连主控；不要再引用 OV3660/DVP/`esp32-camera` 方案，也不要设计成额外独立摄像头节点。
- GPIO28/29 共享 I2C 同时接摄像头 SCCB 和 ES8311，必须分时复用；涉及"拍照 + 语音"并存的功能先想清楚总线串行化再动手。
- 不要把毫米波雷达单独作为开门判断依据；它只提供靠近、有人保持和夜间无人上下文。
- 不要让 AI 识别结果直接入库，必须经过规则校验和用户确认。
- 所有代码功能区写中文注释，特别是硬件相关代码。
