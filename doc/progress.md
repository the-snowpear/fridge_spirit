# 冰箱小精灵项目进度

说明：

- 本文档记录已经完成的阶段性进展。
- 较大进展使用 `----` 分隔，并写明完成时间。
- 小修改和小修复默认追加到最近相关进展下，不单独新开分隔块。

----
完成时间：2026-05-17 21:08:38 +08:00

- 完成项目架构文档 `doc/项目架构与工作流设计.md`。
- 早期明确了主控 `ESP32-S3`、独立 `ESP32-CAM`、服务端与移动端的整体分层架构；该记录已被后续 `OV3660` 直连与设备直连 AI API 方案取代。
- 固化了硬件拓扑、推荐组件划分、状态机、MQTT/HTTP 接口、数据模型、分区建议和测试验收路径。

----
完成时间：2026-05-17 22:25:58 +08:00

- 完成项目级 `AGENTS.md` 初版约束整理。
- 明确了硬件安全规则、中文注释要求、组件化建议、实现顺序和测试要求。
- 为后续协作者统一了硬件风险检查和代码组织基线。

----
完成时间：2026-05-18 00:28:46 +08:00

- 完成主控工程向 `ESP32-S3-DevKitC-1 N8R8` 规划配置的基础收敛。
- 已新增 `partitions.csv` 与 `sdkconfig.defaults`，规划为 `8MB Flash + 8MB PSRAM + 双 OTA + 本地数据分区`。
- 分区中已预留 `assets`、`cache`、`coredump`，为后续资源缓存、离线队列和诊断留出空间。

----
完成时间：2026-05-18 20:16:14 +08:00

- 完成主固件基础组件骨架搭建。
- `components/` 下已建立 `diagnostics`、`network`、`usb_protocol`、`display_test` 等组件。
- `main/main.c` 已接入主控启动编排，支持 `NVS`、诊断、网络、USB 配网控制台，以及独立屏幕测试模式切换。
- 当前主控工程已经脱离空白模板，进入“可继续迭代的基础框架”阶段。

----
完成时间：2026-05-18 20:29:21 +08:00

- 完成项目协作文档补充。
- 在 `AGENTS.md` 中补充了当前使用的 `ESP-IDF 6.0.1`、主固件与前端的基础技术栈、编程约定和进度记录规则。
- 新建 `doc/progress.md`，用于后续自动累计项目里程碑与阶段性完成情况。

----
完成时间：2026-05-18 21:47:00 +08:00

- 完成项目硬件方案基线修订。
- 将摄像头方案从独立 `ESP32-CAM + OV2640 + TF 卡` 调整为 `OV3660` 直连 `ESP32-S3` 主控。
- 将人体靠近/存在检测从 `PIR` 调整为 `HLK-LD2410C/LD2410B` 或同类 `24GHz/25GHz` 人体存在毫米波雷达模块。
- 明确首版不新增 `TF/SD` 卡，离线时只保留拍照任务状态、错误码和必要元数据，不承诺保存图片原图。

----
完成时间：2026-05-18 22:34:00 +08:00

- 完成 `display_test` 的最小点亮补强：新增屏幕背光使能脚 `GPIO7`，上电后先拉高背光再进入 QSPI 测试。
- 调整测试首图顺序为白底优先，方便区分“背光未开”和“图像未写入”两类问题。
- 重新构建通过，生成新的 `fridge_spirit.bin`，当前屏幕不亮问题更偏向硬件接线/背光路径，而不是固件未运行。

----
完成时间：2026-05-20 21:52:38 +08:00

- 完成两份屏幕相关 PDF 的 Markdown 整理，便于后续固件和硬件接线时快速检索。
- 新增 `doc/屏幕模组_TS040HDS02CP-B1620A_规格整理.md`，覆盖模组规格、连接器引脚、电气/背光参数、SPI/QSPI 时序、光学参数、可靠性测试、机械图和使用注意事项。
- 新增 `doc/TR230S_DATASHEET_V2.3_整理.md`，覆盖 TR230S 总体能力、DBU 引脚、接口配置、SPI/QSPI/8080 时序、核心寄存器、电气特性和驱动接入建议。
- 新增 `doc/screen_assets/`，保存时序图、机械图、封装图和 RGB 映射图等 Markdown 难以纯文本表达的关键页面截图。
----
完成时间：2026-05-20 22:31:47 +08:00

- 完成 Web 配网与 AI API 测试链路的模块化接入。
- 默认关闭 `CONFIG_FRIDGE_SCREEN_TEST`，正常主控路径会启动 NVS、Wi-Fi、AI 配置组件和 USB JSON Lines 运维面板；屏幕测试代码仍保留，后续可手动重新启用。
- 新增 `components/ai_client`，支持在开发板 NVS 中保存 OpenAI-compatible API Base URL、模型、系统提示词和 API Key，并通过 HTTPS `/chat/completions` 执行文本聊天测试。
- 扩展 Web Serial 协议命令：`get_ai_config`、`set_ai_config`、`clear_ai_key`、`test_ai_chat`；响应中不回传 API Key 明文，只返回保存状态和脱敏预览。
- Web 面板新增 `AI API` 页面，支持配置保存、清除 Key 和模拟聊天测试；Mock 模式同步支持无硬件演示。
- 安全边界：当前为 NVS 开发模式保存 Key，适合今天快速联调；后续可评估 NVS 加密和 Key 轮换方案。
----

## 2026-05-20 23:26:33 USB 协议稳定与 Wi-Fi 扫描恢复

- 修复 Web Serial 连接后初始化并发请求过多的问题：Web 面板改为串行读取设备状态，并避免刷新任务重入，旧固件缺少 AI 命令时不会拖垮网络页面。
- 修复固件 USB JSON Lines 读取：不再用 gets(stdin) 假设一次读完整行，改为逐字节累积到换行后解析，避免半条 JSON 被误判为 command is required。
- 增强固件协议兼容性：忽略非 JSON 串口噪声，并兼容早期 cmd 字段。
- 修复诊断任务快照中 Wi-Fi heartbeat 指向局部变量导致乱码的问题。
- 已重新构建并刷写 ESP32-S3；串口探针验证 get_status、get_network、get_ai_config 和 scan_wifi 均能按 request_id 返回。
- 实测 scan_wifi 能扫描到多个 2.4GHz AP，包括 igo_DC991F、SEU-ISP、SEU-WLAN、SEU-GUEST。
补充：为降低 Web 面板长期打开时的电脑负载，默认自动刷新间隔从 5 秒调整为 15 秒，后台标签页暂停刷新，周期刷新只读取动态状态；Wi-Fi 扫描、联网和 AI 测试期间跳过自动刷新，串口 TX 调试日志默认关闭。
----

## 2026-05-21 01:02:21 Wi-Fi 连接诊断与串口响应稳定性修复

- 按用户要求将 Espressif 官方 ESP-IDF Wi-Fi station/scan 示例以 sparse checkout 方式拉取到 `example/esp-idf-wifi-reference/`，用于对照成熟连接流程。
- 对照官方 station 示例，将 Wi-Fi 连接等待时间从 15 秒放宽到 35 秒，避免还在重试阶段就被 Web 端判断为超时。
- 增强 `components/network` 的断线 reason 诊断：认证失败、安全模式不兼容、找不到 AP、信号丢失、握手超时等场景会写入更明确的中文错误说明，并回传到 Web 面板。
- Wi-Fi 密码只在成功拿到 IP 后写入 NVS，避免错误密码连接失败后污染已保存配置。
- USB JSON Lines 响应输出时锁定 `stdout`，降低 ESP_LOG 日志插入响应中间导致 Web 解析失败或超时的概率。
- AI API 测试在网络已连接但未完成 SNTP 校时时会主动再尝试一次校时，减少“Wi-Fi 已连但 AI 测试被 internet_ready 拦住”的情况。
- 固件已重新构建通过；下一步需要把新固件烧录到 ESP32-S3 后，再观察实际断线 reason 来区分密码、热点兼容性、信号和供电问题。
----

## 2026-05-21 01:23:00 AI API 测试崩溃定位与修复

- 通过 COM16 直接复现 `test_ai_chat`，确认原失败不是 API 服务错误，而是 `usb_protocol` 任务在执行 AI HTTPS 测试时发生栈溢出并重启。
- 将 USB JSON Lines 行缓冲改为静态存储，并把 AI 测试消息与结果结构改为堆分配，降低 USB 任务栈压力。
- 将 USB 协议任务栈提升到 32KB，给 `esp_http_client`、TLS 和 JSON 响应留出更稳定的运行空间。
- AI HTTP 非 2xx 响应现在会优先透传服务端 `error.message`，方便区分 Key 错误、模型错误、限流和 Base URL 错误。
- SNTP 校时增加备用服务器尝试，降低单个 NTP 节点不可达导致 HTTPS/TLS 前置检查失败的概率。
- 已重新构建并刷写 ESP32-S3；实测 `get_network` 在线、`get_ai_config` 配置存在，`test_ai_chat` 返回 HTTP 200 和 `Pong！`，AI API 链路闭环成功。
----
## 2026-05-21 01:51:28 AI 连接参考与运行压力优化完成

- 新增 example/ai_reference/，以参考方式保存 ESP-IDF esp_http_client、ESP32_AI_Connect、espai、ESP32_ChatGPT，不接入 CMake/固件依赖。
- AI API 测试拆到一次性 i_chat_worker，USB 协议任务常驻栈回落到 12KB，worker 使用临时 32KB 栈，完成后释放。
- AI 测试请求增加 max_tokens=128，HTTP 响应缓冲从 8KB 降为 4KB，并显式使用 Accept-Encoding: identity。
- Wi-Fi 获取 IP 后改为后台 SNTP；AI 测试前仍做有界校时确认，SNTP 使用互斥避免并发重入。
- Web 周期刷新不再每轮读取 AI 配置列表，仅进入 AI 页面或配置变更后刷新，降低串口和浏览器压力。
- 已完成固件构建、Web 构建、刷写和真机回归；连续 10 次 	est_ai_chat 均 HTTP 200，无 stack overflow、无重启、无串口 JSON 破碎。
- 验证记录：AI worker 栈水位约 27344-27568 words，USB 协议任务 AI 后栈水位约 8944 words，Wi-Fi 获取到 IP 192.168.0.106，API 配置仍保存在 NVS。
- 2026-05-21 02:05:48 补充：放宽 AI API 系统提示词配置。固件 FRIDGE_AI_MAX_SYSTEM_PROMPT_LEN 从 256B 提升到 2048B，Web 输入框取消 240 字硬限制并改为 UTF-8 字节计数提示；USB 侧相关 AI 配置结构改为堆分配，避免长提示词增加常驻任务栈压力。已通过 
pm run build 和 idf.py build。- 2026-05-21 02:06:48 补充：USB JSON Lines 单行缓冲从 4096B 提升到 6144B，用于容纳长系统提示词转义后的请求；复跑 idf.py build 通过。- 2026-05-21 02:22:55 补充：系统提示词长文本支持升级到 8192B。固件改为使用 NVS blob 保存 systemPrompt，兼容旧版短字符串读取；USB JSON Lines 单行缓冲提升到 16KB，AI 配置/测试大结构改为堆分配。已刷写 COM16，并用当前厨房助手完整提示词真机校验：3779B 写入、3779B 读回、内容完全一致。
----
## 2026-05-21 14:14:26 AI 系统架构第一版落地

- 新增 `doc/AI系统设计架构.md`，固定 AI 链路的任务类型、结构化上下文注入、记忆边界和用户确认闭环；并在主架构文档中加入引用。
- 新增 `components/storage` 存储门面，提供库存快照、临期提醒、用户偏好、结构化记忆摘要和离线队列的统一读取接口；当前使用种子 JSON 与 NVS 记忆摘要，后续在组件内部替换为 LittleFS cache/assets。
- 新增 `components/ai_context`，支持 `chat_assist`、`recognize_ingredients`、`inventory_parse`、`recipe_generate`、`shopping_list_generate`、`reminder_explain`、`voice_intent_parse` 等任务的上下文预览和 Mock 结构化结果。
- 扩展 USB JSON Lines 命令：`get_ai_context_preview`、`test_ai_task`、`get_memory_summary`、`clear_memory_summary`，用于 Web 面板验证任务上下文、记忆边界和“AI 结果不直接入库”规则。
- Web 面板新增“项目 AI”页面，支持选择任务类型、开关库存/提醒/偏好/记忆注入、预览上下文、运行 Mock 项目 AI 任务、读取和清空结构化记忆摘要。
- 当时曾新增服务端参考实现，后续架构已删除该参考并收敛为设备直连 AI API。
- 已通过 `npm run build` 和 `idf.py build`；固件 app 大小 `0xfb160`，最小 app 分区仍有约 56% 空间。----
## 2026-05-21 14:53:10 AI 助手合并与真实上下文调用

- 将 Web 面板的 `AI API` 与 `项目 AI` 合并为 `AI 助手` 单一入口，主界面包含真实多轮对话、任务类型选择、上下文开关、上下文预览、AI 设置和硬件测试记忆维护。
- 新增 USB JSON Lines 命令 `ai_assistant_chat`：设备端先通过 `ai_context` 生成最小项目上下文，再把最近对话历史与上下文注入 OpenAI-compatible `/chat/completions`，返回模型、耗时、HTTP 状态、任务类型和确认标记。
- 新增 `set_memory_summary` 与 `fridge_storage_set_memory_summary()`，允许 Web 面板明确写入结构化硬件测试记忆；仍不自动保存完整聊天记录，也不让 AI 结果直接入库。
- `test_ai_chat` 与 `test_ai_task` 保留为开发探针和 Mock 兜底；Web 默认路径改为真实上下文助手调用。
- 已通过 `npm run build` 和 `idf.py build`；固件 app 大小 `0xfc7d0`，最小 app 分区仍有约 56% 空间。
----
## 2026-05-21 14:59:00 AI 方案调整为设备直连 API

