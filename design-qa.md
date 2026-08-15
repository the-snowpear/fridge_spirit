# HOME-01 首页竖屏隐私模式设计验收

- 参考真值：`tmp/cache/cabin_shots/home01_portrait.png`，来自 `ui-reference/ver2` HOME-01 竖屏隐私样式。
- 最终实机画面：`tmp/cache/portrait_home/device_home01_portrait_final_heading.bmp`，固定入口从 COM17 读回，逻辑尺寸 600×1024。
- 布局：主区 600×672、横向导航 64px、底部隐私状态舱 288px；首页保留在 HOME-01，未移植页面不抢跳到 HOME-02。
- 页头与统计：标题、HOME-01 胶囊、日期/状态、副标题、搜索和快捷添加按钮齐全；统计区为 ver2 的 2×2 卡片，颜色、字体层级、圆角和图标已在实机首屏核对。
- 临期优先：单列三行卡片保留食材、位置/数量、估算标签、到期胶囊、箭头和右侧“全部 7 项 >”；顶部右侧标签在最终镜像中已修复并重新验收。
- 主滚动区：推荐、Agent 建议、今日菜单、购物清单和同步信息均位于同一个竖屏 PPA 滚动容器；最终 HOME-01 缓存为 `600×1268 + 80px elastic`，硬件探针 15.8ms。现有 8px 拖动抑制逻辑保持不变，未把滚动手势补发为点击。
- 隐私状态舱：保持现有竖屏隐私舱，摄像头健康格和“摄像关”按钮为灰色关闭态；时间、温湿度、健康状态和 CTX-01 摘要正常显示。
- 页面切换：已实测 HOME-01 → HOME-02 → HOME-01 竖屏直接切换，每次都重新绑定对应 PPA，并连续满足 `ready/resourceReady/framebuffersSynced/pageFrameReady` 稳定门。
- 最终验证：HOME-01、HOME-02 均完成稳定门、BMP 读回、CRC32 和文件头校验；最终启动日志 `brownout=0`、32MB PSRAM memory test OK，`panic=491` 与 `task_wdt=66` 保持不变，未见本轮新增崩溃或 WDT。启动时既有 coredump 分区 `ESP_ERR_INVALID_CRC` 提示仍存在，未作为本次 UI 回归结果混淆。

final result: passed

----

# HOME-02 竖屏隐私模式设计验收

- 参考真值：`tmp/cache/portrait_privacy/reference_home02_portrait_600x1024.png`，来自 `ui-reference/ver2/index.html?layout=portrait#/p/HOME-02` 的精确 600×1024 画面。
- 实机画面：`tmp/cache/portrait_privacy/device_home02_portrait_privacy_7px_gap.bmp`，由设备物理 1024×600 RGB565 截屏读回后按逻辑方向还原为 600×1024。
- 局部放大：`tmp/cache/portrait_privacy/privacy_pill_7px_gap_crop.png`，按实屏原始像素裁切后使用最近邻放大，便于检查锁与首字边界。
- 同屏比较：`tmp/cache/portrait_privacy/comparison_reference_left_device_right_privacy_overlap_fixed.png`，左侧参考、右侧实机，采用相同视口和 HOME-02 顶部默认滚动位置。
- 布局：600×672 主区、64px 横向导航和 288px 底部状态舱均与参考结构一致；87px 顶栏、16px 内容边距、卡片分组、导航高亮和底舱左右操作列未见重叠或异常裁切。
- 字体与文案：标题、页面编号、副标题、临期分组、卡片主副文本和状态舱摘要层级清晰，无缺字或乱码。状态舱时钟为 40px Bold；日期/星期、温湿度为 15px Medium；“今日摘要 / 下一事项 / 最近同步”为 18px 加重字并带标题下横线。日期与星期完整显示。
- 图标：隐私胶囊和摄像关闭按钮使用同源 `lock.a8`，下一事项使用 `timer.a8`；隐私锁与文字采用独立固定槽位，局部放大确认二者之间约 7px，既不重叠也不过度分离。胶囊与“周日”间距清晰；日期与温湿度视觉起点对齐，五个健康状态格与湿度间距清晰，A8 图标均在各自区域内居中；摄像健康图标、状态点和按钮在隐私模式下保持灰色关闭态。
- 颜色与边界：纸白、奶油底、鼠尾草绿、薄荷绿、番茄红和蛋黄黄沿用共享主题；标题横线与模块竖线厚度统一为 1px，竖屏顶部导航栏具备 1px 底线；边框、圆角、禁用摄像按钮及健康点语义与参考一致。
- 交互与状态：USB 单会话完成横屏→竖屏隐私→横屏，三阶段四项稳定门均连续两次通过；竖屏摄像头保持关闭，返回其它页会先恢复横屏。
- 性能：实机建立 `600×1668 + 80px elastic` PPA 快照，硬件探针 15.8～15.9ms；竖屏主视口为 600×585，未回退 LVGL 逐帧长列表重绘。连续真实手指拖动的主观手感仍由用户最终确认，但加速路径本身已通过日志与硬件探针验收。
- 未发现 P0/P1/P2 视觉问题。可接受的 P3 差异为真实日期/温湿度内容、设备 FreeType 字宽和参考浏览器字体的细微差别。

