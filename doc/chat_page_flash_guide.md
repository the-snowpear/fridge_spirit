# 对话页烧录指南

## 当前状态
对话页代码已完成，需要烧录到设备进行真机测试。

## 烧录步骤

### 方法 1：使用项目脚本（推荐）
在项目根目录 `D:\esp-IDF\Project\fridge_spirit_p4` 打开 PowerShell 执行：

```powershell
# 仅构建（检查编译错误）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 build

# 构建并烧录到 COM17
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM17 flash

# 烧录并监视串口输出
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM17 flash monitor
```

### 方法 2：使用标准 ESP-IDF 命令
如果项目脚本不存在，可以使用标准命令：

```powershell
# 激活 ESP-IDF 5.5.1 环境
D:\esp-IDF\v5.5.1\esp-idf\export.ps1

# 设置目标（仅首次或切换目标时）
idf.py set-target esp32p4

# 构建
idf.py build

# 烧录到 COM17
idf.py -p COM17 flash

# 监视串口
idf.py -p COM17 monitor
```

## 预期编译结果
- 新增文件应该正常编译
- 新增代码约 560 行
- 如果有编译错误，请检查语法

## 烧录后测试步骤

### 1. 观察启动日志
等待设备启动完成，应该看到：
- PSRAM 初始化成功
- TF 卡挂载成功
- UI 初始化成功
- 显示 "ready" 事件

### 2. 进入对话页
**方法 A：触摸导航（推荐）**
- 点击屏幕左侧导航栏第 3 个按钮"对话"

**方法 B：USB 命令（备用）**
通过串口发送：
```json
{"command":"tool_call","request_id":"test001","tool":"ui.switch_page","arguments":{"page":"chat"}}
```

### 3. 验证显示效果
应该看到：
- ✅ 页面标题："对话"
- ✅ 页面 ID："CHAT-01"
- ✅ 副标题："与 Agent 讨论库存、菜谱和计划"
- ✅ 导航按钮高亮（"对话"按钮变白色）
- ✅ 3 条示例消息：
  - 用户问题（右侧蓝色气泡）
  - 工具调用卡（绿色边框）
  - Agent 回复（左侧深绿气泡）
- ✅ 底部输入区：
  - 3 个常用指令 chips
  - 输入框
  - 语音按钮
  - 发送按钮

### 4. 当前限制
⚠️ 以下功能尚未实现（点击无响应是正常的）：
- 常用指令 chips 点击
- 输入框点击
- 语音按钮点击
- 发送按钮点击
- 工具卡/结果卡点击

这些交互需要后续开发 Agent 集成才能工作。

## 常见问题

### Q1: 编译错误 - 找不到符号
**原因**：语法错误或头文件引用问题  
**解决**：检查编译输出的具体错误信息

### Q2: 烧录失败 - COM17 被占用
**原因**：串口被其他程序占用  
**解决**：关闭 idf_monitor、Web Serial、串口助手等

### Q3: 屏幕白屏或花屏
**原因**：UI 初始化失败  
**解决**：查看串口日志，检查 LVGL/显示初始化错误

### Q4: 点击"对话"按钮无响应
**原因**：触摸驱动或事件分发问题  
**解决**：
1. 使用 USB 命令切换页面验证页面本身是否正常
2. 检查触摸驱动日志
3. 验证其他导航按钮是否工作

### Q5: 显示乱码
**原因**：字体未加载  
**解决**：
1. 确认 TF 卡已插入
2. 确认 TF 卡上有 Noto Sans CJK 字体
3. 查看串口日志中的字体加载信息

## 调试命令

### 获取设备状态
```json
{"command":"get_status","request_id":"status001"}
```

### 切换到其他页面
```json
{"command":"tool_call","request_id":"nav001","tool":"ui.switch_page","arguments":{"page":"home"}}
{"command":"tool_call","request_id":"nav002","tool":"ui.switch_page","arguments":{"page":"plan"}}
{"command":"tool_call","request_id":"nav003","tool":"ui.switch_page","arguments":{"page":"more"}}
```

## 下一步开发

烧录测试成功后，可以开始：
1. 实现输入事件处理
2. 集成 agent_runtime
3. 实现消息流动态更新

---

**注意**：由于我无法直接执行 PowerShell 命令，请你手动执行上述烧录命令。如果遇到问题，请告诉我具体的错误信息。
