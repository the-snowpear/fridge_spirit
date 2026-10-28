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

----
完成时间：2026-07-11 01:55:02 +08:00

### ESP32-P4 第一阶段最小移植软件闭环：Wi-Fi、板载麦克风、QSPI 屏与 TF

完成慧勤智远 ESP32-P4 CB V1.2 第一阶段最小固件、接线方案和离线构建验证；本里程碑是“软件/资料已就绪”，尚未声称任何 P4 真机外设通过。

- 构建与配置：工程目标切为 `esp32p4`，使用 ESP-IDF 6.0.1；按 16MB Flash + 32MB HEX PSRAM 重建配置，新增 `partitions_recovery_p4.csv`（ota_0 6MiB、ota_1 2MiB、assets 4MiB、cache 1MiB、model 2MiB、coredump 64KiB，末端 0xF40000）。主/恢复依赖闭包加入 `espcoredump`，recovery CMake profile 与 Kconfig 双向校验，配置报告零 warning。
- Wi-Fi：P4 通过板载 C6 的 SDIO Slot1 联网，固定 GPIO47-52、GPIO54 复位和 LDO4 电源管理；固定厂商版本 `esp_hosted 2.2.3` + `esp_wifi_remote 0.15.0`，并做 IDF 6 头文件/驱动组件依赖兼容补丁。网络首次初始化失败会锁定原始错误并要求重启，避免半初始化状态重复创建 hosted/netif 资源。
- 麦克风：新增板载 ES8311 录音路径，I2S0 为 MCLK=2、BCLK=4、WS=6、DOUT=3、DIN=5，16kHz/16-bit/mono；GPIO11 功放始终保持低。GPIO28/29 使用统一 I2C1 句柄和 owner gate，录音期间禁止未来 OV5647 SCCB 并发访问。
- 屏幕：TR230S 继续走 QSPI-4SDA，P6 飞线 GPIO 为 CS=7、SCLK=9、D0=8、D1=10、D2=20、D3=21、RST=22、WAIT=19，固定 10MHz 起步，最小固件显示黑/红/绿/蓝/白静态色带。触摸默认关闭；只有 20-pin FPC 独立转接板才能把 FT6336U 的 GPIO15/16/17/18 引出。
- TF：板载卡座使用 SDMMC Slot0（GPIO39-45），复用 hosted 已为 Slot1 初始化的唯一 SDMMC 控制器，只新增/释放 Slot0，避免二次 claim 控制器；不自动格式化，只有精确串口确认口令 `ERASE_TF_CARD` 才允许重建 FAT32。TF 依赖 hosted 已成功持有 LDO4，网络/hosted 失败时明确跳过，避免把电源域问题误报成卡或文件系统损坏。
- 串口与回滚：新增 P4 最小 JSON Lines 协议，支持 Wi-Fi、录音/WAV、TF 状态/格式化/自检和诊断。启动先发送 `booting` 并屏蔽硬件命令，所有首阶段组件完成一次初始化尝试后才发送 `ready`，消除上位机与 `app_main` 并发初始化竞态；USB 健康门通过后确认 `PENDING_VERIFY` OTA 镜像，避免下次复位误回滚到 recovery。
- 接线文档：新增 `doc/P4第一阶段最小移植与接线方案.md`，分别给出屏幕原生 2.00mm 2×7 和 20-pin FPC 转接板方案、P6 编号、万用表检查、上电顺序、JSON 测试命令与成功判据。继续严禁 TR230S FPC 直插板载 20-pin SPI 座。
- 验证：完成全部审查修复后，从空 `build/` 目录执行干净全量构建成功；配置与 C/C++ 日志中 `warning:`/`error:` 为 0。最终镜像版本 `p4-minimal-20260711`，`fridge_spirit.bin` 为 0xCB4B0（832,688 字节），SHA-256 为 `C9A8B0D1F53BBFEB85758637CF653498793001B592F0400A2B479150CE310186`，最小 2MiB app 分区剩余约 60%；bootloader 仅余 0x380 字节，后续加安全启动配置前必须重新评估。
- 真机待办：当前 Windows 未发现 P4/CH343P 新串口，仅有蓝牙虚拟 COM；没有执行烧录、屏幕上电、TF 格式化或 Wi-Fi/麦克风/屏幕/TF 真机验收。下一步必须先确认 P6/屏幕 Pin1 照片、接口类型和实际 COM 号。
- 主要备份：`tmp/backups/manual/20260711-004359_p4_minimal_takeover/`、`tmp/backups/manual/20260711-015033_p4_review_fixes/`、`tmp/backups/manual/20260711-102738_ota_rollback_confirm/`、`tmp/backups/manual/20260711-103009_tf_shared_sdmmc_host/`；最终干净构建前备份：`tmp/backups/before_run/20260711-103040_p4_final_p1_fixes_build/`。

----
完成时间：2026-07-11 17:36:36 +08:00

### P4 TR230S SPI-4WIRE 与 FT6336U 最小验证固件

按 20-pin FPC 转杜邦线的实物方案完成显示传输层迁移并启用触摸；本里程碑为“代码与接线方案已就绪、构建通过”，尚未进行 P4 真机上电验证。

- 显示：TR230S 从 QSPI-4SDA 改为 SPI-4WIRE Mode 0，固定 CS=GPIO7、MOSI=GPIO8、SCLK=GPIO9、MISO=GPIO10、WAIT#=GPIO19、D/C#=GPIO20、RESET#=GPIO22，10MHz 起步；GPIO21 不再占用。
- 时序：移除 QSPI `0x02/0x12` 封包与 `SPI_TRANS_MODE_QIO`，改为 D/C# 低发命令、高发参数/RGB565 像素；采用软件 CS，保证 `0x2C` 到最后一个像素块持续选中屏幕。
- 触摸：启用 `CONFIG_FRIDGE_P4_TOUCH_ENABLE=y`，FT6336U 走独立 I2C0（INT=15、SCL=16、SDA=17、RST=18），100kHz、地址 0x48；当前最小固件通过串口日志输出按下/抬起坐标。
- 测试入口：独立屏幕测试组件改为复用正式显示驱动，消除旧 QSPI 测试路径误驱动 GPIO20/21 的风险；P4 `get_pins` 返回值同步更新。
- 接线文档：新增 `doc/P4_TR230S_SPI4WIRE_FT6336U接线方案.md`，按 FPC 1~20 与开发板实物 GPIO 丝印逐脚列出；再次明确板载 20-pin 屏座空置、FPC19 TP-VCC 悬空、I2C 上拉到 3.3V。
- 验证：ESP-IDF 6.0.1 `idf.py reconfigure build` 通过；`build/config/sdkconfig.h` 确认触摸开关已生效。镜像 `build/fridge_spirit.bin` 为 0xCBEF0（835,312 字节），SHA-256 `0714C46693409AF143CB18FE4367194D3A08CFFD0926990758759E72D76405D9`，最小 2MiB app 分区剩余约 60%。
- 未验证：未烧录、未给屏幕上电、未确认 P4 实际 COM 号；完整 S3 LVGL UI 不在本次最小移植范围内。
- 备份：`tmp/backups/manual/20260711-172726_spi4wire_ft6336u/`；构建前备份：`tmp/backups/before_run/20260711-173409_spi4wire_build/`。

----
完成时间：2026-07-11 18:33:37 +08:00

### ESP32-P4 COM15 首次真机烧录与启动验收

完成 P4 CB V1.2 首次真机识别、芯片 revision 配置修正、低速 ROM 模式烧录和启动日志验收；显示总线与板载麦克风已通过固件初始化，触摸与 TF 仍有明确待排查项。

- 串口与芯片：Windows `COM15` 为 `VID_1A86&PID_55D3` USB 串口；esptool 识别 ESP32-P4 rev v1.3。工程原先误用 IDF 6 默认 rev v3.1+ 配置，已切到 `<3.0`/最低 v1.0，并把 CPU 从 400MHz 调整为本板标称 360MHz。
- 烧录：CH343 在 RAM stub 阶段持续出现 `Invalid head of packet (0xE0)`，改用 esptool ROM loader `--no-stub -b 115200` 后成功写入 bootloader、分区表、OTA data 和 ota_0 主固件，全部 Hash 校验通过。
- 固件：`fridge_spirit.bin` 大小 833,168 字节，SHA-256 `D9666D9B8CE32BC1533A759FC59FBF726ABF3B75C14EEAF18F56BE521B85F156`；启动确认 rev 支持范围 v1.0-v1.99、CPU 360MHz、32MB PSRAM 200MHz memory test OK。
- C6 Wi-Fi：SDIO Slot1 40MHz 握手成功，识别 ESP32-C6 并启动 STA；当前无保存的 Wi-Fi 凭据。Host esp_hosted 版本比 C6 slave 新，保留版本匹配待办。
- 屏幕：TR230S SPI-4WIRE 10MHz 初始化成功，静态色带数据刷新完成；等待用户确认实屏色带是否正确、是否有花屏/闪烁。
- 触摸：FT6336U 在 0x48 读取芯片 ID 时返回 `ESP_ERR_INVALID_RESPONSE`，未通过。下一步检查 SDA/SCL 对应关系、3.3V 外部上拉、RST/GND 和杜邦线接触。
- 音频：板载 ES8311 初始化成功，麦克风 16kHz/16-bit/mono ready，未进行实际录音波形测试。
- TF：64GB 卡 FAT 挂载失败，状态为 `formatRequired=true`；未格式化，等待用户确认数据是否允许清空。
- 运行健康：诊断返回 brownout=0、watchdog=0，未见 panic；JSON Lines `get_status/get_diagnostics/get_pins/get_network/get_tfcard_status` 均正常响应。
- 备份：`tmp/backups/manual/20260711-182527_p4_rev1_config/`、`tmp/backups/manual/20260711-182749_p4_cpu360/`、`tmp/backups/manual/20260711-183337_com15_bringup_docs/`；烧录前备份：`tmp/backups/before_run/20260711-182114_com15_flash/`。

----
完成时间：2026-07-11 19:33:59 +08:00

### P4 TR230S 恢复 S3 已验证的 QSPI-4SDA 驱动

针对 SPI-4WIRE 固件上电后屏幕无背光且液晶无明显颜色变化的问题，核对旧点亮记录与 `s3-final-20260702` 后确认同一屏幕的已验证路径实际为 QSPI-4SDA，接口模式由屏内 IM0/IM1 硬件配置决定，因此恢复已知可用协议。

- 显示驱动：恢复 `0x02/0x12` TR230S QSPI 封包、硬件 CS 连续保持和 `SPI_TRANS_MODE_QIO` 四数据线像素传输；P4 固定 10MHz 起步。
- GPIO：CS=7、D0=8、SCLK=9、D1=10、WAIT=19、D2=20、D3=21、RST=22；D/C# 不接。触摸继续使用 INT=15、SCL=16、SDA=17、RST=18。
- 文档与诊断：新增 `doc/P4_TR230S_QSPI_FT6336U接线方案.md`，旧 SPI-4WIRE 文档标记停用；最小串口 `get_pins` 与启动日志同步改为 QSPI。
- 验证：ESP-IDF 6.0.1 全量构建通过；`fridge_spirit.bin` 为 833,440 字节，SHA-256 `BB6CAD6BA6F4B6AD75497BE09CF626E0467328EAF94F537BE5EDDC2046FE261C`，最小 app 分区剩余约 60%。尚未烧录，需用户先在完全断电状态完成 QSPI 改线。
- 备份：`tmp/backups/manual/20260711-193020_restore_tr230s_qspi/`；构建前备份位于 `tmp/backups/before_run/` 对应 `qspi_build` 目录。
- 真机补充（2026-07-11 19:53）：COM15 通过 `--no-stub -b 115200` 完成烧录，bootloader、分区表、OTA data 和主固件均通过 Hash 校验。启动日志确认 TR230S QSPI-4SDA 10MHz 初始化及静态色带刷新完成；FT6336U 成功识别为 0x48（chip 0x64、vendor 0x11）。屏幕实际画面待用户肉眼确认。
- 独立待办：本次 C6 SDIO 能完成卡枚举，但 esp_hosted slave 握手失败，重试时出现 LDO4 已占用，导致 Wi-Fi 初始化失败并跳过 TF；不影响显示和触摸结果。烧录前备份：`tmp/backups/before_run/20260711-194551_qspi_com15_flash/`，启动日志：`tmp/logs/20260711-1952_qspi_boot_com15_second_reset.log`。

----
完成时间：2026-07-11 22:46:02 +08:00

### P4/C6 ESP-Hosted 2.12.11 同版本升级与真机稳定性闭环

修复 C6 Hosted 握手失败、LDO4 重复申请和不安全热重枚举问题，并通过 P4 串口把板载 C6 从旧 `0.0.6` APP-only 在线升级到与 Host 一致的 `2.12.11`。

- Hosted：Host 端替换为官方 `esp_hosted 2.12.11`；SDIO LDO4 句柄在板生命周期内只申请一次并与 TF Slot0 共用，失败路径补齐 card/DMA 释放和错误上传；移除 read task 运行期间的二次 card 重初始化，恢复命令改为安全整板重启。
- C6 OTA：新增流式 OTA 组件和 `tools/c6_hosted_ota.py`，支持 SHA-256、CRC32、会话/offset、断点续传、重复末块确认、旧固件 end 自动激活模式；单块 1024 字节，镜像上限按旧分区表 0x180000 校验。升级工具已修正“7 秒延迟重启期间误读旧进程状态”的判断。
- 真机升级：C6 `network_adapter.bin` 为 0x130EC0（1,248,960 字节），SHA-256 `30FC480E297917F319AAFEA5533EAAE848E9173656608C9717269F9697B3B502`；完整传输、C6 校验、激活及 P4/C6 协调重启成功，当前 Host/Slave 均报告 `2.12.11`。
- 稳定性回归：COM15 连续 3 轮执行安全重启、Hosted 重新握手和真实 Wi-Fi 扫描；每轮 `hostedReady=true`、`c6OtaGuard=false`、Slave=`2.12.11`，分别发现 11/14/8 个热点，无 LDO 重复申请和 SDIO 热重枚举故障。
- Web 面板：继续复用 `get_network/scan_wifi/set_network`，新增 C6 Hosted 就绪/版本/升级保护显示，保护态禁止扫描和连接，清理 ESP32-S3 遗留文案；`npm run build` 通过。当前 NVS 没有保存 Wi-Fi 凭据，因此已验证真实扫描和配网命令链路，实际热点认证/获取 IP 需用户在面板输入 SSID 与密码后完成。
- P4 构建与烧录：ESP-IDF 6.0.1 全量构建通过，P4 `fridge_spirit.bin` 为 898,912 字节、SHA-256 前缀 `23AB067C`；COM15 使用 ROM loader 115200 烧录并通过各区域 Hash 校验。启动日志位于 `tmp/logs/20260711-2238_hosted_2_12_11_boot_com15.log`。
- 编辑前主要备份：`tmp/backups/manual/20260711-220735_c6_hosted_fix/`、`tmp/backups/manual/20260711-224245_c6_ota_wait_fix/`、`tmp/backups/manual/20260711-224436_web_c6_hosted_status/`。
- TF 真机补充（2026-07-11 23:48）：用户明确授权清空 64GB 卡后，COM15 发送 `format_tfcard` + 精确口令 `ERASE_TF_CARD` 成功；卡挂载于 `/sdcard`，容量 59,638 MB、空闲 59,623 MB，`formatRequired=false`、`formattedThisBoot=true`、无错误。随后 `tfcard_selftest` 写入并读回 `/sdcard/fridge_selftest.txt`，内容校验通过。运行前备份：`tmp/backups/before_run/20260711-234659_tfcard_format/`。

----
完成时间：2026-07-12 01:18:00 +08:00

### WebUI TF 卡存储工作台与 COM15 真机闭环

完成 P4 TF 文件系统安全层、JSON Lines 文件协议、Wi-Fi HTTP 大文件服务和独立 Web 存储工作台，并将最终固件烧录到 COM15 真机。

- TF 核心：统一 Slot0 挂载与互斥，启用 UTF-8 API、CP936 和 255 字节长文件名；新增分页列表、属性、目录、移动、回收/恢复、文本原子保存、1024 字节 CRC32 分块和 NVS 专家模式；默认保护 `.fridge_spirit/{trash,uploads,exports}`。
- 安全：拒绝路径穿越、反斜杠、控制字符和非法 FAT 名称；写入先落 `.part`、同步后原子替换；格式化/清空回收站/专家模式继续使用精确口令。安全卸载只释放 TF Slot0，不触碰 C6 Slot1/LDO4。
- HTTP：新增端口 8080 的流式 GET/PUT 服务，路径和方向绑定串口 transfer session；使用 32 字符十六进制一次性 Bearer token、十分钟滑动过期、localhost/127.0.0.1 Origin 白名单、Range 和 offset 续传；无 Wi-Fi/Hosted/IP 时安全拒绝。
- Web：新增独立“TF 卡”页面，包含容量、文件/回收站/维护、中文路径、分页搜索排序、多选、拖放上传、下载、文本编辑、媒体/十六进制预览、传输暂停/继续/取消、专家模式、诊断导出和受保护格式化；MockTransport 覆盖主要流程，S3/N8R8 残留文案已换为 P4 CB V1.2。
- 真机：验证中文目录和 UTF-8 JSON、4096B 二进制串口上传/下载与 CRC、Range 跳读、移动、回收/恢复、专家模式持久开关、自检、64KiB 测速、安全卸载/挂载和诊断包写入/读回。测速约写 1954 KiB/s、读 6458 KiB/s；C6 Hosted 仍为 2.12.11 ready。
- 构建与烧录：Web TypeScript/Vite 构建通过并用真实浏览器检查文件预览及维护页面；ESP-IDF 6.0.1 构建通过，镜像 1,150,480 字节、SHA-256 `457ED52E42C8A7506C87915BA53B329DB65BB41D66A323CF838B97485FEFF035`，2MiB app 分区剩余 45%；COM15 ROM loader 115200 烧录且全部 Hash 校验通过。
- 验证边界：设备尚未保存 Wi-Fi 凭据，当前只验证 HTTP 无网络拒绝；真实 HTTP 1/10/100MiB 吞吐、断网续传和 token 越权测试需配网后继续。同开机 session 可续传，重启后 `.part` 保留但不会自动重建 session；后台 job 进度事件仍为后续体验优化。
- 说明：`doc/TF卡存储工作台使用说明.md`。主要备份：`tmp/backups/manual/20260712-000743_tf_storage_workspace_full/`、`tmp/backups/manual/20260712-005833_tf_workspace_hardening/`；最终构建/烧录备份位于 `tmp/backups/before_run/20260712-010553_tf_workspace_final_build/` 和对应 `tf_workspace_final_flash` 目录。

----
完成时间：2026-07-12 17:10:00 +08:00

### P4 microWakeWord 开源推理链路与模型训练工具

针对 P4 rev1.3 的 ESP-SR less-v3 预编译库无法在 IDF 6/GCC 15 链接的问题，完成 microWakeWord/TFLite Micro 替换方案、启动验证模型、训练数据工具和 Web 诊断适配；本里程碑已经构建通过，但当前开发板未连接，尚未烧录和真机推理验收。

- 推理后端：保留 ES8311 I2S0、NVS 持久开关、音频互斥、自动“唤醒→5秒录音→云 ASR”编排，只替换 `wake_word` 内部算法；使用 30ms/10ms/40维 micro-speech 特征、TFLite Micro 流式 MixConv、5帧滑窗和冷却窗。P4 推理任务栈为 8192 字节，模型在 Flash，64KiB tensor arena 在 PSRAM，I2S DMA 和变量区保留内部 RAM。
- 依赖：移除 `esp-sr 2.4.5`、`esp-dsp`、`dl_fft`，固定 `esp-tflite-micro 1.3.3~1`、`esp-nn 1.1.2`、`esp-micro-speech-features 1.2.3`；实际 `P4 less-v3 + IDF 6.0.1 + GCC 15.2` 主工程已完成编译和链接，不再依赖无法重编译的 WakeNet 静态库。
- 启动模型：内置 ESPHome 官方 Apache-2.0 `Okay Nabu` v2（60,264B，SHA-256 `0689ABE1912A95A3318A0D8CB2E67BAD0CBCFE3E24DD6E050C75DEBDDFB6F891`），只用于验证运行链路；状态固定返回 `targetModelReady=false`，不会冒充“小冰小冰”。公开模型仓库仅找到 Alexa/Hey Jarvis/Hey Mycroft/Okay Nabu 等英文模型，没有找到可直接使用的中文“小冰小冰”。
- 诊断：`wake_status` 新增最近/滑窗/最大概率、推理次数、最近/最大/平均耗时、tensor arena 实际占用和目标模型就绪标志；WebUI 文案从 WakeNet 改为 microWakeWord，并在启动模型阶段显示明确警示。
- 训练工具：新增 COM15 ES8311 真实 WAV 采集脚本、中文近音困难负样本词表、两阶段起始训练参数、manifest 模板和模型安装器；安装器校验 TFLite/manifest，自动备份并原子替换模型，生成 cutoff、sliding window、arena 和 SHA-256 配置。
- 文档：`doc/microWakeWord小冰小冰模型训练与部署.md` 覆盖 Python/Colab 环境、TTS/真人/厨房背景数据、说话人隔离切分、增强、RaggedMmap 特征、MixedNet 训练、阈值扫描、模型安装和 8 小时误触发验收。
- 构建：ESP-IDF 构建和 `size-components` 通过；最终 `fridge_spirit.bin` 为 1,715,792B，SHA-256 `AE145DF5C93917E0200C4C2B49FE49674F007E6AD901CF43D67C1420635AAAE7`，最小 2MiB app 分区剩余约 18%。TFLite Micro 约 132KB Flash，wake_word（含模型）约 71KB，特征组件约 9.8KB。Web TypeScript/Vite 构建通过。
- 真机边界：本轮检查时 Windows 只有蓝牙虚拟 COM3～COM14，未枚举 COM15/CH343P，因此没有猜端口烧录。待开发板重新连接后先烧录启动模型，验证 `inferenceCount` 增长、arena、任务栈、实时耗时和说 “Okay Nabu” 触发，再开始“小冰小冰”数据训练。
- 备份：`tmp/backups/manual/20260712-164741_microwakeword_port/`、`tmp/backups/manual/20260712-170553_microwakeword_model_install/`、`tmp/backups/manual/20260712-170726_microwakeword_webui/`；构建前备份：`tmp/backups/before_run/20260712-165338_microwakeword_build/`。

----
完成时间：2026-07-13 14:00:00 +08:00

### ESP-IDF 6.0.1 → 5.5.1 降级，恢复本地“小冰小冰”WakeNet 并真机验证

把主固件从 ESP-IDF 6.0.1 降级到 5.5.1，撤销上一轮 microWakeWord，恢复 ESP-SR WakeNet less-v3 本地唤醒“小冰小冰”，并在 COM15（ESP32-P4 rev1.3）真机烧录、启动、C6/Wi-Fi/TF/触摸/麦克风/WakeNet 全链路验证通过；仅“说小冰小冰触发 + ASR 转写 + 20 次长稳”待用户参与。详见 `doc/IDF5.5迁移与已验证版本.md`。

- 根因与选型：P4 rev1.3 的 WakeNet/AFE 预编译 less-v3 静态库由 riscv GCC 14.2.0 编译，在 IDF6/GCC15 下 `R_RISCV_RVC_LUI relocation truncated`，删检查/`--no-relax` 均无效。改用官方匹配的 **ESP-IDF 5.5.1（GCC 14.2.0）+ esp-sr 2.4.5**；依据是厂商随板 `小智AI` 例程 `wks_esp32p4_cb_xiaozhi` 的 dependencies.lock 锁定 IDF 5.5.1。esp-sr 2.4.5 的 CMake 在 IDF 5.x 且 PATCH<3 时自动选 `esp32p4_less_v3`——5.5.1 的 PATCH=1 恰好锁定 less_v3；5.5.3+ 会误选 rev3.0 库。构建日志与 map 均确认链接 `lib/esp32p4_less_v3/*.a`。
- 工具链并存：IDF 5.5.1 装到 `D:\esp-IDF5.5.1\esp-idf`（公共工具目录仍 `D:\esp-IDF\.espressif`），IDF 6.0.1 保留可回退；`tools/idf.ps1` 指向 5.5.1。
- 撤销 microWakeWord：逐文件比对 `20260712-164741_microwakeword_port` 备份还原 WakeNet 版 wake_word（CMakeLists/idf_component.yml/fridge_wake_word.c/.h）、usb_protocol_minimal.c、sdkconfig.defaults.esp32p4 的 WakeNet 段，还原 4 个 WebUI 文件；删除 mww 专属文件（fridge_mww_backend.cpp/.h、fridge_wake_model_config.h、models/、tools/microwakeword/、mww 训练文档）——删除前均已备份。全仓 mww 残留清零，web `npm run build` 通过。
- 兼容性改动（未动硬件/分区/C6 Slave）：① codec_es8311 的 idf 依赖 `>=6.0.0,<7.0.0` 放宽到 `>=5.5`；② main/CMakeLists 最小闭包显式加 `esp_psram`（5.5.1 下 esp_psram 是可选依赖不自动纳入，否则 CONFIG_SPIRAM 未注册、PSRAM 关闭）；③ defaults 新增 `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`（5.5.1 把 P4 PSRAM 200MHz 归为实验特性，不开则回落 20MHz）；④ defaults 新增 `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` 修复 hosted SDIO mempool 崩溃；⑤ wake init `esp_log_level_set("AFE", ESP_LOG_ERROR)` 抑制 AFE `Ringbuffer empty` 刷屏。
- C6 Hosted SDIO mempool 崩溃修复：首次 5.5.1 启动 hosted 即 `assert sdio_mempool_create sdio_drv.c:255 (buf_mp_g)`、`mempool create failed: no mem`。根因是 SDIO 缓冲池需约 48KB 片内 DMA 内存，而 IDF6 给 hosted 挂了约 120KB RETENT_RAM 堆区、5.5.1 不再暴露，启动早期片内 DMA 不足。开启 hosted 官方为 P4 提供的 MEMPOOL_PREFER_SPIRAM，让 SDIO 缓冲优先走 DMA 可达的 PSRAM，崩溃消除、片内 RAM 得以保留。
- 干净构建：删 build/managed_components/dependencies.lock/sdkconfig 后 set-target→reconfigure→build。依赖锁定 esp-sr 2.4.5 / esp_hosted 2.12.11 / esp_wifi_remote 0.15.0 / idf 5.5.1；无 R_RISCV 错误、非 Kconfig 编译告警 0。最终 `fridge_spirit.bin` 1,873,728B、SHA-256 `496c8179657694e8b08a81b1397d93b457e6ac167f672e9b8354c65ab6bfd29e`（ota_0 6MiB，2MiB 口径剩约 11%）；`srmodels.bin` 579,120B（565KB<2MiB model 分区）、SHA-256 `baab09bb3822c45bb25eb7cd4889fdb526d033b957b4610a226569557fe96443`，含 wn9_xiaobinxiaobin_tts + vadnet1_medium、无 MultiNet；DIRAM 静态用 145,588B(25%)。web `npm run build`、`tools/c6_hosted_ota.py` 语法检查通过。
- 真机烧录（COM15，CH343P，ROM loader `--no-stub -b 115200`）：bootloader/分区表/otadata/ota_0/model 五区全部 Hash 校验通过。
- 真机验证通过：无 brownout/watchdog/panic；PSRAM 32MB@200MHz memtest OK、运行空闲约 31,750KB；CPU 360MHz、rev v1.3、IDF v5.5.1；C6 Hosted Host/Slave 均 2.12.11、hostedReady=true；Wi-Fi 连 eric_2G、IP 192.168.31.228、scan_wifi 有真实结果；TF /sdcard 59,638MB 已挂载未格式化；FT6336U 0x48/0x64/0x11 读取成功；ES8311 麦克风 16k/16bit/mono、监听 rms≈20-31、timeout=0、largestInternalDmaBlock=278528；WakeNet 加载 wn9_xiaobinxiaobin_tts、`wake word ready: 小冰小冰`、Ringbuffer 刷屏清零；NVS 持久唤醒重启后自动 listening；唤醒开关 disable/enable 幂等；ASR 编排就绪（TeleAI/TeleSpeechASR，缺 Key ready=false）。
- 仍未验证（需用户参与）：实际说“小冰小冰”触发 wake_word_detected、5 秒录音→SiliconFlow ASR 转写（需 ASR API Key）、连续 20 次唤醒成功率/误触发/长稳。屏幕 TR230S QSPI 本次上电 `WAIT# stayed low` 超时——属硬件飞线/供电问题、与本次 IDF 迁移无关（显示代码未改，2026-07-11 IDF6 下同代码曾点亮），需断电检查屏线后复测。
- 备份：编辑前 `tmp/backups/manual/20260713-131500_idf55_wakenet_migration/`；干净构建前 `tmp/backups/before_run/20260713-132000_idf55_clean_build/`；烧录前 `tmp/backups/before_run/20260713-134500_idf55_com15_flash/`。启动日志：`tmp/logs/20260713_idf55_final_boot.log`。

----
完成时间：2026-07-13 17:41:11 +08:00

### WKS70WSV114-WCT 7 寸 MIPI-DSI 独立点屏固件

完成慧勤智远 ESP32-P4 CB V1.2 板载 26-pin MIPI-DSI 屏的首版显示测试固件；本里程碑已构建通过，尚未烧录和真机上电验收。

- 硬件与协议：目标屏为 WKS70WSV114-WCT（EK79007AD3，1024x600，RGB565），使用 2-lane MIPI-DSI、700Mbps/lane、50MHz DPI 像素时钟；时序为 HBP/HFP/HSW=160/160/20、VBP/VFP/VSW=23/12/10。DSI PHY 使用 LDO_VO3 通道 3 输出 2.5V，LCD_RST=GPIO22（板上分压到 1.8V）、LCD_BL=GPIO23。
- 驱动：新增 `components/display_mipi`，移植厂商 `09_mipilcd` 的 EK79007 九条初始化序列，使用 IDF 原生 `esp_lcd_mipi_dsi + DBI + DPI`；双帧缓冲位于 PSRAM，首次写 fb1 避开正在扫描的 fb0，并通过刷新完成回调同步翻页。初始化失败按 DPI panel、DBI IO、DSI bus、LDO、信号量逆序清理；刷新超时会关闭背光并禁止继续刷新。
- 测试图案：新增独立 `CONFIG_FRIDGE_MIPI_SCREEN_TEST` 模式，只启动显示任务，不启动 Wi-Fi、音频、TF、摄像头或 GT911 触摸；循环显示红、绿、蓝、白、黑全屏和白/黄/青/绿/洋红/红/蓝/黑垂直彩条，每幅保持 2 秒。
- 构建入口：修复 `tools/idf.ps1` 的 MSYS 环境键清理方式，包装脚本可稳定激活 ESP-IDF 5.5.1。当前 `sdkconfig` 已切到 MIPI 测试模式；公共 defaults 保持该测试关闭，正常 P4 服务固件不会意外进入点屏循环。
- 构建验证：`tools/idf.ps1 reconfigure build` 与后续增量 `build` 均通过，`build/config/sdkconfig.h` 确认 MIPI 测试开启、P4 最小服务模式与旧 QSPI 测试关闭；map 文件确认 MIPI 驱动和测试任务已链接。最终 `fridge_spirit.bin` 为 570,560 字节（0x8B4C0），SHA-256 `82C52AEB896D438A00B7B280378BFA8AD89DF99AB85085A88E29FF6938D4207D`，最小 2MiB app 分区剩余 73%。无新增 C/C++ 编译告警，仅保留已记录的 `ESP32P4_SELECTS_REV_LESS_V3` 未知 Kconfig 告警。
- 真机边界：未执行烧录。上电前必须断电插入并锁紧 26-pin DSI 排线，确认无偏插、反插和裸露短路；使用稳定 5V/2A 供电。GT911 触摸不在本版范围，待显示图案真机稳定后单独移植。
- 备份：Claude 中断前备份 `tmp/backups/manual/20260713-145505_mipi_dsi_ek79007_display/`；本次接手编辑前备份 `tmp/backups/manual/20260713-172246_complete_mipi_dsi_screen_test/`。

----
完成时间：2026-07-13 20:10:00 +08:00

### WKS70WSV114-WCT MIPI-DSI 真机点亮与首帧 DMA 修复

完成开发板手册、屏规格、EK79007 原厂初始化代码、慧勤智远 7 寸专用例程和 ESP-IDF 5.5.1 DPI 驱动的交叉核对，并在 COM15 真机完成厂商对照、DSI 内建图案诊断、主工程修复、恢复烧录和两轮显示回归。

- 官方资料结论：板上 26-pin DSI 座只连接 D0/D1 两路，LCD_RST=GPIO22、LCD_BL=GPIO23，复位信号由板上网络分压到 1.8V；屏为 1024x600、5V、GT911、EK79007，固定 2-lane、700Mbps/lane、50MHz、RGB565，时序 HBP/HFP/HSW=160/160/20、VBP/VFP/VSW=23/12/10。程序参数与板厂 7 寸专用综合例程一致，未改官方排线走线。
- 厂商对照：用厂商原始 `mipi_lcd.c` 构建最小控制固件后，初始化、PSRAM、DBI/DPI 均正常，但 `on_refresh_done` 只触发一次；切换 ESP-IDF 官方 DSI Pattern Generator 的竖彩条、横彩条和 BER 图案均正常，用户确认随后红/绿/蓝/白/黑/彩条循环显示正常。
- 根因与修复：ESP-IDF 5.5.1 在 ESP32-P4 rev1.x 上首次启动 DPI/DW-GDMA 后可能停在首帧。主驱动在背光关闭期间短暂启用官方竖彩条发生器 40ms，再切回 `MIPI_DSI_PATTERN_NONE` 重新打开 DPI bridge；此后 DMA 刷新回调持续运行。该操作不改变 GPIO、lane 数、lane rate、像素时钟或面板初始化序列，启动时不可见。
- 主工程验证：隔离 MIPI 测试固件重新构建成功，`fridge_spirit.bin` 为 242,544 字节，SHA-256 `0E01BCB545F0D8169F258779BC9A1E756D48A728FD7D52C9DECB56DCEC5402B8`。COM15 使用 ROM loader 115200 恢复 bootloader、分区表、OTA data 和 ota_0，四段均通过 Hash 校验；模型分区未被厂商对照固件擦除。
- 启动回归：日志出现 `DSI video bridge rearmed (40 ms, backlight off)` 和 `EK79007 MIPI-DSI ready`，连续完成至少两轮红、绿、蓝、白、黑和 RGB565 彩条；无 refresh timeout、panic、brownout、watchdog、PSRAM 错误或 DSI underrun。
- 日志：`tmp/logs/20260713-1938_vendor_70inch_control_com15.log`、`tmp/logs/20260713-1952_vendor_dsi_pattern_com15.log`、`tmp/logs/20260713-2008_mipi_bridge_rearm_main_com15.log`。关键备份：`tmp/backups/manual/20260713-195650_mipi_bridge_rearm/`、`tmp/backups/before_run/20260713-200722_mipi_bridge_rearm_com15_flash/`。

----
完成时间：2026-07-18 17:50:00 +08:00

### ESP32-P4 摄像头官方组件源码补齐

在不启用摄像头、不修改硬件配置和不新增 Agent 功能的前提下，补齐事故恢复中严重缺失的 Espressif 官方摄像头组件源码，并建立可复核的版本与哈希基线。