- 按当前开发目标，将首版正式 AI 路线调整为“ESP32-S3 设备直连 OpenAI-compatible API”。
- 更新 `AGENTS.md`、`doc/项目架构与工作流设计.md` 和 `doc/AI系统设计架构.md`：`components/ai_client` 作为首版 AI API 正式入口，`ai_context` 负责上下文注入和任务约束，远程同步降级为后续可选扩展。
- 明确安全边界：API Key 允许在 Demo 阶段保存到设备 NVS，但串口响应、日志和仓库不得回显明文；公开部署前再评估 NVS 加密和 Key 轮换。
----
## 2026-05-21 15:40:44 设备直连 AI 架构收敛

- 按最新项目架构，明确首版 AI 只走 ESP32-S3 设备直连 OpenAI-compatible API。
- 更新 `AGENTS.md`、`doc/AI系统设计架构.md` 和 `doc/项目架构与工作流设计.md`，将远程能力限定为后续同步、日志或 OTA 运维扩展，不参与 AI 主链路。
- 删除服务端参考实现，避免后续实现误回到服务端中转方案。
- 保留本地 Mock 结果作为比赛现场兜底；真实 AI 仍由设备端 `ai_client` 和 `ai_context` 完成配置、上下文注入与 HTTPS 调用。
----
## 2026-05-27 Backend Phase A 完成：契约对齐 + MQTT 接入 + AI 双向同步

- 全局响应壳：新增 `app/schemas/common.py`（`CamelModel` + 泛型 `ApiResponse[T]`），所有路由统一 `{ok, data, message, requestId}` 输出；字段 ORM 仍保持 snake_case，序列化自动转 camelCase，给小程序提供稳定契约。
- 数据模型扩展：`InventoryItem` 新增结构化位置 `zone`(`freezer/left/right/door/custom_*`) / `slot`(`A1`-`C3`)；新增 `UserSettings`（用户隐私偏好）与 `SystemConfig`（home_id + config_key + value + config_updated_at，唯一约束 `(home_id, config_key)`，承载 AI 配置双向同步）。
- 鉴权升级：`services/token.py` 改 HS256 JWT（`python-jose`），新增 `app/core/deps.py` 提供 `get_current_user` / `get_active_home` 依赖注入，小程序无需再显式传 `home_id`；自动建 demo home 兜底。
- 路由对齐与新增：保留 `/wx/login`、`/device/*`、`/reminder/*`、`/inventory/list` 旧 alias；新增 `/auth/wechat-login`、`/devices/primary`、`/devices/bind`（bindCode=DEMO 走演示设备）、`/home/overview`、`/inventory/scan`、`/ai/chat`、`/ai/config`、`/settings`、`/reminders/{id}/confirm`，全部 ApiResponse[T] 包裹。
- 视觉识别 `/inventory/scan`：multipart 上传 → Pillow 压到长边 ≤ 1024 JPEG q=85 → base64 进 SiliconFlow 多模态接口 → JSON 候选解析 → 按"同名合并 / 类别默认 zone / 最空 zone"三档规则补 `suggestedZone+suggestedSlot+reason` → 写一条 `InventoryEvent(event_type="inventory.scan")` 留痕，原图不存。
- AI 对话 `/ai/chat`：路由先拉当前 home 的近 20 条活跃库存 + 临期清单作为 context；若有 active 设备且 MQTT 在线 → MQTT 转发 `command=ai_chat` + `request_id` + `prompt` + 序列化 context → 在 ack_waiter 上等最多 `AI_DEVICE_TIMEOUT_SECONDS` 秒；超时 / 无设备 / 未连接 → 自动回落 SiliconFlow OpenAI 兼容 `/chat/completions`，回复标 `source=cloud_fallback` + `fallbackReason`，便于前端展示降级原因。
- gmqtt 真客户端 `app/services/mqtt_client.py`：FastAPI lifespan 启动连接 EMQX，订阅 `fridge/+/+/{state,sensor,cmd_ack,error,ai_config}` 五种上行；`state` 入站更新 `Device.status/last_seen_at/firmware_version` + 写 `DeviceStatusEvent`；`cmd_ack` 完成 future；`ai_config` 走 last-write-wins 同步；`publish_command` 自动反查 home_id 后拼 topic 下发。
- **AI 配置双向同步（核心扩展）**：
  - `services/ai_config_service.py`：`get_ai_config(db, home)` 返不含明文 key 的展示视图（含 preview）；`get_ai_config_full(db, home)` 返含明文 key 的完整配置（vision/ai 服务调用与 MQTT 推送共用，回落 `settings.siliconflow_*` env）；`upsert_ai_config()` 小程序写入入口；`merge_from_device()` 处理设备 retained `ai_config` 上报，按 `config_updated_at` 时间戳比对返回 `("noop"|"updated_from_device"|"should_push_to_device", payload?)`。
  - `routes/ai_config.py`：`GET /ai/config` 取配置；`POST /ai/config` upsert 并 MQTT 推送给当前 home 所有 active 设备（`command=ai_config_update`），推送失败仅日志（设备离线下次上线 retained 同步会重新协调）。
  - `mqtt_client._handle_ai_config()`：收到设备 retained `ai_config` 后调 `merge_from_device`，若需反推则 `publish_command("ai_config_update")`。
  - `vision_service._call_vision_model` / `ai_service.chat_via_cloud` 改为接收 `ai_config` dict，apiKey / baseUrl / model 优先级：SystemConfig 行 > env 回落。
- 基础设施：`deploy/emqx/emqx.conf` 最小可用配置（匿名 + ACL allow `fridge/#` + WS 8083 暴露 `/mqtt-ws/`，生产前必须改 auth+ACL+TLS）；`deploy/nginx/default.conf` 加 `/mqtt-ws/` 反代 + `/api/` 60s 超时；`.env.example` 加 `SILICONFLOW_*` / `VISION_MODEL` / `CHAT_MODEL` / `MQTT_TOPIC_PREFIX=fridge` / `AI_DEVICE_TIMEOUT_SECONDS`；`requirements.txt` 加 `gmqtt` / `python-jose[cryptography]` / `python-multipart` / `pillow`。
- 启动种子：`app/core/lifespan.py` 在 `app_env=local` 时插入 demo user / demo home / `DEMO-FRIDGE-001` 设备 + active binding，配合 `/devices/bind` 的 bindCode=DEMO 实现"一键绑定演示设备"。
- 文档与编码：本块写入前修复了 `doc/progress.md` 自 2026-05-18 起的 GBK 残留段（offset 2550 起）；脚本容错混合解码 → 备份原文件到 `tmp/backups/manual/20260527-progress-utf8-fix/` → 重写为纯 UTF-8（LF 换行），0 替换字符，0 乱码 marker。
- 验证状态：所有新文件 `py_compile` 通过、AST parse 通过；`from app.api.v1.router import api_router` 导入级测试在本地 venv 因 `gmqtt` 未安装而预期失败，docker 镜像内 `pip install -r requirements.txt` 后即通；真机联调待 task #9 docker compose 与 task #10/#11 设备端配套实现。
- 风险与下一步：(1) 设备端 `fridge_mqtt_protocol.c` 需加 `ai_chat` worker task + `ai_config_update` 命令处理 + 启动时 retained publish `ai_config`，是真机闭环的前置条件；(2) 小程序需要按新契约重写网络层与类型层，并新增 `pages/scan` / `pages/ai-chat` / `pages/inventory-detail`；(3) EMQX 当前匿名 + ACL 全开，仅本地开发用，上公网必须改 auth+ACL+TLS。
----
## 2026-05-27 Miniapp Phase B 完成：网络层重写 + 9 个页面 + AI 双向同步 UI

- 前端设计 skill 决策：marketplace 上 `frontend-design@claude-plugins-official` 已在用户全局装过，但 scope 在另一项目；本期把 SKILL.md 的指导原则手动内化（不靠 Skill 工具调用），配合 `ui-reference/WLW/` 提炼出 design tokens，落在 `miniapp/styles/tokens.wxss`（草绿 sage 主色 / 暖黄 yolk 强调 / 番茄红 tomato 警示 / 米色 paper 纸感）+ `miniapp/app.wxss` 全局原子（卡片纸质渐变 + 1rpx sage 描边 + 暖灰阴影 + rise 入场动画），完全避开 frontend-design SKILL 警告的 generic AI 风格。
- 网络层 / 类型层重写：
  - `miniapp/types/models.ts` 完全按 backend CamelModel 输出字段对齐（id/name/quantity/unit/zone/slot/location/expireDate/status/source/confidence/updatedAt 等），新增 `ScanCandidate / ScanResult / AiChatMessage / AiChatResponseData / AiConfigData / AiConfigUpdateRequest / HomeInfo / RefreshData / InventoryWritePayload` 等业务模型，并导出 `STANDARD_ZONES` / `STANDARD_SLOTS` 常量；
  - `miniapp/utils/request.ts` 重写为支持 `RequestOptions(timeoutMs/retryOnFail/headers)` 的统一封装，封装 `RequestError(kind: network/http/business)`，GET 默认重试一次 + 5xx 短退避；新增 `uploadFile()` 包装 `wx.uploadFile`（multipart + Bearer + ApiResponse 解包）；
  - `miniapp/services/api.ts` 全量重写 16 个接口（auth × 1, devices × 2, home × 1, inventory × 6 含 CRUD + refresh + scan, reminders × 2, settings × 2, ai × 3）；
  - `miniapp/config/env.ts` 默认 baseUrl 改 `http://127.0.0.1:8000` 与 docker-compose 对齐，新增 `API_PREFIX` / `LONG_REQUEST_TIMEOUT_MS` / `DEMO_BIND_CODE`；
  - 删 `miniapp/services/mock.ts`（旧字段名死代码）+ `miniapp/utils/format.ts`（无人引用）。
- `miniapp/app.ts` 扩展为完整全局态：`apiConfig / session / activeHome / activeDevice / lastOverview` + 异步 `bootstrap(force?)` 在 onLaunch 后预热 home overview；新增 `MiniAppInstance` 公开方法签名供页面 `getApp<>()` 类型推断；`app.json` 注册 9 个页面，tabBar 仍为 home/inventory/reminders/settings 四项（AI 走悬浮按钮）。
- 6 个旧页面重写：
  - **pages/login** — wx.login + `loginWithWechatCode(code, nickname, avatarUrl)`，成功后根据 `hasBoundDevice` 跳 home / bind；
  - **pages/home** — ui-reference 三层视图：设备状态卡（含 3 数字 inventory/expiring/reminder）+ 4 zone fridge-map（点击带 zone query 跳 inventory）+ 临期食材清单 + 右下 AI 悬浮按钮；
  - **pages/bind** — 顶部"一键绑定演示设备"（bindCode=DEMO 直连 backend 种入的 DEMO-FRIDGE-001）+ 手输 / 扫码备用；
  - **pages/inventory** — 顶部 zone tab（全部 / freezer / left / right / door）+ 3×3 九宫格 grid-board（空格"+"占位，占用格按 freshness 染色）+ 待归位 list + 顶部"刷新冰箱"按钮触发 MQTT `inventory_refresh`；
  - **pages/reminders** — 列出 pending 提醒，按 reminder_type 染 tag，确认 / 忽略双按钮；
  - **pages/settings** — 4 分组（后端 API / 隐私 / AI 配置（双向同步入口！）/ 设备操作）；AI 区可改 apiBaseUrl / chatModel / visionModel / 输入新 apiKey 后保存即通过 MQTT 推送给所有 active 设备，"清除 Key" 按钮以 `apiKey=""` 触发后端清空。
- 3 个新页面：
  - **pages/scan** — `wx.chooseMedia` 选图 → `uploadFile(/inventory/scan)` 60s 长超时 → 渲染候选列表（含置信度 tag + 服务端推荐 zone/slot/reason + 调试用 modelUsed/rawText 200 字）→ 用户"直接入库"调 `createInventoryItem`，或"先编辑再入库"跳 `inventory-detail?mode=create&prefill=...`；
  - **pages/ai-chat** — 进入有欢迎语 + 输入框 + 发送按钮，回复气泡按 source 分色（device=mint, cloud_fallback=yolk）并显示 fallbackReason / modelUsed / deviceSn 元信息；消息仅内存，不持久化避免 PII；
  - **pages/inventory-detail** — `mode=create|edit`，create 模式从 URL query 预填，edit 模式 fetch `/inventory` 找 id 后填表；表单：名称 + 数量/单位 + 类别 + zone picker + slot picker + 到期日 picker；编辑模式带"删除"按钮 + 二次确认。
- 备份与编码：所有改动前在 `tmp/backups/manual/20260527-miniapp-network-layer/` + `20260527-miniapp-app-shell/` + `20260527-miniapp-pages-redo/` 共备份 30 余个文件；新增 / 重写 / 删除文件全部 UTF-8；`tmp/notes/20260527-task12-frontend-design-skill.md` 记录了 plugin scope 不匹配的处理决策。
- 验证状态：未跑微信开发者工具或 npx tsc（环境无 tsc，避免装包污染）；Grep 已扫除所有旧字段残留（mock / displayName / quantityValue / freshnessLevel / storageLocationLabel / DeviceStatus）；微信开发者工具加载时会做最终编译，遗留小问题随用随改。
- 风险与下一步：(1) task #9 docker compose 启动需要用户提供 SiliconFlow API Key 并 `cp .env.example .env`；(2) task #10/#11 设备端 `fridge_mqtt_protocol.c` 仍需加 `ai_chat` worker / `ai_config_update` 命令 / 启动时 retained publish `ai_config`，否则 AI 链路只能走 cloud_fallback、AI 双向同步只能单向（小程序→设备）；(3) AI 配置 apiKey 明文保存在 backend SystemConfig + 设备 NVS（用户已同意比赛风险），上线前必须做 KMS 或 vault 加密。
----
## 2026-05-27 21:00 Miniapp Phase B+：自定义 tabBar 1:1 复刻 ui-reference dock

