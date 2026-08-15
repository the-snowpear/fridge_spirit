# P4 竖屏隐私模式与横竖屏切换

## 适用范围

当前竖屏实现覆盖 `HOME-01 首页`、`HOME-02 今日摘要详情`、`PLAN-01 计划总览`、`CHAT-01～04`、`MORE-01、MORE-02、MORE-04～06、MORE-07、MORE-08`：稳定页面键分别为 `home`、`home_detail`、`plan`、`chat`、`chat_history`、`chat_tool_detail`、`chat_pending_actions`、`more`、`timer`、`safety`、`settings`、`maintenance`、`alarm`、`stopwatch`。横竖屏控制结构已独立于页面内容；其它页面尚未移植时保持横屏，不会自动跳转到 HOME-02、CHAT-01 或 MORE-01，也不会把横屏对象强行裁切到竖屏。

## 几何与隐私规则

- 横屏标准：逻辑和物理均为 `1024×600`，保持 `704 主区 + 72 导航 + 248 状态舱`。
- 竖屏隐私：逻辑为 `600×1024`，分为 `672 主区 + 64 导航 + 288 状态舱`。
- 竖屏 HOME-01 主区按 ver2 采用 `2×2` 统计卡、临期优先、今日推荐、Agent 提示、今日菜单/购物清单和同步信息的单列堆叠；正文内容宽度 `568px`，不嵌套滚动。
- 竖屏 HOME-02 主区仍使用 87px 共享顶栏，正文视口为 `600×585`，内容 `x=16、w=568`，单层纵向滚动。
- 竖屏 PLAN-01 使用 `568px` 单列正文，按 ver2 顺序展示今日菜单、购物摘要、临期利用计划、本周菜单和 Agent 建议；正文高度约 `1120px`，只使用一个外层纵向滚动容器。
- 竖屏 CHAT-01 使用 `568px` 单列消息流、状态行、工具/结果卡和固定底部输入区；输入区关闭 IME 时消息 PPA 视口为 `473px`，展开完整拼音 IME 时暂停 PPA，避免键盘覆盖缓存。
- 竖屏 CHAT-02/03/04 使用同一 `568px` 正文滚动视口和 60px 固定返回区：会话历史只展示当前 foreground 会话，工具详情展示真实调用 ID/状态/风险/耗时，待确认页只展示真实 `requires_confirmation` 工具记录。
- 竖屏 MORE-01 使用 `568px` 双列入口卡；MORE-02、MORE-07、MORE-08 使用 `568px` 单列工具正文，定时器和秒表的动态数字通过小范围覆盖层刷新，不把固定导航或隐私状态舱纳入 PPA 滚动。
- 竖屏 MORE-04～06 使用 `568px` 正文、`150px` 左侧分区栏、`12px` 分区间距和约 `406px` 右侧内容；分区按钮在竖屏使用较小字号，正文行高为 `94px`，三页正文高度约 `1120px`，只绑定一个 `portrait_scroll` 纵向容器。
- 竖屏始终绑定隐私模式：状态舱显示隐私胶囊，摄像头状态与按钮为关闭态，USB/Agent 不能在该状态直接进入摄像头页。
- HOME-01/HOME-02、PLAN-01、CHAT-01～04 与 MORE-01/02/04/05/06/07/08 可在竖屏状态下直接切换并重新绑定对应的 PPA 滚动缓存；CHAT-02/03/04 返回 CHAT-01、定时器/闹钟/秒表返回 MORE-01、设置三页返回 MORE-01 时均不恢复横屏。PLAN-02～06、MORE-03 和库存等未移植入口保持横屏并提示“该页面尚未移植竖屏”。

## 显示实现

EK79007 MIPI-DSI 的物理扫描参数固定为 `1024×600 RGB565`，横竖屏切换不会改动面板寄存器、lane、DPI 时钟、背光或 GPIO。