- 固定版本：`esp_cam_sensor 0.5.3`、`esp_video 0.5.1`；官方归档 SHA-256 分别为 `E4214A99993C012161DFDDB7661DAEDF0F11361C5E94071DD944E5ED782C40FE`、`17513950F305E964BF9A7F301D6EECF8D34D04C66D5C8F296D6C2D9A1D107D73`。
- 完整性：活动目录共 113 个文件、840,388 字节；相对官方归档逐文件比较无缺失、无额外文件、无内容差异，并确认无空文件、文本 NUL 或恢复工具 `.component_hash`。
- 兼容性：隔离工程固定 IDF 5.5.1 / ESP32-P4，并组合 `esp_sccb_intf 0.0.3`、`esp_h264 1.0.4` 完成编译和链接。
- 主工程回归：`reconfigure build size` 通过；`fridge_spirit.bin` 仍为 1,873,728 字节、SHA-256 `7ED6D49CD52E6330AD14BCF6BD967C20C6181063A1C16D8A3BA3DF2D6C736B09`，`dependencies.lock` 与摄像头组件进入前均未变化。
- 边界：摄像头 manifest 继续隔离，主固件仍使用 stub；没有猜测或重建事故前 Bayer/CCM/CSI 私有补丁，也没有烧录或真机测试。
- 技术文档：`doc/ESP_CAM_SENSOR_ESP_VIDEO源码补齐与接入边界.md`。编辑前备份：`tmp/backups/manual/20260718-173545_camera_vendor_completion/`；构建前备份：`tmp/backups/before_run/20260718-174624_camera_vendor_main_build/`。

----
完成时间：2026-07-18 18:40:00 +08:00

- 完成 Agent 接入阶段0地基：新增 `components/agent_storage`，在 P4 最小固件中于 TF 卡成功挂载后初始化 `/sdcard/agent/` 目录树。
- Agent 运行所需的配置、业务数据、长期记忆、技能、工作流、运行缓存、日志和 TF 卡内技术文档默认文件均优先创建在 TF 卡；API Key 与 Wi-Fi 凭据继续留在 NVS，不写入 TF 卡明文。
- 新增仓库技术文档目录 `doc/agent/`，包含 Agent 架构、存储边界、Tool、Skill、Workflow、MCP 与 Trace 约定，方便后续 AI 修改和扩展前读取。
- 扩展 P4 最小 USB JSON Lines 协议命令 `agent_storage_status`，用于查看 Agent TF 存储初始化状态、创建目录数、创建文件数和最近错误。
- 已通过 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 build` 构建验证；当前 `fridge_spirit.bin` 大小 `0x1cb870`，最小 app 分区剩余约 10%。
- 风险与下一步：app 分区余量已降至约 10%，后续接入 Tool Broker / Agent Runtime 前应优先完成 P4 16MB 分区表重整或裁剪非必要组件；下一阶段建议实现 `tool_system`，先以 USB 命令验证 `inventory.list`、`memory_md.read` 和 `device.state.get`。

----
完成时间：2026-07-18 18:50:00 +08:00

- 完成 Agent 接入阶段1最小 Tool System：新增 `components/tool_system`，包含 Tool 公共类型、固定数组注册表、Tool Broker 和内置只读工具注册入口。
- 首批只读工具已落地：`inventory.list` 读取 `/sdcard/agent/data/inventory.json`，`memory_md.read` 读取 `/sdcard/agent/MEMORY.md`，`device.state.get` 读取诊断状态与 Agent 存储 ready 状态。
- Tool Broker 已接入 P4 最小启动流程：TF 卡与 Agent 存储初始化成功后注册只读工具；当前阶段有意拒绝 WRITE / DANGEROUS 自动执行，等待确认流、幂等键和 Trace 完成后再开放。
- 扩展 USB JSON Lines 调试命令：`tool_list` 用于查看已注册工具，`tool_call` 用于通过 Broker 调用只读工具，便于后续 Agent Runtime 接入前单独验证本地能力。
- 更新 `doc/agent/TOOLS.md`，记录当前最小实现、已注册工具和 USB 调试命令示例，方便后续 AI 扩展前读取。
- 已通过 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 build` 构建验证；当前 `fridge_spirit.bin` 大小 `0x1cd3e0`，最小 app 分区剩余约 10%。
- 风险与下一步：主 app 分区空间继续收紧；下阶段建议优先做最小 `agent_runtime` / `agent_router` 文本链路，同时尽快重整 P4 16MB 分区以避免后续模型适配和 Workflow 接入时溢出。

----
完成时间：2026-07-18 19:05:00 +08:00

- 根据用户决策补齐 Agent 三档权限模式：`confirm`、`standard`、`auto` 均在 `components/tool_system` 中实现，长期保留；MCP 按用户决策暂缓，不进入近期实现范围。
- Tool Broker 新增权限模式状态与判定 API：`fridge_tool_broker_get_permission_mode()`、`fridge_tool_broker_set_permission_mode()`、`fridge_tool_broker_can_execute()`；ToolCall 新增 `user_confirmed` 字段，供后续 UI/语音确认流传入。
- 权限规则：`confirm` 模式 READ 自动执行、WRITE/DANGEROUS 等待确认；`standard` 模式 READ/WRITE 自动执行、DANGEROUS 等待确认；`auto` 模式 READ/WRITE 自动执行，但 DANGEROUS 仍需显式确认，避免误删库存、清空记忆、恢复出厂或修改密钥。
- 扩展 USB JSON Lines 调试命令：`tool_permission_status` 查看当前模式，`tool_permission_set` 设置 `confirm|standard|auto`；`tool_call` 支持 `confirmed` / `userConfirmed` 字段，用于调试已确认调用路径。
- 更新 `doc/agent/TOOLS.md` 与 `doc/agent/MCP.md`：记录三档权限策略、USB 调试命令，以及 MCP 暂缓决策。
- 已通过 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 build` 构建验证；当前 `fridge_spirit.bin` 大小 `0x1cda10`，最小 app 分区剩余约 10%。
- 下一步建议：在不接 MCP 的前提下推进最小 `agent_runtime` / `agent_router`，先实现 USB 文本输入 → 本地 Router → Context Composer 草稿 → Tool Broker 调用 → Trace 的最小闭环。

----
完成时间：2026-07-18 19:18:00 +08:00

- 完成最小 Agent Runtime 本地闭环：新增 `components/agent_runtime`，实现 USB 文本输入 → 关键词 Router → Tool Broker → 本地只读工具 → 本地回复 → 内联 Trace JSON。
- 当前阶段不接云端模型、不接 MCP、不执行写入工具，先验证 Run 状态机和 Tool 调度骨架；MCP 暂缓决策继续有效。
- 已支持本地意图：`inventory_query` 调用 `inventory.list`，`memory_read` 调用 `memory_md.read`，`device_state` 调用 `device.state.get`，`help` 返回本地帮助，未知意图提示后续接入 `ai_client` 后处理开放式对话。
- 扩展 USB JSON Lines 命令：`agent_status` 查看 Runtime 状态，`agent_run` 执行最小本地 Agent Run；示例输入包括“还有什么食材？”、“读取记忆”、“查看设备状态”。
- 更新 `doc/agent/AGENT_ARCH.md` 与 `doc/agent/TRACE.md`，记录当前最小 Runtime 的链路、意图规则、USB 示例和内联 Trace 结构。
- 已通过 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 build` 构建验证；当前 `fridge_spirit.bin` 大小 `0x1ceb60`，最小 app 分区剩余约 10%。
- 风险与下一步：当前 Trace 只随 USB 响应返回，尚未落盘写 `runs.jsonl`；下一阶段建议做 `trace_service` 的有界 JSONL 追加，或先接入 `ai_client` 做“本地工具结果 → 模型总结”的受控 Agent Loop。分区空间仍偏紧。

----
完成时间：2026-07-18 19:28:00 +08:00

- 完成 Agent Trace Service 落盘：新增 `components/trace_service`，把 Agent Runtime 每次 Run 的内联 Trace JSON 追加到 `/sdcard/agent/logs/runs.jsonl`。
- Trace 日志采用有界策略：当前 `runs.jsonl` 上限 64KB，超限后轮转为 `/sdcard/agent/logs/runs.prev.jsonl`，避免 TF 卡日志无限增长；Trace 写入失败只记录 warning，不让已完成业务 Run 变失败。
- 启动流程更新为：TF 卡初始化 → Agent Storage 初始化 → Trace Service 初始化 → Tool Broker 初始化 → Agent Runtime 初始化。
- 扩展 USB JSON Lines 命令：`agent_trace_status` 查看 Trace 状态、追加条数、轮转次数和当前文件大小；`agent_trace_read` 读取 `runs.jsonl` 尾部最多 4096 字节，便于 Web/串口调试。
- 更新 `doc/agent/TRACE.md` 与 `doc/agent/AGENT_ARCH.md`，记录 Trace Service 落盘路径、轮转策略、USB 命令和当前架构顺序。
- 已通过 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 build` 构建验证；当前 `fridge_spirit.bin` 大小 `0x1cf6e0`，最小 app 分区剩余约 9%。
- 风险与下一步：主 app 分区余量降至约 9%，继续接入 Agent 能力前应优先重整 P4 16MB 分区或裁剪非必要组件；ai_client 受控总结按用户要求暂不处理。

----
## 2026-07-18 20:15:28 +08:00 Agent 业务写入工具、Workflow Engine 与 Skills 最小版完成

- 新增业务写入 Tool：`inventory.add/update/delete/list_expiring`、`task.list/create_timer/create_reminder/cancel`、`memory.search/add`、`memory_md.append`。写入仍统一经过 Tool Broker 权限模式；`inventory.delete` 标记为 `DANGEROUS`，始终需要显式确认。
- 新增 `components/workflow_engine` 最小确定性流程模板：`inventory.edit_confirm`、`task.create_timer`、`memory.preference_confirm`、`memory_md.append_confirm`、`device.self_check`。Workflow 副作用步骤前后覆盖写入 `/sdcard/agent/runtime/active_run.json`，并把结果摘要追加到 `runs.jsonl`。
- 新增 `components/skill_manager`：启动时在 TF 卡补齐默认 Skill 的 `manifest.json` 与 `SKILL.md`，支持 USB `skill_status/list/read/match`，默认覆盖库存查询、临期晚饭、厨房计时、偏好记忆确认和设备帮助。
- 扩展 USB JSON Lines：新增 `skill_status`、`skill_list`、`skill_read`、`skill_match`、`workflow_status`、`workflow_start`；`tool_call` 可直接调新增业务工具，确认字段继续使用 `confirmed` / `userConfirmed`。
- 补充 USB 参数兼容：`tool_call` 与 `workflow_start` 的 `arguments` 现在可直接传 JSON object/array，也继续兼容旧的字符串 `arguments` / `argsJson`。
- 更新 Agent 技术文档：`doc/agent/README.md`、`AGENT_ARCH.md`、`STORAGE.md`、`TOOLS.md`、`SKILLS.md`、`WORKFLOWS.md`、`TRACE.md`、`MCP.md`。
- 当前边界：本阶段只改 TF 卡数据、Tool Broker、Workflow/Skill 和 USB 调试，不改 GPIO、时钟、电源、摄像头、音频、屏幕或联网链路；MCP 和 `ai_client` 受控模型总结继续暂缓。
- 风险/下一步：新增代码会进一步压缩当前 2MiB app 分区余量，构建后需重点查看 `fridge_spirit.bin` 大小；后续应优先推进 P4 16MB 分区重整或裁剪组件。

----
完成时间：2026-07-18 21:56:30 +08:00

### Agent P0/P1 稳定性修复：栈、恢复、分页、Schema、幂等与计时调度

- USB P0 栈风险：把 `ToolCall`、`ToolResult`、Agent/Workflow Result、Tool/Skill 列表等大对象移到 PSRAM 优先堆，Workflow 与 Agent Runtime 内部大工作区同步移出任务栈；USB 协议任务栈增至 8KB，并每 16 条命令检查一次栈高水位，低于 1536 字节时告警。
- TF 断电恢复：Agent JSON 改为 `.part` 写入、`fsync`、正式文件换名并保留 `.bak`，写后从盘读回、校验 Schema 和逐字节复验；启动/读取时按“有效正式文件 → 有效 `.bak` → 有效 `.part` → 首次默认值”恢复，并拒绝静默降级未来 `schema_version`。Workflow 检查点和 MEMORY.md 也复用可恢复写入。
- 大数据查询：Agent 业务 JSON 上限统一为 64KB，读取缓冲优先放 PSRAM；`inventory.list` 新增 `offset/limit` 分页并按 4KB Tool Result 上限动态收口，`memory_md.read` 新增字节分页、UTF-8 边界修正和 `nextOffset/hasMore`。
- Schema 与幂等：Tool Broker 在权限判定前执行 JSON object、必填字段、类型、数值范围和字符串长度校验；写工具在串行执行锁内使用调用幂等键，库存更新/删除、任务取消/触发、结构化记忆和 MEMORY.md 追加均持久化去重记录。
- 计时器：新增 TF 持久化本地调度器，扫描 `tasks.json` 中 pending timer，SNTP 时间有效时按 `dueAtEpochMs` 做掉电恢复，无绝对时间时使用单调时钟保守恢复；到期后通过 Tool Broker 幂等执行 `task.trigger`，并输出 USB `task_due` 事件和 `task_scheduler_status` 状态命令，不直接控制扬声器或 UI。
- Trace：Agent Run 与 Tool Call Trace 改用 cJSON 生成，用户输入、工具名和状态均由 JSON 库转义，避免引号、反斜杠、换行或控制字符破坏 JSONL。
- 构建验证：固定 `ESP-IDF v5.5.1`、目标 `esp32p4` 完整构建通过；`fridge_spirit.bin` 为 `0x1dd5f0` 字节，2MiB app 分区剩余 `0x22a10`（约 7%）。未烧录、未执行真机断电测试；后续需在 COM15 回归 USB 栈高水位、重复幂等请求、64KB 临界分页、计时到期和写入各换名窗口断电恢复。
- 明确边界：按用户决定，本次不处理旧数据与 Agent TF 数据“两套权威源”的重设计，也不接真实模型；UI 和扬声器播报仍留给后续阶段。
- 备份：编辑前 `tmp/backups/manual/20260718-213023_agent_p0_p1_fixes/`；构建前 `tmp/backups/before_run/20260718-215355_agent_fixes_build/`。

----
完成时间：2026-07-20 21:02:00 +08:00

### P4 全 CJK 字体与 26 键全拼输入法第一版接入

- 新增 P4 原生 `ui_p4`：1024x600 MIPI-DSI 全屏 LVGL 双帧缓冲、GT911 触摸输入、Agent 对话/状态/输入弹层和 26 键软键盘；旧 S3 `components/ui` 不进入 P4 正常闭包。
- `display_mipi` 新增双帧缓冲获取与整帧提交接口，提交路径等待 DPI refresh-done 后再向 LVGL 报告 flush 完成，保留已验证的 DSI 参数和 40ms bridge rearm。
- 新增 `font_manager`：Flash 16px 应急字体、TF BIN 常用字和 TF FreeType CJK 回退，固定 18/22/28/36/48/64px 角色；新增 `tools/fontpack/build_manifest.py` 生成 SHA-256 manifest，并以 `.part`/fsync/原子替换写清单。
- 新增 `ime` 与 `tools/fontpack/build_pinyin_dict.py`：FIME1 紧凑词库加载到 PSRAM，支持连续拼音、`v`、最多 12 个候选和无 TF 内置厨房词回退；输入法热路径不访问 TF。
- 新增 GT911 Product ID 探测、有限 I2C 超时和可配置横屏坐标校准；GPIO15/16/17/18 仅按 P4 板卡手册使用，I2C 上拉保持 3.3V 约束。
- recovery 构建图在组件发现阶段排除 UI/字体/输入法；新增 `sdkconfig.recovery.p4` 与 defaults。P4 recovery 镜像已构建通过，大小约 `0x1008e0`，2MiB ota_1 剩余约 50%。
- 主固件构建通过，`fridge_spirit.bin` 约 `0x28fe00`；当前 ota_1 2MiB 对主镜像有预期的 overflow 警告，正常刷写仍使用 ota_0。未烧录、未执行真机 MIPI/GT911/TF 长稳测试。
- 备份：`tmp/backups/manual/20260720-201357_p4_full_cjk_ime/`、`tmp/backups/manual/20260720-205521_p4_cjk_ime_hardening/`；构建前备份位于 `tmp/backups/before_run/` 对应事项目录。

----
完成时间：2026-07-20

### FY-SP703 降噪麦克风 I2S1 与 WebUI 音源选择接入

- 新增 `components/fy_sp703`：FY-SP703 使用 GPIO7/8/9 接入 I2S1 从机 RX，默认 48kHz/32-bit/Philips 双时隙；录音时转换为 48kHz/16-bit/mono 公共 PCM，统计左右通道 RMS、有效通道、I2S 超时和 DMA 溢出。
- 扩展 `components/audio` 音源抽象：保留 ES8311 默认路径与 WakeNet 兼容，新增 FY-SP703 手动录音源；FY 录音与旧音频会话互斥，音源选择不写入 NVS。
- `components/asr` 新增固定 3:1 低通 FIR，将 FY 的 48kHz PCM 转成 16kHz/16-bit/mono 后复用既有 HTTPS ASR 接口。
- USB JSON Lines 的 `mic_record_*` 命令新增 source/native format/channel/RMS/overflow 字段；`mic_record_wav` 限制最长 3 秒预览并返回动态采样率 WAV。`get_pins` 已改为 FY-SP703 I2S1 接线说明，旧 QSPI 引脚冲突由 MIPI UI Kconfig 约束。
- WebUI 麦克风页新增 ES8311/FY-SP703 音源选择、接线提示、原生格式与左右 RMS 展示；停止硬件录音后自动拉取 WAV 并浏览器回放；WAV 串口超时提高到 120 秒。
- 构建验证：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 reconfigure build` 与 `npm run build` 均通过；主镜像 `fridge_spirit.bin` 为 `0x2910e0`，ota_1 2MiB recovery 分区仍有预期 overflow 警告，正常刷写使用 ota_0。
- 当前未烧录、未连接 FY-SP703 真机；仍需用示波器确认 LRCK≈48kHz/BCLK≈3.072MHz，并在 COM15/WebUI 上完成录音、回放、ASR 和 30 分钟长稳验证。AEC±、OUT± 保持未接。
- 备份：`tmp/backups/manual/20260720-220000_fy_sp703_audio_webui/`、`tmp/backups/before_run/20260720-221000_fy_sp703_build/`。

----
完成时间：2026-07-21

### FY-SP703 音频审查问题修复

- 在 `fridge_audio_p4.c` 增加 `_Static_assert`，锁定 FY 每次读取帧数不得超过栈上 `samples[]` 容量，避免宏独立调整后静默溢出。
- FY 左右通道探测保留为诊断并用于选择有效通道；探测前后均保持 `/2` 电平标定，消除 500ms 探测完成时从平均值切换到单通道造成的幅度台阶。
- 将原 3 点抽取滤波替换为无第三方依赖的固定 63-tap Q15 低通 FIR（截止约 6kHz），ASR 与 WAV 预览共用同一转换接口；代码注释明确其为语音抗混叠链路，不作为高保真重采样器。
- FY `mic_record_wav` 预览先从 48kHz 抽取到 16kHz，最长 3 秒，降低串口 base64 负载至约 96KB PCM，保留返回的实际音源字段。
- 构建前备份：`tmp/backups/manual/20260721-101500_fy_audio_review_fixes/`、`tmp/backups/before_run/20260721-102000_fy_audio_review_fixes_before_build/`。
- 本阶段尚未烧录或连接 FY-SP703 真机；需继续完成示波器、WebUI 回放、ASR 和长稳验证。
- 验证：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/idf.ps1 build`、`tools/idf.ps1 size` 和在 `web/` 目录执行的 `npm run build` 均通过；主镜像 `fridge_spirit.bin` 为 `0x291330`。`ota_1` 2MiB recovery 分区溢出仍是既有预期警告，正常刷写使用 `ota_0`。
- 追加备份：`tmp/backups/before_run/20260721-102500_fy_channel_step_fix_before_build/`、`tmp/backups/manual/20260721-104000_fy_audio_review_progress_build_result/`。

----
完成时间：2026-07-21

### P4 启动任务栈保护重启修复

- 分析用户日志确认故障为 ESP32-P4 `main` 任务的硬件 `Stack protection fault`，不是 FY-SP703 I2S、brownout 或 watchdog；日志中的栈指针 `0x4ff35540` 已低于栈边界 `0x4ff35670`。
- 将 P4 主固件 `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 从 3584 提升到 10240 字节，并同步生成配置中的 `CONFIG_MAIN_TASK_STACK_SIZE`；recovery 独立配置未修改。
- 该值与厂商 ESP32-P4 例程的 10KB 主任务栈一致，覆盖 hosted、MIPI/GT911、ES8311、TF 和 UI 串行启动时的调用深度。
- 验证：`tools/idf.ps1 reconfigure build` 首次并行编译遇到无诊断的 ccache 子任务失败，随后设置 `CMAKE_BUILD_PARALLEL_LEVEL=1` 重跑成功；主镜像 `fridge_spirit.bin` 为 2691888 字节（`0x291330`），仍有既有 ota_1 分区溢出警告。
- 构建完成时尚未烧录；后续已改用当前枚举的 COM17 完成真机验证，结果见下方补充。
- 备份：`tmp/backups/manual/20260721-111500_main_task_stack_overflow/`、`tmp/backups/before_run/20260721-112000_main_task_stack_build/`、`tmp/backups/manual/20260721-113000_main_task_stack_progress/`。
- 真机补充：通过 ESP32-P4 原生 USB-Serial/JTAG `COM17`、ROM loader `--no-stub` 以 115200 完成 bootloader、ota_0 主固件、分区表、OTA data 和 `srmodels` 烧录，所有写入均通过 Hash 校验。
- 烧录后复位并抓取约 15 秒启动日志：`ready` 事件正常发出，`app_main()` 正常返回，Agent/TF/UI/WakeNet 启动完成，Wi-Fi 获取 `192.168.31.228` 并完成 SNTP；未再出现 `Stack protection fault`、Guru、brownout 或 watchdog。启动时显示的 23492 字节 coredump 是 Flash 中保留的上一次崩溃记录，不代表本次再次崩溃。
- 烧录前备份：`tmp/backups/before_run/20260721-115000_flash_com17/`；进度编辑备份：`tmp/backups/manual/20260721-120000_progress_flash_com17/`。

----

## 2026-07-21 WKS70 GT911 触控驱动对齐与诊断

完成 WKS70WSV114-WCT 电容触摸（GT911）驱动与厂商 7 寸例程对齐，并增加 USB 诊断；**软件构建通过，真机烧录因串口占用未完成**。

- 驱动：components/touch/fridge_touch_gt911_p4.c 对齐 0x14 优先、400kHz、RST 时序、0x8040 软复位、0x8150 读点；新增 ready/地址/status/scan API。
- USB：	ouch_status、	ouch_scan 接入 ridge_usb_protocol_minimal.c；get_pins 改为 GT911 说明。
- UI：前几次按下打印坐标，便于不看 USB 时确认链路。
- 验证：	ools/idf.ps1 build 通过，镜像 2680960B SHA-256 E931BAA2A14101EC3DC40E054C16F2394C730FA0B1DB45ABD32746B97786F667。
- 下一步：释放 COM 口后烧录，确认 GT911 ready 与软键盘可点；若坐标镜像再调校准。
- 备份：	mp/backups/manual/20260721-011918_wks70_gt911_touch/。

----
完成时间：2026-07-21

### COM17 Web Serial 卡死与 FY-SP703 启动修复

- 根因确认：COM17 是 ESP32-P4 原生 USB-Serial/JTAG（VID=303A、PID=1001），原配置将其设为 secondary console；该模式只能输出日志，不能把 WebUI JSON Lines 输入送给固件，导致 `get_status`、`wake_status` 和麦克风命令依次超时。
- 将 USB-Serial/JTAG 改为主控制台并关闭 secondary console；COM17 实测 `get_status`、`wake_status` 均能按 `request_id` 立即返回。
- Web Serial 传输层在设备丢失时立即拒绝全部待处理请求、释放 reader/writer/port，并通知页面清空刷新、录音、WakeNet 等忙状态，避免断线后按钮禁用和刷新图标持续旋转；同时兼容启动日志前缀与首条 JSON 响应粘连。
- FY-SP703 I2S1 从机启动曾因 P4 默认 40MHz XTAL 无法满足 48kHz/32-bit/双时隙和最小 `bclk_div=8` 的校验而返回 `ESP_ERR_INVALID_ARG`；接收逻辑改用 APLL 后初始化成功，GPIO7/8 的 BCLK/LRCK 仍保持外部输入。
- COM17 真机验证已完成：临时停用 WakeNet 后，FY 的开始、状态、停止命令均成功，最后恢复 WakeNet 为 `listening`；当前录音状态为 `pcmBytes=0`、`timeoutCount>0`，固件明确报告 `FY-SP703 external I2S clock/data timeout`，仍需检查模组供电及 GPIO7/8/9 上的 LRCK≈48kHz、BCLK≈3.072MHz 和 SDIO 活动。
- 验证：`tools/idf.ps1 build` 与 `web/npm run build` 均通过；固件 `fridge_spirit.bin` 为 `0x28e890`，已烧录 COM17 且 Hash 校验通过。`ota_1` 2MiB recovery 分区溢出仍为既有预期警告。
- 备份：`tmp/backups/manual/20260721-123000_usb_jtag_webserial_recovery/`、`tmp/backups/manual/20260721-012548_fy_i2s_apll_json_recovery/`，烧录前备份见 `tmp/backups/before_run/20260721-011951_usb_jtag_console_flash/` 与 `tmp/backups/before_run/20260721-012636_fy_apll_flash/`。

----
完成时间：2026-07-21 11:36:14 +08:00

### P4 全 CJK 字体包与拼音词库部署到 TF

- 通过 COM17 实测主固件、Hosted/Wi-Fi、TF 和 GT911 状态正常：TF 为约 64GB FAT32，GT911 Product ID=`911`、I2C 地址 `0x14`，网络地址为 `192.168.31.228`。
- 固定 Noto CJK Sans 2.004，准备 SC Regular/Medium、TC/JP/KR Regular 和 Noto Sans Symbols2 完整轮廓字体；生成 18/22px GB2312 常用字位图、28/36px UI 位图及 64px 数字位图。
- 将 Rime `pinyin_simp.dict.yaml` 转换为 FIME1 轻量词库，共 65,125 条、1,928,629 字节，满足固件 100,000 条和 4MiB 上限。
- 字体包共 14 个资源文件、86,287,560 字节，已复制到 TF 的 `/fridge/assets/fontpack/v1/`；电脑端逐文件 SHA-256 和项目 `build_manifest.py --verify` 均通过。
- 写卡前通过 COM17 安全卸载 TF，完成后由 Windows 安全弹出；未格式化卡，也未覆盖现有 Agent 数据。TF 重新插回开发板后需断电重启，使 Font Manager 和 IME 在启动阶段加载新资源。
- 进度编辑前备份：`tmp/backups/manual/20260721-113614_fontpack_tf_deploy_progress/`。

----
完成时间：2026-07-21 11:59:45 +08:00

### P4 TF 字体、FreeType 与输入法资源真机闭环

- 首轮 COM17 上电检查确认 TF 位图包已加载，`roles=0x7f`，但 FreeType 状态为 0。根因一是 LVGL 9.2.2 的 `lv_init()` 已自动初始化 FreeType，字体管理器重复初始化后把“已初始化”误判为失败；已删除重复调用。
- 根因二由链接映射确认：实际链接的是 FreeType 标准文件 I/O，原 `S:/...` 路径无法通过 ESP-IDF VFS 打开。关闭 `LV_FREETYPE_USE_LVGL_PORT`，轮廓字体改用 `/sdcard/...` 绝对路径；LVGL BIN 字体仍使用 `S:` 驱动。
- FreeType 首次成功渲染 Noto CJK CFF 字形时暴露 `ui_p4` 16KB 栈不足，panic 地址解析到 `cf2_interpT2CharString()`；UI 任务栈提升到 32KB并记录高水位，最终余量为 12,588 字节。
- 最终真机日志确认：`FreeType CJK fallback roles=7/7`、`font manager ready: tf=1 freetype=1 roles=0x7f`、P4 MIPI LVGL UI ready；空闲 PSRAM 24,221KB，超过 8MB验收线。
- TF 挂载正常，manifest 为 3,080 字节，FIME1 拼音词库为 1,928,629 字节；IME 初始化没有损坏/内存错误日志。GT911 仍为 Product ID `911`、地址 `0x14`，Wi-Fi 获取 `192.168.31.228` 并完成 SNTP。
- 最终观察窗口内无 Guru、Stack protection、watchdog、assert、abort 或实际 brownout；仍需用户实际点按 26 键验证候选选择，并继续完成 30 分钟 MIPI/输入法长稳测试。
- 构建使用 ESP-IDF 5.5.1；并行全量构建遇到一次既有 ccache 无诊断失败，`CMAKE_BUILD_PARALLEL_LEVEL=1` 重跑通过。最终镜像 `0x28e8e0`，SHA-256 `70C37A3CFC3F4B566E9907AB6A01C647D8D1C86183CFB012786929317296B723`，COM17 `--no-stub` 烧录各区 Hash 均通过；ota_1 2MiB overflow 仍为既有 recovery 容量警告。
- 最终日志：`tmp/logs/20260721-fontpack-stack-final-com17.log`；相关备份：`tmp/backups/manual/20260721-114442_freetype_double_init_fix/`、`tmp/backups/manual/20260721-114911_freetype_vfs_path_fix/`、`tmp/backups/manual/20260721-115557_ui_freetype_stack_fix/`、`tmp/backups/manual/20260721-115945_fontpack_runtime_acceptance_progress/`，构建/烧录备份位于 `tmp/backups/before_run/` 对应事项目录。
- 连续全拼补充修复：Rime 源词典的音节间空格和分隔用单引号在生成 FIME1 时统一移除，使软键盘连续输入 `fanqie`、`pingguo`、`niunai` 能命中词条；新增回归测试覆盖 `fan qie -> fanqie` 和 `xi'an -> xian`。
- 修正 manifest 中位图字体的 Unicode 覆盖描述；重新生成词库为 65,125 条、1,859,918 字节，SHA-256=`C0E4DB7E525E0EE05E265F3C7359EAA6FCFB6E8014C6C90382744F49E19AC5E1`，manifest SHA-256=`7F4B44F194F200B07BD99E54B10B1EAC9F09C6A39D9FB062C2701FE8B1B6DEAA`。
- 新词库和 manifest 已通过 COM17 授权的局域网 HTTP 会话分块写入 TF，上传端显式禁用系统代理，失败会执行 transfer abort；写入后整板软复位，文件尺寸、TF 无活动会话、FreeType 7/7、GT911、Wi-Fi 和 UI 均复核正常，未出现 Guru、栈保护、watchdog 或 brownout。日志：`tmp/logs/20260721-fontpack-continuous-pinyin-final-com17.log`；进度编辑备份：`tmp/backups/manual/20260721-120935_continuous_pinyin_progress/`。
- 中文方框补充修复：实机字体包与字形解析确认 BIN 内含正常 CJK 位图，根因是压缩 BIN 已加载但 `CONFIG_LV_USE_FONT_COMPRESSED` 未启用，LVGL 命中字形后无法解压且不会继续走 FreeType fallback；现已启用压缩字体，并在字体管理器中增加配置漂移保护和 Flash 应急字体链尾。备份：`tmp/backups/manual/20260721-123545_cjk_box_font_fix/`。
- 方框修复真机验证：ESP-IDF 5.5.1 单并发重新配置/构建通过，主镜像 2,682,144 字节，SHA-256=`15D51AE0627EAB04D3DEE08AAABABE23EC1DFCBFB6A09FD760992DFB8C013EDD`；COM17 使用 ROM loader/no-stub 115200 烧录且各区 Hash 通过。受控复位日志确认 `tf=1 freetype=1 compressed=1 roles=0x7f`、FreeType 7/7、GT911 与 P4 UI ready，空闲 PSRAM 24,301KB，未出现 Guru、栈保护、watchdog、assert、brownout 或字体解压警告。烧录前备份：`tmp/backups/before_run/20260721-124500_cjk_box_font_flash/`。

----
完成时间：2026-07-21

### P4 输入法：全拼前缀联想 + 简拼 + 选词后联想

- 根因：FIME1 词库已有连续拼音前缀匹配，但候选按拼音字典序先到先得且只保留 64 条，高频多字词（如「番茄」）被大量单字/短词挤出首页，体感像“必须打完整拼音”。
- 重写 `components/ime/fridge_ime.c`：
  - 全拼前缀：拼音有序表二分锁定区间 + 词频/贴合度 Top-K（64）排序后再分页 12 条；
  - 简拼：启动时用合法音节贪心切分构建内存简拼倒排（无需重刷 FIME1），`fq/pg/nn` 等长度≥2 时与全拼结果融合；
  - 选词后联想：上屏后查找以该词为前缀的更长词，候选展示续写后缀；再输入字母则回到拼音查询；
  - 词库与简拼索引优先分配到 PSRAM；无 TF 时厨房 fallback 同步支持前缀/简拼。
- `fridge_ime.h` 增加 `association_mode`；`ui_p4` 状态栏区分「全拼+简拼 / 联想」提示。
- 主机侧简拼切分抽查：`fanqie→fq`、`pingguo→pg`、`niunai→nn`、`xihongshi→xhs`、`jidan→jd` 均通过。
- 构建：`tools/idf.ps1 build` 通过；主镜像 `0x290d20`（2,690,336 字节），SHA-256=`6F812EDA34D89E89F7FF31A817A2D6182F70510CE2902B83BD14F472D56B95E8`；ota_1 2MiB overflow 仍为既有 recovery 容量警告。
- 尚未烧录真机手测；验收建议：`fan`/`fanq` 首页见番茄，`fq`/`pg`/`nn` 简拼命中，选「西」后出现续写联想。
- 备份：`tmp/backups/manual/20260721-132337_ime_prefix_jianpin_assoc/`；进度备份：`tmp/backups/manual/20260721-132735_ime_assoc_progress/`。
- 后续可选：用户词频 NVS、模糊音、FIME2 固化简拼索引以缩短启动建索引时间。
- 补充修复（性能优先，无新大索引）：
  - 去掉全拼“精确匹配 +50 万”断层；词频为主 + 小幅精确/多字加分。
  - Top-K 插入为 longer 词保留最少 24 槽；首页 12 条中约 5 条给未打完拼音的多字词，字/词交错展示。
  - 同文案去重时保留更高分（厨房 fallback 先写入，可抬词典低权重场景词）。
  - 厨房 fallback 提高番茄/鸡蛋/牛奶等权重，并补番茄酱/土豆等。
  - 选词联想改为固定访问上限 + 稀疏步进，禁止无界全表扫。
  - 主机仿真：`fan`/`fanq` 首页可见「番茄」，`niu` 首页「牛奶」，`xih` 首页「西红柿」。
  - 构建通过；主镜像 `0x291400`（2,692,096 字节），SHA-256=`09054A38706EF7CE9614B3BB806ABFC69166D8D7E7AC3B9DDB88A7AF529211FF`（含 UI 提示文案）；备份：`tmp/backups/manual/20260721-133831_ime_score_quota_fix/`。
  - 仍未烧录真机；烧录后手测 `fan`/`fq`/`pg`/`nn` 与选词联想。