- 用户反馈：上轮重构后底部导航与 ui-reference 仍有大差距（4 tab 原生 + AI 悬浮 fab，没有抬起的中间登记按钮、相机和 AI 没拆出独立页）。授权"推倒重新做"。
- 关键约束：微信原生 tabBar 不能渲染抬起按钮（margin-top 负值会被裁剪），方案选 `tabBar.custom: true` + `custom-tab-bar/` Component。
- 范围：本轮只做"骨架"——dock + 5 个 tab 入口 + mock 内容；其它 ui-reference 子页（standby / cameraResult / wifi modal / offline / editFood 等）下一轮再做。
- 改动清单：
  - **新增 `miniapp/custom-tab-bar/`**：4 文件，1:1 映射 ui-reference `.dock`（border-radius 60rpx + 5 列等宽 + 中间 `.camera-main` margin-top -56rpx + 番茄红投影），icon 全部 CSS 画（home/ai/camera/settings/more），无图片资源依赖；safe-area-inset-bottom 留白避开 home indicator。
  - **`miniapp/app.json`**：`tabBar.custom: true`；`tabBar.list` 改为 5 项 home/ai-chat/scan/settings/more（顺序与 dock 索引一致）；`pages` 数组补齐 11 项（含新增 `pages/more/index` / `pages/recipe/index` / `pages/shopping/index`）。
  - **新增 `pages/more/index`**：2×2 more-grid 入口卡，4 项（食材清单 / 购物清单 / 临期归位 / 离线模式 TODO）。
  - **新增 `pages/recipe/index`**：AI 菜谱页骨架，meal-summary + meal-card 堆叠，mock 数据；下一轮接 `ai_assistant_chat`。
  - **新增 `pages/shopping/index`**：购物清单骨架，每行 checkbox + 数量 + 来源 tag（AI 推荐 / 临期补货 / 手动添加）；导出走剪贴板。
  - **5 个 tabBar 页面在 onShow 同步 dock 高亮**：home/ai-chat/scan/settings/more 各调 `this.getTabBar?.()?.setData({ selected: N })`；非 tab 页面（inventory / inventory-detail / reminders / recipe / shopping / bind）不调。
  - **修正非 tab 页面的导航语义**：home/scan 中所有指向 inventory / reminders 的 `wx.switchTab` 改为 `wx.navigateTo`（这两页已不在 tabBar 列表，再 switchTab 会抛 redirect 错误）。
  - **移除 home 页 AI 悬浮 fab**：现在 dock 已有 AI 入口；wxml 删 `.ai-fab` button，wxss 改 `display:none` 兜底。
- 设置页报错（"Page pages/settings/index has not been registered"）原因：上一版 app.json 缺 settings 注册（被 push 到 tabBar 但 pages 数组遗漏）。本次 app.json 重写后 settings 在 pages 第 4 项，且 tabBar.custom=true 走的是新通路，下次"重新编译 + 清缓存"即解决。
- 备份：`tmp/backups/before_run/20260527-2100_tabbar-rebuild/miniapp/`（app.json / app.wxss / app.ts / pages/）。
- 验证状态：未跑微信开发者工具（环境限制）；目录与文件存在 + ts 类型 / wxss 加载语法人工核对；非 tab 页面不调用 `getTabBar`，5 个 tab 页都加了 selected 同步；遗留细节随用随改。
- 风险与下一步：(1) 跑微信开发者工具时务必"重新编译 + 清缓存"以剔除上版 tabBar 缓存；(2) 中间抬起的"登记"按钮当前指向 `/pages/scan/index`（拍照入库），与用户最初"相机要单独页"诉求一致；(3) ui-reference 还有 standby / wifi modal / offline / cameraResult / editFood 等子页本轮未做，下一轮安排。
----
## 2026-05-27 22:00 修复 custom-tab-bar 编译错误 + 页面切换空白

- 用户报告：底部 dock 报错 `[pages/xxx/index] Some selectors are not allowed in component wxss, including tag name selectors, ID selectors, and attribute selectors.(./custom-tab-bar/index.wxss:24:1)`，且 tab 切换后页面内容完全不显示。
- 根因：`custom-tab-bar/index.wxss` 第 1 行 `@import "/styles/tokens.wxss"`；tokens.wxss 用 `page { ... }`（tag 选择器）暴露 CSS 变量。微信对 Component scope wxss 严禁 tag/id/attribute 选择器，编译失败 → tabBar 渲染异常 → 所有 tab 页 layout 塌陷。
- 修复：
  - 备份 `tmp/backups/before_run/20260527-2200_tabbar-wxss-fix/miniapp/custom-tab-bar/index.wxss`；
  - 改写 `custom-tab-bar/index.wxss`：移除 `@import`，把所需 6 个 token（sage-dark / paper / tomato / line / sage-soft / shadow-card）硬编码为字面值；safe-area-inset-bottom 通过 `calc(48rpx + env(safe-area-inset-bottom))` 补回。
- 旁路确认：
  - app.json 已正确注册 11 页面，"Page xxx not registered" 是 IDE 缓存（重新编译 + 清缓存即解）；
  - `[ensureSession] failed: request:fail` 是后端未启动，与本次 UI 错误无关。
- 验证：未跑 IDE；视觉/语法人工核对，class 选择器只用 `.dock / .dock-btn / .dock-text / .dock-icon / .icon-*`，无 tag/id/属性选择器。

### 追加修复 22:30：ai-chat/index.ts 字符串引号未转义

- 用户继续报告：`Page "pages/more/index" has not been registered yet.`，"除了主页其他三个页面进不去"。
- 根因：`pages/ai-chat/index.ts:38` 欢迎语字符串外层用 `"` 同时内嵌未转义的 `"`：
  `content: "你好！我是冰箱小精灵。问我"今天吃什么"、"哪些临期"、"番茄怎么储存"都可以。"`
  TS strict 编译失败，开发者工具的 TS 插件整轮编译挂掉 → 所有依赖编译产物的 `Page()` 注册都跑不到，反映为"未注册"。
- 修复：备份 `tmp/backups/before_run/20260527-2230_aichat-string-fix/miniapp/pages/ai-chat/index.ts`；该行外层改成反引号模板字符串，保留内部「今天吃什么 / 哪些临期 / 番茄怎么储存」中文引号短语。
- 复查：`grep -n '"[^"]*"[^"]*"' miniapp/pages/**/*.ts` 全量匹配过一遍，剩下的命中都是合法 TS 字面量类型（`"warn" | "danger"`）和对象字面量并列字符串，无第二处同类 bug。
- 复查：`grep -n 'W2P\|W2TC' miniapp` → 无残留协议标记（之前 home/index.ts 出过一次，已不复现）。
- 下一步用户验证：开发者工具 → 工具 → 清缓存 → 全部清除 → 重新编译，五个 tab 页应全部进入。

### 追加修复 22:45：dock 与页面内容遮挡

- 用户截图反馈：(1) ai-chat 底部"发送"输入框被 dock 遮住一半；(2) settings 页滚到底时"安全提示"被 dock 盖住；(3) dock 整体可以更贴底。
- 根因 1：`pages/ai-chat/index.wxss` 用了 `height: 100vh` + flex 列布局，composer 自然贴到屏幕底，完全没给自定义 dock 让位。
- 根因 2：settings/scan 的根 `.page` 没有给 dock 让位的 padding-bottom；more/recipe/shopping 之前留了 240rpx 但与新 dock 几何（144 height + 16 margin + 56 抬起）不匹配，统一调整。
- 根因 3：dock 的 `margin-bottom: calc(48rpx + safe-area)` 偏高，视觉上不够贴底。
- 修复（备份 `tmp/backups/before_run/20260527-2245_dock-overlap-fix/`）：
  - `custom-tab-bar/index.wxss`：dock margin-bottom 从 48rpx → 16rpx，更贴近 home indicator。
  - `pages/ai-chat/index.wxss`：根容器从 `height:100vh` 改 `min-height:100vh`，叠加 `padding-bottom: 220rpx`，composer 改由文档流自然下落，不再贴屏幕底。
  - `pages/settings/{index.wxss,index.wxml}`：根 `<view class="page rise">` 加 `settings-page` hook，wxss 给 `.settings-page { padding-bottom: 220rpx }`。
  - `pages/scan/index.wxss`：补 `.scan-page { padding-bottom: 220rpx }`（wxml 根节点早已是 `class="page scan-page rise"`，无需改 wxml）。
  - `pages/more/index.wxss`、`pages/recipe/index.wxss`、`pages/shopping/index.wxss`：原 240rpx 统一对齐到 220rpx。
  - home 页保持 260rpx 不动（其底部还有"临期清单"等内容，多留一点更舒服）。
- 让位常量来源：dock 总高度 = 144rpx (height) + 16rpx (margin-bottom) + 56rpx (中间抬起按钮顶出) ≈ 216rpx，向上取整到 220rpx。
- 风险：safe-area-inset-bottom 在 iPhone 全面屏会再叠 ~68rpx，理论上还会再让位约 68rpx；视觉测试如有偏差，把 220rpx 整体上调到 260rpx 即可（不需要再改其它地方）。
----
## 2026-05-27 23:38 ESP32-S3 本地 LVGL UI 第一阶段接入

- 执行 `ui-reference-shiny-lake.md` 的固件 UI 阶段任务，但按最新 `doc/ESP32S3_DevKitC1_排线方案.md` 覆盖旧计划中的排线：LCD RESET# 使用 GPIO7，GPIO8 保留给 OV3660 D2；FT6336U 触摸不使用 TP_RST，GPIO16 保留给 OV3660 D7。
- 新增 `components/display`：从已验证的 TR230S QSPI 测试路径拆出正常运行驱动，提供 `fridge_display_init()`、局部刷屏 `fridge_display_flush_area()` 和 NVS 持久化亮度接口。
- 新增 `components/touch`：实现 FT6336U 最小 I2C 读取路径，复用现有 I2C0（SDA GPIO4 / SCL GPIO5），触摸 INT 使用 GPIO15。
- 新增 `components/ui`：接入 LVGL 9.2.2，使用 720x720 RGB565 局部刷新和 PSRAM 行缓冲，完成 standby/home/zone/settings/wifi 五页骨架、状态栏、底部 Dock、Wi-Fi 密码键盘和亮度滑块。
- `main/main.c` 在正常运维路径中启动本地 UI，并保留 `CONFIG_FRIDGE_SCREEN_TEST` 与 `CONFIG_FRIDGE_CAMERA_TEST` 的硬件排查分支；`main/idf_component.yml` 和根 `idf_component.yml` 固定 `lvgl/lvgl` 9.2.2。
- 解决 LVGL 链接期 `.dram0.bss` 溢出：关闭 LVGL builtin 静态内存池，改用 `CONFIG_LV_USE_CLIB_MALLOC=y`，并关闭 `CONFIG_LV_BUILD_EXAMPLES` 和全部 demo；构建后 LVGL 主要占 Flash，DIRAM 占用约 540 bytes。
- 验证：`idf.py reconfigure build` 通过；`fridge_spirit.bin` 大小 `0x1ced90`，最小 OTA app 分区 `0x240000`，剩余 `0x71270`（约 20%）；`idf.py size` 显示 DIRAM 使用 165241 bytes（48.35%）。
- 未做真机烧录与上电验证。烧录前必须再次确认屏幕 VCC 5V、信号 3.3V、所有模块共地、I2C 上拉到 3.3V、GPIO7 接 LCD RESET#、GPIO8/GPIO16 不再接旧计划的 LCD/TP 复位。

### 2026-05-28 00:50 真机烧录与首次 UI 启动测试

- 已按 COM16 烧录当前 LVGL UI 固件，esptool 识别为 ESP32-S3 rev v0.2，8MB Flash + 8MB PSRAM，bootloader / partition table / ota_data / srmodels / app 全部写入并校验通过。
- 启动日志确认 PSRAM 80MHz 初始化与 memory test OK，未出现 brownout、watchdog、heap overflow、PSRAM 初始化失败或重启循环；`get_diagnostics` 返回 `brownoutCount=0`、`watchdogCount=0`。
- 屏幕链路通过：`fridge_display` 使用 CS GPIO10、D0 GPIO11、SCLK GPIO12、D1 GPIO13、D2 GPIO14、D3 GPIO9、RESET GPIO7、WAIT GPIO6 初始化，日志显示 `TR230S display ready, brightness=80%`；`fridge_ui` 显示 `LVGL UI started`。
- USB JSON Lines 正常：`get_status` 返回 `ok=true`，Wi-Fi 已连接到 `192.168.0.106`，PSRAM 剩余约 `7382 KB`。
- 当前异常：I2C0 总线上的 MPU6050 与 FT6336U 都未应答。启动日志显示 `mpu6050 init failed: ESP_FAIL`，随后 `FT6336U chip id read failed at 0x38: ESP_FAIL`，UI 继续运行但触摸不可用。
- 当前判断：由于 0x68（MPU6050）和 0x38（FT6336U）同时失败，优先按硬件链路排查 GPIO4/GPIO5：SDA/SCL 是否接反、触摸/MPU6050 是否 3.3V 供电、GND 是否共地、I2C 上拉是否到 3.3V、OV3660 SCCB 是否已并到总线并拉低。下一步建议先断开 OV3660 SCCB，只保留 FT6336U 或 MPU6050 单设备验证 I2C ACK。
- 串口日志已保存：`tmp/logs/flash_lvgl_ui_20260528-004842.log`、`tmp/logs/flash_lvgl_ui_boot_20260528-004925.log`、`tmp/logs/flash_lvgl_ui_diag_20260528-005049.log`。

