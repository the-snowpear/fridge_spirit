# P4 联网本地组件与兼容补丁

本目录保存 ESP32-P4 CB V1.2 联网链路的固定组件版本，供当前 ESP-IDF 6.0.1
主工程通过 `override_path` 可重复构建：

- `espressif__esp_hosted`：2.12.11（官方提交 `da7412f9b5a31b54bd57acb140247e95d54c7eed`）
- `espressif__esp_wifi_remote`：0.15.0

Hosted Host 与 C6 Slave 必须使用同一版本。2.12.11 官方包 SHA-256 为
`CC8B0C9FEF20339242CEC79DE841B1012132F4134DFD377A5DAA3CC03A24B02F`；该版本修复
共享 SDMMC Host 影响另一槽 TF 卡和 SDIO RX 回收问题。本项目在官方代码上只保留
下述可审计的 IDF 6/板级生命周期补丁。

## ESP-IDF 6.0.1 本地兼容补丁

1. `espressif__esp_wifi_remote/idf_v6.0/include/injected/esp_wifi_types_generic.h`
   - 移除 IDF 6 已删除的 `esp_interface.h` 引用。
   - `wifi_interface_t` 数值顺序与 IDF 6.0.1 官方头文件保持一致。
2. `espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_sdio.c`
   - LDO4 handle 在整个 SDIO 生命周期内只申请一次，card-init 重试复用；完整 deinit
     时才释放，且只反初始化 Hosted 的 Slot1。
3. `espressif__esp_hosted/host/api/src/esp_hosted_api.c`
   - reconfigure/remote init 返回真实底层错误，不再把链路失败伪装成 `ESP_OK`。

审计本地组件时应同时核对 `idf_component.yml` 的版本/提交、上述官方包 SHA-256 和
Git diff；本地补丁后的目录不能再用上游压缩包哈希直接验证整树一致性。