- 2026-07-22 TF 词库延迟加载修复：P4 为尽快显示首帧会在 TF 挂载前启动 UI，原输入法只初始化一次，首次 `fopen` 失败后会永久停留在内置词库；现增加仅由 UI 任务调用的幂等重载接口，并采用独立于 FreeType 的 500ms 快速重试/2s 慢重试。加载成功后保留当前预编辑内容、重算候选并刷新状态栏，词库和简拼索引继续放在 PSRAM。
- 真机 COM17 验证：TF 已挂载（59,638MB，总空闲 59,538MB，`lastError` 为空），第 2 次重试成功加载 65,125 条 FIME1 词条并建立 65,125 条简拼索引，日志出现 `TF dictionary ready` 与 `TF IME dictionary rebound after 2 attempt(s)`；未见 Guru/watchdog/brownout。词库生成回归测试通过，ESP-IDF 5.5.1 构建通过，主镜像 `0x2b50d0`（2,838,736 字节）、SHA-256=`37A6A78129E77BAE25BEDB09A746A7B171F1DF19D7CBD39A9143F708DD499042`；COM17 no-stub 全量烧录各区 Hash 校验通过。备份：`tmp/backups/manual/20260722-153059_ime_tf_retry/`、`tmp/backups/before_run/20260722-153136_ime_tf_build/`、`tmp/backups/before_run/20260722-153219_ime_tf_flash/`。

----

完成时间：2026-07-21

- 接入智谱 GLM-Realtime（设备端 WebSocket + Agent Tool Broker + ES8311 双工音频），WebUI 仅调试。
- 新增 `components/glm_realtime`：NVS `fridge_rt`、会话状态机、session.update 工具导出（READ/WRITE，不导出 DANGEROUS）、function call 经 Broker、每轮单工具。
- 扩展 `codec_es8311` 全双工 I2S RX/TX；`PA_CTRL` 仅写 PCM 时拉高；`audio` 增加 realtime stream/play API。
- USB 最小协议命令：`get/set_realtime_config`、`clear_realtime_key`、`realtime_start/stop/status/send_text/cancel` 与 `realtime_*` 事件。
- Web：侧栏「Realtime」页 + MockTransport；浏览器不持有 Key、不直连智谱。
- 文档：`doc/agent/REALTIME.md`；备份：`tmp/backups/manual/20260721-143212_glm_realtime_revision/`。
- 计划：`doc/plan/GLM-Realtime_P4_双工音频与Agent接入计划.md`。
- 待真机 COM15 验收：配网 → 写 Key → start → 说话/文本工具闭环 → 播放/打断 → stop 恢复 WakeNet。
- 构建：`tools/idf.ps1 build` 通过；主镜像 `0x29c2e0`；`esp_websocket_client` 已写入 `dependencies.lock`；`web npm run build` 通过。
- ota_1 2MiB overflow 为既有 recovery 容量警告，与本轮无关。
- 完成后审查修复：`conversation.item.create` 补齐 `object=realtime.item`，工具输出移除未定义字段；WebSocket 按 TEXT/CONTINUATION/FIN 重组跨 frame JSON，并增加超限、内存不足和 RX 队列满日志。
- Realtime 并发模型改为 WebSocket owner、麦克风采集、20ms 播放、Tool Worker 四条独立任务路径；慢 TF/NVS 工具不再阻塞录音、播放、收包和 stop/cancel，工具结果按会话 generation 丢弃过期回包。
- WRITE 自动授权改为 Realtime 单次 ToolCall 范围，不再修改全局 Tool Broker 权限；工具参数超限明确拒绝，结果 JSON 全部由 cJSON 转义，发送失败不再伪装成 responding。
- WakeNet 改用 dialog claim + 同步 stop 释放 I2S，不再改写用户 NVS 开关；24k→16k 重采样保留跨 delta 相位，音频解码/重采样缓冲改为会话常驻 PSRAM，减少热路径碎片。
- ES8311 增加 RX/TX 独立 I/O 锁；播放超时落到底层 I2S，写失败/部分写立即关闭 PA，停止顺序固定为先 PA off 再关闭 codec/I2S。
- USB JSON Lines 行缓冲扩为 16KiB，字段截断与整行超限改为显式错误，处理后擦除含 Key/密码的静态输入；Web Realtime 命令函数稳定化并限制刷新重入，Mock stop/cancel 不再产生迟到事件。
- 取消与跨会话竞态补充：session/response generation 同时标记麦克风帧、工具 job 和工具结果；cancel 后拒绝迟到 function call/result，连接期主动消费队列以保证 STOP 不被 SEND_TEXT 阻挡；工具完成队列改为预分配固定对象，WRITE 执行后结果不再因临时 OOM 静默丢失。
- cancel 后立即发送的新文本会暂存到 cancel ack，避免命令优先于 RX 时把旧 `response.created` 误认成新响应；收到 `response.cancelled/done` 后再串行发送暂存文本。额外区分 request-pending 与 waiting-ack，空闲状态误按 cancel 不会永久阻塞后续文本。
- 修复后验证：`tools/idf.ps1 build` 与 `web/npm run build` 均通过；主镜像 `0x29f1d0`。未执行烧录，仍需真机验证 WSS 握手、上下行连续音频、工具读写、barge-in、PA 爆音和 stop 后 WakeNet 恢复。
- 本轮备份：`tmp/backups/manual/20260721-193650_realtime_review_fixes/`、`tmp/backups/before_run/20260721-194916_realtime_fix_final_build/`、`tmp/backups/before_run/20260721-195811_realtime_race_fix_build/`、`tmp/backups/before_run/20260721-200120_realtime_cancel_epoch_build/`、`tmp/backups/before_run/20260721-200420_realtime_deferred_text_build/`、`tmp/backups/before_run/20260721-200826_realtime_idle_cancel_build/`、`tmp/backups/manual/20260721-195025_realtime_fix_progress/`。
- 2026-07-22 保存 Realtime API Key 触发重启补充修复：根因是 USB 8KB 协议任务栈上同时叠加约 2.6KB 的更新结构、Realtime 当前配置、配置回读视图及 NVS 调用栈，越过栈保护边界；不是 API Key 内容或 NVS 保存动作主动要求重启。
- `set/get_realtime_config` 的大配置对象现统一优先放入 PSRAM（不足时退回 8-bit heap），保存成功后先擦除并释放更新对象再回读；Realtime 内部 NVS 配置副本同样迁出调用栈，所有错误分支均释放内存，含 API Key 的临时对象释放前使用 volatile 擦除。
- ESP-IDF 5.5.1 `tools/idf.ps1 build` 通过；主镜像 `0x2b3240`（2,830,912 字节），SHA-256=`FF6EDF2BF84B6BB240F9F6D69EDB755F97DEB83490FFF89BAA34AF2686D52C78`。未执行烧录，仍需真机点击“保存 API Key”验证设备不重启且回读 `hasApiKey=true`、`ready=true`；`ota_1` 2MiB overflow 仍为既有 recovery 容量警告。
- 本次编辑前备份：`tmp/backups/manual/20260722-121235_realtime_key_stack_reset/`；构建前备份：`tmp/backups/before_run/20260722-121528_realtime_key_stack_fix_build/`。
- 2026-07-22 真机回归：P4 rev1.3（USB-Serial/JTAG `COM17`，MAC 末尾 `04:85`）以 `--no-stub`、115200 烧录 bootloader、主固件、分区表、OTA 数据和 WakeNet 模型，各写入区 Hash 校验均通过。通过 JSON Lines 写入用户提供的 Realtime API Key（不记录明文），保存响应与配置回读均成功，`hasApiKey=true`、`ready=true`。
- 保存后的 15 秒和额外 30 秒稳定观察均未见重启、Guru、Stack protection、brownout 或真实任务看门狗日志；`get_diagnostics` 返回 `brownoutCount=0`、`watchdogCount=0`。一次脚本初判的 watchdog 命中已确认仅是诊断 JSON 的字段名，不是异常日志。
- 本次烧录/测试前备份：`tmp/backups/before_run/20260722-122247_realtime_key_flash_test/`；进度编辑前备份：`tmp/backups/manual/20260722-122809_realtime_key_flash_test_progress/`。
- 2026-07-22 GLM Realtime TLS 握手修复：真机日志中的 mbedTLS `-0x7100` 发生在证书校验成功之后，定位为服务端握手数据超过原 4KiB TLS 接收上限；将 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 提升到 16KiB，保持 2KiB 发送缓冲与动态 TLS 缓冲，避免握手数据被截断。
- mbedTLS 改用 ESP-IDF 官方外部内存分配器，TLS 动态上下文和缓冲优先进入 PSRAM；Realtime 组件加入编译期配置检查。其余约 2.6KiB 的配置临时对象也从任务栈迁到 PSRAM 优先的 heap，并在释放前擦除 API Key。任务栈、WebSocket 控制对象等实时敏感内存继续留在内部 RAM。
- ESP-IDF 5.5.1 以 `--no-ccache` 完整构建通过；实际烧录的主镜像 2,830,704 字节，SHA-256=`CFB6374337B7E58F2D7486CFE6BDF42EBEF22BDEF139FF8C1AAA1A3A20481824`。`ota_1` 2MiB overflow 仍为既有 recovery 容量警告。
- 真机烧录：检测到 P4 USB-Serial/JTAG `COM17`（VID `303A:1001`），使用 `tools/idf.ps1 --no-ccache -p COM17 flash` 写入完成；烧录工具对各写入区域均报告 `Hash of data verified`，随后 RTS 自动复位。尚未复测 WSS 握手和 `session.created`。
- 本次编辑前备份：`tmp/backups/manual/20260722-123803_realtime_tls_psram_fix/`；构建前备份：`tmp/backups/before_run/20260722-124045_realtime_tls_psram_build/`。
- 2026-07-22 Web Realtime 页新增实时对话预览：区分用户/助手、语音/文本、流式/定稿/打断状态；完整处理用户听录、助手音频转写及文本 done 事件，并以音频转写覆盖并行文本流，避免重复。控制按钮改为统一主/次/危险样式，配置与协议日志降级为折叠诊断区，容器查询保证窄主面板下仍可用；Mock 与浏览器回归通过。
- 无声诊断补充：日志只有 `speech_started`，没有 `response.created`/`audio.delta`，说明该次没有下行 PCM，功放尚未进入播放。固件现显式配置 `far_field` 降噪和可复现的 Server VAD 默认参数，向 Web 上报 `speech_stopped`、`audio_committed`、首个音频分片及播放错误；首包后才切换 speaking，避免分片事件刷屏。
- 修复停止会话时零采样成功返回被误报为 `realtime mic read failed: ESP_OK` 的竞态日志。ESP-IDF 5.5.1 `--no-ccache` 构建与 Web 生产构建均通过；主镜像 `0x2b3570`，`ota_1` 2MiB overflow 仍为既有 recovery 容量警告。
- 本轮备份：`tmp/backups/manual/20260722-130817_realtime_conversation_preview/`、`tmp/backups/manual/20260722-132323_realtime_audio_diagnostics/`、`tmp/backups/before_run/20260722-132600_realtime_audio_diagnostics_build/`。
- 2026-07-22 最新 Realtime 对话预览与音频诊断固件已通过 COM17 烧录至 ESP32-P4 rev1.3；bootloader、主固件、分区表、OTA 数据和 WakeNet 模型区均通过 Hash 校验。复位后 `get_status` 返回 `ok=true`，USB/Wi-Fi 正常、PSRAM 约 28.25MiB 可用，未见重启循环；仍需在 Web 面板进行真人语音下行播放验收。烧录前备份：`tmp/backups/before_run/20260722-132704_realtime_audio_flash/`。
- 2026-07-22 WakeNet 启动门槛修复：原 64KiB 是 S3 阶段标注“待真机校准”的经验值，P4 在 AFE/ES8311 已初始化且经历 Realtime/TLS 生命周期后最大连续内部 DMA 块实测为 63,488 字节，导致模型已加载却被错误拒绝。门槛按 P4 分配结构校准为 48KiB（Feed/Detect 栈合计 14KiB、帧缓冲优先 PSRAM），真实低内存重试退避由 2 秒延长到 30 秒，避免刷屏。ESP-IDF 5.5.1 `--no-ccache` 构建通过；首次 COM17 烧录因 Web Serial 占用而在写 Flash 前退出，待释放端口后复测 listening。备份：`tmp/backups/manual/20260722-133640_p4_wakenet_dma_gate/`、`tmp/backups/before_run/20260722-133658_p4_wakenet_dma_build/`。

----
完成时间：2026-07-21

### P4 摄像头全屏高清预览（OV5647 MIPI-CSI → PPA → MIPI-DSI）

- 在 ui_p4 当前 AI Agent 页右上角增加「摄像头预览」按钮；点击后进入全屏实时视频流，触摸屏幕或 KEY0 返回输入法页。
- 新增 components/camera/fridge_camera_p4.c，替换桩实现：esp_video 打开 /dev/video0，RGB565 MMAP 双缓冲，PPA SRM 按厂商 EK79007 参数裁剪 800x600 居中写入 1024x600 DPI 帧缓冲并 present_frame。
- SCCB 复用 ES8311 共享 I2C1（GPIO28/29，init_sccb=false），预览期间持有 FRIDGE_CODEC_I2C_GATE_OWNER_CAMERA；CSI_RST=GPIO26、CSI_PWDN=GPIO27 写入 fridge_board_pins.h。
- display_mipi 增加帧缓冲所有权 API；ui_p4 预览期间暂停 LVGL present，退出后强制重绘。
- 依赖固定：esp_video 0.5.1 + esp_cam_sensor 0.5.3（third_party override）；sdkconfig 开启 OV5647 800x800 RAW8 与 CSI video device，关闭 DVP/AF。
- 厂商参考源码解压到 example/17_mipicamera_reference/（不进入构建）。
- 本阶段范围：仅实时预览，不做快门 JPEG / AI 识别。
- 构建：tools/idf.ps1 reconfigure build 通过；fridge_spirit.bin 约 2.82MB；ota_0(6M) 足够，ota_1(2M) 仍偏小属既有 recovery 限制。
- 风险/下一步：真机插 OV5647 验证取流与 fps；音频占用 I2C 时预览应提示失败；后续可选 JPEG 抓拍与 AI 登记页。
- 编辑前备份：tmp/backups/manual/20260721-211231_camera_hd_preview/。
- 点击卡死补充修复：共享 I2C 改为“先初始化总线、再持 CAMERA gate”，消除 INIT owner 重取同一二值信号量的自死锁；esp_video DQBUF 改为 100ms 有界等待，预览资源仅由摄像头任务统一清理，停止路径不再跨任务强拆 fd/MMAP/PPA。
- 显存竞态补充修复：显示驱动用整轮渲染锁串行化 LVGL renderer 与摄像头 PPA；摄像头从驱动取得当前安全可写缓冲，不再固定覆写 fb0；预览结束后恢复 LVGL 全屏重绘。
- TF 字体补充修复：延迟加载完成后把 BODY/TITLE 新字体指针重新绑定到既有 LVGL 控件；部分 BIN/FreeType 角色可重复补齐且不覆盖泄漏，TF 晚挂载后改为低频持续重试。
- 验证：ESP-IDF 5.5.1 `tools/idf.ps1 build` 通过，主镜像 `0x2b1040`；ota_1 2MiB overflow 仍为既有 recovery 容量警告。已通过 COM17（ESP32-P4 rev1.3 USB-Serial/JTAG、no-stub）烧录，各分区 Hash 校验通过并硬复位。
- 烧录后诊断：`get_status` 显示 Wi‑Fi/USB 正常、PSRAM 约 26MB 空闲；`get_tfcard_status` 显示 TF 已挂载、约 59.6GB 可用；`touch_status` 显示 GT911 Product ID=`911`、地址 `0x14`；`get_diagnostics` 显示 brownout/watchdog 计数均为 0；启动日志确认 FreeType CJK roles=7/7、TF 字体重新绑定成功。
- 真机预览修复与验收：首次 STREAMON 失败定位为 DSI 已启用 DW-GDMA IRAM-safe、CSI 却未启用 cache-safe，导致 CSI DMA 回调上下文落入 PSRAM并被驱动以 `ESP_ERR_INVALID_ARG` 拒绝；已在 P4 defaults/sdkconfig 开启 `CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE=y`。
- 预览 WDT 补充修复：FreeRTOS 为 100Hz，摄像头/UI 的 1~5ms 延迟会截断为 0 tick并饿死 IDLE0；两条循环均保证至少阻塞 1 tick。
- 最终 COM17 真机回归：OV5647 PID=`0x5647`，RGB565 800x800 成功 STREAMON，PPA 输出到 MIPI 1024x600；连续预览约 50 秒稳定在 16.2~16.4 FPS，无 WDT/Guru/brownout；触摸退出后帧缓冲释放、UI 恢复，`last_err=none`。
- 最终构建/烧录：ESP-IDF 5.5.1 构建通过，主镜像 `0x2b1160`；COM17 no-stub 烧录各分区 Hash 通过。ota_1 2MiB overflow 仍为既有 recovery 容量警告。
- 本轮备份：`tmp/backups/manual/20260721-223318_camera_preview_font_fix/`；构建前备份：`tmp/backups/before_run/20260721-224717_camera_preview_font_build/`。
- 烧录前备份：`tmp/backups/before_run/20260721-225219_camera_preview_font_flash/`；本次进度编辑备份：`tmp/backups/manual/20260721-230019_camera_preview_font_flash_progress/`。
- 本轮 CSI/WDT 修复备份：`tmp/backups/manual/20260721-231404_csi_dwgdma_iram_fix/`、`tmp/backups/manual/20260721-231856_camera_idle_wdt_fix/`、`tmp/backups/manual/20260721-232422_ui_idle_wdt_fix/`；最终烧录前备份：`tmp/backups/before_run/20260721-232519_ui_idle_wdt_flash/`；进度编辑备份：`tmp/backups/manual/20260721-232751_camera_preview_runtime_progress/`。
- 摄像头偏绿最终修复：第一次把旧 R/G/B 增益移到 800x800 传感器寄存器表后，用户实屏确认仍然偏绿，证明 OV5647 RAW8 路径不会可靠地把传感器侧 AWB 设置带到最终 RGB565，已撤回该无效修改。最终在 `esp_video` 的 P4 RAW→RGB ISP 阶段为 OV5647 单独启用 CCM，将旧标定值归一化为 R=`1.192`、G=`1.000`、B=`1.292`；不改变 Bayer、CSI 时钟、分辨率或 PPA 路径。ESP-IDF 5.5.1 构建通过，主镜像 `0x2b1710`（2,823,952 字节）、SHA-256=`64E2A41E2116ACEB5D45C761E3D0C34E3950BE4D5975FD5785516B08DDBDDFDD`；COM17 no-stub 烧录并完成各分区 Hash 校验，用户实屏确认颜色已恢复正常。ota_1 2MiB overflow 仍为既有 recovery 容量警告。本轮备份：`tmp/backups/manual/20260721-233216_ov5647_800x800_color_fix/`、`tmp/backups/manual/20260721-234322_ov5647_isp_ccm_fix/`、`tmp/backups/before_run/20260721-234407_ov5647_isp_ccm_build/`、`tmp/backups/before_run/20260721-234447_ov5647_isp_ccm_flash/`。

----
完成时间：2026-07-22

### P4 摄像头 720p 预览与 1080p AI 抓拍

- 增加 Espressif 官方 OV5647 RAW10 模式表，并通过 `VIDIOC_S_SENSOR_FMT` 在流停止时切换：预览使用 1280x960 合并模式居中裁出 1280x720，显示为 1024x576；AI 抓拍切到 1920x1080@30fps。
- P4 ISP 硬件宽高上限为 1920x1080，因此 OV5647 标称 2592x1944 不能进入当前 ISP/RGB/JPEG 链路；AI 抓拍采用本平台可实现的最大 1920x1080。
- AI 抓拍接入 P4 硬件 JPEG：RGB565、YUV420 子采样、质量 90；最近帧保存在 PSRAM，`get_frame/clear_frame/status` 已完整实现。
- 最小 USB 协议增加 `get_camera_status`、`camera_capture`、`clear_camera_frame`、`camera_preview_start/stop`，只返回图像元数据，不通过 115200 串口回传大 JPEG。
- 真机 COM17 验证：预览→停止→1080p 抓拍连续切换成功，无 Guru/watchdog/brownout；抓拍得到 1920x1080 JPEG 82,136 字节、耗时 341ms，清除后空闲 PSRAM 恢复到约 26,005KB。
- 预览性能实测：固定 5:4 RGB565 缩放约 47.9ms/帧，MIPI 翻页约 7.8ms/帧，稳定约 14fps。PPA 通用缩放亦约 46.5ms/帧，证明瓶颈是当前 PSRAM/显示帧缓冲带宽；在保持 1280x720 输入和 1024x576 输出时无法真实达到 30fps，代码不伪报 30fps。
- ESP-IDF 5.5.1 构建通过，主镜像 `0x2b1f90`（2,826,128 字节）、SHA-256=`AB052E668BBB9E1783B0D1AC9D201687C281926CB66621E060B43EE640C6DB3B`；COM17 no-stub app-flash Hash 校验通过。ota_1 2MiB overflow 仍为既有 recovery 容量警告。
- 主要备份：`tmp/backups/manual/20260721-235403_camera_720p_fullhd_capture/`、`tmp/backups/manual/20260722-000218_p4_camera_diag_commands/`、`tmp/backups/manual/20260722-001150_camera_fixed_ratio_scaler/`、`tmp/backups/manual/20260722-001548_camera_max_preview_fps/`；构建/烧录前备份位于 `tmp/backups/before_run/` 对应目录。
- RAW10 色彩/曝光补充修复：将 1280x960 预览与 1920x1080 抓拍的 ISP `vts/hts` 元数据对齐 Espressif 官方 OV5647 驱动，修复自动曝光换算使用错误时序的问题；RAW10 CCM 保持既有白平衡比例并将最大通道归一化到 1.0（R=`0.923`、G=`0.774`、B=`1.000`），避免亮部经红蓝增益二次放大后裁白，RAW8 继续保留用户已确认正常的旧标定。ESP-IDF 5.5.1 构建通过，主镜像 `0x2b2010`；COM17 no-stub app-flash Hash 校验通过。运行日志确认 RAW10 CCM 生效、预览稳定约 14.8fps、PSRAM 约 21MB 空闲，未见 brownout/watchdog/取流错误；最终色彩观感等待用户实屏确认。本轮备份：`tmp/backups/manual/20260722-003520_ov5647_raw10_color_exposure/`、`tmp/backups/before_run/20260722-003553_ov5647_raw10_color_build/`。
- 摄像头启动卡死补充修复：共享 I2C 已初始化时不再重复用 `portMAX_DELAY` 获取 INIT gate；进入预览的后台任务会先停止 GLM Realtime、暂停 WakeNet/手动录音并释放 AUDIO gate，退出后解除策略暂停，严格保证 OV5647 与 ES8311 分时复用 GPIO28/29。真机在 WakeNet 占用音频的实际状态下完成两轮“启动→退出→再次启动”，均进入 1280x960 取流/1024x576 显示，约 15fps，无 Guru/watchdog/I2C 超时；ESP-IDF 5.5.1 构建、COM17 no-stub app-flash 与 Hash 校验通过。备份：`tmp/backups/manual/20260722-202232_camera_i2c_start_deadlock/`、`tmp/backups/manual/20260722-202716_camera_audio_handoff/`、`tmp/backups/before_run/20260722-202750_camera_audio_handoff_build/`。

----

## ui-reference/ver2：P4 横屏 UI 网页参考预览第一版完成（2026-07-22）

- 内容：按 `ui-reference/ver2/doc/` 规划文档实现完整 UI 参考预览（纯静态 HTML/CSS/原生 JS，零依赖零构建，双击 index.html 经 file:// 可开）：29 个页面模板 + 12 个全局浮层 + 8 种状态侧舱 CTX，严格 1024×600 三栏壳（704 主区 + 72 侧导航 + 248 状态侧舱），5 个一级导航（首页/库存/对话/计划/更多）。
- 视觉：暖色+鼠尾草绿（用户选定，延续 ver1 调色板；布局为 ver2 全新三栏壳）。字号六档 18/22/28/36/48/64 对齐固件 font_manager 字库角色；圆角 ≤8px；触控 ≥48px；纯色平铺 RGB565 友好。
- 架构：window.FS 命名空间；页面注册表（唯一事实源）+ hash 路由（可刷新/深链）+ 模块现场记忆；侧舱独立状态机（自动跟随 浮层>页面，可手动锁定）；浮层栈（阻塞型 OV-03/04/10/12，OV-10 全屏接管）；六态切换器（正常/加载/空/离线/错误/待确认/冲突）；页面总览缩略图墙（56 张，INV-01 五态/SYS-02 四步均出变体）；5 条流程演示（拍照登记/语音登记/Agent高风险/烹饪计时/安全告警）。
- 实现方式：核心运行时与首页由主线实现作为参考实现，库存/对话/计划/更多/系统 5 个模块由 5 个并行子代理实现（各自独立 js+css）。
- 验证：Node 冒烟测试（tmp/notes/ver2/ver2-smoke-test.js）全部通过——29 页注册齐全、101 种 页面×变体×状态 渲染无异常无 undefined、12 浮层 + 8 CTX 渲染正常、总览 56 张缩略图；全部文件 UTF-8 无 BOM；node --check 16 个 JS 全过。
- 涉及文件：ui-reference/ver2/ 下 index.html、README.md（含设计令牌表/组件→LVGL 映射/清单矩阵/保真规范/验收清单）、styles/（8 个 css）、js/core/（10 个运行时 js）、js/data/mock-data.js、js/pages/（6 个模块 js）。未改动 ver2/doc/。
- 风险/下一步：INV-05 为静态取景框（不开真实摄像头）；计时数字为静态 mock（固件用 lv_timer 局部刷新）；README 第九节验收清单建议真机浏览器逐条走查；固件 ui_p4 多页实现时可对照 README 的组件→LVGL 映射与页面注册表。

----
完成时间：2026-07-22

### GLM Realtime 服务端 VAD 不结束修复与真机回归

- 同一 API Key 的电脑端最小 WebSocket 对照复现：16k/16-bit/mono 语音后追加 1.5~2 秒静音，服务端只发送 `input_audio_buffer.speech_started`，20 秒内不发送 `speech_stopped` 或自动响应；这与 P4 上“持续录音中”一致，排除 Web 状态、ES8311 采集和上行链路故障。
- 对照验证主动发送 `input_audio_buffer.commit` + `response.create` 后，服务端立即返回用户转写、助手文本 delta、助手音频转写和 24k PCM 音频。因此固件关闭 Server VAD 的自动创建响应，改为“服务端 `speech_started` 起点 + 本地能量 VAD 尾点”的双重判定：ES8311 双工预热 1 秒，连续 200ms 有效语音确认，800ms 静音主动提交，单轮最长 15 秒。
- 本地 RMS/Peak 计算使用整数平方和比较，不新增音频缓冲；PCM、Base64、WebSocket RX、解码和重采样大缓冲继续优先放 PSRAM。工具导出上限从排查用的临时 0 恢复为 24，真机会话实际导出 14 个工具。
- `response.done` 解析改为读取嵌套 `response.status/output/usage`，并从 `response.output_item.done`、最终文本/转写事件兜底提取内容；若服务端以 completed 结束但没有任何文本或音频，明确上报 `EMPTY_RESPONSE`，Web 不再误显示为持续生成。
- 真机 COM17 最终回归：会话连续空闲 25 秒，上行 1,036,800 字节，状态保持 listening，`lastError` 为空、无误触发/空提交；启动、session.update、ES8311 上行与 stop 均正常。真人说话的最终声学验收仍需用户在 Web 面板进行一次。
- WakeNet 同期完成 P4 内部 DMA 门槛校准：48KiB 门槛下状态稳定 listening，最大内部 DMA 块实测 129,024 字节，不再出现每 2 秒 `skip wake start`；低内存重试延长到 30 秒。
- ESP-IDF 5.5.1 构建通过；主镜像 2,833,760 字节，SHA-256=`D16C89C7F2BEDCD40C75F11FE56F809C6E31CCDAD4A54E68075B7C398A8FCB0A`。COM17 no-stub 全量烧录完成，各写入区 Hash 校验通过；`ota_1` 2MiB overflow 仍为既有 recovery 分区容量警告。
- 主要备份：`tmp/backups/manual/20260722-140601_realtime_local_vad_commit/`、`tmp/backups/manual/20260722-141324_realtime_vad_warmup/`、`tmp/backups/manual/20260722-141717_realtime_dual_vad_gate/`、`tmp/backups/before_run/20260722-141747_realtime_dual_vad_build/`、`tmp/backups/manual/20260722-142057_realtime_vad_progress/`。
- 工具 422 补充修复：服务端要求每个工具参数均含 `description`，对象 Schema 必须含对象类型的 `properties`，`required` 必须是数组；导出到 Realtime 的临时 cJSON 副本会递归补齐这些兼容字段，本地 Tool Registry/Broker 的权限和校验语义不变。
- COM17 真机发送 14 工具的 `session.update` 后，文本请求成功进入 `response.created`，不再出现 422；纯文本 Realtime 仍由服务端以 completed 空内容结束并被明确标记为 `EMPTY_RESPONSE`，需与真人语音路径分开判断。当前会话明确只有 `text` + `audio`，未启用摄像头或视频模态、未上传视频帧。
- 修复后主镜像 2,834,496 字节，SHA-256=`A9BB38B2A0D7CBBC432A0A926FFEDDEED9E784D7C88F076C8A3AA1E96145DB3F`；ESP-IDF 5.5.1 构建、COM17 no-stub 全量烧录及各区 Hash 校验通过。备份：`tmp/backups/manual/20260722-142700_glm_tool_schema_422/`、`tmp/backups/before_run/20260722-142834_glm_tool_schema_flash/`。
- 语音工具校验二次修复：智谱语音路径还会把空 `required: []` / `properties: {}` 归一化为 null；有参数但原本全可选的 Schema 现将实际参数名补入 required，`device.state.get`、`task.list` 两个无参数工具只从 Realtime 导出中省略，本地 Registry/Broker 仍保留。电脑端同 Key、12 工具、合成语音真实请求成功返回文本 delta 和音频，无 422。
- 最终 COM17 真机回归：日志确认 `session.update sent, tools=12`，会话 listening、音频上行 553,600 字节、`lastError` 为空、未出现 422，随后正常 stop 并释放串口。主镜像 2,834,544 字节，SHA-256=`70B7B8819BB9492439068D3AB06A5DE3281D6BAB689F2042029F45F2FA4518AE`；全量烧录各区 Hash 校验通过。备份：`tmp/backups/manual/20260722-143340_glm_tool_schema_empty_collections/`、`tmp/backups/before_run/20260722-143403_glm_tool_schema_strict_build/`、`tmp/backups/before_run/20260722-143435_glm_tool_schema_strict_flash/`。
- Realtime 下行无声修复：服务端实测单条 `response.audio.delta` 可达约 186KB base64，超过原 24KB WebSocket 消息区和 18KB 解码区；接收区扩为 256KiB、解码区 192KiB、24k→16k 重采样区 64K 样本、播放环形区 12 秒，全部由既有 PSRAM 优先分配路径承载，避免文字正常而大音频事件被丢弃。
- P4 最小协议新增 `speaker_beep`：暂停 WakeNet 后通过板载 ES8311 + I2S TX + NS4150B（PA_CTRL=GPIO11）播放 1kHz/500ms 本地自检音，不依赖云端 TTS/API Key；Web「扬声器测试」页增加“蜂鸣器测试”按钮，并移除过时的 S3/MAX98357 引脚提示。
- ESP-IDF 5.5.1 与 Web 生产构建均通过；主镜像 2,835,360 字节，SHA-256=`EC9CF9C4DC29805C504D4A804FA4C8094BF3EC2330981E40218F204D26C4D3DB`。`ota_1` 2MiB overflow 仍为既有 recovery 容量警告；构建完成后的真机结果见下一条。备份：`tmp/backups/manual/20260722-144531_realtime_audio_beep/`、`tmp/backups/before_run/20260722-144740_audio_build/`、`tmp/backups/before_run/20260722-145125_speaker_beep_race_build/`。
- COM17 全量烧录成功：ESP32-P4 rev1.3 的 bootloader、主固件、分区表、OTA 数据和 WakeNet 模型区均通过 Hash 校验并自动复位。启动后 `get_status` 返回 `ok=true`、PSRAM 空闲约 28.0MiB；自动调用 `speaker_beep` 返回 `played=true`，串口确认 ES8311 双工启动、播放与停止均为 `ESP_OK`。实际声压/音质仍需设备旁人工听感确认；烧录前备份：`tmp/backups/before_run/20260722-145347_realtime_audio_beep_flash/`。
- 第二轮语音截断补充修复：真机日志显示首个 73,930B PCM 分片已播放，随后下一条 WebSocket 消息仍超过 256KiB 固定上限并被丢弃。RX 重组、base64 解码、24k→16k 重采样及播放环形区现改为有硬上限的 PSRAM 动态扩容：JSON 最大 2MiB、对应约 32 秒 PCM 工作区、播放环形区最大 40 秒；大消息队列副本强制 PSRAM-only，禁止失败时回退内部 RAM。扩容只增不减，播放环形区在锁外分配、持锁搬运，减少碎片和播放任务停顿。
- ESP-IDF 5.5.1 构建通过，主镜像 2,837,936 字节，SHA-256=`575BFCE1F0AF327A0564F948D3CFA8AEE45761DC3059FBCFFBC27A71BBBD9C08`；COM17 no-stub 全量烧录完成且五区 Hash 校验通过。复位后 `get_status` 返回 `ok=true`、PSRAM 空闲约 27.3MiB、USB 正常；状态 JSON 中的 brownout 提示文案不是异常日志。真人连续两轮下行播放仍需 Web 面板验收。备份：`tmp/backups/manual/20260722-150115_realtime_dynamic_audio_buffers/`、`tmp/backups/before_run/20260722-150521_realtime_dynamic_audio_build/`、`tmp/backups/before_run/20260722-150612_realtime_dynamic_audio_flash/`。
- Realtime 自然停顿误判补充修复：官方协议确认 Server VAD 会自动提交音频，而 `create_response=false` 仅关闭自动创建回复；固件现把服务端尾静音从 500ms 调整为 1500ms，收到 `input_audio_buffer.committed` 后显式发送一次 `response.create`，并把本地能量 VAD 改为 2000ms 尾静音兜底，避免约 800ms 的句中停顿被截断，也避免服务端已提交后再次手动 commit。记忆工具日志已确认 `memory.add` 返回“已写入结构化记忆”，随后模型完成结果复述。ESP-IDF 5.5.1 构建通过，主镜像 2,838,176 字节，SHA-256=`32537CF84D5D03876519C9E236025F5B91C93EAA3AD4BFF19AD0C5968EC49313`；`ota_1` 2MiB overflow 仍为既有 recovery 容量警告。备份：`tmp/backups/manual/20260722-152603_realtime_vad_tuning/`、`tmp/backups/before_run/20260722-152637_realtime_vad_build/`。

----
完成时间：2026-07-23 00:10:41 +08:00

### ver2 HOME-01 与横屏 CTX-01 首次固件移植