### 2026-05-28 01:22 FT6336U 触摸地址实测修复

- 用户确认屏幕触摸芯片丝印为 `FT6336U/TRN0706B`，且当前只接屏幕。按文档复核后确认：20-pin FPC 的 `TP-SDA/TP-SCL` 才是触摸 I2C，2x7 排针不直接暴露触摸 I2C；随屏 `example/screen` 的 TOUCH 目录是 GT9xxx 例程，不能直接套用到本屏 FT6336U。
- 新增/增强 USB 命令 `touch_i2c_diag`：扫描 GPIO4/GPIO5 的 I2C0，并对 `0x38`、`0x48`、`0x14`、`0x5D`、`0x68` 读取 FT 系列常见寄存器 `0x00/0x02/0x03/0x80/0x88/0xA3/0xA8`。
- 真机诊断结果：总线只扫描到十进制 `72`，即 `0x48`；`0x48` 读取 `0xA3=0x64`、`0xA8=0x11`、`0x02=0x00`，而标准候选 `0x38` 无 ACK。判断本批实物 FT6336U 实际 7-bit I2C 地址为 `0x48`，不是常见 `0x38`。
- 已修改 `components/touch`：保留 `0x38` 优先探测，失败后自动回退到 `0x48`，并打印清晰日志，避免后续不同批次屏幕地址差异导致触摸不可用。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；复位启动日志显示 `FT6336U did not answer at 0x38 (ESP_FAIL), using detected addr 0x48`、`FT6336U ready addr=0x48 chip=0x64 vendor=0x11`、`fridge_ui: LVGL UI started`。PSRAM 初始化正常，未见 brownout/watchdog。
- 日志保存：`tmp/logs/touch_i2c_fingerprint_20260528-011803.log`、`tmp/logs/touch_addr_fallback_reset_20260528-012200.log`。下一步需要用户手指点按屏幕底部 Dock 和各页面按钮，确认坐标方向、旋转和点击区域是否正确。

### 2026-05-28 01:52 固定触摸地址、中文字体与 UI 刷新稳定版

- 用户确认触摸已可用后，将 `components/touch` 固定为本批实测地址 `0x48`，移除 `0x38` 优先探测和 fallback 逻辑；`touch_i2c_diag` 也只保留已验证触摸地址 `0x48` 与 MPU6050 候选，避免后续日志继续误导。
- 新增 `components/ui/fonts/fridge_font_cn_16.c`，使用 Noto Sans SC 生成当前本地 UI 文案子集字体，并在 warm 主题中作为默认字体，解决 LVGL 默认 Montserrat 缺少中文字形导致的中文方框问题。
- 刷新观感做了保守优化：TR230S QSPI 从 `10MHz` 提到 `40MHz`，内部 flush 分块从 `8` 行提到 `20` 行，LVGL 双缓冲从 `40` 行提到 `120` 行；页面容器改为不透明背景，并只隐藏当前旧页，减少无意义全区判脏。
- 曾试验 `80MHz + 40 行分块 + 360 行缓冲 + 切页临时关背光`，实板出现大面积黑屏和点击乱闪，已回退；该方案暂判定为 TR230S/QSPI 时序或背光寄存器与刷屏竞争风险，不再作为当前稳定路线。
- 验证：`idf.py build` 通过，`fridge_spirit.bin` 大小 `0x1d3d80`，最小 OTA app 分区剩余 `0x6c280`（约 19%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20`、`FT6336U ready addr=0x48 chip=0x64 vendor=0x11`、`LVGL draw buffers ready, rows=120 bytes_each=259200`、`LVGL UI started`，未见 brownout/watchdog/panic。
- 日志保存：`tmp/logs/ui_refresh_rollback_20260528-015122.log`。下一步刷新优化应优先做页面结构优化，例如保留公共框架、减少整页 hide/show、把切页改为局部内容区差量更新；不要再直接上 `80MHz` 或切页关背光。

### 追加修复 01:59：补全中文子集字体与减少无效 UI 重绘

- 用户反馈总览左侧按钮仍有两个方框，定位为底部 Dock 的“待机”二字未包含在上一版手工字体子集中。已重新从 `components/ui` 与演示库存相关源码自动收集全部中文字符生成 `fridge_font_cn_16.c`，确认包含“待/机”等当前 UI 文案。
- 为改善观看体验且避免再次花屏，未改动 TR230S 时钟与背光寄存器策略，继续保持稳定的 `40MHz + 20 行分块 + 120 行 LVGL 缓冲`。
- 新增 `fridge_ui_label_set_text_if_changed()` / `fridge_ui_label_set_text_fmt_if_changed()`，并用于 home/standby/zone/settings/status_bar/toast 等高频路径，避免每秒把未变化文本重复写入 LVGL 导致无意义局部刷新。
- Wi-Fi 页面增加轻量缓存，AP 数量、扫描状态和状态文案不变时不重建列表；Dock 仅在 active 状态变化时更新样式，减少切页外的闪动。
- 验证：`idf.py build` 通过，`fridge_spirit.bin` 大小 `0x1db3e0`，最小 OTA app 分区剩余 `0x64c20`（约 17%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20`、`FT6336U ready addr=0x48`、`LVGL UI started`。
- 日志保存：`tmp/logs/ui_font_polish_20260528-015944.log`。

### 追加测试 02:12：TR230S flush 分块上限回退

- 为尝试改善大面积切页刷新观感，曾在保持 `40MHz` QSPI 不变的前提下，将 `LCD_FLUSH_CHUNK_ROWS` 从 `20` 提到 `40`，减少单次刷新中的 SPI 事务次数。
- 实板反馈出现黑屏/花屏，说明本屏在当前 QSPI 封包、WAIT# 轮询和 DMA 事务路径下，单次像素事务过长同样不稳定；该方向不再继续试 `40` 行分块或 `60MHz + 大分块`。
- 已回退到当前已验证稳定参数：`pclk=40000000Hz`、`chunk_rows=20`、`UI_BUFFER_ROWS=120`。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；复位日志显示 `TR230S display ready, pclk=40000000Hz chunk_rows=20`、`FT6336U ready addr=0x48`、`LVGL UI started`。
- 日志保存：`tmp/logs/display_chunk_40_trial_20260528-020829.log`、`tmp/logs/display_chunk_40_rollback_20260528-021223.log`。

----
## 2026-05-28 12:17 ui-reference 全量页面迁移首版

- 按用户确认的策略执行 `ui-reference/WLW` 全量迁移首版：优先视觉与触摸交互完整，只保留 warm 主题，真机验收以屏幕+触摸为主；未再调整已验证稳定的 TR230S 参数，继续保持 `40MHz + 20 行分块 + 120 行 LVGL 缓冲`。
- `components/ui` 从 5 页扩展到 13 页：standby/home/zone/editFood/door/camera/cameraResult/recipe/shopping/settings/wifi/more/offline；Dock 改为首页、AI、登记、设置、更多五项，所有入口都能进入对应页面。
- 新增本地 UI 库存快照持久化：`fridge_storage_get_ui_inventory_snapshot()` / `fridge_storage_set_ui_inventory_snapshot()`，数据保存在 cache LittleFS 的 `/cache/ui_inventory.json`；九宫格编辑、清空格子、拍照确认登记会写回本地缓存。
- 新增轻量拼音/常用词输入弹层：Wi-Fi 继续使用 LVGL 原生键盘，食材名称/数量/到期/备注可通过通用文本键盘和常见中文候选输入；完整手机级中文输入法留待后续。
- 首次烧录后发现 `fridge_ui` 栈溢出，已将 UI 任务栈从 12KB 增至 24KB，并把库存 JSON 大缓冲移出任务栈；再次烧录后 25 秒启动观察稳定，未见 brownout、watchdog、Guru Meditation 或 stack overflow。
- 验证：`idf.py build` 通过，`fridge_spirit.bin` 大小 `0x1de7d0`，最小 OTA app 分区剩余 `0x61830`（约 17%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20`、`FT6336U ready addr=0x48`、`LVGL UI started`。
- 日志保存：`tmp/logs/ui_reference_boot_20260528-121212.log`、`tmp/logs/ui_reference_boot_20260528-121430.log`、`tmp/logs/ui_reference_final_boot_20260528-121702.log`。当前仍可见 SNTP 超时和 MPU6050 未接/未应答警告，不影响本次屏幕 UI 验收。

### 追加优化 13:03：首页新版参考视觉与中文字库修复

- 根据用户补充的新版首页参考图，先做逐页迁移的第一页：首页从 6 张普通占位卡片改为四区冰箱地图结构，上层冷冻横跨顶部，左/右冷藏位于下方，门架占右侧整高，并补齐“检测到有人靠近 · 已启动”“编辑空间”“临期 N”等首页头部内容。
- 全局 warm 主题颜色调整为新版参考图的暖白背景、深绿主色、番茄红登记按钮和更深的正文色；状态栏改为圆角胶囊样式，Dock 改为底部胶囊导航，中间“登记”按钮凸起并使用番茄红。
- 重新生成中文字体资源：保留 16px 字体用于 Dock 小标签，新增 24px 主字体和 32px 标题/件数字体子集；字体字符从当前 UI 源码与本地库存种子自动抽取，补齐“添加/登记/编辑/临期”等缺字，减少方框风险。
- 未改动已验证稳定的屏幕刷新参数，继续保持 `40MHz + 20 行分块 + 120 行 LVGL 缓冲`，避免重复触发此前黑屏/花屏问题。
- 验证：`idf.py build` 通过，`fridge_spirit.bin` 大小 `0x1e6c40`，最小 OTA app 分区剩余 `0x593c0`（约 15%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20 brightness=96%`、`FT6336U ready addr=0x48`、`LVGL draw buffers ready, rows=120 bytes_each=259200`、`LVGL UI started`，未见 brownout/watchdog/Guru。
- 日志保存：`tmp/logs/ui_home_boot_reset_20260528-130232.log`。下一步按同样方法逐页对齐 zone、editFood、camera、settings/more 等页面，并持续关注 OTA 剩余空间；如果继续加大中文字体，需要同步做字体子集精简或分区/资源方案评估。

### 追加修复 13:11：Dock 选中态遮挡与实屏字重优化

- 修复底部 Dock 切换时选中项绿色大色块盖住内容的问题：非主按钮不再整块填充绿色，只切换图标/文字颜色并显示 34x4 的小指示条；中间“登记”主按钮继续保持番茄红凸起样式。
- 将 16/24/32 三档中文 UI 字体从 Noto Sans SC 常规字形切换为 Windows 独立粗体 `Dengb.ttf` 生成，保持现有字符子集和 bpp 设置不变，提升 72mm 实屏上的笔画重量与可读性，同时避免直接把 24px 字库升到 bpp2 带来的 OTA 体积压力。
- 未改动已验证稳定的屏幕刷新参数，继续保持 `40MHz + 20 行分块 + 120 行 LVGL 缓冲`。
- 验证：`idf.py build` 通过，`fridge_spirit.bin` 大小 `0x1e7500`，最小 OTA app 分区剩余 `0x58b00`（约 15%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20 brightness=96%`、`FT6336U ready addr=0x48`、`LVGL UI started`，未见 brownout/watchdog/Guru。
- 日志保存：`tmp/logs/dock_font_weight_boot_20260528-131100.log`。

### 追加修复 13:24：首页 toast、编辑空间与顶部布局

- 修复 toast 不消失的问题：`fridge_ui_toast()` 现在记录 1.8 秒过期时间，UI 主循环到期后自动隐藏；toast 位置上移到 Dock 上方，避免遮住底部导航。
- 补齐“编辑空间”的首版固件闭环：点击首页“编辑空间”会创建或进入自定义空间，并跳转到该空间九宫格；自定义空间页显示“重命名”和“删除空间”按钮，普通标准分区不显示删除入口。
- 首页顶部留白压缩：状态文案、标题、操作按钮整体上移；“编辑空间”和“临期 N”按钮加宽，解决实屏中文字贴边问题；冰箱地图同步上移并略增高度。
- 重新从当前 UI 源码抽取中文字符并生成 Dengb 粗体 16/24/32px 字库，覆盖新增“重命名”“删除空间”“最多两个自定义区域”等文案，避免新增按钮出现方框。
- 未改动已验证稳定的屏幕刷新参数，继续保持 `40MHz + 20 行分块 + 120 行 LVGL 缓冲`。
- 验证：`idf.py build` 通过，`fridge_spirit.bin` 大小 `0x1e6cd0`，最小 OTA app 分区剩余 `0x59330`（约 15%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20 brightness=96%`、`FT6336U ready addr=0x48`、`LVGL UI started`，未见 brownout/watchdog/Guru。
- 日志保存：`tmp/logs/home_space_toast_layout_boot_20260528-132430.log`。

### 追加迁移 13:58：ui-reference 全功能移植 P0/P1 交互骨架