final result: passed

----

# PLAN-06 购物清单页设计验收

- 参考真值：`ui-reference/ver2` 的 `PLAN-06`（704px 主区、72px 导航、248px 状态舱；87px 页头、440px 正文和 73px 固定底栏）。
- 已完成代码核对：PLAN-01 入口、`shopping` 稳定页面键、返回 PLAN-01、计划导航高亮、本地状态文案、三组 656×72 行、32px 瞬时选择勾选、来源标签、8px 滚动点击保护、中文输入法弹层均已实现；底栏批量“加入待购买 / 加入已购买”以单次原子写入提交选择结果，已移除按超市动线；仅复用现有主题、字体及 back/plus/check A8 图标。
- 构建：ESP-IDF 5.5.1 / LVGL 9.2.2 / RGB565 构建通过，主镜像 `0x3203c0`。既有 `ota_1` 2MiB 容量不足告警仍存在，未在本页任务中修改分区。
- 性能：先前购物页 PPA 错将固定底栏纳入 513px 滚动搬运，并逐帧 ARGB 快照/混合。现只搬运参考稿规定的 440px 正文；73px 底栏由 RGB565 双缓冲保持，避免每帧覆盖和混合。COM17 新镜像已通过 no-stub 写入与 Flash Hash；`ready/resourceReady/framebuffersSynced/pageFrameReady` 和 `shopping` 页面稳定门均通过，缓存 `704×912 + 80px elastic`，硬件探针 13.2ms，未见缓存几何错误或 LVGL 回退。
- 待完成：固定截屏的视觉比对、连续真实手指拖动的主观验证、保存回滚和重启持久化；未使用旧图代替这些验收。

final result: partial — 真机 PPA 稳定门通过，待完成触摸与视觉验收

----

# CHAT-01～04 竖屏隐私模式设计验收