- 在当前实际启用的 `components/ui_p4` 中完成固定 `1024×600` Shell：`704px` 首页主区、`72px` 五项导航和 `248px` 状态舱；首页保持单一纵向滚动，未引入 JSON UI、JS/Lua 或浏览器运行时。
- 页面结构与事件继续由 C/LVGL 9.2.2 管理；HOME-01 已包含页头、四张统计卡、临期 Top3、今日推荐、Agent 建议、菜单/购物摘要和最近同步，CTX-01 已接入时间日期、Wi-Fi/C6、互联网、TF、摄像头、麦克风的真实状态。无业务温湿度源时继续显示占位值，不伪造数据。
- 语音按钮异步接入现有 GLM Realtime；拍照按钮复用 OV5647 高清预览。语音启动待定态与会话进行中均阻止摄像头启动，保持 GPIO28/29 共享 I2C 的音频/摄像头分时互斥；摄像头退出后重绘完整 Shell。
- 新增 TF 页面资源包 `2.0.0-home.1`：`manifest.json`、`content/home.json` 和 26 个 A8 单色图标；固件按版本、尺寸和 SHA-256 校验，图标加载到 PSRAM。电脑端生成器支持确定性构建和仅替换 `fridge/assets/ui/ver2` 子树的安全部署。
- 继续复用 `/fridge/assets/fontpack/v1/`，六档字体通过全局 `lv_style_t` 绑定；48px 角色使用 FreeType，64px 数字优先使用 `digits_64_2bpp.bin`。TF 未就绪时先显示 Flash 降级首帧，挂载后再绑定完整图标、数据和字体。
- 资源生成/校验的 6 项 Python 回归测试通过；正常 TF、损坏 manifest 和资源包缺失三种启动路径已验证，损坏/缺失时保持可用降级界面且恢复资源后可重新加载。recovery 独立构建为 1,072,496 字节，确认未引入 `ui_p4`、Font Manager、LVGL 或 FreeType。
- 根据首次真机照片补充修复字体裁切和控件交叠：单行标签改用 `LV_SIZE_CONTENT` 真实行高；按钮、胶囊和角标统一双轴居中；统计卡增高并下移后续区块；临期名称、估算胶囊、到期胶囊划分独立横向范围；状态点归入各自健康图标；状态舱晚餐允许两行，下一事项时间取消冗余前缀，语音/拍照按钮按图标上文字下重新对齐。TF 字体重新绑定后会先执行 LVGL 布局重算再整屏失效刷新。
- 最终使用 ESP-IDF 5.5.1 `tools/idf.ps1 reconfigure build` 构建通过；主镜像 2,847,520 字节，SHA-256=`261F1DD0167A4B7FDBABD2AFDA961FC53F867F4EE7CE2405D0DD689CB9A66149`，Flash 使用 2,734,864 字节，DIRAM 使用 174,590/576,464 字节。`ota_1` 2MiB overflow 仍为主镜像不应写入 recovery 槽的既有预期提示。
- 已通过 COM17 对 ESP32-P4 rev1.3 执行 no-stub 全量烧录，五个区域均通过 Hash 校验。短时启动检查确认 32MB PSRAM、自检、Flash 降级首页、TF 字体包、FreeType 7/7 和全局字体重绑定正常；串口查询确认 TF 已挂载且 ver2 manifest/home 文件存在。按用户要求不执行 30 分钟稳定测试，由用户完成最终实屏排布与长稳验收。
- 主要备份：`tmp/backups/manual/20260722-225528_ui_ver2_home_port/`、`tmp/backups/manual/20260722-235928_ui_ver2_layout_fix/`、`tmp/backups/before_run/20260723-000433_ui_ver2_layout_fix_build/`、`tmp/backups/before_run/20260723-000553_ui_ver2_layout_fix_flash/`；部署与异常注入日志位于 `tmp/logs/20260722-ui-ver2-*.log`。
- 字重层级补充：正文 18/22px 继续使用 Regular BIN + Regular FreeType 全量回退；标题、分区标题和 48/64px 大号信息改为 TF 已有 `NotoSansCJKsc-Medium.otf` 主字体，避免 Regular BIN 抢先命中而看不出字重差异，并保留 BIN/Flash 应急链。无需改写 TF 文件。ESP-IDF 5.5.1 构建通过，主镜像 2,847,808 字节、SHA-256=`2D252BBE21132C1E6F5C3BE511A91B04CE0FA6A6E38C285FF88E10E2C8E16441`；COM17 no-stub 全量烧录五区 Hash 通过。启动日志确认 `FreeType CJK roles=7/7 emphasis=4/4 weight=Medium`、全局字体样式重新绑定、GT911/TF/Wi-Fi/32MB PSRAM 正常，无 Guru/watchdog/brownout。日志：`tmp/logs/20260723-font-weight-hierarchy-com17.log`；备份：`tmp/backups/manual/20260723-001307_font_weight_hierarchy/`、`tmp/backups/before_run/20260723-001543_font_weight_flash/`。
- 左侧滚动性能与蓝屏排查补充：当前链路已是 RGB565、MIPI DIRECT 双全屏 PSRAM 帧缓冲、无软件旋转、无逐帧全屏 memcpy，并保持 10ms 刷新请求，不设置 30fps 上限。真机统计显示左侧 `704×536` 滚动会重绘整块视口（约全屏 61.3%），软件渲染约 117～127ms，瓶颈不在约 6～10ms 的 DSI present；右侧约 13.2% 脏区仍可达到 28～31fps。
- 双 SW draw unit 与 `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` 均完成单变量 A/B：前者没有缩短约 120ms 帧耗时，后者仍约 6.1～6.7fps且额外占用约 62KB DIRAM并产生段属性警告，最终均回退，保留单绘制和 128KB/64B L2，避免挤压 WakeNet 所需的连续内部 DMA 内存。
- 持续大面积滚动复现的蓝屏属于 P4 HP 中断看门狗裸复位，不是 Task WDT 或 LVGL 空指针；`CONFIG_ESP_INT_WDT_TIMEOUT_MS` 从默认 300ms 放宽到 800ms 后，同一确定性滚动压力可完整运行，未关闭任何看门狗。同步修正 DSI 提交顺序：在 `draw_bitmap` 前清理旧刷新 token，避免误删本次完成信号并多等一个显示帧。
- 最终移除自动滚动和 Task WDT panic 诊断配置；ESP-IDF 5.5.1 最终构建通过，主镜像 2,890,064 字节（`0x2c1950`）、SHA-256=`6CBB8AC83BBB0AE5C7184966BC83CC3471FBD0E25FBC853D28F1CDDFBD2E46D6`，DIRAM 169,614/576,464 字节。COM17 no-stub 全量烧录五区 Hash 通过，25秒冷启动确认 P4 rev1.3、32MB PSRAM、MIPI/GT911、TF/FreeType、Wi-Fi 与 WakeNet 正常，无自动滚动和重启；按用户要求不执行30分钟长稳，持续触摸与主观滚动由用户验收。

----
完成时间：2026-07-23

## SHT40 温湿度传感器接入（P4 最小移植阶段）

### 改动模块
- 新建 `components/sht4x/`（CMakeLists.txt / include/fridge_sht4x.h / fridge_sht4x.c）
- `main/CMakeLists.txt`：else() 闭包加入 `sht4x`
- `main/main.c`：P4_MINIMAL 路径 GT911 init 后调用 `fridge_sht4x_init()`
- `components/ui_p4/fridge_ui_p4.c`：`collect_device_status()` 替换温湿度占位符为真实读数
- `components/ui_p4/CMakeLists.txt`：REQUIRES 加 `sht4x`
- `components/tool_system/src/builtin_tools.c`：`device.state.get` JSON 追加 `tempC`/`humidityRh`/`envValid`
- `components/tool_system/CMakeLists.txt`：REQUIRES 加 `sht4x`
- `components/usb_protocol/fridge_usb_protocol_minimal.c`：`get_status` 响应替换 `temperatureC:null` 为真实值
- `components/usb_protocol/CMakeLists.txt`：REQUIRES 加 `sht4x`
- 新增 `doc/SHT40温湿度接入.md`

### 技术要点
- SHT40 与 SHT30 命令集不同：测量命令 0xFD（单字节，无 clock stretching），等待 ≥10ms，读 6 字节，CRC-8 poly=0x31 init=0xFF
- 共享 I2C0 红线：通过 `i2c_master_get_bus_handle(I2C_NUM_0)` 复用 GT911 已建总线，不重新创建
- 必须在 `fridge_touch_gt911_init()` 之后调用
- 5s 轮询任务，mutex 保护快照，失败时 `valid=false` 不阻断系统

### 当前结果
- 代码已接入，待真机验证（编译 + 上电日志 + UI 显示 + USB get_status + GLM device.state.get）

### 风险与后续
- 真机验证：确认 SHT40 在 0x44 地址响应，UI 显示真实温湿度，手捂传感器数值上升
- 若 I2C0 速率 100kHz 与 GT911 400kHz 存在冲突，可在 `sht4x_probe_address` 中调整 `scl_speed_hz`

----
完成时间：2026-07-23 10:43 +08:00

## ver2 HOME-01 左侧主区 PPA 页面缓存滚动加速

- 首选方案已落地：LVGL 继续负责 GT911 输入、真实滚动坐标和惯性；静止时将完整 `704×846` HOME 主滚动区快照为 RGB565 页面缓存，缓存位于 64 字节对齐的有界 PSRAM（实测 1,163KiB），手势期间由 ESP32-P4 PPA SRM 做 `704×536` 视窗 1:1 裁剪搬运，不再逐帧软件栅格化复杂控件和中文字体。
- 保留现有 MIPI DIRECT 双缓冲和约 56.8Hz 面板上限，不设置 30fps 软件上限。PPA 热路径为阻塞式、单 UI 任务、无逐帧分配；同一 LVGL 周期内的多个滚动更新只提交最新坐标。
- 新增双缓冲奇偶恢复：手势结束时把显示驱动的下一可写缓冲重新对齐 `lv_display_get_buf_active()`；摄像头接管前请求 PPA 收尾，继续受 UI render lock、present lock 和 camera ownership 三层互斥保护。
- 字体、TF 图标和 HOME 示例数据变化会使缓存失效并在空闲期重建；缓存构建/PPA/显存失败时自动恢复 LVGL 刷新，按 2 秒退避重试，避免失败后高速重建和刷日志。页面高度硬限制为 1,600px，防止异常布局无限占用 PSRAM。
- 启用 `CONFIG_LV_USE_SNAPSHOT=y`，只用于页面静止时的缓存重建，不把 Snapshot 图像对象加入实时渲染树。`CONFIG_ESP_INT_WDT_TIMEOUT_MS=800`、单 SW draw unit、10ms 刷新请求均保持不变。
- 真机 ESP32-P4 rev1.3 / COM17 验证：两次缓存构建均成功，PPA 真实探针分别为 16.6ms、17.9ms；缓存后 PSRAM 空闲约 27.8MiB、UI 栈高水位 28,168 字节。启动与约 80 秒短时运行未见 Guru、WDT、brownout 或蓝屏；实际手指连续滑动的 `PPA scroll=... fps` 尚未在采样窗口内触发，最终主观流畅度和手势帧率由用户实屏验收。
- ESP-IDF 5.5.1 完整构建和 `size` 通过：主固件 `2,911,136` 字节，SHA-256=`46AE0DA385ACD4AAFF704C6FBC0BFC93778132AC2FA001C590BB089D25E83232`；Flash 使用 2,803,380 字节，DIRAM 使用 169,814/576,464 字节。`ota_1` 2MiB overflow 仍是大主固件不写 recovery 槽的既有预期提示。
- COM17 no-stub 全量烧录成功，bootloader、主固件、分区表、OTA 数据和 WakeNet 模型区均通过 Hash 校验。为解除本轮首次完整构建暴露的既有闭包错误，仅在 `components/sht4x/CMakeLists.txt` 补充其源码已经实际使用的 `board` 依赖。
- 备份：`tmp/backups/manual/20260723-102443_ppa_scroll_accel/`、`tmp/backups/manual/20260723-103345_sht4x_build_dependency/`、`tmp/backups/before_run/20260723-103020_ppa_scroll_build/`、`tmp/backups/before_run/20260723-103836_ppa_scroll_flash/`。

----
完成时间：2026-07-23 11:57 +08:00

## P4 ver2 横屏“更多”与时间工具页移植

- `components/ui_p4` 的固定 `1024×600` Shell 已扩展为首页、更多、定时器、闹钟和秒表五个主内容页；导航与状态舱常驻，工具页返回更多，首页/更多导航高亮随页面同步。
- MORE-01 只保留定时器、闹钟、秒表三张卡片与 Wi-Fi、TF、校时摘要。静态文案、默认名称、六个倒计时预设和 A8 图标由受校验的 TF `content/more.json` 提供；缺卡、旧包或校验失败时继续使用固件文本/图标降级。
- 定时器、闹钟和秒表均接入既有 `kitchen_tools`：单倒计时支持 1/3/5/10/15/30 分钟、暂停/继续/取消/到时确认；最多五个固定日闹钟支持内联时分新增、删除和到时确认，未校时会明确提示；秒表仅以秒级精度提供开始、暂停和复位。未增加多定时器、闹钟编辑/开关、周期提醒、+1 分钟、厘秒或计次等后端未支持能力。
- `ui_p4` 新增 `kitchen_tools` 组件依赖，全部操作仍在唯一 LVGL 任务内执行并每秒读取快照，仅更新变化标签。标题、卡片名、数字面和操作按钮继续使用 `TITLE/SECTION/LARGE/HUGE` 字体角色，TF 的 Noto CJK Medium 负责真实加粗，正文保持 Regular。
- TF 资源包升级至 `2.0.0-more.1`，新增 `bell.a8`、`stopwatch.a8` 和 manifest SHA/A8 校验；资源包 Python 回归 9/9 通过，`build_ver2_pack.py --verify-only` 通过。
- `tools/idf.ps1 build` 已在 ESP-IDF 5.5.1 的 CMake 依赖解析阶段被当前工作区既有问题拦截：`storage` 所需 `littlefs` 组件未找到，尚未进入本次 `ui_p4` 编译或真机烧录。恢复该全局依赖后需补做完整构建、资源部署、实屏操作和串口稳定性验证。
- 备份：`tmp/backups/manual/20260723-114540_p4_more_pages/`、`tmp/backups/manual/20260723-115734_p4_more_pages_progress/`。

----
完成时间：2026-07-23 13:10 +08:00

## LVGL 截屏写 TF 卡功能

- 新增 `components/screenshot` 组件：截取当前 LVGL 显示帧（`display_mipi` 双缓冲中正在被 DPI 扫描的那块，即屏幕可见帧）并写入 TF 卡 `.fridge_spirit/exports/`。持 `begin_ui_render()` 互斥期间只做一次整帧 memcpy 到 PSRAM 暂存缓冲后立即放锁，避免长时间阻塞 LVGL 与画面撕裂。
- 格式策略按用户要求做降级：摄像头空闲时用 P4 片内硬件 JPEG 编码器（复用 camera 路径同款 `jpeg_encoder_process`，输入 RGB565、子采样 YUV422 因 600 行只能 8 对齐不能用 420、Q85），输出 `shot_<sec>.jpg`；摄像头正占用编码器时降级为 16-bit BI_BITFIELDS BMP（零依赖、自下而上行序），输出 `shot_<sec>.bmp`。JPEG 意外失败也回退 BMP，保证截屏不空手而归。
- `camera` 组件新增公共接口 `fridge_camera_is_busy()`（P4 临界区读 `s_capture_running/s_preview_running/s_preview_task`；S3 路径恒返回 false），供截屏判断硬件 JPEG 编码器（单例资源）是否被占用。截屏只读显存不碰摄像头 SCCB，故不涉及 GPIO28/29 共享 I2C gate。
- `usb_protocol`（实际编译的 `fridge_usb_protocol_minimal.c`）新增 JSON 命令 `screenshot_capture`，同步执行，成功回复 `payload.path` 与 `payload.format`（jpeg|bmp）。`usb_protocol`、`main` 的 REQUIRES 均加入 `screenshot`。
- 触及文件：`components/camera/include/fridge_camera.h`、`components/camera/fridge_camera_p4.c`、`components/camera/fridge_camera.c`、`components/usb_protocol/fridge_usb_protocol_minimal.c`、`components/usb_protocol/CMakeLists.txt`、`main/CMakeLists.txt`，新增 `components/screenshot/{CMakeLists.txt,include/fridge_screenshot.h,fridge_screenshot.c}`。
- ESP-IDF 5.5.1 `tools/idf.ps1 build` 通过（Project build complete），截屏相关代码全部编入固件；`ota_1 2MiB overflow` 为大主固件不写 recovery 槽的既有预期提示，与本功能无关。尚未真机烧录验证——需 COM15 flash 后下发 `screenshot_capture`，取卡确认图片内容与屏幕一致，并在摄像头预览期间复测 BMP 降级。
- 备份：`tmp/backups/before_run/20260723-125309_screenshot/`。

----
完成时间：2026-07-23 13:47 +08:00

## HOME-01 PPA 连续滚动与按钮误触稳定性修复

- 左侧主滚动区增加滚动手势窗口：卡片和按钮收到 `CLICKED` 时，若仍在滚动或距最后一次滚动不足 300ms，则不再执行“后续移植”提示；正常静止点击行为不变。
- PPA 手势结束不再强制失效整个 `704×536` 左区，只重绘右缘 7px 滚动条条带；在 `SCROLL_BEGIN` 清除 LVGL 已排队的首个整区 invalid 区，避免每次手势结束都补一次约 110～126ms 的软件全区重绘。
- 修复 UI 初始化竞态：主启动流程在 TF/Agent 重型初始化前后会调用两次 `fridge_ui_p4_init()`，现以任务句柄作为“已经启动”屏障，确保只创建一个 LVGL 任务。修复前真机可见两次 `ui_task start` 和第二次 `shell create failed`，修复后冷启动只保留一次 UI 任务。
- PPA 保持 128-byte 最大 burst、10ms UI 调度请求和约 56.8Hz 面板上限，不增加 30fps 软件锁。手势期间跳过相同整数 y 坐标的重复提交；每个手势输出帧数、FPS、PPA copy/present 平均值和最大值，便于继续实屏量化。
- COM17 真机触摸采样中，128-byte 路径长手势达到约 30～32fps，PPA copy 约 18～19ms；对照 64-byte burst 仅约 19～24fps，因此最终保留 128-byte。短时连续触摸与后续 50 秒无复位监听均未出现 Guru、WDT、brownout、MIPI refresh timeout 或 PPA fallback。
- ESP-IDF 5.5.1 完整构建、独立干净构建目录复核、`size` 和 COM17 no-stub 烧录通过；最终烧录主固件 3,001,984 字节，SHA-256=`3ABF6E0EC109F1C99E57E4997DF6D8BF33AF6D2F81B2A83F151415641A5BDE66`。Flash 使用 2,894,204 字节，DIRAM 使用 173,638/576,464 字节；`ota_1` 2MiB overflow 仍为既有 recovery 容量提示。
- 本轮完整重配置同时暴露并局部解除两个既有构建阻塞：`storage` 正式声明并由组件管理器锁定 `joltwallet/littlefs 1.22.3`；扬声器为 IDF 5.5.1 补充 HTTP 不完整响应错误码兼容，并把可能触发 `-Wformat-truncation` 的 `snprintf` 改为有界 `strlcpy/strlcat`，不改变失败重试或音色归一化行为。
- 主要备份：`tmp/backups/manual/20260723-132448_ppa_scroll_longrun_fix/`、`tmp/backups/manual/20260723-133738_ui_single_start_guard/`、`tmp/backups/manual/20260723-134335_ppa_max_burst_clear_pending/`、`tmp/backups/before_run/20260723-134433_ppa_max_burst_app_flash/`、`tmp/backups/before_run/20260723-135232_final_locked_app_flash/`。

----
完成时间：2026-07-23 18:08 +08:00

## Codex 实屏截屏读取入口验证

- 新增 `tools/capture_screenshot.ps1` 作为 Codex/PowerShell 固定入口：默认使用 `COM17` 和项目 IDF Python 环境，调用 `tools/test_screenshot.py` 完成 `screenshot_capture`、TF 分块读回、CRC32 和 JPEG/BMP 文件头校验，最后输出包含 `local_path` 的 JSON。
- 为适配 P4 原生 USB-Serial/JTAG，串口打开状态固定为 `DTR=false、RTS=true`；实测 `RTS=false` 时端口可打开但固件收不到命令，`RTS=true` 后可正常收发。WebUI、脚本和其他串口工具仍必须互斥占用 COM17。
- `tools/capture_screenshot.ps1` 实机验证通过：`screenshot_capture` 返回 `.fridge_spirit/exports/shot_102.jpg`，格式 JPEG，读回 101251 bytes，逐块 CRC32 和文件头校验通过，生成 `tmp/cache/screenshot_test/codex_latest.jpg`；图像读取确认是当前 1024×600 首页画面，无花屏或传输损坏。
- Python 脚本支持省略端口（默认 COM17）、无扩展名输出基名自动追加实际格式后缀，并在失败时正确输出 `ok:false`。此前为兼容日志前缀保留 JSON 提取逻辑；摄像头忙时 BMP 降级仍待单独实机复测。
- 相关文档同步维护：`doc/screenshot.md`、`doc/screenshot_quickstart.md`、`doc/screenshot_implementation.md`、`doc/screenshot_implementation_final.md`。
- 本轮备份：`tmp/backups/manual/20260723-175250_codex_screenshot_entry/`、`tmp/backups/manual/20260723-180722_codex_screenshot_verified_docs/`、`tmp/backups/before_run/20260723-180612_codex_screenshot_rts_capture/`。

----
完成时间：2026-07-23 18:21 +08:00

## HOME-01 顶底边界稳定、80px 弹性回弹与时间双缓冲同步

- 根据实屏复现确认，连续滑动卡死集中发生在内容到顶/到底时：LVGL 会在手指仍按住期间反复产生极短的滚动开始/结束。PPA 现把这些边界事件合并为同一手势，只有真实抬手并静止 120ms 后才执行一次双缓冲同步，避免反复恢复 LVGL、整帧同步和 HP WDT 裸复位。
- 修复右侧动态上下文区滚动时，时间数字概率性来回跳变：PPA 接管前不再直接清空 LVGL DIRECT 双缓冲的 `sync_areas`，而是按 LVGL 原生同步方向把所有待更新区域从当前显示缓冲复制到下一写入缓冲，确认成功后再清队列，避免两个全帧缓冲中的时间区域停留在不同秒值。
- HOME 主滚动区新增上下各 80px 的 RGB565 同色弹性缓冲，继续使用 LVGL 9.2.2 原生 `SCROLL_ELASTIC` 阻尼和惯性。PPA 每帧仍只搬运 `704×536`，热路径没有动态分配；页面缓存由 1,163KiB 增至约 1,383KiB，较无弹性缓存额外占用 220KiB PSRAM，较原 48px 方案增加约 88KiB。
- PPA 当前使用 64-byte burst 为持续扫描的 MIPI-DSI/GDMA 保留仲裁窗口，不设置 30fps 软件上限。此前 48px 版本的 COM17 真机往返日志确认弹性路径可正常回位，活动采样约 22～32fps、PPA copy 平均约 18.7ms；80px 版本保持相同热路径，仅扩大缓存可视余量。
- ESP-IDF 5.5.1 完整构建通过，主镜像 `0x2dd720`、SHA-256=`385242D72C4F3562CE2B90407F90A90A0A89C2799F4218EC4216DE3FB5124BDC`；COM17 no-stub app-flash 写入 3,004,416 字节并通过 Hash 校验。启动日志确认 `704×846 + 80px` 缓存为 1,383KiB、PPA 探针 18.5ms，短测无 WDT、Guru、panic、brownout、PPA fallback、待同步失败或显示 underrun；按用户要求未执行 30 分钟长稳，时间显示与回弹手感由用户实屏验收。
- 本轮备份：`tmp/backups/manual/20260723-175930_ppa_elastic_48px/`、`tmp/backups/manual/20260723-182115_ppa_elastic_progress/`、`tmp/backups/manual/20260723-182354_ctx_time_sync_elastic80/`、`tmp/backups/manual/20260723-183027_ctx_time_sync_progress/`、`tmp/backups/before_run/20260723-180058_ppa_elastic_build/`、`tmp/backups/before_run/20260723-180206_ppa_elastic_app_flash/`、`tmp/backups/before_run/20260723-182520_ctx_time_sync_elastic80_build/`、`tmp/backups/before_run/20260723-182612_ctx_time_sync_elastic80_app_flash/`。
- 首页字体层级补充：字体管理器新增 18px/22px Medium 强调角色，使用 TF `NotoSansCJKsc-Medium.otf` 作为主字体，Regular BIN/内置字体作为后备。按 ver2 CSS 的 `font-weight` 逐项映射卡片标题、食材名、到期胶囊、推荐菜名、操作按钮、摘要卡标题、状态舱日期/分区标题/下一事项和语音/拍照文字；元数据、原因说明、同步信息和导航保持 Regular。
- 截屏工具新增可选 `-CaptureDelaySeconds` / `--capture-delay`，用于冷启动后等待 TF 字体与图标异步绑定，再执行视觉回归。实机稳定截图 `tmp/cache/screenshot_test/home_bold_after_stable.bmp` 已与 Playwright 生成的无缩放参考图 `output/playwright/ver2_home_reference_stage_full_1024x600.png` 逐区比对，未发现新增裁切、换行溢出或按钮文字偏移。
- 字体补充后的 ESP-IDF 5.5.1 构建通过，主镜像 `0x2dd760`（3,004,256 字节）、SHA-256=`2F2ACA91919A7CB8533C48B888B9BF41B0608F4210879D3BE824655A25B2641F`；COM17 no-stub app-flash 写入 3,004,416 字节并通过 Hash 校验。既有 `ota_1` 2MiB 容量告警不变。
- 字体补充备份：`tmp/backups/manual/20260723-190417_home_bold_fonts/`、`tmp/backups/manual/20260723-191600_home_bold_progress/`、`tmp/backups/before_run/20260723-190740_home_bold_build/`、`tmp/backups/before_run/20260723-190930_home_bold_app_flash/`、`tmp/backups/before_run/20260723-191214_home_bold_after_capture/`、`tmp/backups/before_run/20260723-191310_home_bold_stable_capture/`。
- 首页几何细修：通用卡片保留原 1px border，并增加贴边 1px outline，视觉边线约为 2px，内部内容坐标不变；首页状态行由 64px 增至 68px，四张统计卡由 104px 增至 112px，后续内容整体下移 8px，修复小字下缘贴线。
- “番茄炒蛋”元数据改为固定两行“15分钟 · 简单 / 库存覆盖 90%”，标签区域增至 `180×56`，保留原字号并完整显示“食材齐全”；PPA 视窗同步改为 `y=68、704×532`，缓存实测为 `704×854 + 80px`，探针 17.9ms。
- 本次 ESP-IDF 5.5.1 构建通过，主镜像 `0x2dd7f0`（3,004,400 字节）、SHA-256=`BE9DA7B0B66B03A1B4FDDEAFC70CABFEA6EC94428B033CCD72D3AF20DB9BA32F`；COM17 no-stub app-flash 写入 3,004,416 字节并通过 Hash 校验。稳定 20 秒后的实机截图 `tmp/cache/screenshot_test/home_geometry_after.bmp` 已确认三处裁切/贴线问题消失；短时日志未见本次启动产生 Guru、WDT、brownout 或 PPA fallback，既有 coredump 分区保留旧转储，未执行 30 分钟长稳。
- 几何细修备份：`tmp/backups/manual/20260723-192339_home_geometry_border_recipe/`、`tmp/backups/manual/20260723-193643_home_geometry_progress/`、`tmp/backups/before_run/20260723-192445_home_geometry_build/`、`tmp/backups/before_run/20260723-192553_home_geometry_app_flash/`、`tmp/backups/before_run/20260723-193419_home_geometry_after_capture/`。
- 首页字重与纵向节奏补充：TF 字体包升级为 `1.1-bold-ui.1`，新增 64,008 字节的 Noto Sans CJK SC Bold UI 子集（SHA-256=`0e704a2af39a84b9f5a0fb133d995c3ed7a3448060b15e701aa3597324e83769`）；字体管理器新增 28/36/48px Strong 角色，分别用于“首页”、四张统计卡数字和右上时间，缺字时回退同字号 Medium。
- HOME-01 增加显式 2px 顶部分隔线，并用统一的 10px 卡片底部安全区对齐末行文字；推荐原因、Agent 正文和最近同步统一使用紧凑行距。推荐菜摘要拆为 `y=43/69/95` 的三条独立单行标签，避免字体行高差异再次造成相邻文字重叠。
- 右上时间调整为 `y=1`，日期保持 `y=12`，星期调整为 `y=35`，时间组合在顶部状态区内重新取得视觉垂直居中。最终实机截图 `tmp/cache/screenshot_test/home_typography_final.bmp` 已确认字重、分隔线、行距和卡片底部留白符合本轮要求。
- ESP-IDF 5.5.1 构建通过，主固件 3,005,248 字节、SHA-256=`520CE563A18986F1F6C13202ECE4394364B86D52D7E48050BA2B19E7EB85F19E`；COM17 no-stub app-flash 写入 3,005,440 字节并通过 Hash 校验。冷启动、资源加载与截屏短测未见 Guru、WDT、brownout 或 PPA fallback；硬件 JPEG 临时内存不足时按设计回退 BMP，既有 `ota_1` 2MiB 容量提示不变，按用户要求未执行 30 分钟长稳。
- 本轮备份：`tmp/backups/manual/20260723-195019_home_typography_spacing/`、`tmp/backups/manual/20260723-195938_recipe_meta_three_lines/`、`tmp/backups/manual/20260723-200516_home_typography_progress/`、`tmp/backups/before_run/20260723-195352_home_typography_build/`、`tmp/backups/before_run/20260723-195453_home_bold_font_tf_deploy/`、`tmp/backups/before_run/20260723-200009_recipe_meta_build/`、`tmp/backups/before_run/20260723-200055_recipe_meta_app_flash/`、`tmp/backups/before_run/20260723-200218_recipe_meta_capture/`。
- 标题纵向间距细修：临期食材卡标题及“今日菜单/购物清单”标题统一上移 4px，增大与下一行正文的视觉间隔；首页顶部区域由 68px 增至 70px，使副标题下方的 2px 分隔线下移 2px。PPA 滚动视窗同步调整为 `y=70、704×530`，保持整屏边界严密匹配。
- 本次 ESP-IDF 5.5.1 构建通过，主固件 3,005,248 字节、SHA-256=`13108CD7DB93E00FD9043DBA045C045180FDCBB300ABC97DBDB01F52270144B7`；COM17 no-stub app-flash 写入 3,005,440 字节并通过 Hash 校验。实机截图 `tmp/cache/screenshot_test/home_title_spacing_final.bmp` 确认临期卡和顶部横线间距正常，启动时 PPA 缓存探针 17.9ms，短测未见 Guru、WDT、brownout 或 PPA fallback，未执行 30 分钟长稳。
- 本轮备份：`tmp/backups/manual/20260723-201306_home_title_vertical_spacing/`、`tmp/backups/manual/20260723-201718_home_title_spacing_progress/`、`tmp/backups/manual/20260723-201759_home_title_spacing_progress_path/`、`tmp/backups/before_run/20260723-201332_home_title_spacing_build/`、`tmp/backups/before_run/20260723-201410_home_title_spacing_app_flash/`、`tmp/backups/before_run/20260723-201520_home_title_spacing_capture/`。
- 上一条标题间距方案按实屏反馈纠正：临期食材标题恢复 `y=4`、今日菜单/购物清单标题恢复 `y=6`，标题本身不再移动；对应卡片分别增高到 76px/80px，使正文下移 4px且继续保留既定底部安全区。四张统计卡增高到 116px，数字坐标保持不动，底部“库存食材”等标签随安全区下移 4px。
- 首页顶部状态栏按要求由 70px 增至 80px，PPA 视窗同步调整为 `y=80、704×520`。右侧动态上下文区为两行晚餐摘要增加 12px 独立间隔，后续“下一事项/最近同步”顺延；摘要值与最近同步正文增加右侧 4px 安全边距，动态视窗增高到 376px、底部按钮下移到 `y=528`，避免文字在右边缘或初始视窗底边被裁切。
- ESP-IDF 5.5.1 完整构建通过，主固件 3,005,248 字节、SHA-256=`B822B32541436A01A6A9BDEC8FB3D65888EB59FFD4ED134C2B84CC65A7EC2D3D`；COM17 no-stub app-flash 写入 3,005,440 字节并通过 Hash 校验。冷启动截图 `tmp/cache/screenshot_test/home_spacing_correction_final.bmp` 确认标题位置保持、正文和统计标签下移、80px 顶栏及右侧动态区裁切修复生效；PPA 缓存探针 17.4ms，短测未见 Guru、WDT、brownout 或 PPA fallback。既有 `ota_1` 2MiB 容量提示不变，未执行 30 分钟长稳。
- 本轮备份：`tmp/backups/manual/20260723-202951_home_spacing_correction_ctx_clip/`、`tmp/backups/manual/20260723-203049_ctx_text_safe_bounds/`、`tmp/backups/manual/20260723-203256_home_spacing_correction_progress/`、`tmp/backups/manual/20260723-203336_home_spacing_progress_path/`、`tmp/backups/manual/20260723-203829_home_spacing_flash_result_progress/`、`tmp/backups/before_run/20260723-203117_home_spacing_correction_build/`、`tmp/backups/before_run/20260723-203152_home_spacing_correction_app_flash/`、`tmp/backups/before_run/20260723-203524_home_spacing_correction_flash_test/`、`tmp/backups/before_run/20260723-203636_home_spacing_correction_capture_test/`。

----

## 2026-07-23 更多页像素级布局修复

- 修复 `components/ui_p4/fridge_ui_shell.c` 的 MORE-01：移除当前固件多出的“更多 · 工具 / 时间工具”二级大标题与纵向大卡，按 ver2 参考稿改为两列入口网格、48px 圆角图标、22px Medium 卡片名、18px Regular 描述和底部前进图标。
- 更多页与 HOME-01 复用同一个主滚动容器；隐藏首页内容后在该容器内挂载 MORE-01，切页时使 PPA 缓存失效，避免滚动加速器继续使用首页快照。一级更多页隐藏返回/搜索/加号，定时器、闹钟、秒表子页仍保留返回路径。
- 设备摘要改为参考稿的单行状态摘要，动态显示设备状态、Wi-Fi 和 TF 资源状态；计时业务与 `kitchen_tools` 回调未改造。
- ESP-IDF 5.5.1 构建、COM17 no-stub 刷写和 `tools/capture_screenshot.ps1` 冷启动截屏通过；启动日志未见本轮新增 WDT、Guru、panic、brownout 或 PSRAM 失败。既有 `ota_1` 2MiB 容量告警保持不变。
- UI 资源回归测试 `python -m unittest test_build_ver2_pack`（在 `tools/ui_assets` 目录执行）9/9 通过。标准截屏当前从首页启动，固件协议未提供程序化点击导航命令，因此本轮未能自动取得 More 页实屏截图；需用户在屏上点击“更多”后再按固定截屏流程做最终视觉确认。
- 本轮备份：`tmp/backups/manual/20260723-203716_p4_more_pixel_match/`、`tmp/backups/before_run/20260723-204052_p4_more_pixel_match_build/`、`tmp/backups/before_run/20260723-204145_p4_more_pixel_match_flash/`。

----

## 2026-07-23 定时器/闹钟/秒表像素对齐与更多页占位入口