- 按用户确认“ui-reference 就是目标设计”继续迁移，不再把参考稿作为简化灵感；已读 `AGENTS.md`、`doc/memory.md` 并使用 ESP32/LVGL 工作流，所有编辑前备份放入 `tmp/backups/manual/`，构建/烧录前备份放入 `tmp/backups/before_run/`。
- 补齐全局页面状态骨架：待机页现在隐藏状态栏和 Dock，并使用完整 720x720 内容区；状态栏左侧支持“返回”，可按参考流程从 zone/editFood/door/camera/cameraResult/settings/wifi/more/offline 回到上一层；点击状态栏可展开设备状态面板，展示 Wi-Fi、信号、传感器、雷达和库存摘要。
- 补齐“位置编辑”闭环：食材编辑页点击位置字段后进入位置选择模式，先回首页选择冰箱区域，再进入九宫格选择具体格子，完成后更新 `editing_food` 的 zone/cell/location、清理旧格位置并回到食材编辑页；选择模式中可用状态栏返回取消。
- 首页“编辑空间”改为参考语义的首页内联编辑模式：可选择总览区域、编辑名称/备注/宽度/高度、添加自定义区域、删除自定义区域；标准区域仍禁止删除。当前固件保持固定内存上限，最多 2 个自定义区，后续若要完全动态区域需单独扩大 `FRIDGE_UI_ZONE_COUNT` 和库存数组。
- 拍照登记页补齐模拟业务闭环：结果页按参考显示识别结果卡、建议位置、语音/手动补充提示、确认放入继续拍摄和修改入口；确认会轮换本地样本并写入库存缓存，修改会进入食材编辑页。
- 菜谱、购物、更多、设置页补齐第一轮参考交互：菜谱卡可展开做法详情，购物清单支持勾选并保留已加入项，更多页补齐 AI 菜谱/购物清单/烹饪问答/营养助手/定时器/家庭同步/离线模式/设备运维入口，设置页补齐设备运维、开门提醒、语音唤醒和临期提前提醒切换。
- 重新从 `ui-reference/WLW` 与 `components/ui` 源码抽取中文字符，用 `Dengb.ttf` 生成 16/24/32 三档字体，当前字形覆盖约 599 个非 ASCII 字符，避免后续参考页面文案再次出现方框。
- 未改动已验证稳定的屏幕刷新参数，继续保持 `pclk=40000000Hz`、`LCD_FLUSH_CHUNK_ROWS=20`、`UI_BUFFER_ROWS=120`，避免回到此前黑屏/花屏风险。
- 验证：三轮 `idf.py build` 均通过，最终 `fridge_spirit.bin` 大小 `0x1fd580`，最小 OTA app 分区剩余 `0x42a80`（约 12%）；`idf.py -p COM16 flash` 成功。复位日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20 brightness=96%`、`FT6336U ready addr=0x48`、`LVGL draw buffers ready, rows=120 bytes_each=259200`、`LVGL UI started`，未见 brownout/watchdog/Guru。
- 日志保存：`tmp/logs/20260528-135741_ui_reference_batch3_boot.log`。当前仍可见 MPU6050 未应答、SNTP 偶发超时、MQTT 未配置警告，不影响本次屏幕 UI 移植验收。
----
完成时间：2026-05-28 18:18:40 +08:00

- 完成 OTA 分区策略从等大小双 OTA 调整为 `ota_0` 大主固件 + `ota_1` 小型本地 recovery 的方案落地，避免把云端备份作为唯一兜底。
- 新增 `partitions_recovery.csv`，当前 active 分区表为 `ota_0 3584 KB`、`ota_1 1024 KB`、`assets 1536 KB`、`cache 704 KB`、`model 768 KB`、`coredump 64 KB`。
- 新增 recovery 构建 profile：`CONFIG_FRIDGE_RECOVERY_APP=y` 与 CMake 参数 `FRIDGE_RECOVERY_BUILD=ON`，recovery 模式只启动 NVS、诊断、Wi-Fi 和 JSON Lines 兜底命令。
- recovery 支持 `get_status`、`set_network`、`boot_main`、`restore_main`，其中 `restore_main` 通过 HTTPS OTA 覆盖 `ota_0` 并切回主固件启动；当前只依赖 HTTPS 证书校验，后续生产前必须补固件签名/哈希校验。
- 已同步 `AGENTS.md`、`CLAUDE.md`、`doc/项目架构与工作流设计.md`、诊断信息和 Web Mock 文案；`partitions.csv` 当前被系统锁定，实际构建以 `partitions_recovery.csv` 为准。
- 验证结果：主固件 `idf.py build` 通过，主镜像约 `0x201900`，预期提示 `ota_1` 小分区无法容纳完整主固件；recovery profile 构建通过，recovery 镜像约 `0xf62e0`，能放入 1 MB `ota_1`，但仅剩约 4% 空间，后续新增 recovery 功能前需要先做体积裁剪或重新评估分区。

----
## 2026-05-28 19:05 设置页与 Wi-Fi 配网页重构

- 按 `ui-reference/WLW` 的设置页结构重新移植开发板 LVGL 设置页：保留“设备与画面设置 / 小精灵偏好”层级，设置项改为可滚动列表，包含屏幕亮度、Wi-Fi 网络入口、设备运维、开门提醒、语音唤醒和临期提前提醒。
- 重新移植 Wi-Fi 配网页：页面改为“设备网络 / 选择 Wi-Fi”，扫描结果不再放在白色整块容器中，而是按参考稿使用独立网络卡片；列表区域支持纵向滚动，长 SSID 和状态文案使用裁剪，避免扫描完成后白框、显示不全或挤压 Dock。
- Wi-Fi 交互保持现有后台模型和连接流程不变：点击刷新仍调用 `fridge_ui_model_start_wifi_scan()`；加密网络继续打开原有密码键盘；开放网络可直接调用 `fridge_ui_model_connect_wifi_async()` 连接。
- 未改动 TR230S/QSPI 刷新参数、Wi-Fi 驱动、NVS 保存逻辑和硬件引脚配置，继续保持已验证稳定的屏幕刷新路线。
- 编辑前备份：`tmp/backups/manual/20260528-185514_ui_settings_wifi/`；构建前备份：`tmp/backups/before_run/20260528-190209_before_ui_build/`。
- 验证：`idf.py build` 通过，生成 `build/fridge_spirit.bin`；构建仍提示主固件大于 1 MB `ota_1` recovery 小分区，这是当前大主固件 + 小 recovery 分区策略下的既有预期提示。本次尚未烧录实机验证触摸和扫描后的视觉效果。

----
## 2026-05-29 00:08 登记页摄像头预览卡死修复

- 修复登记页预览失败后反复自动重启 `esp_camera_init/reset` 的问题：页面进入后只自动尝试一次实时预览，失败后停止预览任务并释放摄像头驱动，避免系统卡顿到无法切换页面。
- 快门按钮改为始终可见：预览失败时仍显示“拍照”，用户可直接触发高清抓拍上传 AI；取景框支持点击手动重试预览，不再出现 disabled 的“等待/重试”导致无入口恢复。
- 摄像头组件新增递归互斥锁，串行化 UI 预览、AI 抓拍、USB 诊断和 reset/get/clear 状态访问，避免多个任务并发 `esp_camera_deinit()`、`esp_camera_fb_get()` 导致驱动状态错乱。
- `init_raw_camera()` 与硬件 JPEG 初始化在 warmup 失败时会主动反初始化并清理模式标志，避免保留 `s_initialized=true` 的半初始化状态，被下一次预览误复用。
- 编辑前备份：`tmp/backups/manual/20260528-235249_camera_page_freeze_fix/`、`tmp/backups/manual/20260528-235615_camera_mutex_fix/`；构建前备份：`tmp/backups/before_run/20260528-235456_camera_page_build/`；进度文件备份：`tmp/backups/manual/20260529-000728_progress_camera_page_fix/`。
- 验证：`idf.py build` 通过并生成 `fridge_spirit.bin`；`idf.py -p COM16 flash` 成功；复位日志确认 PSRAM memory test OK、`TR230S display ready`、`FT6336U ready`、`LVGL UI started`，未见 brownout/watchdog/Guru。USB `get_status` 与 `get_sensors` 可正常响应。仍需实屏触摸确认登记页失败态下可切回首页/设置、快门按钮可见且可点击。

### 追加修复 00:35：完整接线后 OV3660 预览初始化失败修复

- 复现用户反馈的完整接线后错误：登记页/USB 摄像头链路报 `yuv422 qvga software jpeg esp_camera_init failed: ESP_FAIL`。安全探测仍能读到 OV3660 `PID=0x3660`，说明电源、XCLK 和 SCCB 短读基本正常，问题出在完整初始化阶段。
- 第一处根因是正常 UI 固件里 GPIO4/GPIO5 已由 FT6336U/MPU6050 使用 I2C0，而摄像头完整初始化仍尝试用 I2C1 驱动同一组 SCCB 物理线。已改为：正常 UI 固件复用已安装的 `I2C_NUM_0`，`pin_sccb_sda/scl=-1`，摄像头专用测试固件继续保留 I2C1 独占 bring-up 路径。
- 第二处根因是完整 UI 启动后内部 SRAM 碎片较多，esp32-camera 默认 32KB 级 DMA buffer 申请失败，日志曾显示 `DMA buffer 30720 Byte malloc failed, largest free block 22528 Byte`。已将 `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX` 收敛到 `16384`，实测 DMA buffer 变为 `15360`，能在当前屏幕、触摸、USB、网络框架已加载后完成初始化。
- 额外保持摄像头互斥锁和失败态清理：UI 预览、Web 拍照测试、RGB565 诊断和 AI 高清抓拍串行访问 esp32-camera；初始化/warmup 失败会反初始化并清理模式标志，避免留下半初始化状态导致页面卡死。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；COM16 串口回归 `camera_probe` 返回 `PID=0x3660`，`camera_capture` 返回 `320x240` JPEG 约 `8119 bytes`，`get_camera_status` 显示 `initialized=true`、`lastError=""`，`camera_rgb565_diag` 返回 `160x120`、`38400 bytes`、`ESP_OK`。这覆盖了 Web 面板摄像头测试的 QVGA JPEG 路径和登记页实时预览的 RGB565 底层路径。
- 编辑前备份：`tmp/backups/manual/20260529-002141_camera_i2c_guard/`、`tmp/backups/manual/20260529-002744_camera_dma_size/`、`tmp/backups/manual/20260529-003531_progress_camera_dma_i2c_fix/`；构建前备份：`tmp/backups/before_run/20260529-002325_camera_i2c_guard_build/`、`tmp/backups/before_run/20260529-002822_camera_dma_build/`。后续仍建议用户在实屏登记页确认实时画面连续刷新、点击快门能进入 AI 识别结果页。

### 追加修复 00:43：登记页预览改用稳定 YUV422 初始化路径

- 用户实屏反馈 Web 面板摄像头已可用，但固件登记页仍显示 `rgb565 ui preview esp_camera_init failed: ESP_FAIL`。复核确认 Web 路径使用 `YUV422/QVGA + 软件 JPEG`，登记页预览使用 `RGB565/QQVGA` 传感器输出，两者走不同 OV3660 寄存器表和 esp32-camera 原始帧路径。
- 已将 `fridge_camera_capture_preview_rgb565()` 改为复用当前已验证稳定的 `YUV422/QVGA` 初始化路径，再在固件中把 YUYV 软件转换为 LVGL 可显示的 RGB565 帧；这样登记页不再触发 `rgb565 ui preview esp_camera_init`，避免完整 UI 环境下 RGB565 传感器模式初始化失败。
- 新增轻量整数 YUV->RGB565 转换函数，输出帧优先放 PSRAM，单帧约 `320x240x2=153600 bytes`；不新增第三方依赖，不改变摄像头引脚、XCLK、SCCB/I2C 和 AI 高清抓拍路径。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；COM16 回归 `get_status` 正常，`camera_capture` 返回 `320x240` JPEG 约 `8485 bytes`，`get_camera_status` 显示 `YUV422->JPEG`、`lastError=""`，`camera_rgb565_diag` 仍返回 `160x120`、`ESP_OK`。后续需用户在实屏登记页确认取景画面显示方向和颜色是否符合预期。
- 编辑前备份：`tmp/backups/manual/20260529-004016_camera_preview_yuv_path/`、`tmp/backups/manual/20260529-004334_progress_camera_preview_yuv/`；构建前备份：`tmp/backups/before_run/20260529-004108_camera_preview_yuv_build/`。

### 追加修复 00:48：登记页预览任务创建失败修复

- 用户实屏反馈登记页一直停在“正在打开摄像头”，右上角显示“预览任务创建失败”，点击拍照无反应；同时浏览器实时预览可用。判断摄像头底层已正常，问题在 LVGL 登记页 `xTaskCreate()` 为预览/识别任务分配内部 SRAM 栈失败。
- 已将登记页预览任务和 AI 图片识别任务改为优先使用 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 创建 PSRAM 栈，并保留内部 SRAM 创建兜底；预览栈从 `8192` 降到 `6144`，识别栈从 `32768` 降到 `24576`，减少内部内存压力。
- 任务创建失败时会写入更明确状态，并设置 `s_preview_failed=true`、清理 `s_preview_task`，允许用户点取景框重试；避免之前“已经尝试但未标失败”的状态让重试入口失效。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；COM16 回归 `get_status` 正常，`camera_capture` 返回 `320x240` JPEG 约 `6527 bytes`，`get_camera_status` 显示 `YUV422->JPEG`、`lastError=""`。仍需用户实屏确认登记页预览任务可创建、快门按钮点击后能进入识别流程。
- 编辑前备份：`tmp/backups/manual/20260529-004532_camera_page_task_caps/`、`tmp/backups/manual/20260529-004829_progress_camera_task_caps/`；构建前备份：`tmp/backups/before_run/20260529-004602_camera_task_caps_build/`。

### 追加修复 01:10：拍照后 AI 失败反馈与抓拍降级

- 用户反馈登记页预览已成功，但点击快门后 AI 没反应并回到预览页。复核确认这不是预期体验：普通抓拍、上传、HTTP、API Key 或网络失败都应进入结果页显示原因，不能静默回预览。
- 已修复登记页 AI 任务创建失败分支：即使 `ui_cam_ai` worker 因内存/PSRAM 栈不可用创建失败，也会写入 `camera_result` 并跳转识别结果页显示“识别任务创建失败”，不再自动回预览造成误判。
- `lv_async_call(analyze_done_async)` 失败时补充状态复位和错误日志，避免极端情况下卡在“识别中”；AI/上传失败仍统一进入结果页展示失败原因。
- USB/AI worker 已优先使用 PSRAM 栈，串口回归确认 `camera_analyze` 不再返回 `camera analyze worker create failed`，说明任务创建压力已缓解。
- AI 抓拍新增 XGA -> VGA -> QVGA 降级策略：完整 UI 场景下 XGA 失败时继续尝试低一档分辨率，优先保证“能拍照上传给 AI”，后续再单独优化高分辨率稳定性。
- USB `camera_analyze` 失败前会先保存摄像头 `last_error` 到 AI 结果，避免 `fridge_camera_clear_frame()` 清空错误后 Web/串口只看到裸 `ESP_FAIL`。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；COM16 回归 `camera_analyze` 已推进到真实 AI 上传前置校验，当前失败原因变为“网络未校时或外网不可用，请确认 SNTP/互联网连接”，且 `get_camera_status` 显示 `frameId` 增加、`pixelFormat=YUV422->JPEG`，说明拍照链路已启动并完成抓拍。下一步应排查 Wi-Fi 虽 connected 但 `internet_ready=false` 的 SNTP/外网验证问题。
- 编辑前备份：`tmp/backups/manual/20260529-010116_camera_ai_failure_feedback/`、`tmp/backups/manual/20260529-010255_camera_ai_capture_fallback/`、`tmp/backups/manual/20260529-011009_progress_camera_ai_feedback/`；构建前备份：`tmp/backups/before_run/20260529-010138_camera_ai_feedback_build/`、`tmp/backups/before_run/20260529-010322_camera_ai_fallback_build/`。

----
完成时间：2026-05-29 10:38:42 +08:00

- 完成三端数据备份与双向同步第一版，覆盖库存、冰箱分区、AI/ASR/TTS 配置、提醒确认、购物清单、菜谱缓存和 AI 对话历史。
- 后端新增同步层：`/api/v1/sync/status`、`/api/v1/sync/snapshot`、`/api/v1/sync/pull`、`/api/v1/sync/push`，并补了可重复执行迁移脚本 `backend/scripts/migrate_sync.py`；同步事件使用 `clientEventId` 幂等，`serverRevision` 单调递增。
- 固件侧 `components/storage` 新增库存同步元数据，`ui_inventory.json` 现在持有 `snapshotVersion`、`serverRevision`、`updatedAtMs`、`lastSyncAtMs`、`dirty`、`homeId`、`deviceId`；`components/mqtt_protocol` 现在支持 `inventory_refresh`、`inventory_replace`、`fridge_zones_update`、`ai_config_update`、`asr_config_update`、`tts_config_update`，并把 `cmd_ack` 扩展为 `received/completed` 两阶段语义。
- 小程序侧补齐同步队列与离线回放：库存、提醒、分区、设置、购物清单、菜谱和 AI 历史入口已接入 `syncQueue` / `dirtyDomains` / `syncNow`，离线后再恢复网络会先推本地变更再拉云端快照。
- 验证：`python -m compileall backend/app backend/scripts` 通过；`web/node_modules/.bin/tsc.cmd --noEmit -p miniapp/tsconfig.json` 通过；`$env:IDF_TOOLS_PATH='D:\esp-IDF\.espressif'; . D:\esp-IDF\.espressif\v6.0.1\esp-idf\export.ps1; idf.py build` 通过。
- 风险与下一步：当前 `ota_1` 仍有分区过小警告，说明主固件体积继续超过 recovery 分区；这次未做实机 COM16 联调，`inventory_refresh` / `inventory_replace` / `cmd_ack completed` 语义还需要结合真实 MQTT 路径再确认一轮。

----
完成时间：2026-05-29 12:00:44 +08:00

- 新增 `components/ai_actions` 白名单动作层，统一解析 AI 回复里的 UI 页面切换和已有厨房工具 JSON；当前只允许低风险页面跳转、定时器、秒表和闹钟，不开放拍照、亮度、音量、Wi-Fi 连接、库存写入、OTA 或 GPIO。
- UI 组件新增 `fridge_ui_set_page_async()` 和页面 key 映射，AI/USB/语音后台任务只投递页面切换请求，实际 LVGL 操作仍回到 UI 线程执行，避免跨线程触碰 LVGL 对象。
- USB `ai_assistant_chat`、屏幕 AI 语音页和公共语音会话都改为调用统一动作执行入口；`MEMORY_OP` 先处理，AI 动作 JSON 再执行并从用户可见回复中剥离。
- AI 提示词和 `ai_context` 新增 `ui_control` 任务说明，明确 `camera` 页面暂不允许自动进入，避免未确认硬件状态时启动 OV3660 预览。
- 文档已同步 `doc/AI运行逻辑与记忆策略.md` 和 `doc/AI系统设计架构.md`。后续需要通过 Web Serial 和实屏语音各验证一次“打开菜谱页/回主页/打开 Wi-Fi 设置/拒绝拍照页”。

----
完成时间：2026-05-29 12:47:02 +08:00

- 修复状态机休眠开关后屏幕仍黑的问题。根因不是 `sleepEnabled` 配置，而是状态机/音频/唤醒词等组件先于 UI 启动后挤压并碎片化内部 DMA SRAM，导致 `fridge_display_init()` 申请 TR230S QSPI 刷新缓冲失败；同时排查过程中临时显示参数偏离了 2026-05-28 已验证的稳定版本。
- `components/display/fridge_display.c` 恢复到已验证屏幕参数：`LCD_PCLK_HZ=40000000`、`LCD_FLUSH_CHUNK_ROWS=20`，TR230S 初始化命令恢复为 `0x20=0 -> 0x21=0x64 -> 0x29 -> 0x20=brightness`，移除 WAIT# 低电平时强发命令的实验路径；保留初始化失败后的 SPI device / buffer 清理，避免 UI 重试时卡在半初始化状态。
- `main/main.c` 调整正常启动顺序：网络/AI/MQTT 后先初始化传感器，再尽早启动 LVGL UI，并等待显示 ready 后再启动 ASR、雷达、音频、状态机、扬声器、厨房工具和唤醒词。这样屏幕先占住连续 DMA 内存，同时避免触摸初始化与主线程并发二次初始化 ADC。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功。启动日志确认 `TR230S display ready, pclk=40000000Hz chunk_rows=20 brightness=96%`、`FT6336U ready addr=0x48`、`LVGL draw buffers ready, rows=120 bytes_each=259200`、`LVGL UI started`、`usb_protocol: USB JSON Lines protocol task started`，未见 Guru/abort/brownout/watchdog。USB 回归 `get_state_machine_config` 返回 `sleepEnabled:false`，`get_state_machine_status` 当前为 `SLEEP` 且不会黑屏休眠。
- 仍需注意：当前 MPU6050 未连接时日志会提示 `mpu6050 init failed: ESP_FAIL`，不影响屏幕点亮；扬声器和麦克风仍共用 GPIO40/41/I2S，日志会出现 I2S 占用提示，后续音频双向同时使用前需要单独梳理。构建仍保留主固件超过 1 MB `ota_1` recovery 小分区的既有警告。

### 追加修复 13:56：黑屏休眠与颜文字待机的 2 分钟策略

- UI 策略层新增 `UI_IDLE_SLEEP_MS=120000`，分别记录最近用户互动时间和最近雷达人体上下文时间。触摸、用户页面切换和 AI/USB 页面指令仍会刷新互动时间；系统自动退颜文字页不再刷新互动时间，避免自动切页反复重置 idle 计时。
- 当“黑屏休眠”开启时，只有同时满足“用户 2 分钟无互动”和“雷达 2 分钟无可靠人体/接近/2m/1m 上下文”，且当前处于 `SLEEP/NIGHT_SAVE/OFFLINE` 这类静默状态，才进入亮度 0 的黑屏休眠。
- 当“黑屏休眠”关闭时，只要用户 2 分钟无互动或雷达 2 分钟无人体上下文，就自动退回待机颜文字页，并保持屏幕亮度 65，触摸仍可唤醒到主页。
- `SLEEP/NIGHT_SAVE` 状态不再立即抢占当前页面，改由上述 2 分钟 idle 策略决定是否黑屏或退颜文字；`APPROACH/INTERACTIVE/DOOR_* /OFFLINE` 仍保留状态机页面联动。
- 验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；启动日志确认 `TR230S display ready`、`FT6336U ready`、`LVGL UI started`、`usb_protocol: USB JSON Lines protocol task started`，未见 Guru/abort/brownout/watchdog。尚未实机等待满 2 分钟做完整 idle 长测。
- 追加修复 14:09：重启/重新烧录后默认页改为首页，离线状态只作为 `offline` 标志和状态栏/USB/Web 上报信息，不再把 UI 自动切到离线整页；离线页仍可从“更多”页面手动进入。状态机在网络未连接且本地传感无事件时保持 `SLEEP` 主状态，避免 Wi-Fi 尚未连上时抢占本地首页流程。
- 追加修复 14:17：修复 2 分钟退到颜文字页后，点击唤醒到首页又被立即拉回颜文字的问题。触摸/页面操作现在会同步刷新雷达人体上下文计时，并把触摸唤醒保护窗口扩展到一个完整 idle 周期，避免雷达缺席或旧的“无人已超时”状态抢占用户操作。
- 追加修复 14:34：网络组件新增已保存 Wi-Fi 自动恢复任务。设备未连接且不在手动连接中时，会每 45 秒定向扫描 NVS 中保存的 SSID，扫描到后复用现有 `fridge_network_connect()` 自动连接；同时新增扫描互斥，避免 UI/USB 扫描、后台扫描和连接流程抢占 Wi-Fi driver。`idf.py build` 通过并已烧录 COM16；`ota_1` 小 recovery 分区过小提示仍为当前分区策略下的既有预期。

----
完成时间：2026-05-29 21:12:59 +08:00

- 新增 Google (Gemini) 原生 API 格式兼容，可在 Web 面板按配置槽选择"接口格式"（OpenAI 兼容 / Google Gemini），与现有 base_url/model/key 一样按 profile 持久化到 NVS。
- 固件 `components/ai_client`：新增 `fridge_ai_api_format_t` 枚举与 `api_format` 字段；NVS 增加 `api_format` key（0 号槽固定 key、其余槽前缀 'f'），缺省 0=OpenAI 保证旧配置兼容。
- 请求构造按格式分发：保留原 OpenAI builder（重命名为 `_openai`）不动，新增 `build_chat_request_google` / `build_assistant_request_google` / `build_image_request_google`。Google 走 `{base}/models/{model}:generateContent`、`x-goog-api-key` 头、`system_instruction`+`contents[{role,parts}]`+`generationConfig` 请求体，历史助手角色映射为 `model`，图片用 `inline_data{mime_type,data}`。assistant 策略提示词与图片系统提示词提取为公共函数，两种格式共用避免行为漂移。
- 响应解析未改动：现有 `parse_chat_response()` 的 `text` 回退正好命中 Gemini 的 `candidates[].content.parts[].text`，错误体 `{"error":{"message"}}` 已被 `set_http_status_error()` 覆盖。
- USB 协议 `print_ai_config_payload` 输出 `apiFormat`，`set_ai_config` 读入 `apiFormat`；Web `App.tsx` 加"接口格式"下拉、切到 Google 时自动建议 `https://generativelanguage.googleapis.com/v1beta` 端点、base URL/模型 placeholder 联动、配置槽列表项加格式标签；`types.ts`/`mockData.ts`/MockTransport 同步 `apiFormat`。
- 覆盖链路：文字测试聊天、AI 助手对话（含语音复用）、摄像头图片识别三条链路都按格式分发；UI 页(page_ai/page_camera)与 voice_session 经统一入口自动继承。
- 验证：`web` `npm run build` 通过；`idf.py build` 通过，主固件 0x261d20，ota_0(3584K) 仍有约 31.9% 余量，`ota_1` recovery 小分区过小提示为既有分区策略预期。真机 COM16 烧录与 Google/OpenAI 双格式二轮对话、识图端到端回归待在硬件侧执行。
- 编辑前备份：`tmp/backups/manual/20260529-205750_google_api_format/`（含 ai_client 头/源、usb_protocol、web types/App/MockTransport/mockData）。