- 参考真值：`ui-reference/ver2` 的 `pages-chat.js`、`pages-chat.css` 与共享组件样式；逻辑尺寸为 `600×1024`，主区/导航/隐私状态舱为 `600×672 / 64 / 288px`。
- 固件实现：CHAT-01 使用 `568px` 单列消息流、状态行、工具/结果卡、输入框、语音、发送/停止和完整拼音 IME；CHAT-02～04 保留真实页面键，分别展示当前 foreground 会话、最近工具记录和待确认工具记录，无 Runtime 数据时显示空态。
- 交互边界：消息卡、工具卡、结果卡及确认按钮复用既有 8px 拖动取消规则；CHAT-02/03/04 返回 CHAT-01 仍保持竖屏；横屏二级 CHAT 键继续折叠为 CHAT-01 兼容行为。
- PPA 几何：CHAT-01 关闭 IME 时消息视口为 `600×473`，展开 IME 时暂停 PPA；CHAT-02～04 使用约 `600×517` 正文视口，60px 固定返回区不纳入滚动快照。
- 构建核对：ESP-IDF 5.5.1 包装脚本 `build` 通过；Web `npm run build`（TypeScript + Vite）通过。构建仍报告既有 `ota_1` 2MiB recovery 槽容量不足告警，本次未修改分区。
- 实机最终截图：`tmp/cache/portrait_chat/chat_final.bmp`、`tmp/cache/portrait_chat/chat_history_final.bmp`、`tmp/cache/portrait_chat/chat_tool_detail_final.bmp`、`tmp/cache/portrait_chat/chat_pending_actions_final.bmp`；四页均由固定入口从 COM17 读回，逻辑尺寸 600×1024，并使用 `view_image` 实际检查。
- 稳定门与 PPA：四页均连续两次满足 `ready/resourceReady/framebuffersSynced/pageFrameReady` 且页面/方向匹配；CHAT-01 日志为 `600×2874 + 80px elastic`、PPA probe 约 12.9ms，CHAT-03 为 `600×992`、约 13.9ms，CHAT-04 为 `600×772`、约 14.0ms；CHAT-02 正文小于视口，仍完成竖屏 PPA 重绑定并安全跳过快照申请。
- 视觉检查：主区、64px 导航栏和 288px 隐私状态舱均未越界；CHAT-03/04 长标题的页面 ID 胶囊已避让末字；CHAT-01 输入区、CHAT-02 当前会话、CHAT-03 工具信息/中性 Runtime 空态和 CHAT-04 无待确认空态均可见，摄像头保持关闭态。
- 启动与硬件边界：最终 COM17 烧录 Hash 校验通过；连续启动日志确认 ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`，未见本轮新增 Guru Meditation、WDT 或反复重启。既有 coredump `ESP_ERR_INVALID_CRC`、I2C NACK 和 recovery `ota_1` 容量告警仍记录为既有环境问题；MIPI/GPIO/电源/摄像头路径未修改。
- 触摸/IME 说明：消息卡、工具卡、结果卡和确认按钮已接入代码层 8px 拖动取消保护，IME 展开/收起也接入 PPA 暂停/重建路径；当前固定 USB 工具只提供 `touch_status/touch_scan` 诊断，没有触摸注入能力，因此真实手指拖动、抬手点击和 IME 展开收起仍需现场手动验收，未将其冒充为已完成。

final result: partial — 固件/WebUI、四页稳定门和截图视觉验收通过；现场真实触摸/IME 手动回归待补

----

# PLAN-01 计划总览竖屏隐私模式设计验收

- 参考真值：`ui-reference/ver2` 的 PLAN-01 竖屏布局；逻辑尺寸为 `600×1024`，主区/导航/隐私状态舱为 `600×672 / 64 / 288px`。
- 固件实现：计划正文使用单一 `portrait_scroll` 纵向容器，按 ver2 单列展示今日菜单、购物摘要、临期利用计划、本周菜单和 Agent 建议；内容高度约 `1120px`，底部留安全间距。
- 数据边界：今日菜单、临期食材、7 天摘要和 Agent 文案来自现有 `plan_business_model_t`；购物摘要按 `shopping[].group` 统计，不注入演示业务数据。未移植的 PLAN-02～06 入口只提示，不伪造安排、菜谱或操作结果。
- 交互边界：内容包装层清除 `CLICKABLE`，卡片入口使用统一 8px 拖动取消规则；7 天菜单保持单行横向条带，不嵌套竖向滚动。计划页与 HOME/CHAT 竖屏切换后重新绑定 PPA。
- WebUI/路由：`plan` 加入竖屏白名单，当前计划页切换方向保持 `plan`；USB 直接进入未移植计划子页继续走横屏兼容逻辑，竖屏卡片入口停留 PLAN-01 并提示。
- 构建与实机：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build`、COM17 no-stub flash/Hash 校验均通过；构建保留既有 `ota_1` 2MiB recovery 槽容量不足告警，未修改分区。
- 实机最终截图：`tmp/cache/portrait_plan/plan_portrait_final.bmp`，固定入口按 `plan + portrait_privacy` 完成 `ready/resourceReady/framebuffersSynced/pageFrameReady` 连续两次稳定门、BMP/CRC32/文件头检查，并已使用 `view_image` 查看。
- 稳定门与 PPA：目标页面/方向匹配，PPA 日志为 `600×1132 + 80px elastic`，探针约 15.8ms；截图首屏确认标题、PLAN-01 标识、今日菜单、购物统计、临期卡首行、计划导航高亮及隐私状态舱均未越界，摄像头保持关闭态。
- 启动与硬件边界：日志确认 ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`、`panic=491`、`task_wdt=66` 未增加；未见本轮新增 Guru Meditation、WDT 或反复重启。既有 coredump `ESP_ERR_INVALID_CRC` 与 I2C NACK 仍存在，MIPI/GPIO/电源/摄像头路径未修改。
- 触摸说明：代码层已清除透明内容层 `CLICKABLE` 并接入 8px 拖动取消保护；固定 USB 工具没有触摸注入能力，因此计划页上下滑动后的中后段画面、真实拖动与抬手点击仍需现场手动验收，未将其冒充为已完成。
- 购物摘要裁切修复（2026-08-04）：统计卡由 `264×108` 调整为 `264×116`，购物摘要卡由 `168px` 调整为 `176px`，为 TF 字体真实行高和底部边框增加安全间距；修复版截图 `tmp/cache/portrait_plan/plan_portrait_shopping_fix.bmp` 已通过固定入口读回并用 `view_image` 检查，底部文字与边线完整可见。

final result: partial — 固件/WebUI、稳定门、PPA 几何和首屏截图视觉验收通过；现场真实触摸回归待补

----

# MORE-01/02/07/08 竖屏隐私模式设计验收

- 参考真值：`ui-reference/ver2` 的 MORE-01、MORE-02、MORE-07、MORE-08；逻辑尺寸为 `600×1024`，主区/导航/隐私状态舱为 `600×672 / 64 / 288px`。
- 固件实现：MORE-01 使用双列入口卡；MORE-02 使用主计时卡和 3×2 快捷预设；MORE-07 使用真实固定闹钟、周期提醒空态和开关；MORE-08 使用大数字、开始/暂停、复位和计次列表。MORE-03 编辑页仍保持未移植边界，MORE-04～06 已恢复为独立竖屏设置页。
- 数据边界：定时器、闹钟、秒表和 Wi-Fi/TF/时间同步摘要均来自现有快照或状态；不注入 SSID、容量、安全告警、多条虚构运行记录或不存在的业务操作。新建/编辑入口只提示“该页面尚未移植竖屏”。
- 交互与 PPA：四页共用唯一 `portrait_scroll`，透明内容层清除 `CLICKABLE`；卡片和工具按钮复用 8px 位移、`PRESS_LOST`、`SCROLL_BEGIN/SCROLL` 点击取消规则。定时器/秒表动态数字使用小范围动态覆盖，页面/方向切换后重新绑定 PPA。
- WebUI/路由：`more`、`timer`、`alarm`、`stopwatch` 已加入固件、WebUI 和 MockTransport 竖屏白名单；工具页之间及与 HOME/CHAT/PLAN 切换保持竖屏，未移植页面继续提示并按 USB 横屏兼容路径打开。
- 实机最终截图：`tmp/cache/screenshot_test/portrait_more_final.bmp`、`portrait_timer_final.bmp`、`portrait_alarm_final.bmp`、`portrait_stopwatch_final.bmp`；四页均由固定入口从 COM17 读回，并使用 `view_image` 实际检查。
- 视觉结果：四页顶部、中部可见内容没有进入导航栏或 288px 状态舱；MORE-01 双列卡、定时器 3×2 预设、闹钟真实开关/周期空态、秒表控制/计次空态均未裁切；最终标题与 MORE 页面 ID 胶囊已避让，无重叠；隐私状态舱摄像头保持关闭态。
- 构建与启动：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build`、COM17 no-stub flash/Hash 校验均通过；固定入口连续两次满足 `ready/resourceReady/framebuffersSynced/pageFrameReady` 且页面/方向匹配。启动日志未见本轮新增 brownout、PSRAM 初始化失败、WDT、Guru Meditation 或反复重启。
- 已知环境项：构建仍保留既有 `ota_1` 2MiB recovery 槽容量不足告警；启动仍可能输出既有 coredump `ESP_ERR_INVALID_CRC` 和非致命 I2C NACK，本次未修改分区、MIPI、GPIO、电源、摄像头路径。
- 触摸与动态状态：代码路径已完成拖动取消和动态覆盖接入；当前 USB 诊断没有触摸注入能力，因此真实手指拖动超过 8px、真实抬手点击、活动倒计时/秒表更新和长列表主观手感仍需现场手动回归，未将其记为已完成。
- 备份：编辑前 `tmp/backups/manual/20260804-003433_portrait_more/`；运行前 `tmp/backups/before_run/20260804-005326_portrait_more/`、`tmp/backups/before_run/20260804-010137_portrait_more_id_fix/`。