竖屏使用两块 `600×1024 RGB565` PSRAM 逻辑缓冲，继续采用 LVGL DIRECT 双缓冲。最后一个 flush 到达时，PPA SRM 将逻辑帧逆时针旋转 90°，写入显示驱动当前安全的 `1024×600` DPI 帧缓冲，再按既有 VSYNC 路径提交。该方向已按设备实际竖直安装姿态校正，相对早期竖屏实现翻转 180°。GT911 保持原生物理坐标，竖屏时在 UI 输入回调中执行逆变换：

```text
logical_x = 599 - physical_y
logical_y = physical_x
```

主滚动加速器支持横竖屏两套几何配置并在 UI 任务内重绑定：

- 横屏：`704×513` 视口，PPA 把页面快照裁剪到物理 DPI 双缓冲后直接提交。
- 竖屏：普通页面和 PLAN-01 使用 `600×585` 视口；CHAT-01 关闭 IME 时使用 `600×473` 消息视口，CHAT-02/03/04 使用约 `600×517` 正文视口。PPA 只把当前滚动窗口裁剪到 `600×1024` 逻辑缓冲，再复用上述整帧旋转路径写入物理 DPI 双缓冲。

手势期间 LVGL 仍负责触摸坐标、惯性和回弹状态机，但暂停软件刷新定时器；同一 UI 周期的多次滚动只提交最后位置。结束时两块逻辑 DIRECT 缓冲同步到最终位置，再恢复滚动条局部重绘。因此竖屏长列表不会退回逐帧大面积软件栅格化。方向切换会结束当前 PPA 手势、清空旧页面快照并为新几何重建缓存。

为保留 SDMMC DMA 连续内存，竖屏渲染缓冲、PPA client 和竖屏对象树均延迟到首次进入竖屏时创建。进入竖屏前释放横屏动态正文/CHAT-01 输入层；切回横屏时销毁竖屏对象树并恢复横屏 CHAT-01 兼容层，避免两套长页同时占用内部堆。

## 状态舱图标资源

竖屏状态舱使用与 `ui-reference/ver2/js/core/icons.js` 同源的 A8 图标。`lock` 已加入 `data/ui/ver2/icons/lock.a8`，用于隐私模式胶囊和“摄像关”按钮；“下一事项”复用既有 `timer.a8`。资源包版本为 `2.0.0-portrait.1`，旧 `2.0.0-more.1` 仍可加载，但缺少锁图标时只保留文字语义。

资源包使用 `tmp/tools/ui_assets/build_ver2_pack.py` 生成和校验；设备部署必须上传到临时目录、逐文件回读校验，最后原子切换 `fridge/assets/ui/ver2`。部署脚本为 `tmp/notes/deploy_ver2_pack_serial.py`，串口保持 `DTR=false、RTS=true` 并等待本次会话 `ready` 后才写 TF。

状态舱排版继续以参考稿 `cabin.css` 为准：

- 时钟使用独立 40px Bold 角色；日期/星期、温湿度使用 15px Medium 角色。日期必须完整显示为“日期 · 星期”。
- “今日摘要 / 下一事项 / 最近同步”使用 18px 加重字；标题下横线和模块间竖线统一通过无边框纯色线对象绘制，厚度均为 1px。
- 隐私胶囊为 116×30px，整体相对日期/星期右移 14px。锁图标与文字使用独立固定槽位：94px 内容区居中放置，图标槽从 `x=0` 开始，文字槽从 `x=28` 开始，实屏可见间距约 7px；文字只做垂直居中，禁止调用会覆盖横坐标的通用双轴居中函数。
- 日期略向右、温湿度略向左微调，使两行视觉起点对齐；温湿度与五个 28×28px 健康状态格在同一基线排列，状态格整体右移并与湿度保留明确间距，A8 图标在状态格内居中。隐私模式下摄像头状态格、状态点和图标始终使用离线灰，不继承后台摄像头硬件状态。
- 竖屏顶部导航栏底部使用独立 1px 横线，与横屏共享顶栏的边界语义一致。

## 任务与状态边界

USB、Agent 和 Shell 不直接修改 LVGL 或显示缓冲。外部调用只向 `ui_p4` FreeRTOS 队列投递页面或方向请求，实际重绑、布局切换和整帧失效都由唯一 UI 任务执行。