----
完成时间：2026-05-30 02:30:00 +08:00

### 三端同步 7 个数据互相覆盖/丢失问题修复

修复用户报告的 P0/P1/P2 共 7 个同步缺陷，核心是把"后写覆盖"从"服务器到达顺序"改为基于真实编辑信号判定，并阻止旧设备数据反向覆盖新云端数据。涉及后端 Python、小程序 TypeScript，固件侧无需改动。

- P0（设备重连用旧数据反向覆盖云端）：`sync_service._replace_inventory_from_device_snapshot` 新增结构变更门槛 `allow_structural = device_dirty or device_revision >= cloud_revision`。设备 `updatedAtMs` 来自 `esp_timer_get_time()`（自启动相对毫秒），与云端 epoch 不同源**不能直接比大小**，因此改用固件 `dirty` 标志（本地编辑置 true、同步成功置 false）+ 设备上报的 `serverRevision` 是否落后于云端 `SyncState.current_revision` 判定。设备未脏且 revision 落后时只刷新已匹配的活跃条目值，绝不新增/删除/复活条目；首次绑定（cloud_revision=0）或设备真有本地修改时才允许整快照对齐。新增 `_home_current_revision()`。
- P1 #2（命令 publish-and-forget）：`sync_device_bridge.push_cloud_snapshot_to_devices` 新增 `wait_for_ack`，只对 `status=='online'` 的设备实际下发，逐条注册 ack waiter 并 `asyncio.wait_for` 等 completed ACK（`DEVICE_ACK_TIMEOUT_SECONDS=8`），区分 `queued_count`/`acked_count`/`skipped_offline`。**仅 HTTP `/sync/device-push` 传 `wait_for_ack=True`**；MQTT `_on_message` 回调链里调用保持 False，避免 gmqtt 串行 dispatch 下等 ACK 自我死锁。`SyncDevicePushData` + 小程序类型扩字段。不建持久 outbox，离线补发仍靠固件重连。
- P1 #3（在线 REST 成功后又入队造成重复投影）：`inventory-detail`/`scan`/`reminders` 页 REST 成功后不再 `enqueueSyncOp`，只在失败/离线走 `*_pending` 入队；后端 `push_events`/`_apply_event` 的后写覆盖作为旧 payload 回放的兜底。
- P1 #4（设置双存储分叉）：`settings_route.settings_update` 加 `get_active_home` 依赖，写完 `UserSettings` 后同事务镜像到 `SystemConfig('settings')`（merge 合并 privacy/preferences）+ `record_server_event` + 推设备，使 `/sync/snapshot` 与设备拿到同一份设置。
- P1 #5（AI/API Key 普通同步通道不真正同步）：小程序 `maskAiSyncPayload`→`buildAiSyncPayload`，保留真实 `apiKey/asrApiKey/ttsApiKey` 并附 `configUpdatedAt`；清 key 改发空串 `{apiKey:""}` 等，与后端"空串=清空"对齐。**安全权衡：明文 Key 会进小程序本地 syncQueue 与 `sync_events.payload`，仅适用于受控比赛环境，公开部署需改加密通道。**
- P2 #6（固件非库存文档无本地编辑→上报闭环）：经核查固件当前无购物/菜谱/提醒/偏好的本地 UI 编辑器（只从云端接收），不存在实际 bug，按"不提前堆抽象"暂缓。未来新增本地编辑器时应参照 `fridge_ui_model.c` 的 `model_persist_snapshot → fridge_mqtt_request_inventory_publish` 模式，为各 `fridge_storage_set_*` 加 local-write 触发上报。
- P2 #7（后写覆盖按到达顺序而非编辑时间）：新增 `_coerce_updated_at_ms`/`_event_updated_at_ms` 统一解析毫秒/ISO/datetime；`_upsert_inventory_item` 与 `_upsert_config_doc` 对**小程序来源**事件按 `updatedAtMs` 做后写覆盖（旧的跳过覆盖，事件仍记录供审计）。小程序 `enqueueSyncOp` 自动注入 `updatedAtMs`，后端 inventory/fridge_zone/ai_config REST 的 `record_server_event` 与库存行 `extra.syncUpdatedAtMs` 补编辑时间。设备快照因时钟不同源关闭单条数值比较（传 `updated_at_ms=0`），由 P0 结构门槛兜底。
- 数据库：全部用既有 JSONB 字段（`InventoryItem.extra.syncUpdatedAtMs`、`SystemConfig.value.updatedAtMs`）承载编辑时间，**零 schema 迁移**，不新增表/列。
- 验证：所有改动后端文件 `py_compile` 通过；纯函数单测（`_coerce_updated_at_ms`/`_event_updated_at_ms` 含 bool/ISO/Z/嵌套 item 边界）通过；P0 门槛用 fake DB 三场景单测通过（旧 clean 快照保留云端不删/不增、脏设备正常增删、首绑种子导入）；小程序 `tsc --noEmit`（strict）零错误。真机 COM16 端到端回归（设备离线改库存→上线不反覆盖、立即同步 acked_count、离线存/清 Key 恢复后生效）待硬件侧执行。
- 编辑前备份：`tmp/backups/manual/20260530-022129_sync_fixes/`（含后端 services/routes/schemas、小程序 services/pages/types 共 16 文件）。