- 三个时间工具页统一挂入 HOME-01 使用的主滚动区，移除固件私加的二级大标题，按 ver2 body 起点、卡片间距、64px 大数字、64px 触控按钮和 96px 闹钟卡片重新排布；计时后端能力和回调未改变。
- MORE-01 恢复参考稿“系统与维护”三张入口卡（设备与厨房安全、设置中心、维护中心）作为占位，点击只提示暂未移植，不伪造未实现页面。
- ESP-IDF 5.5.1 构建无新增编译警告；COM17 no-stub 刷写和固定截屏流程通过，冷启动确认 `PPA cache ready 704x854 RGB565 + 80px elastic`，未见本轮新增 WDT、Guru、panic、brownout 或 PSRAM 失败。既有 `ota_1` 2MiB 容量告警保持不变。
- 本轮备份：`tmp/backups/manual/20260723-211312_more_tool_pixel_alignment/`、`tmp/backups/manual/20260723-2120_more_tool_remove_unused/`、`tmp/backups/manual/20260723-2128_more_tool_pixel_progress/`、`tmp/backups/manual/20260723-2131_more_placeholder_icons/`、`tmp/backups/manual/20260723-2136_more_placeholder_progress_update/`、`tmp/backups/before_run/20260723-2118_more_tool_pixel_build/`、`tmp/backups/before_run/20260723-2121_more_tool_pixel_build2/`、`tmp/backups/before_run/20260723-2123_more_tool_pixel_flash/`、`tmp/backups/before_run/20260723-2132_more_placeholder_icons_build/`、`tmp/backups/before_run/20260723-2134_more_placeholder_icons_flash/`。

----

## 2026-07-23 滚动按钮误触保护

- `components/ui_p4/fridge_ui_shell.c` 为 MORE-01、定时器、闹钟、秒表所有主滚动区内回调增加拖动/滚动结束 300ms 点击抑制，手指从按钮上拖动不再触发页面跳转或业务操作。
- 规则同步写入 `AGENTS.md`，后续 LVGL 主滚动区控件必须遵守该保护；固定顶栏和导航栏保持独立点击。
- ESP-IDF 5.5.1 构建、COM17 no-stub 刷写及固定截屏流程通过；启动日志确认 PPA `cache ready`，未见本轮新增 WDT、Guru、panic、brownout 或 PSRAM 失败。
- 本轮备份：`tmp/backups/manual/20260723-215303_scroll_click_guard/`、`tmp/backups/manual/20260723-2245_scroll_click_guard_progress/`、`tmp/backups/before_run/20260723-2238_scroll_click_guard_build/`、`tmp/backups/before_run/20260723-2240_scroll_click_guard_flash/`。

补充修正：上一版仅依赖滚动结束时间窗，实机仍可能在按下按钮拖动时提前触发 `CLICKED`，已撤销该判断，改为控件 `PRESSED/PRESSING` 记录触摸位移（8px 阈值）后抑制本次点击。新固件已重新构建、刷写并完成冷启动截屏。

----

## 2026-07-24 P4 UI 异步页面 Tool 接入

- `ui_p4` 新增线程安全、非阻塞的 `fridge_ui_p4_request_page_async()`；USB/Tool 任务只向 FreeRTOS 队列投递页面枚举，LVGL 页面对象统一由 `ui_p4` 任务消费并更新，支持启动首帧完成前排队。
- 新增 `ui.switch_page` Tool，注册到现有 Tool Broker，按 READ 权限处理；通过既有 USB `tool_call` 命令接收 `page` 参数，支持 `home`、`more`、`timer`、`alarm`、`stopwatch`。
- `components/tool_system` 增加对 `ui_p4` 的组件依赖；`doc/agent/TOOLS.md` 补充 USB JSON Lines 调用示例。
- ESP-IDF 5.5.1 P4 完整构建通过并生成 `build/fridge_spirit.bin`；既有 `ota_1` 2MiB 分区容量告警保持不变，未执行烧录和实机页面验收。
- 本轮备份：`tmp/backups/manual/20260724-011646_ui_switch_page_tool/`、`tmp/backups/manual/20260724-012103_ui_switch_page_tool_enum_fix/`、`tmp/backups/manual/20260724-012424_ui_switch_page_docs/`、`tmp/backups/before_run/20260724-011953_build_ui_switch_page_tool/`、`tmp/backups/before_run/20260724-012123_build_ui_switch_page_tool_enum_fix/`。

----

## 2026-07-24 TF 卡诊断日志与 coredump 导出

- 新增 `components/diagnostics_log/`：最早期安装全局日志 tee，INFO 及以上日志进入 8KiB RAM 环形缓冲；TF 挂载后由低优先级任务批量写入 `.fridge_spirit/logs/diag_0.log`～`diag_3.log`，每个 256KiB，轮转总容量约 1MiB，并统计丢失记录/写入错误。
- 新增受保护的 `.fridge_spirit/diagnostics/` 导出链路：Flash ELF coredump 先校验，再以 4KiB 流式缓冲原子写入 ELF 和 metadata；只有两者同步成功后才擦除 Flash。普通 TF/HTTP/USB 浏览和专家模式拒绝访问该目录。
- `fridge_diagnostics` 将启动序号、复位原因、brownout、panic、Task-WDT、Interrupt-WDT 和计划重启原因持久化到 NVS；`get_diagnostics` 仅返回状态/计数，不回传日志正文或 coredump。
- 修复 coredump metadata 的 `snprintf` 截断边界：截断或路径溢出时返回错误，避免把“应写长度”误当作缓冲区实际长度而从栈外读取；损坏 coredump 不再被误判为空分区。
- `sdkconfig`/`sdkconfig.defaults` 已启用 Flash ELF coredump、Task-WDT panic 和 768KiB coredump 分区；Flash 尾部保留 64KiB。
- 直接使用 ESP-IDF 5.5.1 工具链构建目录 `build_diag_direct/` 已完成链接，主镜像约 2.88MiB；通用分区检查提示主镜像大于 2MiB recovery 槽，需继续使用 `FRIDGE_RECOVERY_BUILD=ON` 单独构建 recovery 镜像确认其容量。项目包装脚本暂被损坏的 Python 3.14 虚拟环境拦截。
- 尚未烧录，也未完成 TF 实机持续写入、WDT/panic/brownout 注入、断电一致性和 coredump 解码测试。详细使用说明见 `doc/诊断日志与TF卡导出.md`。
- 本轮备份：`tmp/backups/manual/20260724-152437_tfcard_diagnostics/`、`tmp/backups/manual/20260724-153158_tfcard_diagnostics_continue/`、`tmp/backups/manual/20260724-161805_tfcard_diagnostics_finish/`、`tmp/backups/before_run/20260724-154326_tfcard_diagnostics_build/`。

----

## 2026-07-24 秒表 Agent Tools 注册

- 在 `components/tool_system` 注册 `stopwatch.status`、`stopwatch.start`、`stopwatch.pause`、`stopwatch.lap`、`stopwatch.reset`；`ui.switch_page` 继续作为页面导航工具保留。
- 秒表 Tool 直接复用 `kitchen_tools` 的毫秒级快照和操作 API，不访问 LVGL；开始/暂停为状态感知幂等操作，计次增加最近 32 条运行态幂等键缓存，复位时清理缓存。
- `stopwatch.reset` 按 DANGEROUS 处理，必须显式确认；`stopwatch.lap` 返回 `elapsedMs`、`lapCount`、`lapsMs` 和 `droppedOldest`，达到 20 条计次后明确报告丢弃最旧记录。
- 更新 `doc/agent/TOOLS.md` 和 `doc/agent/AGENT_ARCH.md`，记录权限、参数、毫秒字段、重试规则、运行态不持久化边界及旧 AI JSON 兼容层边界。
- ESP-IDF 5.5.1 项目构建通过，`build/fridge_spirit.bin` 生成成功；仍保留既有 `ota_1` 2MiB recovery 分区容量告警。本轮未烧录，尚未做 COM17 上的 Tool Broker 真机回归。
- 本轮备份：`tmp/backups/manual/20260724-181406_stopwatch_agent_tools/`、`tmp/backups/before_run/20260724-181849_stopwatch_agent_tools_build/`、`tmp/backups/manual/20260724-182031_stopwatch_lap_cache_reuse_fix/`、`tmp/backups/before_run/20260724-182056_stopwatch_lap_cache_reuse_build/`、`tmp/backups/manual/20260724-182156_stopwatch_agent_tools_progress_finalize/`。

----

## 2026-07-24 秒表页两个视觉重叠修复

- 保持字体、计时逻辑和计次逻辑不变，仅将顶部 `MORE-08` 胶囊宽度由 66px 调整为 78px，并同步扩大内部文字区域，修复标题贴边/裁切。
- 开始按钮在 A8 播放图标资源存在时隐藏文本回退图标；资源缺失时才显示回退字符，避免播放图标重复绘制造成重叠。
- ESP-IDF 5.5.1 构建通过，已使用 COM17 完成 no-stub 刷写；固定截图流程切换到秒表页并成功读回 `tmp/cache/screenshot_test/stopwatch_overlap_final.bmp`，实屏确认胶囊文字完整、开始按钮无图标重叠。
- 截图过程中硬件 JPEG 因临时内存不足按既有设计回退 BMP；日志中的 PPA fallback、I2C NACK 和 USB 任务栈余量告警均为已有运行现象，本次未改动相关逻辑。
- 本轮备份：`tmp/backups/manual/20260724-174516_stopwatch_digit_font/`、`tmp/backups/before_run/20260724-175406_stopwatch_overlap_build/`、`tmp/backups/before_run/20260724-175458_stopwatch_overlap_flash/`、`tmp/backups/manual/20260724-180412_stopwatch_overlap_visual_verify/`。

----

## 2026-07-24 PPA 视口尺寸与 HP WDT 诊断修正

- 现场 COM17 日志确认设备重复发生 `HP_SYS_HP_WDT_RESET`；启动诊断计数为 `resetReason=watchdog`、`brownoutCount=0`、`taskWdtCount=0`、`interruptWdtCount=0`。
- 根因首要嫌疑为 UI 主滚动区已改为 `y=87、h=513`，但 PPA 仍使用旧的 `y=75、h=525`，导致 `rebuild_cache()` 持续返回 `ESP_ERR_INVALID_SIZE`，设备长期退回 LVGL 全量重绘路径。
- 已将 `components/ui_p4/fridge_ui_scroll_ppa.c` 的 PPA 视口同步为 `PPA_SCROLL_Y=87`、`PPA_SCROLL_H=513`，并更新几何注释；未改动其他 UI 或硬件参数。
- ESP-IDF 5.5.1 固定包装脚本构建通过，生成 `build/fridge_spirit.elf` 与 `build/fridge_spirit.bin`。构建仍提示现有 `ota_1` recovery 分区小于主固件，这是既有分区布局问题，未在本轮擅自调整。
- SHT40 启动时可探测到 `0x44`，运行中约每 5 秒出现一次 I2C NACK，作为独立的共享 I2C/传感器异常保留后续排查；本轮未修改 SHT40。
- 尚未重新连接或烧录 COM17，因此 PPA cache 成功建立和 WDT 是否消失仍需实机验证。备份：`tmp/backups/manual/20260724-182401_ppa_viewport_fix/`、`tmp/backups/before_run/20260724-182436_ppa_viewport_build/`、`tmp/backups/manual/20260724-182647_ppa_wdt_fix_progress/`。
- 后续已使用 COM17 no-stub 烧录并完成约 5 分钟只读监听：启动为 `boot #309 reset=usb`，PPA 连续输出 `cache ready 704x854 RGB565`，`ESP_ERR_INVALID_SIZE`、HP WDT 和 brownout 均为 0；SHT40 仍出现 1 次独立 I2C NACK，未导致重启。日志：`tmp/logs/live_COM17_ppa_fix_20260724-1829.log`、`tmp/logs/live_COM17_ppa_fix_stability_20260724-1832.log`。烧录前备份：`tmp/backups/before_run/20260724-182906_ppa_viewport_flash/`；本次进度备份：`tmp/backups/manual/20260724-183717_ppa_flash_test_progress/`。

----
完成时间：2026-07-24 19:23 +08:00

## 秒表连续计次触底滚动 panic 修复

- COM17 30 秒复现日志确认：连续计次并滑动到触底时发生 panic 复位，日志中的 ESP 时间戳由 `422151 ms` 回退到 `232306 ms`；`get_diagnostics` 对应 `resetReason=panic`、`bootSequence=311`、`panicCount=270`，`brownout=0`、`taskWdt=0`。
- 根因是 PPA 滚动缓存原本同时覆盖动态工具页；秒表计次数量变化后旧缓存未及时失效，20 条计次可能继续显示旧的 6 条缓存，触底时滚动偏移还可能超出旧缓存边界。秒表每 5ms 重复设置列表和页面高度也会持续触发布局与滚动边界重算。
- 已将 PPA 限定为 HOME 静态滚动页，秒表/定时器等动态页恢复标准 LVGL 滚动；增加 PPA 输入缓存边界检查；秒表列表高度仅在实际变化时更新；计次数量变化时主动使缓存失效。
- 已使用 ESP-IDF 5.5.1 构建通过，并用 COM17 no-stub 成功烧录、Hash 校验通过。既有 `ota_1` 2MiB recovery 分区容量不足告警未改动；panic coredump 已导出到 `.fridge_spirit/diagnostics/coredump-311.elf`，因 TF 安全策略暂未解码。
- 待用户在新固件上完成 20 条计次、上下滑动及反复触底回归；需再次比较 `bootSequence`、`panicCount`、`taskWdtCount` 和日志中的 LVGL/PPA 状态。复现日志：`tmp/logs/20260724-184216_stopwatch_lap_scroll_30s.log`。
- 备份：`tmp/backups/manual/20260724-185443_stopwatch_lap_scroll_panic/`、`tmp/backups/before_run/20260724-185746_stopwatch_scroll_build/`、`tmp/backups/manual/20260724-190000_stopwatch_lap_scroll_progress/`。

----

## 2026-07-24 秒表滚动 PPA 热路径优化

- 在现有 PPA 滚动缓存基础上，将 P4 SRM/Blend 数据突发长度从 64B 提升为已验证驱动支持的 128B，减少 PSRAM 事务次数；秒表滚动期间的动态覆盖由整张 `704x234` 主卡缩小为毫秒数字标签，开始/暂停状态变化时主动使静态缓存失效并在空闲期重建。
- ESP-IDF 5.5.1 构建通过，COM17 no-stub 刷写成功；启动日志显示 PPA HOME 探针约 `15.6ms`，较此前约 `17.8–19.2ms` 降低。固定截屏读回成功，秒表页无花屏/空屏：`tmp/cache/screenshot_test/ppa_stopwatch_idle_check.bmp`。
- 已保留 PPA 失败时回退 LVGL 的安全路径。两轮人工触摸监听未收到实际触摸事件，因此尚未取得 `gesture end` / `PPA scroll` 的 20 条计次帧率数据，现场滑动流畅度仍需用户在新固件上完成最终确认。
- 一次自动化测试连续提交 21 个 WRITE 工具调用，但 Tool Broker 处于 `confirm` 模式，调用均未执行；该测试堆积待确认请求并触发一次 `HP_SYS_HP_WDT_RESET`，不作为 PPA 滚动根因。其后诊断为 `brownout=0`、`task_wdt=0`、`interrupt_wdt=0`；后续测试必须避免批量堆积待确认请求。
- 既有 `ota_1` 2MiB recovery 容量告警、截图 JPEG 内存不足回退 BMP、USB 任务栈余量 488B 和偶发 SHT40 I2C NACK 未在本轮改动。
- 本轮备份：`tmp/backups/manual/20260724-202413_stopwatch_ppa_burst_overlay/`、`tmp/backups/before_run/20260724-202513_stopwatch_ppa_burst_overlay_build/`。
- 开机显示补充：`components/ui_p4/fridge_ui_p4.c` 在现有暖米白硬件首帧和正常 Shell 上增加纯 LVGL `Fridge Spirit` 遮罩；112×132px 冰箱精灵以约 120ms 周期上下漂浮，三个圆点分级呼吸。遮罩不新增图片、字体、任务、定时器或公共 API，`fridge_ui_p4_is_ready()` 时序及 main/DSI/背光/TF 初始化顺序均未改变。
- 遮罩仅在 ver2 图标/JSON 原子绑定、TF BIN 字体完整加载、12/12 FreeType 角色创建及全局样式重绑后删除；TF 未判定时持续显示，明确失败后保留 5 秒资源重试窗口，超时才显示现有 Flash 降级首页，后台重试继续且不重新显示遮罩。`TF mutex timeout` 按挂载进行中的瞬态状态处理，避免慢卡被提前降级。
- ESP-IDF 5.5.1 完整构建和 UI 资源包 9 项单元测试通过；`fridge_ui_p4.c.obj` Flash 由 6972B 增至 9072B（+2100B），`fridge_spirit.bin` 由 3037344B 增至 3040096B（+2752B），未新增二进制资产。既有 `ota_1` 容量告警保持不变。
- COM17 no-stub 主应用烧录及 Hash 校验通过；正常 TF 启动日志确认遮罩显示后加载 ver2 `2.0.0-more.1` 资源、12/12 FreeType 角色，并在样式重绑完成后以 `mode=tf_resources_ready` 立即删除，实测遮罩约 14.9～15.1 秒，无额外固定延时。启动期未见 panic、brownout、PSRAM 或 DSI 错误；偶发 SHT40 I2C NACK 仍为既有问题。
- 正常首页截图回归连续两次被既有 `HP_SYS_HP_WDT_RESET` 打断，PPA RTC 追踪均报告 stage=6；因此本轮未把截图标记为通过，也未在该不稳定状态下移动 TF manifest 做失败注入，避免复位期间遗留改名资源。该 PPA/WDT 路径未在本轮修改，需独立排查后补做截图与 5 秒降级真机验收。
- 本轮备份：`tmp/backups/manual/20260724-231952_boot_animation/`、`tmp/backups/manual/20260724-233457_boot_animation_review_fixes/`、`tmp/backups/before_run/20260724-231952_boot_animation_baseline/`、`tmp/backups/before_run/20260724-232922_boot_animation_build/`、`tmp/backups/before_run/20260724-233307_boot_animation_flash/`、`tmp/backups/before_run/20260724-233545_boot_animation_review_build/`、`tmp/backups/before_run/20260724-233700_boot_animation_final_flash/`。
- 实屏反馈修正：首版遮罩使用 LVGL 特殊顶层，删除时与 DIRECT 双缓冲/PPA 会话交错，导致只恢复主滚动视口，固定顶栏、中间导航和右侧状态舱仍被旧帧覆盖。遮罩已改为活动屏幕内的普通最前对象；仅在 PPA 空闲时揭示，并在删除后强制完整 Shell 重绘、整屏失效和 PPA 缓存重建。
- 修正版已再次使用 ESP-IDF 5.5.1 构建并经 COM17 no-stub 烧录、Hash 校验通过；启动日志确认遮罩显示、ver2 资源和 12/12 FreeType 角色加载、全局字体重绑、遮罩删除及后续 PPA cache 重建顺序正常，启动诊断为 `reset=usb brownout=0 task_wdt=0 int_wdt=0`。等待用户直接观察修正版实屏确认固定区域与动画表现；备份：`tmp/backups/manual/20260724-234652_boot_overlay_reveal_fix/`、`tmp/backups/before_run/20260724-234746_boot_overlay_reveal_build/`、`tmp/backups/before_run/20260724-234829_boot_overlay_reveal_flash/`。
- 最终处理：按用户决定撤销开机动画，`components/ui_p4/fridge_ui_p4.c` 已恢复为本任务开始前版本并通过 SHA-256 逐字节校验；不再创建遮罩、动画对象或独立 boot screen，仅保留原有暖米白硬件首帧和既有首页加载流程。ESP-IDF 5.5.1 重新构建、COM17 no-stub 烧录及写入 Hash 校验通过；回滚基线标准截图 `tmp/cache/screenshot_test/boot_animation_reverted_baseline.bmp` 已确认顶部导航、中间导航和右侧状态舱完整恢复。备份：`tmp/backups/manual/20260725-000942_remove_boot_animation/`、`tmp/backups/before_run/20260725-001123_remove_boot_animation_build/`、`tmp/backups/before_run/20260725-001159_remove_boot_animation_flash/`。

----

## 2026-07-24 TF 资源恢复、更多页 PPA 与秒表稳定性修复

- 修正一次错误配置回退：`sdkconfig.defaults.esp32p4` 明确选择 ESP32-C6，生成配置恢复 `esp_hosted` 的 SDIO Slot 1、4-bit、40MHz、GPIO47～52、RESET54 和 LDO4，不再误选 ESP32-H2/SPI。实机启动确认识别 `esp32c6`、SDIO transport active；TF 字体包、33 个图标、12/12 FreeType 角色、21 个 Agent Tool 和截屏 TF 读回均恢复正常。
- MORE-01 重新纳入 PPA 加速范围，并统一首次创建和异步资源重建后的内容高度为 790px。实机日志确认更多页建立 `704x802 RGB565 + 80px elastic` 缓存，最终截图 `tmp/cache/screenshot_test/more_ppa_chunked_final.bmp` 的字体、图标和两列卡片布局正常。
- 秒表大数字增加 620x84 的不透明卡片同色覆盖区，PPA 动态 Blend 改为覆盖该区域，不再把透明数字标签直接叠在旧静态快照上，消除拖动时新旧大字残影。20 条计次最终截图 `tmp/cache/screenshot_test/stopwatch_20laps_chunked_final.bmp` 显示 `计次（20）`，大数字和计次行无重叠。
- 修复秒表时间双重累计：`kitchen_tools` 快照已按单调时钟返回当前 `elapsed_ms`，UI 不再额外叠加 `now-started_at`。最终 5 秒主机对比为设备 5140ms、主机 5146ms，比例 0.998834，误差约 6ms；开始、暂停和 20 条计次仍保持毫秒字段。
- 连续计次的整页 PPA 缓存采用独立 2500ms 稳定窗口，不再拖慢更多页首次缓存；超过一个视窗的大页面改为按视窗高度分段栅格化，段间让出一个 FreeRTOS tick，避免大块 FreeType/PSRAM 快照长时间占住 UI 核。20 次快速计次后仅合并为一次 `704x1274` 缓存重建，继续监听 35 秒未出现 WDT、Guru 或 panic；`bootSequence=368`、`watchdogCount=40`、`panicCount=277` 前后不变。
- 修复压力测试暴露的两个独立栈问题：计划重启任务由 2KiB 增至 4KiB；USB JSON Lines 任务由 8KiB 增至 12KiB，避免 20 条计次 JSON 进入 newlib 格式化时触发栈保护。连续 20 个已确认 Tool 调用全部返回 1～20 的计次数量。
- ESP-IDF 5.5.1 构建通过，主镜像 `0x2e59d0`；COM17 no-stub 烧录、Hash 校验、C6/SDIO 启动、TF BMP 截屏与 1,228,866 字节 CRC32 分块读回通过。既有 `ota_1` 2MiB recovery 分区容量不足告警未改动；最终手指连续拖动和触底手感仍由用户实屏确认。
- 关键日志：`tmp/logs/20260724-232400_stopwatch_chunked_20laps_final.log`、`tmp/logs/20260724-233000_more_ppa_timing_final.log`、`tmp/logs/20260724-233400_stopwatch_timing_final.log`。关键备份：`tmp/backups/manual/20260724-224915_c6_sdio_more_stopwatch_fixes/`、`tmp/backups/manual/20260724-230148_usb_tool_lap_stack_fix/`、`tmp/backups/manual/20260724-232003_stopwatch_cache_wdt_fix/`、`tmp/backups/before_run/20260724-232119_stopwatch_chunked_cache_build/`、`tmp/backups/before_run/20260724-232159_stopwatch_chunked_cache_flash/`。
- 2026-07-25 固定区图标回退：用户实屏确认 `20260724-225351` 归档镜像的顶部按钮、导航栏和状态舱 TF 图标正确；设备已只写回该应用镜像（SHA256 `3954297C1D97C7667D29EDACD3D31E820FDE51EFC39F13470B4DC60BB562781C`），未改 TF、分区表或模型。工作区 `fridge_ui_p4.c`、Shell 与 PPA `.c/.h` 已分别按对应旧构建备份逐字节恢复并完成 ESP-IDF 5.5.1 构建；这也撤销了后续秒表 2500ms 分段快照优化，20 条计次压力场景需重新回归后再决定如何无回归地移植优化。
- 2026-07-25 秒表偶发蓝屏保护放宽：P4 双核 Interrupt-WDT 由 800ms 调整为 1600ms，双 Idle Task-WDT 由 5s 调整为 8s；双核检测、panic 和 Flash coredump 均继续保留，避免直接关闭保护而丢失真正死锁现场。
- 修复 PPA 手势结束后的 DIRECT 双缓冲交接：根据下一可写缓冲与 LVGL active 缓冲的奇偶关系，补齐 1 或 2 次最终滚动位置提交，确保两块帧缓冲内容一致且下一写入仍回到 LVGL active；避免随后毫秒数字局部刷新带出旧滚动帧，造成计次内容偶发上移跳动。
- ESP-IDF 5.5.1 构建、COM17 no-stub 烧录及写入 Hash 校验通过；实机启动和页面切换前后 `bootSequence=416`、`panicCount=290`、`watchdogCount=46` 均未增加，`taskWdtCount=0`、`interruptWdtCount=0`。字体延迟加载阶段的快速切页仅出现页面请求队列主动拒绝，未发生 WDT/panic；日志：`tmp/logs/stopwatch_page_stress_20260725-013000.log`。

----

## 2026-07-25 闹钟管理/编辑双页与 PPA 滚动接入

- 将原单页固定闹钟改为 `MORE-07` 管理页和 `MORE-03` 编辑页：管理页按“闹钟/周期提醒”分组展示 96px 卡片，卡片主体进入详情，右侧独立开关支持启停；编辑页覆盖类型、时刻、每天/工作日/每周日/自定义重复日、名称、铃声/语音播报、播报文字、预览、删除确认、保存与保存并启用，不包含小程序同步。
- `kitchen_tools` 的本地 JSON 持久化升级到 schema v2，保存类型、重复日掩码、声音、来源和语音文本；兼容旧数据的每天固定铃声默认值。到点铃声复用板载 ES8311 本地提示音，语音播报复用既有 TTS 链路，不增加 GPIO、供电或 I2C 访问。
- 管理页和编辑页均重新启用 PPA：管理页使用静态滚动快照；编辑页把固定底栏作为 PPA 动态覆盖层混合回屏。闹钟列表、开关或到点状态变化会主动使缓存失效，避免滚动时出现旧开关或旧卡片。
- ESP-IDF 5.5.1 构建通过并用 COM17 no-stub 成功写入、Hash 校验通过。既有 `ota_1` 2MiB recovery 分区小于约 2.93MiB 主固件的告警未改动。实机单会话完成 `ui.switch_page(alarm)`、BMP 截屏、TF 1,228,866 字节 CRC32 分块读回，画面保存在 `tmp/cache/screenshot_test/alarm_management_ppa.bmp`；JPEG 编码器内存不足时按既有降级路径使用 BMP。
- 启动/截屏过程中仍见既有 SHT40 I2C NACK，未改动共享 I2C 硬件配置；PPA 手势帧率需用户在该固件上实际连续拖动闹钟管理与编辑长表单后最终确认。
- 本轮备份：`tmp/backups/manual/20260725-013100_alarm_management_expansion/`、`tmp/backups/before_run/20260725-020800_alarm_management_build/`、`tmp/backups/before_run/20260725-021000_alarm_management_flash/`、`tmp/backups/manual/20260725-022000_alarm_ppa_enable/`、`tmp/backups/before_run/20260725-022300_alarm_ppa_build/`、`tmp/backups/before_run/20260725-022400_alarm_ppa_flash/`、`tmp/backups/manual/20260725-023000_alarm_management_progress/`、`tmp/backups/manual/20260725-023100_alarm_progress_order_fix/`。
- 触摸交互补充：修正 GT911 将 `0x814E READY=0` 误当抬手的问题，无新数据时保持上一次按压，只有“数据就绪且触点数为 0”才释放；P4 主内容区和状态舱动态上下文滚动区统一采用 8px 移动取消、`PRESS_LOST/SCROLL_BEGIN/SCROLL` 永久取消，正常抬手后立即执行，不再使用更多页 220ms 和闹钟 260ms 固定等待。ESP-IDF 5.5.1 构建、COM17 no-stub 烧录和写入 Hash 校验通过；启动保持同一串口会话等待 15 秒后收到 `ready`，`touch_status` 返回 GT911 0x14 / product 911 / `ESP_OK`，未见本次启动新增 panic、WDT 或 brownout；最终按住按钮拖动与点击手感待用户实屏确认。备份：`tmp/backups/manual/20260725-102516_mobile_scroll_touch/`、`tmp/backups/before_run/20260725-103019_mobile_scroll_touch_build/`、`tmp/backups/before_run/20260725-103208_mobile_scroll_touch_flash/`、`tmp/backups/manual/20260725-103531_mobile_scroll_touch_progress/`。

----

## 2026-07-25 P4 单会话 Agent 对话页重写

- 新增 `agent_runtime` 单 Worker 异步运行时：唯一 `foreground` 会话持久化到 TF `agent/data/sessions.json`，保留最近 48 条记录、页面恢复最近 24 条、云模型上下文取最近 12 轮文本；`active_run.json` 可恢复确认卡，执行中的 Run 重启后明确取消且不重放。
- 文本入口改为本地确定性 Router 优先、再走已保存 `fridge_ai` 配置的 OpenAI-compatible/Google 链路；`agent_structured` 会显式请求 OpenAI JSON object / Gemini `application/json`，并严格只接受 `reply`、可选 `spoken_summary`、至多一个 `tool_call`。无效 JSON 安全降级为普通文本，不执行工具；所有文本会话的 WRITE/DANGEROUS 工具均先显示确认卡，再经 Tool Broker 校验和执行。
- CHAT-01 改为唯一 Agent 会话页：真实 `lv_textarea`、复用完整拼音 IME、键盘展开时输入区上移/关闭或发送时归位、快捷指令直发、发送/停止共用按钮槽位、工具确认/拒绝卡和 8px 滑动取消保护；旧 CHAT-02/03/04 仅兼容映射回 CHAT-01，不再创建多会话入口。
- 新增 ES8311 16-bit/16 kHz 单向 TTS 适配，复用 `fridge_tts` NVS 和 MOSS/SiliconFlow 请求约定而不进入 S3/MAX98357A 路径；TTS 独立于 Agent Worker，持有 AUDIO gate，停止时先关闭 `PA_CTRL`。合成/播放期间“停止”仍可立即取消，收尾未完成时禁止并行写 DAC。
- 解开 `ui_p4 → tool_system → ui_p4` 循环：`ui.switch_page` 注册移至 `ui_p4`，main 在 Broker 就绪后注册；Runtime 仅以非阻塞事件桥通知 UI，网络、存储、TTS 与 Tool 任务不访问 LVGL。
- ESP-IDF 5.5.1 P4 完整构建和 `idf.py size` 通过：总镜像 `3,137,428` bytes，DIRAM `235,226 / 576,464` bytes（40.8%）。既有完整主镜像大于 2MiB `ota_1` recovery 槽的告警保持不变，未烧录。实机 IME、TF 重启恢复、工具确认、取消、共享 I2C 忙、TTS 播放/停止和无 WDT 回归仍需在用户确认刷写后按 `ready` 流程验证。
- 本轮备份：`tmp/backups/manual/20260725-183653_agent_chat_rewrite/`、`tmp/backups/manual/20260725-190840_agent_chat_final_validation/`、`tmp/backups/manual/20260725-191411_agent_chat_compile_fix/`、`tmp/backups/manual/20260725-191757_agent_tts_stop_state/`、`tmp/backups/manual/20260725-192102_agent_chat_progress/`、`tmp/backups/manual/20260725-192153_agent_chat_progress_order/`、`tmp/backups/manual/20260725-192543_agent_structured_protocol/`、`tmp/backups/manual/20260725-192729_agent_chat_size_progress/`、`tmp/backups/before_run/20260725-191050_agent_chat_build/`。
- 后续状态修正：`AGENT_RECORD_RESULT` 不再被错误映射为工具 UI 事件，避免其默认 `PENDING` 覆盖已完成工具卡；工具离开待确认阶段后同步清除确认标记。备份：`tmp/backups/manual/20260725-231213_agent_tool_card_state/`。

----

## 2026-07-25 Codex/Web 截屏稳定门与固定入口

- 固化项目根目录唯一入口 `tools\capture_screenshot.ps1`，转发到 `tmp\tools\test_screenshot.py`；修正 PowerShell 项目根路径计算，支持 `-Page`、ready 超时、资源稳定超时和输出基名参数。
- Codex 链路改为硬性 `ready` → 顶层 `tool_call(ui.switch_page)` → `ui_status` 稳定门 → 单次 `screenshot_capture` → TF 分块 CRC32 读回；不再依赖固定 8 秒等待，也不会在 ready 超时后继续截图。
- 固件新增 `ui_status` 四项状态：资源/字体完成、双缓冲同步和页面首帧；截屏前拒绝未稳定 UI。JPEG 内部 DMA 连续空间不足时主动 BMP 降级，并修正降级日志含义。
- Web 总览页截屏按钮与 Codex 使用相同 `ui_status` 连续两次稳定门；Mock 传输同步支持 `ui_status`，保留首页截屏、TF 读回、CRC32、预览和下载回归。
- 已同步更新 `AGENTS.md`、`doc/screenshot.md`、`doc/screenshot_quickstart.md`、`doc/screenshot_implementation.md` 和 `doc/screenshot_implementation_final.md`。本轮代码构建、烧录和实机单次截屏回归待当前编辑完成后执行；备份：`tmp/backups/manual/20260725-221736_screenshot_chain_final/`。

----

## 2026-07-25 单会话聊天稳定性、完整 IME 与截屏回归

- CHAT-01 输入区关闭键盘时固定在 `y=397, h=104`，底部保留 12px 安全边距；展开键盘时移至 `y=153, h=100`。实屏 BMP 已确认输入条完整可见，未被 1024x600 底边裁切。
- TF 拼音词库采用挂载后延迟重试加载；实机确认 `65125` 条词与简拼索引成功绑定到活动编辑器。候选区保留上一页/下一页与完整词库状态，闹钟编辑器不再重复初始化词库。
- 修复发送蓝屏的两类内存问题：Agent Worker 栈由 16KiB 收紧为经高水位验证的 12KiB，命令/事件队列放入 PSRAM，启动后内部 DMA 最大连续块恢复到 `63488` 字节；诊断日志 writer 跳过自身日志的 1KiB 二次格式化，避免 SDMMC 异常日志叠加 FATFS/stdio 调用压穿 4KiB 栈。
- 截屏在资源稳定后曾触发 IDF JPEG 创建失败的空句柄清理 panic；回溯定位为 `jpeg_new_encoder_engine()` → `jpeg_release_codec_handle()`。截屏 JPEG 连续内部 DMA 门槛由 32KiB 提升至 48KiB，实机 `45056` 字节时安全 BMP 降级，不再进入该驱动缺陷路径。根截图入口同时修正为命名参数哈希转发。
- ESP-IDF 5.5.1 构建通过，COM17 只写 `ota_0` 主应用并完成 Hash 校验；CHAT-01 固定入口截图 `tmp/cache/screenshot_test/chat_after_all_fixes.bmp` 为 1,228,866 字节，已完成稳定门、TF 分块 CRC32 和 BMP 文件头校验。最终 `agent_run(查看库存)` 返回 `COMPLETED / inventory.list`，`agent/data/sessions.json` 成功增长至 3765 字节，过程未见 Guru、SDMMC 内存错误或 diag_log 栈保护故障。
- 既有 `ota_1` 2MiB 槽容不下约 3.0MiB 主镜像的告警未改动；此前遗留 Flash coredump 校验为 `ESP_ERR_INVALID_CRC`，不影响本轮文本会话和截屏 BMP 回归，但后续应在独立 coredump 清理/导出任务中处理。
- 本轮备份：`tmp/backups/manual/20260725-220657_diag_log_dma_fix/`、`tmp/backups/manual/20260725-223636_screenshot_jpeg_dma_guard/`、`tmp/backups/manual/20260725-222943_capture_wrapper_splat_fix/`、`tmp/backups/before_run/20260725-223711_chat_screenshot_fix_build/`、`tmp/backups/before_run/20260725-223827_chat_screenshot_fix_app_flash/`、`tmp/backups/before_run/20260725-223958_fixed_chat_capture_verify/`。

