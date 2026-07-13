# P4 联网本地组件与兼容补丁

本目录保存慧勤智远 ESP32-P4 CB V1.2 厂商例程所使用的联网组件版本，供当前
ESP-IDF 6.0.1 主工程通过 `override_path` 固定引用：

- `espressif__esp_hosted`：2.2.3
- `espressif__esp_wifi_remote`：0.15.0

固定旧版本的原因是先与板载 ESP32-C6 的现有 slave 固件和厂商例程对齐；真机握手
确认前不直接升级到组件仓库最新版本。原始来源位于本地厂商资料目录
`doc/慧勤智远 ESP32-P4 CB V1.2小系统板/3. 程序源码/`。

## ESP-IDF 6.0.1 本地兼容补丁

1. `espressif__esp_wifi_remote/idf_v6.0/include/injected/esp_wifi_types_generic.h`
   - 移除 IDF 6 已删除的 `esp_interface.h` 引用。
   - `wifi_interface_t` 数值顺序与 IDF 6.0.1 官方头文件保持一致。
2. `espressif__esp_hosted/CMakeLists.txt`
   - 补充 IDF 6 拆分后的 `esp_driver_gpio`、`esp_driver_sdmmc`、`esp_driver_dma`
     私有依赖；不改变 hosted 协议或运行逻辑。

两个组件目录中的 `.component_hash` 是复制时保留的**上游原始树哈希**。由于上述文件
存在明确的本地补丁，它们不再代表当前整个目录的完整性哈希；审计时应结合本说明和
Git diff，而不能把该值当作修改后树的校验结果。

