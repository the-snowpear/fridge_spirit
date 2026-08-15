# fridge_spirit_p4 恢复状态

更新时间：2026-07-18（Asia/Shanghai）

## 结论

本目录是事故后的**重建工作区**，不是事故前未提交状态的完整镜像。

- 可用基线来自完整 Git 对象库：`C:\Users\123\fridge_spirit_recovery_20260717`
- 基线提交：`150a77ebe9e0750180c4901c9bdac1825d626520`（`屏幕点亮`）
- 当前分支：`recovered-main`
- R-Studio 恢复源：`C:\Recovery\d`
- 原损坏目录：`D:\esp-IDF\Project\fridge_spirit`
- 重建目标：`D:\esp-IDF\Project\fridge_spirit_p4`

原目录、R-Studio 输出和安全 Git 副本均未删除。本目录可以继续整理和重建，但在完成 P4 差异复原前，不应被视为事故前最新版。

## 重建方法

1. 先从安全 Git 副本复制完整提交基线。
2. 扫描 R-Studio 输出，排除构建目录、工具环境、缓存及明显误恢复目录。
3. 拒绝全 NUL、零长度、无效 UTF-8，以及文件签名不匹配的常见二进制文件。
4. 对通过校验且内容不同的文件覆盖基线；对通过校验的新文件追加到目标目录。
5. 保存完整筛选清单和 Git 检查结果。

扫描统计：

- 扫描恢复源文件：125,247
- 全 NUL 文件：112,865（未复制）
- 零长度文件：172（未复制）
- 文件签名无效：3（未复制）
- 排除项：173
- 与基线相同：87
- 有效新增：10,982
- 有效覆盖：1,138
- 复制错误：0

注意：通过基础校验只表示文件不是明显的空数据或错误格式，并不证明代码逻辑、版本配套或硬件行为正确。

## Git 与结构检查

- `git fsck --full --no-reflogs` 未发现 fatal、corrupt 或 missing 对象。
- 6 份 `data/agent_defaults` JSON 均可解析。
- 根 `CMakeLists.txt`、`main/main.c`、ESP32-P4 默认配置及主要组件基线文件均存在且非全 NUL。
- 当前工作区包含 1,138 个已跟踪变更和若干有效新增文件；大量变化来自恢复出的文档和第三方资料，不能全部视为事故前人工修改。
- 尚未执行 ESP-IDF 构建、烧录或真机验证。

## 已知事故前重点文件恢复情况

重点清单共 50 项：

- 39 项：R-Studio 版本为全 NUL，当前仅保留最后 Git 提交版本。
- 4 项：Git 基线中没有，R-Studio 版本也不可用，因此缺失。
- 3 项：恢复出的有效新增文件。
- 3 项：恢复出的有效覆盖或变更文件。
- 1 项：Git 基线可用，恢复版本相同或等价。

完全缺失的 4 项：

- `components/camera/fridge_camera_p4.c`
- `sdkconfig.recovery.defaults.esp32p4`
- `tools/agent_tf_package.py`
- `tests/tools/test_agent_tf_package.py`

关键限制：`main/main.c`、TF 卡、雷达、传感器、扬声器、触摸、USB 协议、语音、唤醒词、P4 配置及 Web 代码的恢复副本多数为全 NUL。因此当前文件虽然可读，但主要是提交 `150a77e` 的旧基线，不能代表 2026-07-14 左右尚未提交的最新实现。

## 已恢复的重点新增内容

- `components/camera/idf_component.yml`
- `components/speaker/idf_component.yml`
- `data/agent_defaults/source.json`
- `data/agent_defaults/config/*.json`
- `data/agent_defaults/skills/kitchen/inventory_query/manifest.json`
- `data/agent_defaults/workflows/inventory/query.json`
- 部分 ESP32-P4、摄像头、音频和传感器参考资料及第三方组件文件

## 后续 AI 使用规则

1. 先阅读本文件和 `_rebuild_reports/preincident_focus.csv`，不要默认当前目录是事故前最新版。
2. 以提交 `150a77e` 为可信代码基线；恢复覆盖文件应逐项审查后再提交。
3. 不要从 `D:\esp-IDF\Project\fridge_spirit` 或 `C:\Recovery\d` 直接批量覆盖本目录。
4. 重建 P4 功能时，优先依据原设计文档、任务历史、串口日志、已知硬件接线和厂商示例逐组件恢复。
5. 未确认具体 P4 板卡版本、GPIO、外设、电气约束、ESP-IDF 与组件版本兼容性前，不进行硬件相关改写或声称构建可用。
6. 每轮修改前先备份，修改后至少检查 Git 差异；具备完整环境和兼容性证据后再构建。
7. UI 不是当前恢复优先级；优先保证 TF 卡资源、设备功能、协议和技术文档。

## 报告索引

- `_rebuild_reports/summary.json`：恢复统计
- `_rebuild_reports/preincident_focus.csv`：事故前重点文件分类
- `_rebuild_reports/overlaid_files.txt`：覆盖文件清单
- `_rebuild_reports/copied_extra_files.txt`：新增文件清单
- `_rebuild_reports/all_nul_files.txt`：全 NUL 文件清单
- `_rebuild_reports/zero_length_files.txt`：零长度文件清单
- `_rebuild_reports/invalid_signature_files.txt`：签名无效文件清单
- `_rebuild_reports/excluded_files.txt`：排除项清单
- `_rebuild_reports/git_fsck.txt`：Git 对象检查
- `_rebuild_reports/git_status.txt`：重建后的 Git 状态快照
- `_rebuild_reports/salvage_overlay.py`：本次筛选与覆盖脚本

## 下一阶段建议

下一阶段应先重建 4 个完全缺失文件，再对 39 个“仅 Git 基线”重点文件按照功能模块逐一比对和复原。每个模块完成后分别构建验证，避免把大量恢复覆盖一次性混入同一提交。