----

## 2026-07-26 P4 Agent Runtime、AI API 与 PSRAM 上下文修复

- CHAT-01 与 Web AI 测试统一切换到 P4 `agent_runtime`，云模型步骤使用 `fridge_ai_client_agent_chat_with_config()`，不再经过 S3 旧 `assistant_chat`。Agent Loop 最多 6 步，每步至多一个工具调用；READ 自动执行，WRITE/DANGEROUS 等待确认，确认或拒绝结果均回传模型；取消以 generation 立即释放 UI 忙碌态，并丢弃迟到的 HTTPS 结果。
- Agent 上下文改为可增长的 PSRAM JSON Writer，完整包含固定策略、P4 设备上下文、Persona、匹配 Skill、全部 21 个工具 Schema、最近 12 轮文本和当前 Run 工具结果；原始上下文上限 192KiB、请求体上限 384KiB，超过上限或 PSRAM 不足时明确失败，不再用 8KiB 固定数组静默截断。HTTPS Worker 使用约 24KiB 外部栈，控制任务继续使用内部 SRAM 栈。
- P4 minimal USB/Web 协议补齐 AI Profile 配置、测试和 Agent 状态命令；API Key 仅写入 NVS，串口只返回 `hasApiKey/apiKeyPreview`。Base URL 写入继续强制 HTTPS，旧 `systemPrompt` 仅作字段兼容，不改变 P4 Agent 固定安全策略。
- 修复三项实机问题：六条 Agent 状态响应末尾多一个 `}` 导致严格 JSON 解析失败；云请求失败路径释放 job 后继续读取错误文本的 use-after-free；`create/select/delete_ai_profile` 在 12KiB USB 任务栈上放置约 8.7KiB 配置视图导致 `__ssprint_r/_svfprintf_r` 栈保护异常。Profile handler 的大配置视图现均由 PSRAM 优先堆分配，并覆盖所有成功/失败释放路径。
- ESP-IDF 5.5.1、RISC-V GCC 14.2 完成编译、链接与镜像生成；当前沙箱无法启动 IDF Python 3.14 解释器，因此使用 Python 3.9 代执行同一 IDF 5.5.1 `ldgen.py/esptool.py` 命令。新应用镜像为 3,163,360 字节，SHA-256 `531B4BFD237555009F6564D9E8D54F59210489234BF75D32AE5581502A7CD939`；COM17 以 ESP32-P4、115200、ROM loader、`--no-stub` 只写 `0x30000` 并通过写入 Hash 校验。
- 实机 Agent 回归通过：TF 恢复 33 条记录且不重放副作用；本地 HELP、`device.state.get` READ 工具和 CHAT-01 可见状态运行均完成；开放请求完整构建 `6084` 字节上下文、21 个工具 Schema，云 Worker 栈高水位 `19796`，PSRAM 约 16.9MiB，内部 DMA 最大连续块约 43KiB。当前默认 AI Profile 没有真实 Base URL/API Key，因此真实云端 HTTP 200 二轮对话仍待用户配置有效 Profile 后验证。
- AI Profile 栈修复在实机连续两轮完成“创建临时 Profile → 写入假 HTTPS 配置和假 Key → 读取脱敏元数据 → 清 Key → 切回 Profile 0 → 删除临时 Profile”；完整假 Key 未出现在响应中。随后硬复位收到新 `ready`，确认 `activeProfileId=0`、Profile 数量为 1、Agent `lastState=IDLE`、`cloudPending=false`、空闲 PSRAM 31,689,316 字节，未见实时 Guru、栈保护、Task/Interrupt WDT 或 brownout。
- 用户在最终 Profile 回归前已拔下屏幕，因此本轮最后一次固件的串口、NVS、PSRAM 与 Agent 验证有效，但未重复执行 CHAT-01 实屏、触摸、IME 或截图验收；重新接屏必须先断电。既有 `ota_1` 2MiB recovery 容量不足告警未按本任务约束修改。
- 关键备份：`tmp/backups/manual/20260726-104610_agent_status_json_fix/`、`tmp/backups/manual/20260726-110102_agent_error_uaf_fix/`、`tmp/backups/manual/20260726-110934_ai_profile_stack_fix/`、`tmp/backups/before_run/20260726-111450_ai_profile_stack_flash/`、`tmp/backups/manual/20260726-112142_p4_agent_api_progress/`。
- Web/Runtime 适配补充：会话记录与 Agent 回复统一为 2KiB UTF-8 上限，TF Runtime 读写和 USB 会话读取缓冲统一提升至 192KiB（PSRAM 优先），避免页面运行时得到完整回复、重启恢复却被 768B 截断；Web 恢复先筛选 user/assistant 再取最近 24 条，实时窗口也保留最近 24 条。确认/拒绝命令成功入队后 Runtime 立即进入 `WAITING_TOOL`，Web 同时兼容旧固件的短暂确认态，消除连续轮询误显示旧确认卡；清空 TF 会话和删除备用 Profile 均增加浏览器二次确认。Web 状态改为“配置完成”，明确这只校验 URL/模型/已保存 Key，云端可达性以实际 Agent Run 为准。`npm run build` 与 `tools\\idf.ps1 build`（ESP-IDF 5.5.1 / esp32p4）均通过；未烧录，也未因屏幕拆除执行实机 UI 测试。备份：`tmp/backups/manual/20260726-123603_web_agent_runtime_adaptation_fix/`。

----

## 2026-07-26 HOME-02 今日摘要详情移植与首页入口接通

- 在当前启用的 `components/ui_p4` 中新增 HOME-02，保持 `704px 主区 + 72px 导航 + 248px 状态舱`、87px 共享顶栏、CTX-01 和 HOME 导航高亮；正文按参考稿使用 `x=16、w=672` 的单层纵向滚动，覆盖已过期、今天到期、3 天内到期、今日推荐、菜单计划缺料和今日库存动态。
- 页面演示内容使用有界静态数组，不扩展库存查询、`home.json`、TF 资源或存储 schema。无后端的食材/库存详情继续显示“尚未移植”提示；已有安全目标页的推荐、菜单和缺料入口接入 PLAN-01，Agent 入口接入 CHAT-01，返回进入 HOME-01。
- 新增稳定页面键 `home_detail` 和 `FRIDGE_UI_P4_PAGE_HOME_DETAIL`，枚举追加在既有页面之后；`fridge_ui_p4_page_from_key()`、`fridge_ui_p4_page_key()`、`ui.switch_page` schema 与 `ui_status` 均可识别该页。HOME-01 的“3天内临期”“已过期”和“全部 N 项”已接入 HOME-02。
- HOME-02 复用现有 PPA 页面缓存和统一 8px 滑动取消保护，所有滚动区按钮/行均通过 `add_click_event()` 注册，`PRESS_LOST`、`SCROLL_BEGIN` 或达到位移阈值后不补发点击。
- 修复项目固定 `tools\\idf.ps1` 对已失效 Python 3.14 环境的硬编码：优先使用项目已验证的 `tmp/cache/idf_venv`，并保留原公共环境作为回退；`tools\\idf.ps1 -Version` 已正确返回 ESP-IDF 5.5.1。
- 使用独立 `build_home02_final` 完成 ESP-IDF 5.5.1、RISC-V GCC 14.2.0、LVGL 9.2.2、esp-sr 2.4.5 全量构建（1759/1759）。应用镜像为 3,172,288 字节；既有 `ota_1` 2MiB recovery 槽容量不足告警保持不变，`ota_0` 6MiB 主槽可容纳本镜像。
- COM17 通过 ROM loader/no-stub 只写主应用并完成 Flash Hash 校验。固定截屏入口对 `home_detail` 返回 `queued=true`，随后连续两次满足 `ready/resourceReady/framebuffersSynced/pageFrameReady` 且 `page=home_detail`；顶部实屏 BMP 为 `tmp/cache/home02_ui_port/device_home02_top_ppa.bmp`（1024×600，1,228,866 字节）。
- HOME-02 实际内容高度为 1668px，原 PPA 1600px 上限会回退普通滚动；已将有界上限调整为 1700px。重新构建、刷写和截屏后日志确认建立 `704x1668 RGB565 + 80px elastic` 缓存（约 2513KiB，probe 15.7ms），不再出现 `ESP_ERR_INVALID_SIZE`。
- 实机启动确认 EK79007 MIPI-DSI、GT911、TF 字体 12/12、C6/SDIO 与网络链路恢复；本次回归未出现新增 brownout、Task/Interrupt WDT 或实时 panic。用户确认本页面本轮收口；中段/底部独立截图、逐入口触摸和超过 8px 拖动误触仍未单独留存自动化证据。
- 本轮备份：`tmp/backups/manual/20260726-115542_home02_ui_port/`、`tmp/backups/manual/20260726-130030_idf_wrapper_python_fallback/`、`tmp/backups/manual/20260726-131205_home02_ppa_height/`、`tmp/backups/before_run/20260726-130326_home02_build_flash_verify/`、`tmp/backups/before_run/20260726-131226_home02_ppa_rebuild_flash/`、`tmp/backups/manual/20260726-131933_home02_final_report/`。

----

## 2026-07-26 HOME-02 竖屏隐私模式、状态舱与 PPA 加速

- 新增 `landscape` / `portrait_privacy` 稳定方向枚举和异步 UI 请求队列；EK79007 物理扫描始终保持 `1024×600 RGB565`，竖屏使用两块 `600×1024` 逻辑 DIRECT 缓冲，经 PPA SRM 顺时针旋转后提交。GT911 竖屏触摸执行逆坐标映射，未修改面板 lane、DPI 时钟、GPIO、背光或触摸驱动时序。
- HOME-02 竖屏采用 `672 主区 + 64 导航 + 288 状态舱`，主正文视口 `600×585`、内容宽 568px；摄像头入口和状态固定为隐私关闭，导航到其它现有页面时先安全恢复横屏。新增 `ui.set_orientation` USB Tool，`ui_status` 返回 `orientation/privacyMode`，固定截图脚本支持 `-Orientation portrait_privacy`。
- 将原仅支持横屏的 PPA 滚动器改为可重绑定几何：横屏 `704×513`，竖屏 `600×585`。竖屏手势期间 PPA 从 `600×1668` RGB565 快照裁剪可视区到逻辑缓冲，再复用硬件旋转提交；LVGL 只维护坐标、惯性和回弹，不再逐帧软件重绘长列表。实机日志确认竖屏缓存约 2142KiB、PPA probe 15.8～15.9ms，切回横屏后重新建立 `704×1668` 缓存。
- 状态舱补齐参考稿同源的锁和计时器语义：隐私胶囊、下一事项、摄像关闭按钮均使用正式 A8 图标；日期改为紧凑完整日期，避免与隐私胶囊重叠。TF UI 包升级为 `2.0.0-portrait.1`，新增 `icons/lock.a8`；电脑端资源生成/校验 9 项通过，COM17 部署完成 37/37 文件逐项回读校验，并以临时目录原子切换，旧包保留为 `.prev`。
- ESP-IDF 5.5.1 最终构建通过，主应用 `3,182,864` 字节，SHA-256 `765A69C9693955611950C1DB635C144317DF72ADCC7FC777829BBFDC1C05AD02`；COM17 ROM loader/no-stub 写入 `ota_0` 并通过 Flash Hash 校验。既有 2MiB `ota_1` recovery 容量不足告警保持不变。
- 固定入口完成 `ready → home_detail → portrait_privacy → 连续两次稳定门 → BMP 截屏 → TF CRC32 读回`，最终画面为 `tmp/cache/portrait_privacy/device_home02_portrait_final.bmp`，对比图为 `tmp/cache/portrait_privacy/comparison_reference_left_device_right_final.png`。另在同一串口会话完成 `landscape → portrait_privacy → landscape` 双向回归，三阶段均连续两次稳定，最终 `privacyMode=false`。
- 本轮未出现新增 panic、Task/Interrupt WDT 或 brownout。截屏因既有内部 DMA 最大连续块 40960B 小于 JPEG 49152B 门槛安全降级 BMP；方向回归中出现一次既有 SHT40/共享 I2C NACK，未导致重启且本轮未改动 I2C。PPA 硬件路径和缓存已验证，连续手指拖动的主观手感/FPS 仍需用户在当前固件上最终确认。
- 技术说明见 `doc/portrait_privacy_mode.md`，Tool 契约见 `doc/agent/TOOLS.md`。主要备份：`tmp/backups/manual/20260726-135558_portrait_privacy_ui/`、`tmp/backups/manual/20260726-150922_portrait_cabin_ppa_fix/`、`tmp/backups/before_run/20260726-152333_portrait_cabin_ppa_build/`、`tmp/backups/before_run/20260726-153327_portrait_asset_version_build/`、`tmp/backups/before_run/20260726-154902_portrait_date_build/`。
- 状态舱像素修正补充：新增 40px Bold 时钟和 15px Medium 状态说明角色；日期恢复“日期 · 星期”完整灰色加重显示；三段上下文标题改为 18px 加重字并补齐标题下横线；重新排布隐私胶囊、温湿度和五个健康状态格，摄像头图标随隐私态强制灰显。PPA 主滚动与方向旋转路径未改动。
- 独立 `build_portrait_cabin_alignment` 构建通过，应用镜像 `0x309340`，COM17 ROM/no-stub 写入及 Hash 校验通过。固定入口再次完成连续稳定门、BMP 截屏、TF 分块 CRC32 和文件头校验；最终实屏为 `tmp/cache/portrait_privacy/device_home02_portrait_cabin_final.bmp`，同屏对比为 `tmp/cache/portrait_privacy/comparison_reference_left_device_right_cabin_final.png`。实机温湿度真实值下无裁切；截图期间仍偶发一次既有 SHT40 I2C NACK，未重启。
- 本次补充备份：`tmp/backups/manual/20260726-160145_portrait_cabin_alignment/`、`tmp/backups/before_run/20260726-161356_portrait_cabin_date_build/`、`tmp/backups/before_run/20260726-161852_portrait_cabin_health_row_build/`。
- 状态舱最终间距与线条补充：三段标题横线和模块竖线改用同一无边框纯色对象，厚度统一为 1px；五个状态格整体右移并与湿度拉开间距，隐私胶囊右移并与星期留白，锁图标与文字在胶囊内双轴居中；竖屏顶部导航栏新增与横屏一致的 1px 底线。PPA 路径未改动。
- 更新后的 `build_portrait_cabin_alignment` 构建通过，应用镜像 `0x309280`（3,183,232 字节，SHA-256 `80777F34E9D55D220CADB3ECAFE9DADD94861BAA388F3B69479F1F2EFFB250A9`），COM17 ROM/no-stub 写入及 Hash 校验通过。固定入口再次通过四项稳定门、BMP/TF CRC32 校验，最新实屏与同屏对比沿用上述 `*_cabin_final` 路径；本次启动未见新增 panic、WDT、brownout 或 SHT40 NACK。备份：`tmp/backups/manual/20260726-162451_portrait_cabin_spacing_lines/`、`tmp/backups/before_run/20260726-162532_portrait_cabin_spacing_lines_build/`。
- 按实屏反馈继续收紧状态舱顶行：日期右移 8px，温湿度整体左移 6px，五个健康状态格再右移 16px；隐私胶囊内部改为居中的 90px 内容组，锁与“隐私模式”保留 14px 可见间距。更新镜像仍为 `0x309280`（3,183,232 字节，SHA-256 `4DA51B91FA85624919C1E6D2C381C0531454E8D5BC26A8EA232713BA0ACA115F`），构建、COM17 no-stub 写入和 Flash Hash 校验通过。
- 固定入口再次完成 `home_detail + portrait_privacy` 连续稳定门、BMP 截屏、TF CRC32 与文件头检查；实屏为 `tmp/cache/portrait_privacy/device_home02_portrait_cabin_spacing_final.bmp`，同屏对比为 `tmp/cache/portrait_privacy/comparison_reference_left_device_right_cabin_spacing_final.png`。PPA 竖屏缓存仍为 `600×1668 + 80px elastic`、probe 15.8ms；未见新增 panic、WDT 或 brownout，截屏阶段出现一次既有 SHT40 I2C NACK且未重启。备份：`tmp/backups/manual/20260726-163128_portrait_cabin_final_spacing/`、`tmp/backups/before_run/20260726-163156_portrait_cabin_final_spacing_build/`。
- 隐私胶囊末次微调：胶囊整体右移 14px，与“周日”拉开距离；内部内容组扩为 94px，锁图标与文字的可见间距增至 20px，并保持整体双轴居中。镜像仍为 `0x309280`，SHA-256 `1B702E215E730789C73FD7469A934C2CB9542A332063C0CB22A7721EA0938D98`；构建、COM17 no-stub 写入、Flash Hash、USB 稳定门及截图 CRC32 均通过。实屏：`tmp/cache/portrait_privacy/device_home02_portrait_privacy_pill_final.bmp`；对比：`tmp/cache/portrait_privacy/comparison_reference_left_device_right_privacy_pill_final.png`。PPA probe 15.9ms，未见新增 panic、WDT、brownout 或 SHT40 NACK。备份：`tmp/backups/manual/20260726-163902_portrait_privacy_pill_gap/`、`tmp/backups/before_run/20260726-163919_portrait_privacy_pill_gap_build/`。
- 用户局部截图确认锁仍与首字粘连后定位到真实根因：`center_single_line_label()` 会执行 `LV_ALIGN_CENTER`，覆盖创建标签时指定的横坐标。隐私胶囊现改为固定图标槽和文字槽，文字仅垂直居中，实屏放大图确认锁与“隐”之间已有稳定空隙。镜像 `0x309280`，SHA-256 `4CA0BAECE893505954E8461EF953C6C4FD1CB3C5ACB5CF4B41A15566FF1F690D`；构建、COM17 写入、稳定门和 TF CRC32 均通过。实屏：`tmp/cache/portrait_privacy/device_home02_portrait_privacy_overlap_fixed.bmp`；局部放大：`tmp/cache/portrait_privacy/privacy_pill_overlap_fixed_crop.png`；同屏对比：`tmp/cache/portrait_privacy/comparison_reference_left_device_right_privacy_overlap_fixed.png`。PPA probe 15.9ms，未见新增 panic、WDT、brownout 或 SHT40 NACK。备份：`tmp/backups/manual/20260726-164737_portrait_privacy_icon_text_overlap/`、`tmp/backups/before_run/20260726-164754_portrait_privacy_icon_text_overlap_build/`。
- 根据后续“间距过大”反馈，将固定槽位的实屏空隙由约 17px 收回至约 10px，胶囊整体位置不变。最终镜像 `0x309280`，SHA-256 `15B62742BE5F469EA4FF71FB7B326FB0479FF79EEA6E7BB56D6979D1AD5DB713`；构建、COM17 写入、稳定门和 TF CRC32 均通过。实屏：`tmp/cache/portrait_privacy/device_home02_portrait_privacy_balanced_gap.bmp`；局部放大：`tmp/cache/portrait_privacy/privacy_pill_balanced_gap_crop.png`。PPA probe 15.8ms，未见新增 panic、WDT、brownout 或 SHT40 NACK；启动阶段 SNTP 前两源超时后第三源同步成功。备份：`tmp/backups/manual/20260726-165427_portrait_privacy_balanced_gap/`、`tmp/backups/before_run/20260726-165444_portrait_privacy_balanced_gap_build/`。
- 最终按用户指定将锁与文字的实屏空隙微调为约 7px，固定槽位和胶囊整体位置保持不变。镜像 `0x309280`，SHA-256 `4B7A3E6D3FD056F9B73EFB0BAF21DA8BABFC34275DB99855247958F86FE2343C`；构建、COM17 写入、稳定门和 TF CRC32 均通过。实屏：`tmp/cache/portrait_privacy/device_home02_portrait_privacy_7px_gap.bmp`；局部放大：`tmp/cache/portrait_privacy/privacy_pill_7px_gap_crop.png`。PPA probe 15.9ms，未见新增 panic、WDT、brownout 或 SHT40 NACK。备份：`tmp/backups/manual/20260726-165916_portrait_privacy_7px_gap/`、`tmp/backups/before_run/20260726-165932_portrait_privacy_7px_gap_build/`。
- 竖屏安装方向修正：PPA 最终输出由逆时针 270°改为逆时针 90°，使全部 `portrait_privacy` 页面相对旧实现旋转 180°；GT911 逆坐标映射同步调整为 `x=599-y、y=x`。WebUI 顶栏新增单按钮横竖屏切换，真实串口与 Mock 均复用 `ui.set_orientation`；进入竖屏时自动先稳定切到 HOME-02，方向切换完成以连续两次 `ui_status` 四项稳定门为准。固定截图脚本同步反向还原物理竖屏帧。备份：`tmp/backups/manual/20260726-170720_portrait_180_webui_toggle/`。
- WebUI `npm run build` 通过（TypeScript + Vite，1586 modules）；ESP-IDF 5.5.1 固件构建通过，镜像保持 `0x309280`，COM17 no-stub 主应用写入和 Flash Hash 校验通过，只有既有 `ota_1` 2MiB recovery 槽容量告警。固定入口完成 HOME-02、竖屏方向、连续稳定门、BMP/TF CRC32 与文件头校验，新方向实屏为 `tmp/cache/portrait_privacy/device_home02_portrait_180_fixed.bmp`；竖屏 PPA 缓存仍为 `600×1668 + 80px elastic`、probe 15.8ms。读回期间真实触摸按新坐标映射命中竖屏底部导航并安全恢复横屏，未见新增 panic、WDT 或 brownout。正确构建前备份：`tmp/backups/before_run/20260726-171146_portrait_180_webui_toggle_build/`。

----

## 2026-07-26 P4 Agent MQTT v2、公网二维码绑定与多端同步首期落地

- 固件新增 P4-only `mqtt_protocol` 与 `device_binding`：固定 `fridge/v2/{homeId}/{deviceSn}` Topic、QoS 1、retained state/LWT、64KiB envelope、分片组装、worker 隔离、1/2/5/10/30 秒重连、`bindingEpoch` 校验和最近 8 个远程 Tool requestId 的 NVS 幂等。MQTT 大 payload 与 ESP-MQTT outbox 已启用 PSRAM，未恢复任何 S3 schema/topic。
- P4 屏幕新增绑定页和 LVGL QR；设备经 USB 预置 `apiBaseUrl/deviceSn/keyId/bootstrapSecret`，向 Backend 创建 5 分钟一次性二维码，小程序 claim 后设备领取 Device/MQTT 凭据、NVS 落盘、首期 12 域 seed 或云端快照、启动 MQTT 并 ACK。首次 seed 成功后也会立即回拉权威快照，为本地实体补齐 revision。
- 12 个结构化域统一为 `inventory/reminders/timers/alarms/stopwatch/shopping/recipes/nutrition/agent_sessions/agent_memory/device_settings/ai_config`，支持 upsert/delete/append、entity/server revision、tombstone、冲突 ACK 和 HTTP 完整快照。Tool Broker 本地库存、提醒/计时器、记忆、秒表写操作及 Agent foreground 会话持久化已自动转成上行 MQTT 事件；云端事件经 Agent Storage 事务锁落盘，避免 Tool 的读改写与远程 apply 交叉覆盖。
- AI Key 按用户确认的比赛模式允许经 Device 鉴权 HTTP/MQTT 明文同步，但日志、普通 GET、WebSocket、ACK、USB 状态均脱敏；设备有效 Key 只写 AI Client NVS，TF `ai_config.json` 镜像删除明文并记录 `hasApiKey`。
- Backend 新增独立 P4 v2 表、幂等 migration/provision 脚本、P4-safe 微信登录、二维码 claim、12 域初始种子/快照/增量、乐观并发、MQTT bridge 和 JWT 首帧 WebSocket；旧 v1/S3 数据不导入 v2。Nginx 已代理 `/api/v2/ws`，Compose 启动时自动运行 `migrate_p4_v2.py`。
- 小程序权威契约维护在 `doc/P4_Agent_MQTT公网多端同步技术文档.md`，覆盖真实请求/响应、二维码 grammar、MQTT envelope、12 域 TypeScript 模型、WebSocket、USB 预置、公网部署、冲突与验收清单；`backend/README.md` 已切换为 P4 v2 部署入口。
- 验证：ESP-IDF 5.5.1 / ESP32-P4 构建通过，`build/fridge_spirit.bin` 为 `3,266,864` 字节（`0x31d930`），ELF 已确认两个弱回调被实际链接；Backend `compileall`、`docker compose config --quiet`、作用域 `git diff --check` 通过。隔离环境缺少 pytest，Docker daemon 未启动，故 PostgreSQL/EMQX/双小程序/实机扫码端到端尚未执行；未烧录。既有 `ota_1` 2MiB recovery 槽小于完整主镜像的告警保持不变，主固件使用 6MiB `ota_0`。
- 当前明确边界：P4 MQTT outbox 位于 PSRAM、不是断电持久队列；`shopping/recipes/nutrition/device_settings/ai_config` 当前没有 P4 本地编辑器，但小程序/Backend 到设备的下发已接通；秒表云端状态已进入 Agent canonical store，既有 `kitchen_tools` 暂无运行态导入接口。公网正式发布前仍必须升级 HTTPS/WSS/MQTTS、EMQX 鉴权/ACL、NVS/Flash 安全能力并轮换比赛凭据。
- 本轮备份：`tmp/backups/manual/20260726-185818_p4_agent_mqtt_sync/`、`tmp/backups/manual/20260726-191943_p4_sync_integration_fix/`、`tmp/backups/manual/20260726-192229_p4_mqtt_sync_apply/`、`tmp/backups/manual/20260726-192626_mqtt_doc_contract_fix/`、`tmp/backups/manual/20260726-193607_agent_storage_transaction_lock/`、`tmp/backups/manual/20260726-193733_tool_broker_storage_lock/`、`tmp/backups/before_run/20260726-192925_p4_mqtt_build/`。
- 实机烧录补充：首次 COM17 全量 no-stub 烧录及各分区 Hash 校验通过，但未预置 Broker 时 `p4_mqtt_start` 的 4KiB 栈触发 Stack protection fault。已让空配置自动启动任务直接进入“等待设备预置/扫码绑定”并退出、把启动任务栈调整为 6KiB，同时统一未配置启动返回 `ESP_ERR_INVALID_STATE`；MQTT 大消息和 Outbox 仍使用 PSRAM。
- 修复后 ESP-IDF 5.5.1 构建通过，主镜像 `0x31d980`、SHA-256 `2A14BF4B3C3520F6637F25730388ACC9AE147E1BD9FEA90DF75BBB2E5D035755`；COM17 ROM loader/no-stub 全量写入和 Hash 校验通过。实机 32 秒启动窗口确认 P4 rev1.3、16MB Flash、32MB/200MHz PSRAM 自检、C6 SDIO、Wi-Fi、UI/Agent/MQTT 初始化及 `ready` 正常，MQTT稳定停在等待绑定，未新增 panic、brownout、Task/Interrupt WDT 或重启。仍观察到历史 coredump CRC 无效、一次既有共享 I2C NACK；SNTP 前两源超时后第三源同步成功。
- 本次补充备份：`tmp/backups/before_run/20260726-202231_p4_mqtt_flash/`、`tmp/backups/manual/20260726-202715_mqtt_autostart_stack_fix/`。
- 2026-07-27 Web MQTT 控制面补齐：定位到 P4 minimal 只注册内部 `mqtt_status`，而 Web 固定调用 `get_mqtt_config`，因此此前出现 `unsupported in p4_minimal profile`。现已在 P4 USB JSON Lines 中补齐 `get_mqtt_config/get_mqtt_status/set_mqtt_config/clear_mqtt_secret/mqtt_publish_state`，同步加入 `ready.commands`，返回 `statusText/keepaliveSeconds/queueDroppedCount` 等 Web 契约字段；password/token 仍不回显，临时配置副本在响应后清零。
- ESP-IDF 5.5.1 构建、COM17 ROM loader/no-stub 全量写入和各分区 Hash 校验通过；主镜像 SHA-256 为 `65D827FEA11A04DDBB9180B48FAC53B3CC7A1D7C51979F849463B0FCD9E28D94`。实机收到本次 `ready` 后发送 Web 同款 `get_mqtt_config`，返回 `ok=true`、`protocolVersion=2`、`configured=false`、`hasPassword=false`、`statusText="not configured"`，确认警告已消除且当前只是尚未完成设备预置/扫码绑定。备份：`tmp/backups/manual/20260727-125737_p4_mqtt_usb_commands/`、`tmp/backups/before_run/20260727-125819_p4_mqtt_usb_build/`、`tmp/backups/before_run/20260727-125959_p4_mqtt_usb_flash/`。

----

## 2026-07-26 PLAN-06 购物清单页实现（真机阶段验证）

- 在当前启用的 `components/ui_p4` 新增 `FRIDGE_UI_P4_PAGE_SHOPPING` 与稳定键 `shopping`；PLAN-01 的“去购物清单”已接入，返回回 PLAN-01，计划导航和 CTX-06 侧舱保持高亮/一致。
- 新增最多 8 项的本地购物模型：兼容 schema v1 的缺字段，支持 `id/name/quantity/reason/priority/group/source/relation/checked`；空 `local_seed` 首次写入 PLAN-06 六项示例，手动添加默认“1份/手动添加/待购买”，AI 建议进入“建议购买”，勾选在待购/已购间迁移。
- Shell 只处理 LVGL；UI 编排层先以模型副本写 LittleFS 成功后再刷新。`/cache/shopping_list.json` 改为 `.part → fsync → .bak → rename → 读回校验` 的原子替换，失败恢复旧文件。厨房 AI 改为合并建议条目，不再覆盖用户手动或已完成记录。
- 完成三组清单行、来源标签、排序演示态、固定底栏、二次确认清空、中文拼音输入弹层、8px 滚动误触保护以及 PPA 动态覆盖层；“转为入库”按边界仅提示接口未接通。
- `tools\\idf.ps1 build` 通过（ESP-IDF 5.5.1 / LVGL 9.2.2 / RGB565），应用镜像 `0x3203c0`；既有 `ota_1` 2MiB 槽不足以容纳主镜像的告警保持不变。COM17 烧录在 64 秒无输出后超时，虽确认端口仍存在且无残留 monitor/esptool 进程，但没有新 `ready` 会话；因此未把真机稳定门、截图或触摸结果标为通过，详情见根目录 `design-qa.md`。
- 本轮备份：`tmp/backups/manual/20260726-214438_plan06_shopping/`、`tmp/backups/before_run/20260726-215817_plan06_shopping_flash/`。
- 性能修复：定位到购物页 PPA 仍按 513px 主区搬运，并为固定 73px 底栏逐帧 ARGB 快照/混合。现为 PPA 增加可见视口覆盖，PLAN-06 仅搬运 440px 正文，底栏保留在 RGB565 双缓冲中；不改变其它页面的 513px 加速路径。
- 修复后构建通过，COM17 no-stub 主应用写入 `0x30000` 并通过 Flash Hash 校验。复位后的 `ready → resourceReady/framebuffersSynced/pageFrameReady → shopping` 稳定门均通过，购物缓存为 `704×912 + 80px elastic`，硬件探针由 16.1ms 降至 13.2ms；未出现缓存几何错误或 LVGL 回退日志。主观连续手指拖动和固定截图仍待用户最终确认。备份：`tmp/backups/manual/20260726-222301_shopping_ppa_scroll_fix/`、`tmp/backups/before_run/20260726-222438_shopping_ppa_scroll_flash/`。
- 按实屏反馈隐藏购物页顶部 `PLAN-06` 编号胶囊，保留“购物清单”标题和其它页面的页码胶囊。构建通过，COM17 no-stub 主应用重新写入并通过 Flash Hash 校验。备份：`tmp/backups/manual/20260726-222811_shopping_header_id_remove/`、`tmp/backups/before_run/20260726-222904_shopping_header_id_flash/`。
- 购物清单交互改为批量操作：行勾选仅保留 Shell 内存选择态，不再立即改写 JSON 或移动分组；底栏新增“加入待购买 / 加入已购买”，收集最多 8 个已选 ID 后在一个模型副本中原子写入。移除行内“加入待买”与“按超市动线”，保留添加物品和清空已完成。构建通过，COM17 no-stub 主应用写入并通过 Flash Hash 校验。备份：`tmp/backups/manual/20260726-224049_shopping_batch_selection/`、`tmp/backups/before_run/20260726-224400_shopping_batch_selection_flash/`。
- 购物页勾选 A8 图标改为 32px 按钮内的 28px 居中布局；勾选只原地更新当前行并使 PPA 缓存延后重建，不再重建页面或回到顶部。来源标签统一为“来自菜谱 / 低库存 / Agent建议 / 手动添加”四类，分别使用薄荷、蛋黄、鼠尾草、奶油四种胶囊色。构建通过，COM17 no-stub 主应用写入并通过 Flash Hash 校验。备份：`tmp/backups/manual/20260726-224902_shopping_selection_source_pills/`、`tmp/backups/before_run/20260726-225103_shopping_selection_source_pills_flash/`。
- 底栏进一步分组：左侧绿色“添加物品”独立，右侧将“加入待购买 / 加入已购买 / 清空已完成”置于浅奶油批量操作容器，并分别使用常规、薄荷、番茄语义色。构建通过，COM17 no-stub 主应用写入并通过 Flash Hash 校验。备份：`tmp/backups/manual/20260726-232216_shopping_footer_action_group/`、`tmp/backups/before_run/20260726-232319_shopping_footer_action_group_flash/`。
- 2026-08-03 PLAN-01 计划页布局补充：今日菜单改为四行纵向列表并增加行高，购物摘要中的两条食材与“去购物清单”按钮拉开间距，小精灵建议卡增加底部留白，避免第二行文字贴住卡片边线。ESP-IDF 5.5.1 构建、COM17 no-stub 烧录、固定截图稳定门和 TF 分块 CRC32 校验通过；实屏截图：`tmp/cache/screenshot_test/plan_after_layout_fix_2.bmp`。备份：`tmp/backups/manual/20260803-233404_plan_layout_overlap/`、`tmp/backups/manual/20260803-234024_plan_overlap_second_pass/`、`tmp/backups/before_run/20260803-233521_plan_layout_build/`、`tmp/backups/before_run/20260803-234119_plan_layout_second_pass_build/`、`tmp/backups/before_run/20260803-234210_plan_layout_second_pass_flash/`。

----

## 2026-07-26 UI 参考预览 ver2：库存浏览页视觉重做 + 空间形状编辑页重构

本次只改 `ui-reference/ver2/` 网页参考预览（设计走查用），不涉及固件、引脚、sdkconfig 与任何硬件路径。

### 完成内容

1. **库存三级空间统一视觉语言（新鲜度热力图）**
   新增 `ui.freshTone(items)`：按范围内"最差食材"推出语义档（有过期=danger / 3 天内临期=warn /
   都还富余=ok / 空=none）。L1 分区块、L2 层卡、L3 格子共用同一套语义浅底 + 左侧 4px 语义色条 +
   36px 大号项数，用户在任意层级都用同一套颜色判断"哪里要先处理"。
   新增 `ui.fridgeBox()` 冰箱内胆外框，把当前层级的空间包成"一台冰箱"并在顶部给整机汇总；
   L1 与 L3 撑满 704×600 主区不滚动，L2 层卡列表在框内滚动。

2. **INV-01 顶部信息条合并**
   原先"seg + 面包屑"与"范围统计行"分两行，现合并为一条：左侧常驻 空间/列表 seg，
   右侧范围信息条 = 面包屑 + 统计芯片（项数/过期/临期/未定位），底色跟随当前范围语义档。
   L2 层卡新增层序徽标与该层前 3 项食材预览标签（按剩余天数升序，最紧急在前）。