----
完成时间：2026-05-30 03:30:00 +08:00

### 语音链路去重 + 对话历史上行同步打通 + 散落备份整理

消除屏幕 AI 页与公共组件之间重复的语音链路实现，并补齐"设备语音对话历史 → 云端 → 小程序"上行缺口（此前固件从不上报对话历史，云端 `ai_history` 落库分支与小程序 `ai-chat` 页是无数据触发的活代码）。固件改动，后端/小程序无需改动。

- 重构（消除重复）：`page_ai.c` 此前手写了一整套 `ai_voice_task`+`ui_ai_convert_history`（ASR→上下文→AI→剥离指令→存历史），与公共组件 `voice_session` 近乎逐行重复。把 `voice_session` 扩展为可复用全链路：新增 `fridge_voice_session_options_t{play_tts, manage_wake_word}` 和带 options 的入口 `fridge_voice_session_run_latest_recording_ex()`，旧入口 `_run_latest_recording()` 包装为全 false（USB/状态机不变）。result 增加 `visible_reply`（剥离 MEMORY_OP/工具指令后的可展示文本）与 `tts_played`。把 page_ai 独有的 **TTS 播报**和**唤醒词暂停/恢复**收口进 voice_session 用 options 控制——避免简单删除 page_ai 导致丢 TTS。page_ai 改为瘦 worker 调 `_ex({play_tts=true})`，删除约 180 行重复代码；唤醒词围绕"录音窗口"仍由 page_ai 自行管理（录音早于 voice_session 调用），故传 `manage_wake_word=false`。三个调用方（page_ai/USB/状态机）现共享同一套历史裁剪逻辑。
- 上行同步（增量 + miniapp-default + 每轮触发 + 重连补发）：
  - `storage` 新增上行游标（NVS `fridge_store` namespace，key `chat_sync_at`，存"最近已上报消息的 created_at 秒"）：`fridge_storage_get/set_chat_synced_at()` 与增量查询 `fridge_storage_get_chat_history_since(after_ms)`（复用 get_chat_history 的过期/超量裁剪，只回 created_at 更新且非空的消息）。`clear_chat_history` 时游标归零，避免清空后残留游标漏发后续对话。
  - `mqtt_protocol` 新增 `fridge_mqtt_publish_chat_history_delta()`：读游标→取增量→构造 `{"sessionId":"miniapp-default","messages":[{role,content}]}`、域 `ai_history`、op `append`（复用 `add_storage_doc_event` + `make_topic(MQTT_TOPIC_SYNC)` + publish 结构）；**publish 成功后才推进游标**，失败保持游标靠重连补发。messages 只放后端读取的 role/content。
  - 触发点：voice_session 写完本地历史后立即调上报（每轮）；`publish_reconnect_sync_seed()` 末尾追加调用（离线攒下的增量重连补发）。增量保证每条只发一次，后端 `ai_history` 分支天然不重复落库（无需改后端）。sessionId 固定 `miniapp-default`，与小程序 `/ai/history` 同 session，设备对话直接在小程序可见。
- 依赖：`voice_session` CMakeLists REQUIRES 增加 `speaker wake_word mqtt_protocol`。voice_session→这三者均为单向，mqtt_protocol 不反向依赖 voice_session，无循环依赖。
- 散落备份整理：按 AGENTS.md 第 4 条，把散落在 `components/`、`web/src/`、`tmp/` 根目录的 16 个 `.bak`（此前被 git 跟踪，污染源码树）**移动**到 gitignored 的 `tmp/backups/archive/scattered_bak_cleanup_20260530/`（保持相对路径，不删除）。`doc/memory.md.bak-*` 两个文件用户本会话前已自行删除，未触碰。移动后 `git status` 显示对应 .bak 为删除（内容安全保存在归档目录）。
- 验证：`idf.py build` 通过，voice_session/ui(page_ai)/mqtt_protocol 三组件均编译链接成功、无循环依赖报错；`idf.py size` 主固件镜像约 2.45MB，`ota_0`(3584K) 余量充足；`ota_1` 小分区过小提示为既有非对称分区策略预期。真机 COM16 端到端回归（屏幕 AI 页录音→识别→显示干净回复→TTS 开关 ON/OFF 行为、USB voice_chat_stop 仍正常、设备说话后小程序 ai-chat 页可见且增量不重复、断网说话重连补发）待硬件侧执行。
- 编辑前备份：`tmp/backups/manual/20260530-000000_voice_chat_sync/`（含 voice_session 头/源/CMakeLists、page_ai、mqtt_protocol 头/源、storage 头/源共 8 文件）。

----
完成时间：2026-05-30 19:30:00 +08:00

### 开关门状态机改为运动事件翻转模型 + 修复 vibration_peak 重力 bug

把门状态机从"超时关门模型"重构为用户预期的"运动事件翻转模型"，并修掉一个会让关门后自动语音几乎永远触发不了的根因 bug。涉及固件 `sensors`、`state_machine` 与 `web/` mock，后端/小程序无需改动。

- 根因 bug（vibration_peak 含重力）：`fridge_sensors.c` 原本 `vibration_peak = max(|ax|,|ay|,|az|)`，静止时约等于 1g（实测 `MPU6050_IMU_TEST_SUMMARY.md` 记录静止值 1.12），远高于状态机运动阈值 0.12g，导致 `imu_moving` 恒真、门态永远卡在运动态、关门后 `maybe_start_auto_voice()` 几乎不可达。改为去重力动态加速度 `|‖a‖ - 1g|`，静止时约等于 0，与架构文档"加速度模长短时峰值 > 0.15g"的原始定义一致。`web/src/data/mockData.ts` 同步改为同一公式，Web"震动峰值"读数从约 1.0 变为静止约 0。
- 事件翻转模型（符合用户预期）：`update_door_state()` 重写为 `关门 --运动事件--> 开门 --运动事件--> 已关门(POST_CLOSE，触发自动语音) --超时--> 关门`。每检测到一次开关门运动翻转一次门态，第二次翻转（回到关门）时调 `maybe_start_auto_voice()`。删除旧的"运动停止后静置 close_stable_ms 即判关门"逻辑。
- 运动检测可靠性增强（`detect_motion_event()`）：① 以陀螺仪三轴角速度之和为主信号（开关门必带门体转动，压缩机震动几乎无角速度，天然过滤）；② 去重力动态加速度为辅（捕捉关门撞击）；③ 进/出运动用不同阈值形成迟滞，避免阈值附近抖动；④ 只在"静止→运动"上升沿且"事件前已静止 SM_MOTION_QUIET_HOLD_MS=500ms""距上次事件 ≥ close_stable_ms"才采纳事件，保证一次物理开关门只产生一次事件；⑤ 多信号佐证（强角速度/撞击/光照突变/雷达有人任一），弱转动无佐证按噪声丢弃。雷达只作佐证之一，不单独判开关门，遵守 AGENTS.md 硬约束。
- 开机防误触：`fridge_state_machine_init()` 把运动计时基线设为当前时刻，要求上电后先静止一段并满足间隔才允许首次事件，避免安装/上电晃动误翻转。
- 配置契约不变：阈值用编译期 `#define`（沿用既有 `SM_*` 风格），未改 USB/web 的 get/set state machine config JSON 字段，Web 面板无需改动。
- 验证：`idf.py build` 通过，`libstate_machine.a` 与 `fridge_spirit.elf` 链接成功，无错误/新警告；`ota_1` 过小提示为既有非对称分区策略预期。真机 COM16 回归（静止时 `get_sensors` 的 `vibrationPeak` 应约 0；开门一次→`doorState=OPEN`、再动一次→`POST_CLOSE` 且 `autoVoiceState` 进入 RECORDING）与 IMU/光照/雷达阈值标定待硬件侧执行。
- 编辑前备份：`tmp/backups/manual/20260530-191907_door_state_event_toggle/`（含 sensors、state_machine、web mock 共 3 文件）。

----
完成时间：2026-05-30 20:35:00 +08:00

### 手动同步改为设备全量文档上行 + MQTT UTF-8 兜底

修复小程序“点击同步只取库存，分区/AI 模型/API URL/Key 不更新”的问题，并处理后端日志中 `MQTT bad payload ... ai_config: invalid UTF-8` 导致设备 retained 配置整包被丢弃的根因。

- 小程序 `miniapp/services/sync.ts`：`requestInitialDeviceSeed()` 和手动 `syncNow({ forceDeviceRefresh: true })` 的 `/sync/device-push` 请求从 `domains:["inventory"]` 改为全量设备域：`inventory/fridge_zones/ai_config/asr_config/tts_config/shopping_list/recipe_cache/reminder/settings`。修复后预期返回不再是 `commandCount:1 domains:["inventory"]`，而应至少包含库存刷新 + `sync_documents_refresh`。
- 后端 `ai_config_service.should_accept_device_config()`：普通 `/sync` 事件通道与 retained `merge_from_device()` 对齐，支持服务端空表用设备配置作为种子，并识别 `forceImport/acceptCleanSnapshot`，让手动同步可强制把设备当前 AI/ASR/TTS 配置导入云端（比赛版允许 Key 走普通通道）。种子判断补齐 ASR/TTS URL、模型、音色、超时字段。
- 后端 `mqtt_client._on_message()`：MQTT payload 遇到非法 UTF-8 时用 replacement decode 继续 JSON 解析并限频告警，避免半个中文字符让整包配置失效。
- 固件 `components/ai_client/fridge_ai_client.c`：`fridge_ai_client_get_sync_payload()` 在生成 MQTT 同步 payload 前清洗 `apiBaseUrl/apiKey/chatModel/systemPrompt/profileName`，把 NVS 中可能被固定缓冲截断的非法 UTF-8 字节替换为 `?`。
- 固件 `components/mqtt_protocol/fridge_mqtt_protocol.c`：`sync_documents_refresh` 读取 `acceptCleanSnapshot/forceImport` 并透传到非库存文档 payload；`publish_device_sync_documents()` 与 retained `publish_ai_config_retained()` 发布前递归清洗 cJSON 字符串，兜住购物清单/菜谱/提醒/设置/AI 配置中的脏 UTF-8。
- 验证：后端相关 Python 文件 `py_compile` 通过；`idf.py build` 通过；已执行 `idf.py -p COM16 flash`，esptool 写入与 hash verify 均成功，设备已 hard reset。小程序本地没有 `package.json/typescript`，`npx tsc -p miniapp/tsconfig.json --noEmit` 无法执行有效类型检查（只提示需安装 TypeScript）。
- 已知非本次问题：构建仍提示 ESP-IDF 6.0 legacy I2C driver EOL 警告，以及 `ota_1` recovery 小分区装不下主固件，均为既有项目状态。
- 编辑前备份：`tmp/backups/manual/20260530-202205_sync_full_device_docs/`、`tmp/backups/manual/20260530-202507_firmware_ai_utf8_sync/`、`tmp/backups/manual/20260530-202642_mqtt_utf8_publish_guard/`、`tmp/backups/manual/20260530-203348_progress_sync_full_docs/`；构建前备份：`tmp/backups/before_run/20260530-202548_sync_verify_build/`。

