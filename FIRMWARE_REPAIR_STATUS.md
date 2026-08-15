# ESP32-P4 固件恢复修复状态

更新时间：2026-07-18（Asia/Shanghai）

## 修复范围

本次只恢复已有 ESP32-P4 minimal 固件的源码一致性和可构建性，没有新增或开发 Agent 功能，也没有烧录硬件。

可信源码基线：

- 安全仓库：`C:\Users\123\fridge_spirit_recovery_20260717`
- Git 提交：`150a77ebe9e0750180c4901c9bdac1825d626520`（`屏幕点亮`）
- 工具链：ESP-IDF 5.5.1 / RISC-V GCC 14.2.0
- 目标：ESP32-P4 rev v1.3 / 16MB Flash / 32MB PSRAM

## 已修复

1. 从安全仓库恢复 `third_party/espressif__esp_hosted`。
   - 恢复前确认 20 个文件被随机二进制数据覆盖。
   - 构建关键文件包括 `common/mempool/mempool.c`、`common/esp_hosted_header.h`、`common/esp_hosted_lwip_src_port_hook.h` 和 `common/log/esp_hosted_log.h`。
   - 修复后该目录与安全 Git 基线无内容差异。
2. 核验 `third_party/espressif__esp_wifi_remote`，314 个文件与安全副本一致。
3. 将 `components/radar`、`components/sensors`、`components/speaker` 的 `CMakeLists.txt` 恢复为与现有 Git 基线源码配套的版本。
4. 隔离恢复出来但不完整、且不属于 Git 基线的相机与扬声器 manifest。
5. 隔离缺少根 `CMakeLists.txt` 和主体源码的 `esp_cam_sensor`、`esp_video` 恢复目录。
6. 重新生成 `dependencies.lock`，本地依赖路径现已指向 `fridge_spirit_p4`，不再指向事故前目录。
7. 重新生成 ESP32-P4 `sdkconfig` 和默认 `build/` 构建目录。

## 备份与隔离

- 修复前备份：`_rebuild_reports/repair_backups/20260718-164139_firmware_repair/`
- 隔离目录：`_rebuild_reports/quarantine/20260718_firmware_repair/`

隔离内容没有删除，可按需人工审查，但不得直接重新放回活动组件路径。

## 构建验证

使用项目固定包装脚本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 set-target esp32p4
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 size
```

最终再次执行 `build`，无编译错误和 Ninja 恢复警告。

产物：

- `build/fridge_spirit.bin`：1,873,728 字节（`0x1c9740`）
  - SHA-256：`7ED6D49CD52E6330AD14BCF6BD967C20C6181063A1C16D8A3BA3DF2D6C736B09`
- `build/srmodels/srmodels.bin`：579,120 字节
  - SHA-256：`BAAB09BB3822C45BB25EB7CD4889FDB526D033B957B4610A226569557FE96443`
- 最小 APP 分区：2MB
- APP 剩余：`0x368c0` 字节，约 11%
- Bootloader：22,672 字节，剩余约 8%
- 烧录参数确认：ESP32-P4、16MB Flash、`stub=false`

内存静态占用摘要：

- Flash：1,758,452 字节
- DIRAM：145,588 / 576,464 字节（25.26%）
- LP RAM：24 / 32,768 字节

## 依赖版本

- ESP-IDF 5.5.1
- ESP-SR 2.4.5，构建选择 `esp32p4_less_v3`
- ESP-Hosted 2.12.11
- ESP Wi-Fi Remote 0.15.0
- ESP-DSP 1.8.0

## 仍然存在的边界

- P4 摄像头仍为 `fridge_camera_p4_stub.c`，真实 `fridge_camera_p4.c` 未恢复；相机功能明确不可用。
- 雷达、传感器和扬声器仍是旧 S3 基线实现，没有加入当前 P4 minimal 构建闭包，也没有进行 P4 引脚迁移。
- `sdkconfig.recovery.defaults.esp32p4` 仍缺失，因此 P4 recovery 镜像尚不可复现。
- Agent Runtime、Tool Broker、Workflow、Skill、MCP、Trace 及 TF Agent 打包工具没有在本次修复中新增或重建。
- 本次没有烧录、串口监视或真机回归；只确认源码完整性、依赖解析、编译、链接和镜像生成。

## 后续使用约束

1. 当前可构建交付物仅是 P4 minimal 固件。
2. 不要重新启用隔离的相机 manifest 或不完整第三方目录。
3. 项目目录再次移动后，应重新生成 `dependencies.lock`，因为本地 override 会记录绝对路径。
4. 烧录前继续使用 `tools/idf.ps1`，并按历史硬件要求使用 CH343P、ROM loader、115200 和 no-stub。
5. 任何后续源码修改前继续创建备份，修改后重新执行构建验证。

## 2026-07-18 摄像头官方组件源码补齐

- 已从 Espressif 官方固定版本归档补齐 `third_party/espressif__esp_cam_sensor`（0.5.3）和 `third_party/espressif__esp_video`（0.5.1）。
- 两个活动目录共 113 个文件、840,388 字节；与官方归档逐文件 SHA-256 比较均为 `missing=0 / extra=0 / different=0`。
- 使用 IDF 5.5.1 / ESP32-P4 的隔离工程验证 `esp_cam_sensor 0.5.3 + esp_video 0.5.1 + esp_sccb_intf 0.0.3 + esp_h264 1.0.4`，编译和链接通过。
- 主工程重新配置、构建和 size 检查通过；主固件及模型哈希保持不变，`dependencies.lock` 也保持不变。
- 摄像头 manifest 继续隔离，主工程仍使用摄像头 stub；本次只补可信源码，没有启用摄像头或重建事故前私有补丁。
- 详细来源、哈希、依赖和边界见 `doc/ESP_CAM_SENSOR_ESP_VIDEO源码补齐与接入边界.md`。
