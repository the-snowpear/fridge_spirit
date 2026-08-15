# `esp_cam_sensor` / `esp_video` 源码补齐与接入边界

更新时间：2026-07-18（Asia/Shanghai）

## 目的

本次仅恢复 ESP32-P4 摄像头栈所需的两套 Espressif 官方组件源码，为后续 OV5647 MIPI-CSI 接入提供可信基线。当前主固件仍使用 `fridge_camera_p4_stub.c`，没有启用摄像头、没有修改 GPIO/SCCB/CSI 时序，也没有新增 Agent 功能。

## 固定版本与来源

| 组件 | 固定版本 | 官方提交 | 官方压缩包 SHA-256 | 活动目录 |
| --- | --- | --- | --- | --- |
| `espressif/esp_cam_sensor` | `0.5.3` | `870d758608bdb338d55475f69b0d004f28da9e91` | `E4214A99993C012161DFDDB7661DAEDF0F11361C5E94071DD944E5ED782C40FE` | `third_party/espressif__esp_cam_sensor/` |
| `espressif/esp_video` | `0.5.1` | `7c343dc478f73e3234ed898eb358accd8de92ff7` | `17513950F305E964BF9A7F301D6EECF8D34D04C66D5C8F296D6C2D9A1D107D73` | `third_party/espressif__esp_video/` |

官方登记页：

- <https://components.espressif.com/components/espressif/esp_cam_sensor/versions/0.5.3>
- <https://components.espressif.com/components/espressif/esp_video/versions/0.5.1>

组件内容直接来自上述固定版本官方归档，没有把恢复碎片与官方源码混合，也没有带入恢复工具生成的 `.component_hash`。

## 恢复量与完整性

事故恢复碎片中只有 42 个可与官方文件逐项匹配的文件、15,606 字节。完整官方基线共 113 个文件、840,388 字节，因此实际补回 71 个文件、824,782 字节。

| 组件 | 官方文件数 | 官方字节数 | 事故碎片中有效文件 | 补回文件 | 补回字节 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `esp_cam_sensor 0.5.3` | 69 | 416,940 | 31 | 38 | 404,575 |
| `esp_video 0.5.1` | 44 | 423,448 | 11 | 33 | 420,207 |
| 合计 | 113 | 840,388 | 42 | 71 | 824,782 |

补齐后重新解包官方归档并逐文件比较 SHA-256：两个目录均为 `missing=0 / extra=0 / different=0`。同时确认零空文件、零文本 NUL、零 `.component_hash`。

## 依赖兼容性

隔离验证工程固定使用以下组合：

- ESP-IDF `5.5.1`，目标 `esp32p4`
- `esp_cam_sensor 0.5.3`
- `esp_video 0.5.1`
- `esp_sccb_intf 0.0.3`
- `esp_h264 1.0.4`
- 解析得到 `cmake_utilities 0.5.3`

官方 manifest 约束为：

- `esp_cam_sensor`：仅支持 `esp32p4`，要求 IDF `>=5.3`、`esp_sccb_intf >=0.0.2`。
- `esp_video`：仅支持 `esp32p4`，要求 IDF `>=5.3`、`esp_cam_sensor 0.*`、`esp_h264 >=1.0.3`。

上述固定组合满足全部 manifest 约束。隔离工程已完成编译和链接，生成 `camera_vendor_validation.bin`，证明官方组件及其依赖在项目锁定的 IDF 5.5.1 工具链下可构建。

## 当前接入边界

1. 两个组件只作为已验证的第三方源码基线存放在 `third_party/`。
2. `components/camera/idf_component.yml` 的恢复碎片继续留在隔离目录，没有放回活动路径。
3. 主工程 `main` 仍未依赖 `esp_cam_sensor` 或 `esp_video`；重新配置后的 `build_components` 也不包含它们。
4. 主工程 `dependencies.lock` 没有新增摄像头依赖，重配置前后 SHA-256 均为 `8DBC55F0872FB4ED75391D4976E9CB1CD062278E9C1BD9FFF6118C511EA46F3C`。
5. 没有恢复或猜测事故前的 Bayer/CCM/CSI 私有修改。现存相关备份含随机数据和大量 NUL，不能作为可信补丁来源。
6. OV5647 的真实 `fridge_camera_p4.c`、共享 GPIO28/29 SCCB 与 ES8311 的串行化、帧缓冲/JPEG/上传链路仍属于后续单独开发任务。

## 验证结果

- 隔离五件套构建：通过。
- P4 主工程 `reconfigure build size`：通过。
- 主固件：1,873,728 字节，SHA-256 `7ED6D49CD52E6330AD14BCF6BD967C20C6181063A1C16D8A3BA3DF2D6C736B09`。
- `srmodels.bin`：579,120 字节，SHA-256 `BAAB09BB3822C45BB25EB7CD4889FDB526D033B957B4610A226569557FE96443`。
- 最小 2 MiB APP 分区剩余 `0x368c0` 字节，约 11%。
- 主工程构建闭包未出现 `esp_cam_sensor` / `esp_video`，说明此次补源码没有静默启用摄像头或增加当前固件体积。
- 本次未烧录、未上电、未做摄像头真机测试。

## 备份与回滚

- 编辑前备份：`tmp/backups/manual/20260718-173545_camera_vendor_completion/`
- 主工程构建前备份：`tmp/backups/before_run/20260718-174624_camera_vendor_main_build/`
- 原不完整恢复碎片：`_rebuild_reports/quarantine/20260718_firmware_repair/`

如需回滚本次补齐，只移除两个新活动目录和本说明文档即可；不要删除隔离目录或历史备份。