----

## 2026-05-30 ????????????????

- ??????????????App ??? 15 ????? `syncNow()`??? `serverRevision` ????????????????????????????? `onCloudSyncApplied()`???????????????????
- ??????????????????? overview / inventory / fridge zones????????????????????
- ???????????????????????????????? `zones` ???????????? AI/????/???????????
- ?? MQTT `inventory_refresh` ?????????????`sync_documents_refresh` ? MQTT ??????????????????? MQTT command worker ? MQTT event task ????? JSON/cJSON ???
- ??? `idf.py build` ? `idf.py -p COM16 flash`?????? `get_status` ?? Wi-Fi/MQTT ?? ok???????? `ota_1` ???????????????

----
完成时间：2026-05-30 23:55:00 +08:00

### 摄像头 PSRAM DMA、共享 I2C 锁与 UI 图片识别栈修复

修复 OV3660 在完整 UI/Wi-Fi/MQTT 场景下拍照失败、Web/屏幕识别任务创建失败的问题。核心原因是摄像头 DMA/任务栈仍抢内部 SRAM，以及 GPIO4/5 上 OV3660 SCCB、MPU6050、FT6336U 共用 I2C 时缺少统一总线锁。

- `sdkconfig.defaults` 开启 `CONFIG_CAMERA_PSRAM_DMA=y`，让 esp32-camera 在 ESP32-S3 上走 PSRAM DMA，避免 `DMA buffer 15360 Byte malloc failed`。
- `sensors` 增加共享递归 I2C 锁接口，IMU、触摸和摄像头初始化/控制统一串行访问 GPIO4/5，降低 OV3660 SCCB 初始化被传感器轮询插队导致 `esp_camera_init failed` 的概率。
- 摄像头初始化失败后统一 `esp_camera_deinit()` 并清理半初始化状态，重试前短延迟，避免残留 DMA/LEDC/SCCB 状态影响下一次初始化。
- UI 图片识别和 USB `camera_analyze` 都改为预加载 AI 配置与网络状态，然后用 PSRAM 栈创建后台 worker；修复 `ui_cam_ai internal stack create failed` 和 `camera analyze worker create failed`。
- AI 抓拍路径收敛为完整 UI 场景实测稳定的 QVGA YUV422 采集后软件 JPEG，避免 XGA/VGA 在内部 DMA 连续块只有 4KB 时先失败再降级；同时在抓拍前内置 19ms 级 XCLK/SCCB PID 唤醒探测，并保留 QVGA 同档 3 次重试，解决开机后不先 `camera_probe` 就直接 `camera_analyze` 时首轮初始化偶发失败的问题。
- 验证：`idf.py build` 通过；`idf.py -p COM16 app-flash` 成功写入并 hash verify；COM16 回归 `get_status`、`camera_probe`、`camera_capture`、`camera_analyze` 全部 `ok=true`，AI 图片识别 HTTP 200。屏幕端手动进入相机页验证 `ui_cam_preview task created with PSRAM stack=6144`、`ui_cam_ai task created with PSRAM stack=20480`、`AI image HTTP done: status=200`、`UI camera analyze task done`。最终冷启动后不预先发送 `camera_probe`，直接 `camera_analyze` 也验证通过：日志出现 `AI capture wake probe ok: pid=0x3660`，随后 QVGA 抓拍和 HTTP 200 成功。构建仍有既有 legacy I2C driver EOL 提示和 `ota_1` recovery 小分区过小警告，非本次阻塞项。
- 编辑前备份：`tmp/backups/manual/20260530-233438_camera_ai_psram_worker/`、`tmp/backups/manual/20260530-234828_usb_camera_ai_retry_fix/`、`tmp/backups/manual/20260530-235517_camera_ai_skip_unstable_modes/`；运行前备份：`tmp/backups/before_run/20260530-234401_camera_ai_flash_test/`、`tmp/backups/before_run/20260530-234925_usb_camera_ai_retry_build_flash/`。

- 2026-05-31 小修复：食材编辑页修改到期时间后“默认 7 天”被显示成“2026 天后”。根因是 `components/ui/pages/page_edit_food.c` 的 `normalize_expire_days()` 对所有非“今天/明天”文本直接 `atoi`，而后端 `expire_date` 是 ISO 日期（如 `2026-06-07`），`atoi` 取到年份 `2026`；且 `save_cb` 每次保存都会调用，导致云端同步来的 `days_left` 被覆盖。修复：新增 `parse_iso_date()` 严格匹配 `YYYY-MM-DD`，命中则用当前 SNTP 时间求剩余天数（未对时则保留后端 `days_left` 不破坏），否则回退原“今天/明天/N天后”相对文本逻辑。仅改一个 UI 文件，`idf.py build` 通过。编辑前备份：`tmp/backups/manual/20260531-003426_fix_expire_days/`。真机 COM16 验证（编辑云端食材→改到期→保存后区域页/门页显示正确天数）待硬件侧执行。
- 2026-05-31 小修复：首页“临期 N”数量与点开后的临期提醒页对不上。根因是临期窗口阈值不一致——首页徽标 `count_expiring_items()` 用 `days_left<=2` 统计，但点开的临期页(page_door)、营养页、设置页“提前 3 天”以及后端 home overview(`today+3`) 都用 `<=3`，于是首页少算一项。修复：`components/ui/pages/page_home.c` 的 `count_expiring_items()` 改为 `fridge_ui_model_count_expiring_days(3)`，与全局 3 天定义统一。仅改一个 UI 文件，`idf.py build` 通过。编辑前备份：`tmp/backups/manual/20260531-022341_fix_expiring_count/`。真机 COM16 验证（首页“临期 N”与临期页列表条数一致）待硬件侧执行。
- 2026-05-31 小修复：MQTT 重连后三端同步补发失败，串口出现 `queue inventory seed on reconnect failed: ESP_ERR_NO_MEM` 和 `create sync docs publish task failed, largest_internal=3456 bytes`。根因是重连回调里为库存 seed 与同步文档各临时 `xTaskCreate(..., 8192)`，完整 UI/音频/网络场景下内部 SRAM 最大连续块不足。修复：删除运行期临时 `mqtt_inv_pub` / `mqtt_sync_docs` 任务，改为只置 pending/force 标志并通知已在初始化早期创建的 `mqtt_cmd` 内部栈 worker 串行 drain 发布请求；保持 NVS/LittleFS/Flash 路径使用内部 SRAM 栈，不改 PSRAM 栈，避免 cache 关闭窗口风险。补充成功日志 `publish sync documents queued` 便于后续现场判断。验证：`idf.py build` 通过；`idf.py -p COM16 flash` 成功；COM16 90 秒日志确认 `INMP441 ready`、`MQTT connected`、`publish inventory snapshot queued msg_id=0 len=3962 force=false`、`publish sync documents queued msg_id=0 len=7253 force=false`、`MQTT command worker high watermark=5260/5340 words`，未再出现 `ESP_ERR_NO_MEM`、brownout、watchdog。编辑前备份：`tmp/backups/manual/20260531-121707_mqtt_reconnect_publish_worker/`；运行前备份：`tmp/backups/before_run/20260531-121915_mqtt_build_check/`。已知非本次问题：构建仍提示 legacy I2C driver EOL 与 `ota_1` recovery 小分区过小。

----
完成时间：2026-05-31 16:55:00 +08:00

### 小程序移植 AI 菜谱与营养助手，并补齐 nutrition_cache 云同步

把固件侧厨房 AI 的菜谱缓存与营养助手缓存接入小程序“更多”，并让 `nutrition_cache` 成为和 `recipe_cache` 同级的三端同步文档域。

- 小程序：新增 `pages/nutrition/`，展示 `score/summary/focus/suggestions/restrict_note`，支持按当前库存调用 `/ai/chat` 生成营养建议并入队 `nutrition_cache`；“更多”页新增 `AI 菜谱` 与 `营养助手` 入口；`app.json` 注册营养页。菜谱页新增固件缓存结构兼容，把 `tag/time_minutes/use_inventory/missing/reason` normalize 为小程序卡片视图，同时小程序生成的菜谱同步回云端时按固件 `recipe_cache` 文档结构写入。
- 小程序同步：`types/models.ts` 新增 `NutritionCache` 与 `nutrition_cache` 域；`utils/localFeatures.ts` 新增 `NUTRITION_CACHE_KEY/getNutritionCache/saveNutritionCache`；`services/sync.ts` 拉取快照后回填 `nutritionCache`，手动/初次同步域加入 `shopping_list/recipe_cache/nutrition_cache/reminder/settings`，确保设备端厨房 AI 文档能上行到云端再落到小程序。
- 后端与固件同步：后端 schema/service/device bridge/MQTT 过滤/同步路由均加入 `nutrition_cache`；云端快照返回 `nutritionCache`；设备下发新增 `nutrition_cache_update`。固件 MQTT 同步文档上报新增读取 `fridge_storage_get_nutrition_cache()`，并支持接收 `nutrition_cache_update` 写回 `fridge_storage_set_nutrition_cache()`。
- 类型补齐：小程序自带 typings 增加 `getCurrentPages`，并收窄 `app.ts` 云同步页面通知的 null 类型，保证 strict TS 检查可过。
- 验证：`web/node_modules/.bin/tsc.cmd -p miniapp/tsconfig.json --noEmit` 通过；`python -m compileall backend/app` 通过；`idf.py build` 通过。构建仍有既有 legacy I2C driver EOL 提示和 `ota_1` recovery 小分区过小警告，非本次改动引入。真机 COM16 端到端验证（固件生成营养/菜谱→MQTT 上行→小程序同步显示；小程序生成营养/菜谱→云端→设备屏幕缓存更新）待硬件侧执行。
- 编辑前备份：`tmp/backups/manual/20260531-164159_miniapp_kitchen_ai_sync/`。

----
完成时间：2026-07-02 21:10:00 +08:00

### 开发板迁移启动：ESP32-S3 → 慧勤智远 ESP32-P4 CB V1.2

确定项目从 ESP32-S3-DevKitC-1 N8R8 整体迁移到慧勤智远 ESP32-P4 CB V1.2 小系统板（ESP32-P4NRW32，32MB 片内 PSRAM + 16MB W25Q128 Flash，板载 ESP32-C6 联网），完成迁移决策、S3 存档和引导文档改写。

- 用户决策：屏幕暂未定型，先用原 TR230S 屏经转接排线走板上 20pin SPI 座（仅 SPI 4-wire）测试，后续可能换 MIPI-DSI 屏；摄像头换 OV5647（MIPI-CSI，OV3660 退役）；音频优先板载驻极 MIC + ES8311 + NS4150B，保留外接 INMP441/MAX98357A 回退；ESP-IDF 继续用 6.0.1；S3 用 git 完整存档后 main 分支全面转 P4。
- S3 存档：提交 `ea8d0e8`，分支 `s3-legacy`，标签 `s3-final-20260702`。厂商 P4 资料目录（约 2.7GB）与 `video/` 加入 `.gitignore`，仅保留本地。
- 文档改写：`CLAUDE.md` 新增“Board migration in progress”章节（迁移决策、分工作流状态表、P4 板占用引脚汇总、硬件红线）；`AGENTS.md` 更新项目概览、主要硬件设备、架构原则、硬件安全规则、构建/串口经验和协作者提醒，全部对齐 P4 板。
- 已识别的关键硬件风险（已写入两份文档）：① 板上 20pin SPI 座 Pin1/2=5V、Pin3/4=GND，而 TR230S FPC Pin1=GND、Pin2=5V，严禁直插，转接排线须逐脚核对；② GPIO28/29 共享 I2C 同时接 CSI 摄像头 SCCB 和 ES8311，手册要求分时复用，影响“拍照+语音”并存场景；③ P4 无片内 Wi-Fi，须走 esp_hosted + esp_wifi_remote（SDIO 到 C6）并配置 SDIO 电源管理；④ 新板 COM 口待接板确认，COM16 属旧 S3 板。
- 涉及模块：`CLAUDE.md`、`AGENTS.md`、`.gitignore`、`doc/progress.md`、`doc/memory.md`；固件代码本次未改动。
- 下一步：重建 `sdkconfig.defaults` 与分区表（16MB Flash）→ network 组件接 esp_hosted → 串口/USB 协议在新板打通 → 屏幕转接与点亮 → 摄像头/音频逐个移植（顺序见 CLAUDE.md 工作流表）。
- 编辑前备份：`tmp/backups/manual/20260702-204949_p4_migration_docs/`。