final result: partial — 固件/WebUI、四页稳定门和实机截图视觉验收通过；现场真实触摸及活动动态状态回归待补

----

# MORE-04～06 设置页竖屏设计验收（2026-08-04）

- 参考真值：`ui-reference/ver2` 的 MORE-04、MORE-05、MORE-06；逻辑尺寸为 `600×1024`，主区/导航/隐私状态舱为 `600×672 / 64 / 288px`。
- 固件实现：新增稳定页面键 `safety`（MORE-04），并将 `settings`、`maintenance` 纳入竖屏白名单；三页均使用 `150px` 左侧分区栏、`12px` 间距、约 `406px` 右侧正文和唯一 `portrait_scroll`，分区文字使用竖屏小字号。
- MORE-04：设备概览、连接状态、传感器和厨房安全分区只展示设备/网络/TF/SHT4x/P4 雷达现有快照；当前 P4 最小闭包未接入 S3 legacy 光敏/IMU 链路，因此两项显示“未接入”，不把零值冒充真实读数。安全事件无真实队列时为中性空态，摄像头显示隐私态关闭。
- MORE-05：网络与同步、AI Provider 与模型、Agent 权限、语音与显示、隐私与家庭分区接入现有 Wi-Fi 弹层、权限模式、WakeNet 和亮度回调；API Key 不显示，未实现能力保持提示，Wi-Fi 弹层打开/关闭会暂停并重绑 PPA。
- MORE-06：存储、插件、日志、系统更新、关于分区展示真实 TF/诊断/版本/设备信息；第三方插件、日志导出、OTA 和检查更新保持中性提示，不注入序列号、最新版本或首次激活时间。
- 交互边界：分区、卡片和按钮沿用 8px 拖动取消、`PRESS_LOST`、`SCROLL_BEGIN/SCROLL` 保护，透明内容层清除 `CLICKABLE`；未修改 MIPI、GPIO、电源、显示时序或摄像头路径。
- 构建核对：ESP-IDF 5.5.1 包装脚本 `build`、Web `npm run build` 已通过；COM17 no-stub 烧录完成，写入 Hash 校验通过。构建仍保留既有 `ota_1` 2MiB recovery 槽容量不足告警，本次未修改分区。
- 实机截图：固定入口分别完成 `safety + portrait_privacy`、`settings + portrait_privacy`、`maintenance + portrait_privacy`，三页均连续两次满足 `ready/resourceReady/framebuffersSynced/pageFrameReady`，并完成 BMP/CRC32/文件头检查；间距修复版路径为 `tmp/cache/screenshot_test/portrait_safety_spacing_fix.bmp`、`tmp/cache/screenshot_test/portrait_settings_spacing_fix.bmp`、`tmp/cache/screenshot_test/portrait_maintenance_spacing_fix.bmp`。
- PPA 与视觉：三页均建立 `600×1132 + 80px elastic` 缓存、`600×585` 视口；`view_image` 实际检查确认标题、MORE-04/05/06 标识、收窄后的分区栏、右侧数值/说明、导航和隐私状态舱无重叠、裁切或遮挡，摄像头保持关闭态。Wi-Fi 管理入口、真实 TF/网络/诊断数据和未实现能力提示均可见。
- 启动与硬件边界：日志确认 ESP32-P4 rev1.3、32MB PSRAM memory test OK、`brownout=0`，未见本轮新增 Guru Meditation、WDT 或反复重启；既有 coredump `ESP_ERR_INVALID_CRC` 与 `ota_1` 容量告警仍存在。维护页 TF 读回期间有一次 I2C transaction NACK，但截图传输最终成功并通过 CRC32/文件头校验；本轮未修改 MIPI、GPIO、电源、显示时序或摄像头路径。
- 交互待现场：固定 USB 诊断没有触摸注入能力，真实手指滚动/分区切换超过 8px 后取消点击、真实抬手、Wi-Fi 弹层和亮度滑块仍需现场回归。

final result: partial — 固件/WebUI、COM17、三页稳定门与截图视觉验收通过；现场真实触摸回归待补