`fridge_ui_p4_status_t` 和 USB `ui_status` 增加：

- `orientation`：`landscape` 或 `portrait_privacy`
- `privacyMode`：竖屏隐私态为 `true`

方向变化会把 `framebuffersSynced` 和 `pageFrameReady` 重新置为 `false`。至少一帧成功提交并同步另一块物理缓冲后才恢复为 `true`，防止 Codex 截到切换前的旧帧。

## USB 控制

首页保持当前页面直接切换竖屏：

```json
{"command":"tool_call","request_id":"portrait-home","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
```

临期详情同样保持当前页面直接切换：

```json
{"command":"tool_call","request_id":"home02","tool":"ui.switch_page","arguments":{"page":"home_detail"}}
{"command":"tool_call","request_id":"portrait-home02","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
```

计划总览保持当前页面直接切换：

```json
{"command":"tool_call","request_id":"portrait-plan","tool":"ui.switch_page","arguments":{"page":"plan"}}
{"command":"tool_call","request_id":"portrait-plan-mode","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
```

恢复横屏：

```json
{"command":"tool_call","request_id":"landscape","tool":"ui.set_orientation","arguments":{"orientation":"landscape"}}
```

每次 `queued=true` 后都必须连续两次读取稳定的 `ui_status`。固定截图入口支持在同一串口会话中完成切页、方向切换、稳定门和截图：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_screenshot.ps1 `
  -Port COM17 -Page home_detail -Orientation portrait_privacy `
  -Output tmp\cache\portrait_privacy\device_home02_portrait