3. **彻底移除 L3 九宫格"相邻格合并位"**（用户决定）
   九宫格恒为 A1–C3 九个等大单位格。清掉 `mock-data` 的 `merges`、`ui.grid9`/`miniGrid9` 的
   merge 分支、`dataQ.locPath` 的合并名拼接，以及 INV-08 的合并/拆分交互与 `inv-merge-summary`。
   原 `B2+B3 奶制品位` 的 3 项食材改挂 B2 / B3 单格。理由：合并让位置模型、`loc.cells`
   与布局编辑各复杂一档，收益只是一个自定义位名。

4. **INV-08 由三级下钻重构为平级双分段，改名「空间形状编辑」**
   旧结构为"分区画布 → 点分区 → 层管理 → 九宫格编辑"（`level=zones|layers|cells`）。
   新结构顶部固定「分区形状 / 层结构」互斥分段（`tab=zones|layers`），两者平级不下钻；
   层结构页自带分区芯片行可直接横向换分区，少一次返回。因九宫格不可编辑，本页没有第三个分段。
   操作逻辑优化：
   - 命名不强制走软键盘：常用名称/分区类型做成预置芯片一点即改（仍保留 OV-12 键盘入口）；
     新建分区后自动打开命名面板，趁上下文还在改掉无意义的默认名。
   - 命名面板是页面内子容器，不占用 12 个全局浮层名额。
   - 分区/层里还有食材时不再只丢一句警告，而是弹确认框给"去查看"，直接跳到该范围的库存列表
     （`INV-01?zone=..&layer=..&view=list`）去迁移，对应规范"保存前迁移受影响食材"。
   - 层的上移/下移到边界时按钮禁用而不是点了才提示；每个分区至少保留一层。
   - 「精确到格」改为层卡上的 `lv_switch` 对照开关，明确只影响登记是否必须选格，不改九宫格形状。
   - 提交前汇总影响面（被删分区/层里的食材数），有冲突时确认按钮变"仍然提交"并转危险色。

### 涉及模块

`ui-reference/ver2/js/core/ui.js`（空间三级 builder 全部重写、新增 fridgeBox/freshTone/zoneTypeIcon）、
`js/pages/pages-inventory.js`（INV-01 渲染 + INV-08 整段重写）、`js/data/mock-data.js`（去 merges、
locPath）、`js/core/cabin.js`（CTX-02 的 INV-08 分支改分段模型）、`styles/components.css`（三级共用
tone/sp-/fridge-box/zone-map/layer-card/grid9）、`styles/pages-inventory.css`（INV-01 信息条 +
INV-08 全段重写）、`README.md`（页面清单、验收清单、一致化决定）。
新增 `.claude/launch.json`（preview_start 起本地静态服务器用）。

### 当前结果

浏览器实测（`python -m http.server`，横屏 1024×600 + 竖屏 600×1024 双模式）：

- 全部 31 个页面逐页走查，无 JS 报错，主区无横向/纵向溢出（SYS-01/02/03 为 776px 是隐藏侧导航的既定几何）。
- INV-01 五个变体（L1 根空间 / L2 分区空间 / L3 九宫格 / 根列表 / 分区列表）语义色与统计正确，
  L3 恒 9 格。
- INV-08 两个分段全流程实测通过：分区多选建区（1×1→2×1 矩形扩展、非法矩形回退提示）、
  拆成单格、改名+改类型即时生效、脏标记与一步撤销、芯片换分区、层上下移与边界禁用、
  精确到格开关、非空层删除引导到迁移列表、提交写回 `FS.data.layout` 并清脏。
- 选位流程（INV-04 →「选择位置」→ 三步 → 回写）在去掉合并后仍正常，回写文案为 `右冷藏 / 第2层 / A2`。
- 页面总览 59 张缩略图全部渲染，含 INV-08 两个新变体。
- 样式纪律：新增/改动样式的字号全部走六档令牌，圆角 ≤8px，触控目标 ≥48px，纯色平铺无模糊。

### 风险与下一步

- 规划文档 `doc/冰箱小精灵_ESP32-P4_LVGL页面架构与交互规划_V1.0.md` 仍写着"三级位置编辑：
  每层可选 3×3 九宫格及相邻格合并"。本次按用户决定取消了合并，属于**有意偏离规划文档**，
  已在 `ui-reference/ver2/README.md` 第八节写明理由；`doc/` 本身未改动（只读输入）。
- 预置名称/类型列表（`ZONE_NAME_PRESETS` 等）目前写死在 `pages-inventory.js`，
  固件落地时应改为可配置或从 NVS 读取。
- 命名面板的键盘入口在预览里只弹 OV-12 不回填文本；真机需接 `lv_keyboard` 的 READY 事件写回草稿。
- 本次改动仅限网页参考预览，固件 `ui_p4` 侧的库存页尚未按新视觉语言落地，属后续工作。
- 备份：`tmp/backups/manual/20260726-203433_ui_ver2_inventory_redesign/`。
- L3 九宫格改回 ver1 视觉并提高信息密度（用户走查反馈）：去掉 L3 的内胆卡头一行（层名已在页头副标题和面包屑里），把整块高度让给九宫格，格高 111px → 137px；冰箱图标从内胆卡头移到顶部范围信息条最左，空间/列表视图都常驻可见。九宫格由"九张独立卡片"改为 ver1 的"一整块大圆角板 + 发丝线切九格"（`.grid9-board`/`.g9-cell`，分隔线用格子右/下 1px 边框、末列末行去掉），每格内容自上而下 = 位置名·格号（内/中/外 × 左/中/右）/ 食材名 + 数量 / 整格宽到期条，同格多项补一行"另有 N 项：…"；到期条与格底色仍按保质期着色。
- 同时修掉两处配色/布局缺陷：① `ui.freshTone()` 原本 `<0` 才判 danger，与 `dataQ.daysTone()` 的 `<=0` 不一致，导致"今天到期"出现格底色黄、格内到期条红的自相矛盾，现统一复用 `dataQ.daysTone` 阈值；② 长数量（如"半袋(约20)个"）会把食材名挤到 18px，现给 `.g9-amount` 设 `max-width:45%` 并双侧省略，实测长名场景食材名可得 111px（横屏）/ 90px（竖屏）。
- 实测：31 个页面无报错无溢出，INV-01 五变体正确（L3 恒 9 格、无卡头、图标常驻），选位流程回写 `右冷藏 / 第2层 / A2` 正常，横竖屏均无裁切，格底色与到期条语义档零不一致。备份：`tmp/backups/manual/20260726-233630_ui_ver2_grid9_ver1_style/`。

----
完成时间：2026-07-27 12:38:14 +08:00

- 完成 WakeNet 与右侧全局语音入口的连续对话协调：本地 VAD 切句后复用既有 ASR、Agent Runtime 与 TTS；对话页保留原有单次录音、手动提交路径。
- 连续会话仅复用既有 8KB 内部 SRAM 协调任务；候听使用实时帧音量，不保留 10 秒 PCM；10 秒静默或约 1.5 秒句尾静默后安全结束/提交。
- Agent 回复协议新增内部 `dialog_complete` 标志；写操作仍须确认，连续链路可语音确认/取消，右侧只展示状态，不展示对话历史。
- 已通过 `tools\\idf.ps1 build`；实机烧录后固定截屏入口完成 `ready` 与 UI 稳定门验证，首页渲染正常。真实 WakeNet、ASR、TTS 和语音确认的端到端讲话验收仍需现场执行。
- 备份：`tmp/backups/manual/20260727-122314_continuous_voice_dialog/`、`tmp/backups/manual/20260727-123814_continuous_voice_progress/`。
- 2026-07-27 实机讲话补测发现进入“识别中”后蓝屏，结合设备 `panicCount` 增长和同机既有 `Stack protection fault` 记录，确认原 8KB 连续会话协调栈余量不足。已把 `voice_asr_auto` 内部 SRAM 栈增至 16KB，并把约 1KB 的 ASR 结果对象移到 PSRAM，同时新增 ASR 前后栈余量日志。`tools\\idf.ps1 build` 与 COM17 `app-flash` 通过；Agent 返回“语音链路测试正常。”、TTS HTTP 200，随后 150 秒观察无 panic。真实 WakeNet 唤醒后的 ASR 日志仍需再次现场发声确认。备份：`tmp/backups/manual/20260727-191227_voice_asr_stack_fix/`。
- 同日修复“ASR 和文字回复正常，但提示音频任务启动失败且无声”：根因是 Agent 回复后才临时申请 12KB TTS 内部栈，而完整 ASR/Agent/TLS 请求后最大内部连续块实测会降到 8.7KB。现改为 Agent 初始化阶段预创建常驻 TTS worker，运行期只向单槽队列投递 PSRAM Job。构建、COM17 `app-flash` 和真实云端 TTS 回归通过：HTTP 200、ES8311 输出启动、`152236/152236` 字节播放完成、时长 4757ms、TTS 栈余量 5536，随后 WakeNet 报告 `wake word ready: 小冰小冰`；无新增 panic。备份：`tmp/backups/manual/20260727-192345_agent_tts_persistent_worker/`。

----
完成时间：2026-07-27 12:51:27 +08:00

## P4 原生库存页与本地 CRUD 移植

- 新增 P4 原生 `INV-01/03/04/06/07/08` 页面路由：库存浏览器支持分区、层级、固定 3×3 九宫格与列表视图；食材详情、新增/编辑、识别确认、搜索筛选、空间形状入口均保持 1024×600 横屏 Shell 的 704 主区 + 72 导航 + 248 动态状态舱几何。
- 导航“库存”已从未实现入口接入 `inventory` 页面键；库存浏览、详情、编辑、搜索和形状页使用 CTX-02 路径/范围统计，拍照识别确认使用 CTX-04。
- 新增库存模型 `fridge_ui_inventory_model`：兼容原 `ui_inventory.json` v1 参考库存并迁移为 schema v2，提供分区、层、九宫格位置、食材和布局草稿的 load/search/upsert/remove；所有 mutation 在持久化成功后才更新 UI 内存快照。
- 本地库存继续使用既有 `/cache/ui_inventory.json` 同步链路。库存快照写入已强化为 `.part → fsync → .bak → rename → 读回校验`，启动及读取可从 `.bak` 恢复，避免掉电时丢失已有数据。
- 食材名称、备注、搜索复用聊天页相同的 `fridge_ime` 中文拼音词典和 26 键候选输入面板；拍照识别候选只有用户点击“确认并同步”才执行真实库存写入。
- 验证：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\\idf.ps1 build` 通过，生成 `build/fridge_spirit.bin`（`0x3313c0`）。未烧录、未做实屏截图或触摸验收。构建仍报告既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，未在本轮调整分区。
- 备份：`tmp/backups/manual/20260727-123831_inventory_ui_port/`、`tmp/backups/manual/20260727-124108_inventory_model/`、`tmp/backups/before_run/20260727-125039_inventory_ui_build/`、`tmp/backups/manual/20260727-125127_inventory_ui_progress/`、`tmp/backups/before_run/20260727-125238_inventory_ui_rebuild/`、`tmp/backups/before_run/20260727-125323_inventory_ui_warning_fix/`、`tmp/backups/manual/20260727-125359_inventory_ui_progress_final/`。

----
完成时间：2026-07-27 15:12:47 +08:00

## 小程序与 Backend 统一迁移到 P4 Agent v2

- 小程序业务协议已从旧 `/api/v1` 迁移到 `/api/v2`：登录、P4 二维码 claim、设备列表、12 个业务域快照/增量、实体 revision、UUID v4 离线 outbox 和显式冲突保留均已接入。
- 删除 `DEMO`、手输 SN 和“扫码原文当绑定码”的旧绑定路径；绑定页只接受 `fridgespirit://bind?v=2&sessionId=...&claimToken=...`，并调用 `/bindings/claim`。
- 新增小程序 WebSocket 客户端：首帧 JWT 鉴权、25 秒心跳、1/2/4/8/16/30 秒退避重连，并在 `auth.ok`、同步事件、冲突和绑定变化后通过 REST revision 补偿。
- 旧页面模型通过适配层映射到 v2 域；库存 CRUD、提醒确认和设备设置改为实体事件，购物/菜谱/营养使用聚合实体。AI/ASR/TTS Key 仅允许在线 PUT `/agent-config`，不进入 storage、离线队列或日志。
- Backend 新增 v2 命令入口：图片识别、库存刷新、AI 对话与历史、通知订阅，以及设备解绑/凭据撤销。库存上下文与会话历史分别以 v2 `inventory`、`agent_sessions` 为事实源，不再依赖旧 v1 业务表。
- 解绑会停用当前 binding、清除 HTTP/MQTT 凭据、递增 `bindingEpoch`、作废未完成会话并广播 `binding.changed`；出厂 Bootstrap 凭据继续保留，设备可重新生成二维码绑定。
- 库存刷新命令已对齐固件真实支持的 `sync.request_snapshot(domain=inventory)`，避免后端发送固件不识别的 `inventory.refresh`。
- 网络层新增 JWT 401 自动重新登录一次，并解析 FastAPI 字符串或数组形式的 `detail`，页面可展示具体参数错误。
- 验证：小程序 TypeScript 检查通过；Web `npm run build` 通过；Backend `pytest` 10 项通过，`compileall` 和 OpenAPI 路由检查通过，`git diff --check` 通过。
- 尚未执行真实 PostgreSQL、EMQX、公网 HTTPS/WSS/MQTTS、微信真机扫码和 P4 实机三端联调；正式发布前仍需关闭 MQTT 匿名访问并配置设备级 ACL。
- 备份：`tmp/backups/manual/20260727-145032_miniapp_backend_v2/`。

----
完成时间：2026-07-27 17:18:00 +08:00

## 小程序库存三级位置与 Backend 同步适配

- 小程序库存浏览从旧“分区 → 九宫格”改为“区域 → 层 → 固定 3×3 格位”；区域页和层页展示食材数/临期数，格位页按 `cell=0..8` 独立占位并保留待归位清单与面包屑返回。
- 新增/编辑位置选择改为区域、层、格位三步；从空格新增、扫码确认、批量确认、手动登记、首页临期和离线快照均统一显示/写入 `zoneId + layerId + cell`。
- 新增位置兼容层：P4 `zone_id/layer_id/cell`、小程序 camelCase 和旧 `zone/slot(A1~C3)` 可相互归一化；旧两级数据自动归入对应区域默认层，写入时继续附带旧字段，支持滚动升级。
- Backend 图片识别位置推荐改为按“区域 + 层 + 格位”独立判空，并返回 `suggestedZone/suggestedLayerId/suggestedCell`；同一个格号在不同层不再互相冲突。
- Backend 库存 API 正式接收/返回三级字段；`layerId/cell` 复用现有 `extra` JSON 持久化，无需修改已部署数据库表。设备快照升级为 schema v2，包含 P4 默认 `layers` 数组及每条食材的 `zone_id/layer_id/cell`，旧 `zone/slot` 继续保留。
- Backend 默认分区补齐与 P4 一致的“果蔬抽屉”；默认层结构为冷冻 2 层、左冷藏 3 层、右冷藏 2 层、门架 2 层、果蔬抽屉 1 层，自定义区默认 1 层。
- 验证：小程序 `tsc --noEmit` 通过；Backend 14 项协议测试与 `compileall` 通过；Web 运维前端 `npm run build` 通过。未执行微信开发者工具视觉走查、真实 PostgreSQL/EMQX 或 P4 实机三端联调。
- 备份：`tmp/backups/manual/20260727-165927_inventory_three_level/`。

----
完成时间：2026-07-27 17:46:00 +08:00

## 小程序其余 P4 v2 同步域显示适配

- 完成 12 个同步域的页面覆盖核对：库存、提醒、购物、菜谱、营养、Agent 会话、设备设置与 AI 配置沿用并修正现有页面；新增“计时与闹钟”和“家庭记忆”入口，补齐 timers/alarms/stopwatch/agent_memory 的只读显示。
- 新增计时工具页：计时器、闹钟、秒表分段展示，兼容 P4 固件现有 `durationSeconds/dueAtEpochMs/elapsedMs/weekday_mask/speech_text` 与规范 camelCase；明确计时状态以最近同步为准，不在小程序伪造逐秒上报。
- 新增家庭记忆页：区分结构化偏好与 Markdown 长期笔记，支持多会话同步数据的只读浏览；家庭共享敏感数据不提供无确认批量删除。
- AI 对话页增加同步会话切换；P4 foreground 消息使用 `kind/text/time` 时可直接映射为小程序对话，专用历史接口为空时自动回退到 v2 快照内容。
- 修复购物、菜谱、营养缓存只读取第一个同步实体的问题：现在同时兼容旧聚合 `{items:[]}` 与 v2 每条一个实体，并按 serverRevision 合并同 id 数据；购物清单增加“设备”来源筛选，菜谱兼容 `description` 及 `quantity/unit`。
- 营养页新增逐条饮食记录，展示热量、蛋白质、脂肪、碳水和记录时间；原库存营养建议缓存继续保留。
- 提醒完成/忽略状态由旧 `acked/dismissed` 改为 P4 `completed/cancelled`，同时兼容固件 `label/kind/dueAtEpochMs/canceled` 字段和已触发提醒。
- 设置页新增设备同步设置只读区，显示设备 SN、时区、语言、音量、亮度、提醒与 Agent 语音状态；隐私偏好实体与设备上报实体可并存，不再只取 device_settings 第一个实体。ASR/TTS 提示同步更新为已接入 v2 ai_config。
- 验证：小程序 TypeScript 检查通过；全部小程序 JSON 可解析；Backend 14 项测试与 `compileall` 通过；Web 运维前端构建通过。未执行微信开发者工具视觉走查或真实 PostgreSQL/EMQX/P4 三端联调。
- 备份：`tmp/backups/manual/20260727-172358_other_sync_pages/`。

----
完成时间：2026-07-27 18:09:12 +08:00

## P4 Agent 权限模式接入

- 设置中心“Agent 权限”接入确认、标准、自动三档并写入设备 NVS；重启后恢复，NVS 写入在设置后台任务执行，不阻塞 LVGL。
- 确认模式仅自动执行只读工具；标准模式自动执行普通写入，删除、清空、密钥等 DANGEROUS 工具仍显示确认；自动模式在设置页二次风险确认后放行全部工具，等同完全访问权限。
- Agent Runtime、连续语音、对话页和本地 Tool Broker 共用同一判定，避免前端模式与实际执行策略不一致；对话页状态标签随设置模式更新。
- 验证：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\\idf.ps1 build` 通过；构建继续报告既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，未在本轮调整分区。未烧录实机。
- 备份：`tmp/backups/manual/20260727-143900_agent_permission_settings/`、`tmp/backups/before_run/20260727-180800_agent_permission_settings_build/`、`tmp/backups/manual/20260727-180912_agent_permission_progress/`。

----
完成时间：2026-07-27 18:40:39 +08:00

## 拍照上传崩溃、语音会话占用与 P4 TTS 修复

- 修复拍照上传蓝屏重启：拍照工作任务使用 PSRAM 栈时不再读取 NVS/Flash，改由 UI 内部栈预加载 AI 配置和网络状态，再把配置快照传入图像分析任务，规避 Flash cache 暂停期间的外部栈完整性断言。
- 修复两个语音入口误报“麦克风被其他内容占用”：兼容 Agent Runtime 的大写终态事件，并在启动、投递、取消和提交失败路径统一释放手动语音会话，避免失败后会话锁残留。
- 为 P4 USB 协议补齐 `tts_play`、`tts_status`、`tts_stop`，复用板载 ES8311 + NS4150B 播放链路；完善 MOSS 流式请求、HTTP EAGAIN/TLS 提前关闭兼容、并发保护、停止标志和 WebUI 所需状态字段。
- 验证：ESP-IDF 固件构建通过，WebUI `npm run build` 通过；仍有既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，本轮未调整分区。
- 实机状态：`app-flash` 因 COM17 被其他程序占用而在写入前失败，设备固件未改变；需释放 Web Serial、monitor 或串口助手后再烧录并回归拍照上传、两个语音入口、WebUI TTS 和本地对话 TTS。
- 备份：`tmp/backups/manual/20260727-183207_camera_voice_tts_fix/`、`tmp/backups/before_run/20260727-183730_camera_voice_tts_build/`、`tmp/backups/before_run/20260727-183855_camera_voice_tts_app_flash/`。
- 补充修复图片识别状态可见性：CTX-04 现在持续展示正在拍摄、照片已就绪、正在识别、识别完成和识别失败；失败摘要保留到重拍或再次上传，不再只显示短暂 Toast。
- 放宽 OpenAI-compatible 图片回复的非业务性格式差异：兼容 Markdown 代码围栏、单层 `reply/content/result/data` 包装、`items/ingredients` 别名、字符串 schema、数字数量及 0~100 置信度；任务类型错误、无候选或缺少食材名仍会明确拒绝。`needs_confirmation` 不再由模型字段决定，设备始终强制进入 INV-04 人工确认。
- 实机 `app-flash` 已成功写入并完成 Hash 校验，NVS 未擦除；启动后 ESP32-P4 rev1.3、32MB PSRAM、C6 Wi-Fi、MIPI UI 和 OV5647 高清预览均正常，无本轮新增 panic/watchdog。固定截屏在相机占用 DPI 帧缓冲时按设计返回 `ESP_ERR_INVALID_STATE`，未绕过稳定门重复截图。
- 补充备份：`tmp/backups/manual/20260727-185532_camera_ai_status_parser/`、`tmp/backups/before_run/20260727-185928_camera_ai_status_app_flash/`、`tmp/backups/manual/20260727-191206_camera_ai_status_progress/`。
- 修复连续语音进入录音时偶发“语音录音失败”：`audio_record` 改为开机早期预创建的常驻 worker，任务栈放入 PSRAM，避免 WakeNet/ASR/TTS 加载后再申请连续内部栈，同时保留内部 DMA SRAM 给 I2S、SDMMC 和 WakeNet。
- 实机回归：WakeNet 加载后未再出现 SDMMC 512B 分配失败或新增 task watchdog；暂停 WakeNet 后录音 2.032 秒、65024 字节，启动/停止成功且 `timeoutCount=0`。内部栈试验曾复现 DMA 内存耗尽并触发 watchdog，已弃用且未保留在最终代码。
- 补充备份：`tmp/backups/manual/20260727-193309_audio_record_worker/`、`tmp/backups/manual/20260727-193933_audio_record_psram_stack/`、`tmp/backups/before_run/20260727-193951_audio_record_psram_build/`、`tmp/backups/before_run/20260727-194022_audio_record_psram_app_flash/`。

----
完成时间：2026-07-27 18:45:54 +08:00

## P4 Agent 库存问答统一事实源与 LLM 回答

- 修复“库存有什么”只显示“已分页读取”的问题：本地读取工具结果现在会作为可信 `items` 上下文交给已配置 LLM，模型负责生成简短自然回答，而不是向用户暴露工具执行摘要。
- 修复该链路引入的 Core 1 Stack protection fault：含完整 `data_json` 的工具结果改为 PSRAM 分配，不再占用 Agent 主任务栈。
- 统一库存事实源：`inventory.list` 由旧 TF `agent/data/inventory.json` 改为读取库存页实际使用的 LittleFS `ui_inventory.json`，并转换为稳定的 `quantity/expireDate/location/daysLeft` 字段。
- 实机验证：COM17 `app-flash` 写入并 Hash 校验通过；联网 `eric_2G` 下询问“现在库存里有什么？请简短回答。”返回鸡蛋、鲜牛奶、原味酸奶、番茄、菠菜、剩米饭、卤牛肉和速冻水饺，并正确提示过期/临期；HTTP 200，模型耗时 10641ms，无重启。
- 构建仍报告既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，本轮未调整分区。
- 备份：`tmp/backups/manual/20260727-182838_inventory_llm_answer/`、`tmp/backups/manual/20260727-183625_inventory_llm_stack_fix/`、`tmp/backups/manual/20260727-184034_inventory_single_source/`、`tmp/backups/before_run/20260727-184206_inventory_single_source_flash/`。

----
完成时间：2026-07-27 20:14:08 +08:00

## WakeNet/语音并存后的相机 DMA 稳定性修复

- 将 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 从 128KiB 提高到 160KiB，为 CSI、SDMMC、I2S 和 WakeNet 保留内部 DMA SRAM；实机启动时最大连续内部 DMA 块为 155648 字节，WakeNet 加载后相机页仍可正常启动 1280×960 双缓冲预览。
- 高清抓拍在访问 OV5647 共享 I2C 和申请 1920×1080 CSI 帧缓冲前，统一暂停 WakeNet、Realtime 和普通录音；清理后再恢复语音策略，避免 GPIO28/29 共享总线及 DMA 资源并发争抢。
- `VIDIOC_REQBUFS` 失败日志补充 `errno`、内部 DMA 与 PSRAM 的空闲量/最大连续块，后续可区分内部 DMA 耗尽和 PSRAM 碎片，而不是只显示笼统错误。
- 曾验证单 CSI 缓冲会稳定返回 `errno=116 (ETIMEDOUT)`，说明当前 P4 1080p 链路要求双缓冲；该试验已完整撤回，最终固件继续使用双缓冲。
- 实机真实 UI 回归通过：进入相机页后预览正常，触发拍照生成 1920×1080、456065 字节 JPEG，耗时 701ms；诊断计数保持 `panic=487`、`task_wdt=66`，本轮没有新增崩溃、看门狗或 SDMMC 分配错误。
- `app-flash` 已写入 COM17 并完成 Hash 校验。构建仍报告既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，本轮未调整分区。
- 备份：`tmp/backups/manual/20260727-195019_camera_dma_160k_voice_gate/`、`tmp/backups/before_run/20260727-195058_camera_dma_160k_build/`、`tmp/backups/before_run/20260727-195308_camera_dma_160k_app_flash/`、`tmp/backups/manual/20260727-200818_camera_restore_csi_double_buffer/`、`tmp/backups/before_run/20260727-200849_camera_dma_160k_final_build/`、`tmp/backups/before_run/20260727-200926_camera_dma_160k_final_app_flash/`、`tmp/backups/manual/20260727-201408_camera_dma_progress/`。

----
完成时间：2026-07-28 21:51:56 +08:00

## P4 Agent 记忆统一与默认上下文注入

- 停用旧 LittleFS `/cache/memory_summary.json`、NVS `fridge_store/memory` 和 `MEMORY_OP` 隐藏指令；长期记忆唯一权威改为 TF `agent/data/memory.json + agent/MEMORY.md`。旧文件和 NVS key 不再访问但未物理删除，避免误删升级前真实偏好，且旧演示种子不会自动污染新记忆。
- Runtime 每次调用云模型前默认注入紧凑 `memory_context`：最近最多 16 条结构化记忆和 `MEMORY.md` 首页 768 字节；超出上下文缓冲时按最旧优先裁剪，完整内容仍可通过 `memory.search` / `memory_md.read` 查询。
- 修复本地记忆问答只返回“已分页读取”的问题：`memory_md.read` 的真实 `data.content` 进入可信 Tool Result，再由模型生成用户可见回答。
- 删除未实现的 `session_ttl_hours` 配置；现有 TF 配置会在初始化时移除该字段。短期会话只统计 user/assistant，问题和最终回答各算一条，最多 24 条；Tool/Result 不占额度，并随过期 run 一并清理。
- 清理 storage、旧 USB、voice_session、ai_context、kitchen_ai 和 Web Mock 中的旧记忆接口；`ai_context` 保留库存、提醒、偏好和旧短期历史职责，但默认读取新的 TF 记忆。
- 验证：ESP-IDF 5.5.1 固件构建和 Web `npm run build` 均通过；COM17 `app-flash` 写入 3396608 字节并完成 Hash 校验，NVS/TF 未擦除。实机 `agent_storage_status` 正常，权限仍保持用户设置的 `auto`。
- 实机回归：历史物理记录为 user=12、assistant=12、tool/result 各若干，对话消息稳定封顶 24；询问“结构化长期记忆里的第1条内容”正确回答“这是第1条”，证明默认 `memory_context` 已注入，模型步骤上下文约 9 KiB。构建仍报告既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，本次未调整分区。
- 备份：`tmp/backups/manual/20260728-214157_unify_agent_memory/`、`tmp/backups/before_run/20260728-214908_memory_build/`、`tmp/backups/before_run/20260728-215410_memory_final_build/`、`tmp/backups/before_run/20260728-215544_memory_app_flash/`。

----
完成时间：2026-07-28 22:29:00 +08:00

## P4 Agent 记忆 Harness 语义修复

- 将默认模型上下文改为 `user_memory`，只注入最近最多 16 条 `agent/data/memory.json` 结构化用户事实；`MEMORY.md` 不再默认注入，也不再被模型误当成用户长期记忆或系统指令。
- 修正本地记忆路由：普通“记忆/偏好”查询执行 `memory.search`；只有明确提到 `MEMORY.md`、“记忆笔记”或“家庭笔记”时才执行 `memory_md.read`。Agent 协议同步声明两类结果的字段和使用边界。
- 新增危险级、幂等的 `memory.delete` 工具；通过 Tool Broker 删除指定 id，不绕过用户现有权限模式。旧内置 `MEMORY.md` 只有逐字未修改时才迁移成中性家庭笔记模板，用户编辑过的文件保持不变。
- 实机清理测试数据：已删除 `mem_845561991_1`（内容“这是第1条”），随后 `memory.search` 返回 0 条；权限模式仍为用户设置的 `auto`。
- 实机回归：普通长期偏好问题的 Run 明确调用 `memory.search` 并回答当前未保存偏好；`MEMORY.md` 问题的 Run 明确调用 `memory_md.read`，只回答中性笔记为空。两次模型请求均为 HTTP 200，端到端耗时约 9.0 秒和 15.3 秒。
- 验证：ESP-IDF 5.5.1 固件构建和 Web `npm run build` 均通过；COM17 `app-flash` 写入 3398656 字节并完成 Hash 校验。构建仍报告既有 recovery `ota_1`（2MiB）小于主镜像的容量告警，本次未调整分区。
- 备份：`tmp/backups/manual/20260728-222114_fix_memory_harness/`、`tmp/backups/before_run/20260728-222358_memory_harness_build/`、`tmp/backups/before_run/20260728-222521_memory_harness_app_flash/`。

----
完成时间：2026-07-28 22:40:00 +08:00

## P4 Agent 结构化记忆直接回答优化

- 普通“记忆/偏好”问题不再预执行 `memory.search`，直接使用本轮已经注入的 `user_memory.items` 进入首次模型调用；`truncated=false` 时协议明确禁止模型重复搜索。
- 明确要求全部、所有、完整、历史、更早、最早、搜索、查找或指定“第 N 条”的问题仍预执行 `memory.search`；`MEMORY.md`、记忆笔记和家庭笔记仍独立执行 `memory_md.read`。
- 实机路由验证：普通偏好问题的 `run_000047` 和联网后的 `run_000049` 会话中只有 user/assistant，没有 Tool/Result；“所有记忆”问题的 `run_000048` 仍正确生成 `memory.search`，本地检索耗时 15ms。
- 联网回归：普通偏好问题 HTTP 200，端到端 4999ms、模型 4188ms，正确回答当前没有结构化长期偏好；相比上一版同类实测约 9.0 秒，本次单次结果更快，但云模型波动较大，不能把约 4 秒差值全部归因于移除 10~15ms 本地搜索。
- 验证：ESP-IDF 5.5.1 构建通过；COM17 `app-flash` 写入 3399680 字节并完成 Hash 校验。权限和结构化记忆内容未修改。既有 recovery `ota_1`（2MiB）容量告警仍在，本次未调整分区。
- 备份：`tmp/backups/manual/20260728-223510_memory_direct_context/`、`tmp/backups/before_run/20260728-223550_memory_direct_context_build/`、`tmp/backups/before_run/20260728-223646_memory_direct_context_app_flash/`。

----
完成时间：2026-08-03 21:15:00 +08:00

## HOME-01 首页竖屏隐私模式移植

- 完成 `ui-reference/ver2` HOME-01 竖屏隐私正文：600×672 主区、64px 导航、288px 隐私状态舱；新增标题/日期/搜索/快捷添加、2×2 统计卡、单列临期优先、推荐、Agent 建议、今日菜单、购物清单和同步信息。HOME-02 临期详情继续保留。
- 竖屏白名单收敛为 `home` 与 `home_detail`；两页可在竖屏隐私模式内直接互切，并在切页后重新绑定对应 PPA 滚动缓存。其它页面保持横屏，固件和 WebUI 均提示尚未移植，不自动改跳 HOME-02。
- 修复 LVGL DIRECT 双缓冲横竖屏切换的残留同步区域问题：切换分辨率/缓冲前先完成当前刷新，避免旧 1024px 横屏脏区进入 600px 竖屏缓冲造成负宽度崩溃。补充标题右侧标签宽度兜底，最终实机可见“全部 7 项 >”。未修改 MIPI 时序、GPIO、电源或摄像头路径。
- 验证：ESP-IDF 5.5.1 包装脚本 `build`、COM17 no-stub flash、Web `npm run build` 均通过；最终 HOME-01 与 HOME-02 固定截图均完成 ready/resourceReady/framebuffersSynced/pageFrameReady 稳定门、BMP 读回和 CRC32 校验。最终 HOME-01 PPA 缓存 `600×1268 + 80px elastic`，HOME-02 为 `600×1668 + 80px elastic`。
- 实机最终启动日志：ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`；最终两次截图启动的 `panic=491`、`task_wdt=66` 均未增加，未见本轮新增 brownout、PSRAM 初始化失败、WDT 或反复重启。既有 coredump 分区 `ESP_ERR_INVALID_CRC` 与 recovery `ota_1` 2MiB 容量警告仍保留，未在本功能中扩大范围处理。
- 最终截图：`tmp/cache/portrait_home/device_home01_portrait_final_heading.bmp`、`tmp/cache/portrait_home/device_home02_portrait_final_heading.bmp`；设计验收记录见根目录 `design-qa.md`。
- 备份：`tmp/backups/manual/20260803-200145_portrait_home/`、`tmp/backups/manual/20260803-203500_portrait_home_panic_fix/`、`tmp/backups/manual/20260803-204500_portrait_home_lvgl_transition/`、`tmp/backups/manual/20260803-211500_portrait_home_unsupported_toast/`、`tmp/backups/manual/20260803-213000_portrait_home_docs/`、`tmp/backups/manual/20260803-213500_portrait_home_heading_fix/`，以及对应 `tmp/backups/before_run/` 预备份。
- 补充修复（2026-08-03）：按 ver2 菜谱卡将竖屏“15分钟 · 简单 · 库存覆盖 90%”合并为一行，把“食材齐全”上移至底部安全区；保持 96px 菜谱卡、理由区、按钮位置和 PPA 内容高度不变。重新构建、COM17 刷写和 HOME-01 竖屏稳定门/截图读回通过；截图为 `tmp/cache/portrait_home/device_home01_recipe_fix_top.bmp`。备份：`tmp/backups/manual/20260803-213138_portrait_home_recipe_card/`、`tmp/backups/manual/20260803-213723_portrait_home_recipe_card_progress/`、`tmp/backups/before_run/20260803-213204_portrait_home_recipe_card_build/`。

----
完成时间：2026-08-03（CHAT 竖屏实现阶段）

## CHAT-01～04 对话页竖屏隐私模式移植