```

设备端截图仍是物理 `1024×600` 帧；固定脚本会在读回后顺时针旋转为逻辑 `600×1024`，便于和参考稿直接比较。

对话页竖屏直达示例：

```json
{"command":"tool_call","request_id":"portrait-chat","tool":"ui.switch_page","arguments":{"page":"chat"}}
{"command":"tool_call","request_id":"portrait-chat-mode","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
```

进入竖屏后可继续调用 `ui.switch_page` 切换 `chat_history`、`chat_tool_detail` 或 `chat_pending_actions`；四页只派生当前 foreground 模型，不注入 ver2 mock 多会话、参数、业务对象或日志。

更多工具页竖屏直达示例：

```json
{"command":"tool_call","request_id":"portrait-more","tool":"ui.switch_page","arguments":{"page":"more"}}
{"command":"tool_call","request_id":"portrait-more-mode","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
```

进入竖屏后可继续调用 `ui.switch_page` 切换 `timer`、`alarm` 或 `stopwatch`；工具页返回 `more` 时保持竖屏。MORE-04～06 设置/维护页已恢复竖屏；MORE-03 编辑页和其它未移植设置/维护子页仍显示“该页面尚未移植竖屏”，通过 USB 直接切换这些未移植页面仍先恢复横屏再打开目标页。

设置类页面竖屏直达示例：

```json
{"command":"tool_call","request_id":"portrait-safety","tool":"ui.switch_page","arguments":{"page":"safety"}}
{"command":"tool_call","request_id":"portrait-settings","tool":"ui.switch_page","arguments":{"page":"settings"}}
{"command":"tool_call","request_id":"portrait-maintenance","tool":"ui.switch_page","arguments":{"page":"maintenance"}}
{"command":"tool_call","request_id":"portrait-settings-mode","tool":"ui.set_orientation","arguments":{"orientation":"portrait_privacy"}}
```

MORE-04、MORE-05、MORE-06 在竖屏中保持真实页面键和 MORE 导航高亮；从三页返回 `more` 或切换 HOME/PLAN/CHAT 时保持竖屏。横屏直接进入 `safety` 继续使用兼容页面，MORE-03 及其它未移植设置/维护子页仍先恢复横屏并提示“该页面尚未移植竖屏”。

PLAN-01 竖屏中的“编辑日/周菜单、菜谱库、菜谱详情和购物清单”入口保留 ver2 的视觉位置，但点击后停留在 PLAN-01 并提示“该页面尚未移植竖屏”。通过 USB 直接切换到 `shopping` 或其它未移植计划子页时，仍沿用现有逻辑先恢复横屏再打开目标页。

## WebUI 控制

WebUI 顶栏提供单个“切换竖屏 / 切换横屏”按钮，复用同一 `ui.set_orientation` Tool。点击切换竖屏时，WebUI 会保持当前的 HOME-01、HOME-02、PLAN-01、四个 CHAT 页面、MORE 工具页或 MORE-04～06 设置页；如果设备当前页面尚未移植竖屏，则提示“该页面尚未移植竖屏，请先切换到首页、临期详情、计划、对话、更多或设置类页面”，不会自动跳转到其它页面。方向请求入队后继续轮询 `ui_status`，只有连续两次满足四项稳定门且页面/方向匹配，按钮才退出“切换中”状态。Mock 和真实 Web Serial 使用相同交互结构。

## PLAN-01 计划总览竖屏实现

- 固件正文按 ver2 单列堆叠：今日菜单、购物清单两枚统计卡、临期利用计划、7 天本周菜单和 Agent 建议；使用现有 `plan_business_model_t`，购物统计按 `shopping[].group` 计算。
- 未提供的安排详情、菜谱详情和 Agent 采纳接口不写入虚构数据；PLAN-02～06 入口在竖屏内只显示提示，USB 直达未移植页仍按横屏兼容路径处理。
- 计划正文固定使用 `portrait_scroll` 和普通竖屏 PPA 视口，内容约 `1120px`；内容包装层清除 `CLICKABLE`，卡片动作接入 8px 拖动取消保护。
- 本周 7 天条带在 `568px` 正文内保持单行摘要、今日高亮和冲突红点，不增加嵌套竖向滚动；计划导航项在竖屏保持高亮。

## MORE 页面竖屏实现

- MORE-01 使用 ver2 双列入口卡：时间工具列出定时器、闹钟、秒表；系统与维护列出设备安全、设置中心和维护中心。定时器/闹钟角标只来自现有 `fridge_kitchen_tools_snapshot_t`，状态摘要只展示真实 Wi-Fi、TF 和时间同步状态，不注入 SSID、容量或安全告警。
- MORE-02 定时器使用主计时卡、3×2 快捷预设、运行中和已完成区；预设仍调用现有定时器回调，新建/编辑入口留在当前页并提示“该页面尚未移植竖屏”。活动数字由动态覆盖层刷新，避免 PPA 长列表滚动时冻结。
- MORE-07 闹钟展示真实固定闹钟、周期提醒开关和无数据空态；开关沿用现有闹钟后端，新建/编辑只提示，不进入 MORE-03。MORE-08 秒表展示大数字、开始/暂停、复位、计次列表，动态数字沿用现有秒表状态和回调。
- 四个页面共用唯一 `portrait_scroll` 纵向滚动容器；透明内容层清除 `CLICKABLE`，卡片与工具按钮接入统一 8px 位移、`PRESS_LOST`、`SCROLL_BEGIN/SCROLL` 点击取消保护。页面/方向切换和异步刷新后重新绑定对应 PPA，定时器/秒表动态覆盖层不随静态缓存冻结。

## MORE-04～06 设置页竖屏实现

- MORE-04 设备与厨房安全保留“设备概览、连接状态、传感器、厨房安全”四个分区。设备、网络、TF、摄像头、音频、SHT4x 和 P4 雷达内容只读取现有快照；当前 P4 最小闭包未接入 S3 legacy 光敏/IMU 链路，这两项显示“未接入”，不把零值当作真实读数。摄像头在隐私模式始终显示关闭态，安全事件队列为空时显示中性空态，不写入演示告警。
- MORE-05 设置中心保留“网络与同步、AI Provider 与模型、Agent 权限、语音与显示、隐私与家庭”五个分区。Wi-Fi 扫描/连接使用现有弹层，弹层打开时暂停 PPA，关闭后重新绑定并失效缓存；Agent 权限、WakeNet 开关和亮度滑块沿用现有真实回调，API Key 不在设备 UI 显示，未实现的夜间/绑定能力只提示。
- MORE-06 维护中心保留“存储、插件、日志、系统更新、关于”五个分区。TF 挂载/容量、诊断摘要、启动次数、固件版本、启动槽位、设备型号、芯片和内存来自现有状态；第三方插件、日志导出、检查更新和 OTA 只显示中性提示，不写 Flash 或伪造版本/序列号。
- 三页使用 `150px + 12px + 406px` 的竖屏分区布局和唯一 `portrait_scroll`；右侧取值列加宽并固定单行省略，说明行保留安全间距，透明内容层清除 `CLICKABLE`，分区切换、卡片和按钮沿用 8px 拖动取消、`PRESS_LOST`、`SCROLL_BEGIN/SCROLL` 保护。页面切换、方向切换和异步快照刷新后重新绑定 PPA，未修改 MIPI、GPIO、电源、显示时序或摄像头路径。

## MORE-04～06 实机验证记录（2026-08-04）

- 固件使用 ESP-IDF 5.5.1 包装脚本构建，WebUI 使用 `npm run build` 构建，COM17 使用 no-stub 烧录且写入 Hash 校验通过；构建保留既有 `ota_1` recovery 分区小于当前主镜像的容量告警，未修改分区。
- 固定截图入口分别完成 `safety`、`settings`、`maintenance` 加 `portrait_privacy` 的 `ready → 页面/方向匹配 → 连续两次稳定门 → 单次截图 → TF 分块读回`；截图路径为：
  - `tmp/cache/screenshot_test/portrait_safety_final.bmp`
  - `tmp/cache/screenshot_test/portrait_settings_final.bmp`
  - `tmp/cache/screenshot_test/portrait_maintenance_final.bmp`
- 三页均使用 `600×1132 + 80px elastic` 的竖屏 PPA 缓存，视口为 `600×585`；`view_image` 检查确认主区、64px 导航、288px 隐私状态舱、分区栏、标题和正文均未越界或裁切，摄像头保持关闭态。
- 启动日志确认 ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`，未见本轮新增反复重启、Guru Meditation 或 WDT。既有 coredump `ESP_ERR_INVALID_CRC` 和 `ota_1` 容量告警仍存在；维护页 TF 读回期间出现一次 I2C transaction NACK，但 TF 传输、CRC32 和文件头校验均成功。
- USB 固定诊断没有触摸注入能力，因此真实手指分区切换、上下拖动超过 8px 不触发点击、真实抬手和 Wi-Fi 弹层/亮度滑块现场回归仍需手动完成，文档不将其冒充为已完成。

### 分栏间距修复复验（2026-08-04）

- 将竖屏左侧分区按钮从 `180px` 收窄到 `150px`、分区间距从 `16px` 调整为 `12px`，右侧正文从约 `372px` 扩大到约 `406px`；分区文字使用较小字号，右侧取值列从 `104px` 扩大到 `144px`，行高从 `86px` 增至 `94px`。
- 间距修复版固定入口截图并经 `view_image` 查看：`tmp/cache/screenshot_test/portrait_safety_spacing_fix.bmp`、`tmp/cache/screenshot_test/portrait_settings_spacing_fix.bmp`、`tmp/cache/screenshot_test/portrait_maintenance_spacing_fix.bmp`。三页标题、分区按钮、右侧数值/说明、导航和隐私状态舱均未见重叠或裁切。

## MORE 页面实机验证记录（2026-08-04）

固件使用 ESP-IDF 5.5.1 包装脚本构建并烧录到 `COM17`，Hash 校验通过；WebUI `npm run build` 通过。固定截图入口分别完成 `more`、`timer`、`alarm`、`stopwatch` 加 `portrait_privacy` 的页面/方向请求、两次稳定门、单次截图和 TF 分块读回，并用 `view_image` 实际查看：

- `tmp/cache/screenshot_test/portrait_more_final.bmp`
- `tmp/cache/screenshot_test/portrait_timer_final.bmp`
- `tmp/cache/screenshot_test/portrait_alarm_final.bmp`
- `tmp/cache/screenshot_test/portrait_stopwatch_final.bmp`

四页均保持 `600×672` 主区、64px 导航和 288px 隐私状态舱；MORE-01 双列入口、MORE-02 3×2 预设、MORE-07 真实固定闹钟/周期空态、MORE-08 秒表控制与计次空态均可见，最终标题与页面 ID 胶囊无重叠，摄像头继续为关闭态。代码层已接入 PPA 重绑定和动态数字覆盖；固定 USB 诊断没有触摸注入能力，因此真实手指上下拖动、8px 后不触发点击、真实抬手和活动倒计时/秒表现场回归仍需手动完成，文档不将其冒充为已完成。

## CHAT 页面实机验证记录（2026-08-03）

最终固件已使用 ESP-IDF 5.5.1 包装脚本构建并烧录到 `COM17`，Hash 校验通过；WebUI `npm run build` 通过。固定截图入口按 `ready → 页面/方向请求 → 连续两次稳定门 → 单次 screenshot_capture → TF 分块读回` 完成以下四页：

- `tmp/cache/portrait_chat/chat_final.bmp`：CHAT-01 主消息流、工具/结果卡、输入区、语音、发送按钮和导航/隐私状态舱。
- `tmp/cache/portrait_chat/chat_history_final.bmp`：CHAT-02 搜索/同步筛选、当前 foreground 会话、继续/重命名/归档提示、返回/新建入口。
- `tmp/cache/portrait_chat/chat_tool_detail_final.bmp`：CHAT-03 最近真实工具记录、调用 ID、状态、风险、耗时、权限和中性 Runtime 空态。
- `tmp/cache/portrait_chat/chat_pending_actions_final.bmp`：CHAT-04 无待确认行动空态，以及返回/查看对话入口。

四页均由 `view_image` 实际查看，主区、64px 导航栏和 288px 状态舱没有越界；最终日志中的 CHAT-01 PPA 缓存为 `600×2874 + 80px elastic`、探针约 12.9ms，CHAT-03 为 `600×992`、约 13.9ms，CHAT-04 为 `600×772`、约 14.0ms。CHAT-02 正文小于视口，完成 PPA 竖屏重绑定后不申请无必要缓存。

消息卡、工具卡、结果卡和确认按钮均复用现有 8px 拖动取消逻辑，IME 展开时暂停 PPA、收起后重新失效并构建缓存。当前 USB 诊断接口只提供 `touch_status/touch_scan`，没有触摸注入能力；真实手指拖动超过 8px、真实抬手点击和 IME 展开/收起仍需现场手动完成，文档不将其记为已完成。最终启动日志确认 `brownout=0`、32MB PSRAM memory test OK，未见本轮新增 Guru Meditation、WDT 或反复重启；既有 coredump `ESP_ERR_INVALID_CRC`、I2C NACK 和 `ota_1` 容量告警保持原样记录。

## 后续页面接入

为新页面补竖屏时，应保持以下边界：

1. 在 Shell 增加该页面的 600px 宽布局，不修改 MIPI 物理时序。
2. 放宽 `fridge_ui_p4_request_orientation_async()` 的页面白名单。
3. 所有新增滚动区入口继续接入 8px 位移、`PRESS_LOST`、`SCROLL_BEGIN/SCROLL` 取消保护。
4. 摄像头相关页面不得在 `portrait_privacy` 中开放；若未来需要普通竖屏，应新增不同方向/隐私枚举，不能复用隐私键名改变语义。
5. 使用固定截图入口检查同尺寸顶、中、底状态，并在 `design-qa.md` 留存对比证据。
6. 主内容超过一个视口时，应通过 `fridge_ui_scroll_ppa_bind()` 绑定对应滚动容器与方向配置；不能只依赖整帧方向旋转来掩盖 LVGL 软件滚动开销。