- 固件 `components/ui_p4` 新增 CHAT-01～04 的竖屏 Shell：保持 `600×672 + 64 + 288` 几何，CHAT-01 使用 568px 单列消息流、工具/结果卡、固定输入区和完整拼音 IME；CHAT-02～04 使用当前 foreground 模型派生会话历史、工具详情和待确认行动。
- CHAT-02/03/04 在竖屏保留真实页面键并重新绑定 PPA；返回 CHAT-01 不恢复横屏。横屏仍将三个二级键兼容映射到 CHAT-01；对 Runtime 未提供的多会话、参数、业务对象、执行日志和修改动作展示中性空态/提示，不注入 ver2 mock 数据。
- 页面切换、消息卡、工具卡、结果卡和确认按钮沿用统一 8px 拖动取消保护；IME 展开时暂停 PPA，关闭后恢复消息视口缓存。未修改 MIPI 时序、GPIO、电源或摄像头路径。
- WebUI 与 MockTransport 的竖屏白名单增加 `chat`、`chat_history`、`chat_tool_detail`、`chat_pending_actions`，未移植页面仍保持横屏并提示；`doc/portrait_privacy_mode.md`、`design-qa.md` 已同步记录边界和验收项。
- 验证：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build`、COM17 no-stub flash/Hash 校验均通过；应用构建仍有既有 `ota_1` 2MiB recovery 槽容量不足告警，本次未修改分区。四页固定入口均完成 `ready/resourceReady/framebuffersSynced/pageFrameReady` 连续两次稳定门、BMP/CRC32/文件头检查和 `view_image` 实际查看：`tmp/cache/portrait_chat/chat_final.bmp`、`chat_history_final.bmp`、`chat_tool_detail_final.bmp`、`chat_pending_actions_final.bmp`。
- PPA：最终 CHAT-01 缓存为 `600×2874 + 80px elastic`、探针约 12.9ms；CHAT-03 为 `600×992`、约 13.9ms；CHAT-04 为 `600×772`、约 14.0ms；CHAT-02 正文小于视口，完成竖屏 PPA 重绑定并安全不申请缓存。截图实际检查确认四页均保持 600×672 主区、64px 导航、288px 隐私状态舱，摄像头关闭态无越界。
- 启动健康：最终多次启动均为 ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`；`panic=491`、`task_wdt=66` 未在本轮增加，未见新增 Guru Meditation、WDT 或反复重启。既有 coredump `ESP_ERR_INVALID_CRC`、I2C NACK 和 recovery 槽告警仍存在，未归因于本次 UI。
- 交互边界：消息/工具/结果/确认控件已复用 8px 拖动取消保护，IME 展开/收起已实现 PPA 暂停与重建；当前 USB 仅有 `touch_status/touch_scan` 诊断，没有触摸注入能力，因此真实手指拖动、抬手点击和 IME 展开收起仍待现场手动验收，未冒充为已完成。
- 滚动修复（2026-08-03）：定位到竖屏 CHAT 的透明固定层由 `lv_obj_create()` 默认继承 `CLICKABLE`，覆盖在 `portrait_scroll` 上方并截获消息区触摸；已清除该层的 `CLICKABLE` 标志，使空白消息区事件穿透到 PPA 滚动容器。修复版已通过 IDF 构建、COM17 烧录、稳定门和 `tmp/cache/portrait_chat/chat_scroll_fix.bmp` 截图回归，未修改硬件路径。
- 备份：`tmp/backups/manual/20260803-220049_portrait_chat/`、`tmp/backups/manual/20260803-224420_portrait_chat_header/`、`tmp/backups/manual/20260803-225533_portrait_chat_verification_docs/`；构建前预备份：`tmp/backups/before_run/20260803-222015_portrait_chat_build/`、`tmp/backups/before_run/20260803-222712_portrait_chat_ppa/`、`tmp/backups/before_run/20260803-224444_portrait_chat_header_build/`。

----
完成时间：2026-08-03（PLAN-01 计划页竖屏实现阶段）

## PLAN-01 计划总览竖屏隐私模式移植

- 固件 `components/ui_p4` 新增 PLAN-01 竖屏正文，保持 `600×672 + 64 + 288` 几何，按 ver2 单列堆叠今日菜单、购物摘要、临期利用计划、本周菜单和 Agent 建议；正文固定约 `1120px` 并留底部安全间距。
- 购物摘要按现有 `plan_business_model_t.shopping[].group` 统计；计划详情、菜谱详情和 Agent 操作没有 Runtime 能力时只显示中性提示，不注入 mock 业务数据。PLAN-02～06 仍为横屏页面。
- 复用唯一 `portrait_scroll` 和竖屏 PPA；透明内容层清除 `CLICKABLE`，卡片入口保留统一 8px 拖动取消规则，7 天条带不嵌套竖向滚动。计划页与 HOME/CHAT 竖屏切换后重新绑定滚动缓存。
- 固件、WebUI 与 MockTransport 已将 `plan` 加入竖屏白名单；当前 `plan` 切换方向保持页面不变，未移植页面继续提示并保持横屏兼容逻辑。未修改 MIPI、GPIO、电源或摄像头路径，隐私状态舱摄像头保持关闭。
- 备份：`tmp/backups/manual/20260803-235740_portrait_plan/`；构建/烧录前预备份待本轮执行。
- 验证：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build`、COM17 no-stub flash/Hash 校验均通过；固定截图入口完成 `plan + portrait_privacy` 的页面切换、方向切换、两次稳定门、BMP/CRC32/文件头检查和 `view_image` 查看。PPA 缓存为 `600×1132 + 80px elastic`，探针约 15.8ms。
- 实机首屏检查：标题、PLAN-01 标识、今日菜单、购物摘要、临期卡首行、计划导航高亮和 288px 隐私状态舱均在界内，摄像头关闭态正常；日志确认 32MB PSRAM memory test OK、`brownout=0`，未见本轮新增 Guru Meditation、WDT 或反复重启。既有 coredump CRC、I2C NACK 与 `ota_1` 容量告警仍存在。
- 触摸回归：代码已清除透明内容层 `CLICKABLE` 并接入 8px 拖动取消保护；固定 USB 仅提供触摸诊断、没有触摸注入，因此真实上下滑动、中后段画面和抬手点击留待现场手动验收。
- 构建/烧录前备份：`tmp/backups/before_run/20260804-000741_portrait_plan_build/`、`tmp/backups/before_run/20260804-000835_portrait_plan_flash/`。
- 购物摘要裁切修复（2026-08-04）：将两枚统计卡高度从 `108px` 增至 `116px`、购物卡从 `168px` 增至 `176px`，避免“待购买/建议购买”文字贴住或被下边框裁切。修复版重新构建、COM17 烧录，并通过 `plan + portrait_privacy` 两次稳定门、PPA `600×1132 + 80px elastic`、BMP/CRC32/文件头检查；截图为 `tmp/cache/portrait_plan/plan_portrait_shopping_fix.bmp`。新增备份：`tmp/backups/manual/20260804-001736_portrait_plan_shopping_fix/`、`tmp/backups/before_run/20260804-001755_portrait_plan_shopping_fix_build/`、`tmp/backups/before_run/20260804-001846_portrait_plan_shopping_fix_flash/`。

----
完成时间：2026-08-04（MORE 页面竖屏实现阶段）

## MORE-01/02/07/08 更多页竖屏隐私模式移植

- 固件 `components/ui_p4` 新增 MORE-01 更多入口、MORE-02 定时器、MORE-07 闹钟和 MORE-08 秒表的独立竖屏对象树，保持 `600×672 + 64 + 288` 几何；MORE-03 编辑页及 MORE-04～06 设置/维护页继续不支持竖屏。
- MORE-01 使用 ver2 双列入口卡，时间工具和系统维护入口均保留；定时器/闹钟角标、Wi-Fi/TF/时间同步摘要只使用真实快照和状态，不注入 SSID、容量、安全告警或虚构业务记录。定时器预设为 3×2，闹钟保留真实固定开关和周期空态，秒表保留开始/暂停、复位和计次列表。
- 四页共用唯一 `portrait_scroll`；透明内容层清除 `CLICKABLE`，卡片和按钮沿用 8px 拖动取消、`PRESS_LOST`、`SCROLL_BEGIN/SCROLL` 保护；定时器/秒表使用动态数字覆盖，页面、方向和异步刷新后重新绑定 PPA。未支持的入口留在当前页提示，USB 直接切换未移植页继续先恢复横屏。
- WebUI 与 MockTransport 的竖屏白名单增加 `more`、`timer`、`alarm`、`stopwatch`；导航高亮统一为“更多”，工具页互返和 HOME/CHAT/PLAN 互切保持竖屏。未修改 MIPI、GPIO、电源、摄像头和显示时序，隐私状态舱摄像头继续关闭。
- 备份：编辑前 `tmp/backups/manual/20260804-003433_portrait_more/`；运行前 `tmp/backups/before_run/20260804-005326_portrait_more/`、`tmp/backups/before_run/20260804-010137_portrait_more_id_fix/`。
- 验证：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build`、COM17 no-stub flash/Hash 校验均通过；四个固定入口均完成 `ready/resourceReady/framebuffersSynced/pageFrameReady` 连续两次稳定门、BMP/CRC32/文件头检查和 `view_image` 实际查看：`tmp/cache/screenshot_test/portrait_more_final.bmp`、`portrait_timer_final.bmp`、`portrait_alarm_final.bmp`、`portrait_stopwatch_final.bmp`。
- 实机结果：四页均保持主区、64px 导航和 288px 隐私状态舱不越界，MORE-01 双列卡、MORE-02 3×2 预设、MORE-07 真实闹钟/周期空态、MORE-08 秒表控制/计次空态均可见，最终标题和页面 ID 胶囊无重叠。启动日志未见本轮新增 brownout、PSRAM 初始化失败、WDT、Guru Meditation 或反复重启；既有 `ota_1` 容量告警、coredump CRC 和 I2C NACK 保持原样。
- 交互说明：代码层已接入 PPA 滚动和动态覆盖，固定 USB 诊断没有触摸注入能力，真实手指拖动超过 8px、抬手点击、长列表手感和活动倒计时/秒表现场回归仍待补，不冒充为已完成。

----
完成时间：2026-08-04（MORE-04～06 设置页竖屏实现阶段）

## MORE-04～06 设置页竖屏模式恢复

- 固件 `components/ui_p4` 新增 `FRIDGE_UI_P4_PAGE_SAFETY`（稳定页面键 `safety`，MORE-04），并恢复 `settings`（MORE-05）和 `maintenance`（MORE-06）的独立竖屏分区对象树；三页保持 `600×672 + 64 + 288` 几何、MORE 导航高亮和竖屏互切。
- MORE-04 使用设备概览、连接状态、传感器、厨房安全四个分区；MORE-05 使用网络与同步、AI Provider 与模型、Agent 权限、语音与显示、隐私与家庭五个分区；MORE-06 使用存储、插件、日志、系统更新、关于五个分区。MORE-04 读取现有设备/网络/TF/SHT4x/P4 雷达快照；S3 legacy 光敏/IMU 链路不进入 P4 最小闭包，缺少适配时明确显示“未接入”。真实快照之外的安全事件、插件、OTA、导出和绑定能力均保持中性提示。
- 三页正文统一采用 `150px` 左侧分区栏、`12px` 间距、约 `406px` 右侧内容和唯一 `portrait_scroll`；分区文字使用竖屏小字号，右侧取值列加宽并固定单行省略，卡片/按钮保留 8px 拖动取消规则。Wi-Fi 弹层打开时暂停 PPA，关闭后重绑并失效缓存。
- WebUI 与 MockTransport 已将 `safety`、`settings`、`maintenance` 加入竖屏白名单；支持页进入竖屏保持当前页面，MORE-03 及其它未移植页继续恢复横屏并提示。未修改 MIPI、GPIO、电源、显示时序和摄像头路径，隐私模式摄像头保持关闭。
- 依赖稳定性修复：P4 的 `display_mipi`、P4 camera 和 `sht4x` CMake 依赖分别收窄为 `esp_driver_gpio`、`esp_driver_jpeg`、`esp_driver_i2c`，移除 P4 路径对旧 `driver` 组件的整体依赖；未修改源代码中的 GPIO、MIPI、共享 I2C、摄像头或电源路径。该修复消除了启动阶段 legacy I2C 与 driver_ng 冲突。
- 备份：编辑前 `tmp/backups/manual/20260804-020000_portrait_settings/`；I2C 依赖修复前 `tmp/backups/manual/20260804-031500_portrait_settings_i2c_fix/`、`tmp/backups/manual/20260804-040000_p4_i2c_dependency_fix/`；最终烧录前 `tmp/backups/before_run/20260804-041500_portrait_settings_final_before_run/`。
- 构建与实机：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build`、COM17 no-stub 烧录和 Hash 校验均通过；固定入口完成 `safety/settings/maintenance + portrait_privacy` 三页稳定门、BMP/CRC32/文件头检查和 `view_image` 查看。截图分别为 `tmp/cache/screenshot_test/portrait_safety_final.bmp`、`portrait_settings_final.bmp`、`portrait_maintenance_final.bmp`。
- 结果：三页主区、64px 导航、288px 隐私状态舱及分区/正文无裁切；启动日志确认 ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`，未见本轮新增 Guru Meditation、WDT 或反复重启。既有 coredump `ESP_ERR_INVALID_CRC`、`ota_1` 容量告警保留；维护页 TF 读回出现一次 I2C transaction NACK，但传输最终成功。真实手指滚动、分区切换、Wi-Fi 弹层和亮度滑块仍待现场手动回归。
- 间距修复复验（2026-08-04）：竖屏左栏 `180→150px`、分区间距 `16→12px`、右栏约 `372→406px`、右侧取值列 `104→144px`、行高 `86→94px`；修复版三页截图为 `tmp/cache/screenshot_test/portrait_safety_spacing_fix.bmp`、`portrait_settings_spacing_fix.bmp`、`portrait_maintenance_spacing_fix.bmp`，经 `view_image` 确认右侧长值和说明不再重叠。新增备份：`tmp/backups/manual/20260804-050000_portrait_settings_spacing_fix/`、`tmp/backups/before_run/20260804-051500_portrait_settings_spacing_fix/`。

----
完成时间：2026-08-07 15:50:57 +08:00

## ESP32-P4 MPU6050 共享 I2C0 迁移与 WebUI 恢复

- 完全替换主线 `components/sensors` 的 S3 实现：删除 GPIO1 光敏 ADC、GPIO4/5 legacy I2C、`driver/i2c.h`、旧跨组件总线锁和雷达汇总，新增 P4 专用 `fridge_mpu6050_p4.c`。旧 S3 固件仍可从 `s3-legacy` / `s3-final-20260702` 获取，不再进入当前主线。
- 新驱动复用 GT911 唯一创建的 I2C0（SCL GPIO16、SDA GPIO17），MPU6050 使用 3.3V、AD0=GND/0x68、INT 悬空、100kHz device handle；兼容启动阶段探测 0x69，但 `WHO_AM_I` 固定校验 0x68。驱动禁止创建、删除或复位共享总线。
- 初始化按官方 Motion Driver/寄存器手册执行：100ms 上电等待、硬复位、X 轴陀螺 PLL、42Hz DLPF、50Hz 采样、±2g、±250dps，并读回关键寄存器；采样从 0x3B 一次 burst 读取 14 字节。连续三次事务失败后标记离线并以 1Hz 慢速恢复，不影响 GT911/SHT40。
- 姿态变化改为相对首次有效安装姿态的角差，避免竖直门体被旧 `abs(pitch)+abs(roll)` 长期误判；震动继续使用 `abs(|a|-1g)` 去除静态重力。
- P4 main、minimal USB 与本机 MORE-04 安全页接入真实快照；USB ready 能力新增 `get_sensors`，返回总线/引脚、错误名、失败次数、样本计数、六轴、温度和姿态。WebUI 恢复约 300ms 轮询，未迁移的光敏/PIR/门状态改为 `--`，不再返回或展示伪造零值；Mock 引脚与快照同步为 P4 语义。
- 共享总线生命周期修复：GT911 运行期 deinit 只移除自身 device handle，不再删除仍挂 SHT40/MPU6050 的 I2C0。
- 验证：Web `npm run build` 通过；ESP-IDF 5.5.1 串行构建通过（并行首次在既有大型 `fridge_ui_tools.c` 编译单元资源性退出，无代码诊断）；最终镜像已通过 COM17 `--no-stub` 烧录与 Hash 校验。构建仍报告既有 recovery `ota_1` 2MiB 小于主镜像的容量告警，本轮未改分区。
- 实机启动：GT911=0x14、SHT40=0x44、MPU6050=0x68/WHO_AM_I=0x68/50Hz，`get_sensors` 连续三次样本计数 65→90→116。30 秒共享总线压力测试完成 91 次 MPU 快照与 27 次 GT911 状态读取，全部成功；MPU 全程 `ESP_OK`、连续失败最大值 0、样本计数 9620→11102。期间出现 1 次项目既有 SHT40 receive NACK，未影响 MPU/触摸，本轮不扩大范围修改 SHT40。
- 健康状态：本次启动 `brownout=0`，未见新增 Guru Meditation、Stack protection、watchdog 或反复重启。验证日志：`tmp/logs/mpu6050_verify_20260807.log`、`tmp/logs/mpu6050_shared_i2c_stress_20260807.log`。
- 硬件后续：野火模块 SDA/SCL 各自带 2.2kΩ 上拉；与 GT911/SHT40 并联后仍需断电实测等效上拉并确认只上拉到 3.3V。状态机继续保持未启用，须按真实冰箱门安装方向与零偏重新标定后再接回。
- 备份：`tmp/backups/manual/20260807-152904_p4_mpu6050_port/`、`tmp/backups/manual/20260807-153903_p4_mpu6050_mock_fix/`、`tmp/backups/before_run/20260807-153823_p4_mpu6050_verify/`、`tmp/backups/before_run/20260807-154356_p4_mpu6050_flash/`。
- 最终复审收口：运行期 `touch_scan` 改为线程安全的 `i2c_master_probe()`，不再并发 add/rm 临时 device；I2C 错误重试退避改为一个真实 10ms tick；MPU 任务创建失败会有界重试移除自身 device，人工重试不再重复复位配置；采样任务改用 `vTaskDelayUntil()` 固定周期。
- 复审版重新完成串行构建、COM17 no-stub app-flash 与 Hash 校验；实机 10.531 秒内完成 40 次 `get_sensors` 和 10 次并发 `touch_scan`，全部成功，样本计数 118→645，实测 50.04Hz，MPU/GT911 均为 `ESP_OK`、MPU 连续失败数为 0。
- 复审增量备份：`tmp/backups/manual/20260807-155929_p4_mpu6050_review_fixes/`、`tmp/backups/before_run/20260807-160209_p4_mpu6050_review_verify/`、`tmp/backups/before_run/20260807-160256_p4_mpu6050_review_flash/`；前序最终构建/烧录备份：`tmp/backups/before_run/20260807-155426_p4_mpu6050_final_verify/`、`tmp/backups/before_run/20260807-155520_p4_mpu6050_final_flash/`。

----
完成时间：2026-08-08（FY-SP703 16k 时基与 Web 分块回放修复阶段）

## FY-SP703 录音速度、回放丢包与按钮忙碌态修复

- 用户原始 WAV `ef3017ff-e107-492f-8512-7413991a51cc.wav` 为 16k/16-bit/mono、2.149s；按 3 倍拉长后时长 6.448s，验证当前 FY 模组真实输出约 16k，而旧链路仍按 48k 数据执行 3:1 FIR 抽取，造成约 3 倍加速、升调和高频永久损失。
- FY 公共 PCM、状态、缓冲、WAV 和 ASR 元数据统一为 16k；当前 16k 路径直接通过，只有格式明确为 48k 的兼容路径才执行 3:1。I2S1 仍为 slave RX、32-bit Philips 双时隙，BCLK/WS/DIN 均为输入；接收器内部时序单独沿用此前已验证有声的 48k+APLL 配置，不反向输出时钟也不改变外部 16k LRCK 帧率。
- 定位到 `mic_record_wav` 把约 64～96KB WAV 编成一条超长 JSON：USB Serial/JTAG 默认 no-driver VFS 在 TX FIFO 长时间阻塞时会静默丢字节。实测旧响应声明 96044B、Base64 应为 128060 字符，实际仅 121931 字符，导致浏览器无 `<audio>` 或等待 120s。
- 新增 `mic_record_wav_info` / `mic_record_wav_chunk` / `mic_record_wav_end`：固件先生成不可变 WAV 快照并返回 session 与整文件 CRC，Web 再以 1024B 原始数据逐块请求，校验 session、offset、length、IEEE CRC32、eof 和整文件 CRC 后组装，最后显式释放快照；即使跨页面插入其它音频操作，也不会混合两次录音。预览固定截取前 2 秒，不能用其总时长判断是否压速。旧 `mic_record_wav` 暂保留兼容，但当前 Web 不再使用。
- Web 将停止录音与回放传输拆成 `voiceBusy` / `micPlaybackBusy`：停止响应后立即解除主录音忙碌态，读取期间开始按钮显示“正在读取回放”，刷新按钮保持可用；分块失败会立即报告具体校验错误，不再把坏载荷交给 `atob` 或播放器。MockTransport 同步实现相同分块协议。
- 验证：ESP-IDF 5.5.1 构建、Web TypeScript/Vite 构建、COM17 no-stub 烧录与 Hash 校验通过；真机以 63 个分块完整组装 64044B、16k/mono/16-bit、2.000s WAV，所有分块 CRC 通过，日志为 `tmp/logs/20260808-135034_fy_sp703_chunked_com17.log`，文件为 `tmp/cache/audio_review/20260808-135034_fy_sp703_chunked_live.wav`。Web Mock 实测停止后刷新按钮可用，分块完成后 `<audio>` 出现且开始/刷新按钮恢复。
- 快照协议最终复验：COM17 返回 `session=1`，63 块逐块 CRC 与整文件 CRC `1884896806` 全部一致，`mic_record_wav_end` 返回 `released=true`；2.240s 墙钟录音得到 35840 样本/71680B，timeout/overflow 均为 0。日志：`tmp/logs/20260808-140254_mic_wav_snapshot_verify.log`。
- 待现场确认：自动测试环境未能可靠把系统扬声器声音送到 FY 模组，受控录音仍为全零 PCM；需要用户在刷新 Web 页面并重新连接 USB 后，对 FY 近距离说话 2～3 秒，确认 RMS/Peak 非零并试听真实音色。此项未冒充为已完成。
- 主要备份：`tmp/backups/manual/20260808-130559_fy_sp703_16k_rate_fix/`、`tmp/backups/manual/20260808-133532_fy_sp703_silence_web_playback_fix/`、`tmp/backups/manual/20260808-134414_fy_sp703_timing_chunked_wav/`、`tmp/backups/manual/20260808-134638_mic_wav_mock_protocol/`、`tmp/backups/manual/20260808-135755_mic_wav_snapshot_session/`；构建/烧录前备份位于 `tmp/backups/before_run/20260808-131057_fy_sp703_16k_build/` 至 `tmp/backups/before_run/20260808-140057_mic_wav_snapshot_flash/`。

----
完成时间：2026-08-08，FY-SP703 成为 WakeNet/录音/ASR 默认音源阶段：

## FY-SP703 成为 WakeNet、手动录音与 ASR 默认音源

- `components/audio/include/fridge_audio.h`：在 `fridge_audio_source_t` 枚举后新增唯一默认源 `FRIDGE_AUDIO_DEFAULT_SOURCE = FRIDGE_AUDIO_SOURCE_FY_SP703`；更新注释说明无 source 录音默认 FY、显式 ES8311 仍可选作回退。
- `components/audio/fridge_audio_p4.c`：`fridge_audio_start_recording()` 改用默认源；`wake_stream_start` 以默认源为本次源，FY 路径先确保公共初始化再 `fridge_fy_sp703_init/start`，ES 路径保留原 init/start，不做自动回退；保留启动前/后两次 busy 检查，失败按实际源 stop；`CONFIG_FRIDGE_FY_SP703_ENABLE` 关闭时明确返回 `ESP_ERR_NOT_SUPPORTED`；锁内先设 `s_source` 再 `reset_stats_locked()`，启动成功只打一条含 source/sample rate 的日志。
- `wake_stream_read`：锁内只快照 active/source，不在持锁时做 I2S read；FY 用有界 32-bit stereo 帧缓冲（`AUDIO_WAKE_FY_FRAMES=64`，4ms@16k，适配 4096 字节 WakeNet 管理任务栈）逐帧 `raw>>16`，再在短锁内复用已有 `select_fy_sample_locked`（500ms L/R 探测）输出 1 个 mono，绝不越过 `max_samples`；ES 保留原读。统一更新 stats 与 live level，`duration = sample_count*1000/active_sample_rate_locked()`，timeout/error 统计清楚且不忙循环。
- `wake_stream_stop`：快照实际 source 并清 active；FY 先 `finalize_fy_channel_locked()` 再 `fridge_fy_sp703_stop()`，ES 走 `stop_input`；成功置 IDLE、失败置 ERROR，start/stop 清理对称。Realtime 与 speaker 路径未改动，ES8311 speaker/duplex 保持原样。
- `components/wake_word`：陈旧 ES8311 注释改为 FY 16k mono/I2S1 且不占摄像头共享 I2C；`fridge_wake_word_status_t` 新增 `char audio_source[16]`；`get_status` 经 `fridge_audio_get_status()` 取实际 source，取不到时回退 `FRIDGE_AUDIO_DEFAULT_SOURCE`，用 `fridge_audio_source_name()` 填串。模型、帧尺寸、DET_MODE、AFE/VAD/AGC 均未改动。
- `components/usb_protocol/fridge_usb_protocol_minimal.c`：`mic_record_start`（`handle_audio_start`）缺 source/空 source 默认 `FRIDGE_AUDIO_DEFAULT_SOURCE`，显式 `es8311` 与 `fy_sp703`/`fy-sp703` 仍支持，未知值报错；`wake_status` payload 新增 `audioSource`。
- Web：`App.tsx` 初始/复位 `micSource` 改为 `"fy_sp703"`，下拉 FY 标为默认、ES 标为回退，WakeNet 指标区新增“麦克风”显示（读 `wakeStatus.audioSource`）；`types.ts` `WakeStatus` 增加 `audioSource?: MicSource`；`mockData.ts` `createMockWakeStatus` 默认 `audioSource: "fy_sp703"`；`MockTransport.ts` `mic_record_start`/`voice_chat_start` 读取请求 payload 的 source，缺省 FY，并在 `voiceStatus` 回显，显式 ES 可用，WAV 协议未改。
- 验证：`git diff --check` 无空白错误；`rg` 确认剩余 ES 硬编码仅存在于显式选择、Realtime、speaker 与合法分支；Web `npm run build` 通过。未运行 `idf.py build`（由 Codex 统一执行）。
- 硬件未验证项与风险：WakeNet 每 LRCK stereo frame 输出 1 mono 样本、16k/int16/mono 直通，以及 `audioSource=fy_sp703` 的串口/Web 显示，均需真机验收；FY 内部 48kHz RX timing/APLL、GPIO7/8/9、pin/slot/shift 未改；ES8311 speaker 与 Realtime duplex 未动；`AUDIO_WAKE_FY_FRAMES=64` 的分块读取节拍与 4096 栈余量需在真机监控 stack high-water 确认。
- 备份：Codex 已备份至 `tmp/backups/manual/20260808-171742_fy_wakenet_default/`。
- 真机首轮监工复测发现 64 帧 FY 碎片读取会让 WakeNet 消费速率落后并产生 I2S overflow；已改为静态区 256 帧原始 stereo 批量缓冲，mono 直接写 AFE 调用缓冲，既不占 4096 字节 WakeNet 管理任务栈，又减少驱动调用开销。该修正需重新构建/烧录后以 `sampleCount` 增长率和 `overflowCount=0` 复验。
- Codex 最终验收：Web `npm run build` 与 IDF 5.5.1 全量/增量构建均通过，固件已用 no-stub 成功烧录 COM17（ESP32-P4 rev1.3）。无 source 手动录音实际返回 `source=fy_sp703`，2.272s 得到 36352 样本，测得 16000Hz，timeout/overflow 均为 0；等待 UI `ready/resourceReady/framebuffersSynced/pageFrameReady` 连续稳定后，WakeNet 持续处于 `listening`、`audioSource=fy_sp703`，3.030s 增加 48640 样本（约 16052Hz）、PCM 时长增加 3.040s，timeout/overflow 均为 0。测试结束已恢复原 `userEnabled=false`。
- 仍需用户现场验收：自动测试期间无人近距离讲话，FY 的降噪静音门限使 PCM/RMS/Peak 全为 0，因此本轮没有冒充验证“说出小冰小冰可命中”或真实 ASR 文本质量；需用户在 Web 启用 WakeNet 后对模块说唤醒词，并做一轮 ASR 实际讲话测试。

----
完成时间：2026-08-09（P4 光敏 ADC 接入阶段）

## P4 GPIO19 光敏 ADC 接入，软件构建通过，真机待验证

- 在现有 sensors 组件/快照单一数据源内接入光敏模块：GPIO19 = ADC1_CH3（IDF soc/esp32p4/adc_channel.h 与 doc/ESP32-P4扩展板IO口规划.md 确认，P6-32 排针）；接线 VCC→3.3V、GND 共地、AO→GPIO19，AO 输出不得超过 3.3V，严禁 5V 进入 GPIO；本脚只作 ADC 输入，禁止输出、禁止使能内部上下拉。
- ADC 配置为 ADC_UNIT_1/ADC_CHANNEL_3 oneshot、12-bit、12dB 衰减、无校准（业务只需相对亮度）；每约 100ms 连续读 8 次取轻量均值，全部失败保留上次有效值。快照新增 light_ready/light_error/light_raw12bit/light_value10bit/light_percent/light_delta/light_polarity：delta 为相对上次有效值的有符号变化、首次为 0，极性按商品页 raw_high_bright（数值越大越亮）。
- `get_sensors` 增量返回 lightReady/lightRaw12bit/lightValue10bit/lightPercent/lightDelta/lightPolarity/lux/lightError，既有 JSON 字段全部保留；lux 恒等于 lightValue10bit（0..1023 兼容相对值），只是旧 Web 面板回退字段，不是物理 lux，禁止按 lux 解读。Web 无需改动，现有传感器页光敏仪表直接可读。
- 验证：ESP-IDF 5.5.1 包装脚本 `tools\idf.ps1 build` 通过（本改动文件 0 告警），`cd web; npm run build` 通过（零 Web 改动）；构建仍只有既有 recovery `ota_1` 2MiB 容量告警，本次未改分区。
- 未烧录、未上电：极性与 AO 输出范围尚未真机验证，需遮挡/照射复测，必要时用万用表确认 AO 电压不超 3.3V；若实测为反向输出，只需翻转 value10/percent 换算并改 lightPolarity，接口不变。
- 备份：`tmp/backups/manual/20260809-011304_light_sensor_adc/`、`tmp/backups/manual/20260809-012800_light_sensor_progress/`。

----
完成时间：2026-08-09 16:49:21 +08:00（Asia/Shanghai）

## MQ-5 燃气传感器首阶段接入完成（采样/预热/人工老化确认/洁净空气基线，不是安全报警）

- 硬件：模块 5V 供电（P6 SYS_5V，仅到模块 VCC，与主控共地）；AO 经 22kΩ 串联 + 33kΩ 对地 + 100nF 对地滤波接入 GPIO20=ADC1_CH4；DO 本阶段悬空（不定义、不配置任何 DO GPIO）；分压比约 0.6，最坏 5V 输入时节点约 3.0V，严禁 5V 直接进入 GPIO；P6 SYS_5V 整板供电实测留待真机工单。
- 软件：`CONFIG_FRIDGE_MQ5_ENABLE`；光照（CH3）与 MQ-5（CH4）共用唯一 ADC1 oneshot unit，禁止第二次 `adc_oneshot_new_unit`；传感器 owner task 固定 20ms 周期保留 IMU 约 50Hz，光照与 MQ-5 各用 100ms 独立单调时间门控采样；每周期连续读 8 次取整数均值，64 样本滚动窗口约 6.4 秒；日常预热固定 60 秒；NVS namespace `fridge_mq5`、keys `burn_in_ok`/`baseline_raw`（baseline_raw=0 约定为未标定）；六态状态机 DISABLED/ERROR/WARMING_UP/NEEDS_BURN_IN/READY_TO_CALIBRATE/MONITORING；快照新增 14 个 mq5_* 字段；`mq5_reset_calibration` 后 sample_count 归零、窗口清空，从 0 重新累计到 64；三个校准接口统一在快照互斥锁内完成前置校验与 NVS set/erase+commit，commit 成功后才更新 RAM 并释放锁，NVS/RAM 原子一致。
- USB 三命令与确认串：`mq5_confirm_burn_in`/`MQ5_BURN_IN_COMPLETE`、`mq5_capture_baseline`/`CAPTURE_MQ5_CLEAN_AIR`、`mq5_reset_calibration`/`RESET_MQ5_CALIBRATION`；三条命令缺确认串时拒绝执行。
- Web 安全页与 Mock、P4 LVGL 只读显示已接入；raw/value/percent 均为相对 ADC 读数，不输出 ppm、不触发正式报警、不接 MQTT/蜂鸣器。
- 验证：Web `npm run build` exit 0，日志 `tmp/logs/20260809-164559_mq5_review_web_build.log`；固件包装脚本 `tools\idf.ps1 build` exit 0，日志 `tmp/logs/20260809-164626_mq5_review_firmware_build.log`；未烧录、未真机。
- 已知非本功能警告：`ota_1` 小 recovery 分区装不下主固件的既有 warning，与 MQ-5 无关。
- 下一步真机门槛：断电接线/共地；GPIO20 节点上电前确认 <=3.0V；P6 总峰值 <=1.2A、压降 <=0.15V、30 分钟温升 <=10°C，否则改输入并联支路；60 秒预热；首次/储存后 48/72/168h 人工老化确认；洁净空气基线标定；USB reset 后 0 到 64 样本回归；MQ-5 不可替代认证燃气报警器。
- 备份：`tmp/backups/manual/20260809-164921_mq5_docs_closeout/`。

----
完成时间：2026-10-28 16:50 +08:00（仓库整理与 README 发布）

## 仓库规范化、README 与 GitHub 首个公开版本

- 分支规范：本地唯一分支 `recovered-main` 更名为 `main`；新增远端 `origin` 指向 `github.com/the-snowpear/fridge_spirit`，并建立 `main -> origin/main` 上游跟踪。
- 仓库瘦身：从索引移出 1.8GB 厂商例程包 `doc/野火小智【姿态传感器_MPU6050】模块_20260206/`（含 STM32 工程、上位机 exe、CMSIS 库）和误把工程镜像进 shell 错误路径的 `System.Management.Automation.Internal.Host.InternalHost/`，本地文件全部保留，跟踪文件由 22330 降到 1924。
- `.gitignore` 修正：`*.exe`、`flasher_args.json`、`*.workspace`、`*.suo`、`*.sln.docstates` 原本把注释写在同一行导致规则失效，改为独立注释行；补充 `*.dll`/`*.lib`/`*.pdb` 与上述两个目录的忽略规则。
- 新增 `README.md`（产品级说明：能力介绍、真机界面一览、Mermaid 架构图、硬件与分区规格、技术栈、快速开始、仓库结构、文档索引、当前开发状态）与 `doc/assets/` 真机截图；截图取自 `tmp/cache/screenshot_test` 与 `output/playwright`，均为此前已验收画面，未重新摆拍。
- 未改动源码、sdkconfig、分区表、GPIO 与任何硬件路径；本次仅涉及文档、`.gitignore` 与 Git 索引。
- 备份：`tmp/backups/manual/20261028-163904_readme_git_cleanup/.gitignore.orig`。
