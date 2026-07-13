# ESP-IDF 5.5.1 迁移与已验证版本说明（恢复本地“小冰小冰”WakeNet）

> 2026-07-13 完成。主固件从 ESP-IDF 6.0.1 降级到 5.5.1，恢复 ESP-SR WakeNet less-v3
> 本地唤醒“小冰小冰”，并在慧勤智远 ESP32-P4 CB V1.2（COM15，ESP32-P4 rev1.3）真机烧录验证。
> IDF 6.0.1 安装保留，可随时回退。

---

## 1. 为什么降级（根因）

- ESP32-P4 rev1.3 属于 “less than v3” 硅片，ESP-SR 的 WakeNet/AFE 对该硅片提供**预编译静态库**
  `lib/esp32p4_less_v3/libwakenet.a`、`libesp_audio_front_end.a`。
- 这些静态库由乐鑫用 **riscv GCC 14.2.0**（ESP-IDF 5.5 时代工具链）编译。
- ESP-IDF **6.0.1 使用 riscv GCC 15**，链接 less-v3 预编译库时出现
  `R_RISCV_RVC_LUI relocation truncated`，即使删掉 ESP-SR 的 IDF 版本检查、加 `--no-relax`
  也无法解决——这是预编译库与 GCC15 的 ABI/重定位不兼容，不是配置问题。
- 结论：必须使用官方匹配的 **ESP-IDF 5.5.x（GCC 14.2.0）** 工具链，不再尝试绕过 IDF6 检查。

## 2. 选定的精确版本组合与官方依据

| 组件 | 版本 | 依据 |
| --- | --- | --- |
| **ESP-IDF** | **5.5.1** | 厂商随板 `小智AI` 例程 `wks_esp32p4_cb_xiaozhi` 的 `dependencies.lock` 锁定 IDF 5.5.1（同一块 ESP32-P4 CB V1.2 板）。riscv GCC 14.2.0（`esp-14.2.0_20241119`）。 |
| **espressif/esp-sr** | **2.4.5** | 现有 P4 唤醒代码使用 esp-sr 2.4.x 的新 AFE API（`afe_config_init`/`AFE_TYPE_SR`/`fetch_with_delay`），`sdkconfig.defaults.esp32p4` 也用 esp-sr 2.4.x 的 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`。registry 声明 `idf>=5.0`。 |
| WakeNet 库路径 | **esp32p4_less_v3** | esp-sr 2.4.5 的 `CMakeLists.txt` 在 IDF 5.x 且 `MINOR<5 或 PATCH<3 或 SELECTS_REV_LESS_V3` 时自动选 `esp32p4_less_v3`。**IDF 5.5.1 的 PATCH=1<3，因此自动选中 less_v3，与 rev1.3 匹配。** 构建日志确认 `TARGET_LIB_PATH is set to: esp32p4_less_v3`，map 文件确认链接的是 `lib/esp32p4_less_v3/*.a`。 |
| WakeNet 模型 | **wn9_xiaobinxiaobin_tts** | esp-sr 2.4.5 自带，`CONFIG_SR_WN_WN9_XIAOBINXIAOBIN_TTS=y`。 |
| VAD 模型 | vadnet1_medium | `CONFIG_SR_VADN_VADNET1_MEDIUM=y`；未加载任何 MultiNet（`SR_MN_CN_NONE`/`SR_MN_EN_NONE`）。 |
| espressif/esp_hosted | **2.12.11** | 本地 `third_party/espressif__esp_hosted` override（Host 与板载 C6 Slave 同版本）。registry 声明 `idf>=5.3`，兼容 5.5.1。 |
| espressif/esp_wifi_remote | **0.15.0** | 本地 `third_party/espressif__esp_wifi_remote` override，其 CMake 按 `idf_v${MAJOR}.${MINOR}` 自动选 `idf_v5.5/` 适配目录（非 IDF6 注入头文件路径）。 |

> **为什么不是 5.5.3/5.5.4**：esp-sr 2.4.5 只有在 IDF 5.x 且 `PATCH<3` 时才自动选 `esp32p4_less_v3`。
> 5.5.3+ 的 PATCH≥3 会让 esp-sr 误选 rev3.0 的 `esp32p4/` 库（该硅片选择符号 `SELECTS_REV_LESS_V3`
> 在 IDF 5.5 里并不存在，无法强制），与 rev1.3 不匹配。**5.5.1 是能自动锁定 less_v3 的最高稳定补丁版。**

## 3. 工具链安装（与 IDF6 并存，可回退）

- IDF 5.5.1 源码：`D:\esp-IDF\v5.5.1\esp-idf`（`git clone -b v5.5.1 --depth 1 --recursive`）。
- 公共工具目录仍是 `D:\esp-IDF\.espressif`（IDF6 与 5.5 共用，互不覆盖）。
- IDF 6.0.1 保留在 `D:\esp-IDF\.espressif\v6.0.1\esp-idf`，未删除。
- `tools/idf.ps1` 已指向 5.5.1；如需回退 IDF6，改其中一行 `export.ps1` 路径即可。
- 构建/烧录用同样的 `IDF_TOOLS_PATH=D:\esp-IDF\.espressif` + dot-source `export.ps1` 流程。

## 4. 为迁移做的代码/配置改动（仅兼容性，未动硬件/分区/C6）

1. **撤销 microWakeWord，恢复 WakeNet**：逐文件比对 `tmp/backups/manual/20260712-164741_microwakeword_port`
   还原 `components/wake_word/{CMakeLists.txt,idf_component.yml,fridge_wake_word.c,include/fridge_wake_word.h}`、
   `components/usb_protocol/fridge_usb_protocol_minimal.c`、`sdkconfig.defaults.esp32p4` 的 WakeNet 段；
   删除 mww 专属文件（`fridge_mww_backend.cpp/.h`、`fridge_wake_model_config.h`、`models/`、`tools/microwakeword/`、
   mww 训练文档）与 WebUI 的 mww 文案/字段（4 个 web 文件）。删除前均已备份到本次迁移备份目录。
2. **`components/codec_es8311/idf_component.yml`**：`idf` 依赖从 `>=6.0.0,<7.0.0` 放宽到 `>=5.5`。
3. **`main/CMakeLists.txt`**：最小闭包显式加入 `esp_psram`。IDF 5.5.1 用 `set(COMPONENTS main)` 最小
   闭包时，`esp_psram` 属 `esp_system` 的**可选**依赖不会自动纳入，导致 `CONFIG_SPIRAM` 等符号未注册、
   32MB PSRAM 关闭；显式 REQUIRES 后 PSRAM Kconfig 注册、PSRAM 正常。
4. **`sdkconfig.defaults.esp32p4`**：
   - 新增 `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`。IDF 5.5.1 把 P4 PSRAM 200MHz 归为实验特性
     （`SPIRAM_SPEED_200M` 依赖 `IDF_EXPERIMENTAL_FEATURES`），不开则 200M 选项不可见、回落 20MHz。
     该板 PSRAM 实测 200MHz memtest OK，保持既定频率。
   - 新增 `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`。见下节 hosted 修复。
   - `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` 保留（在 IDF 5.5.1 里是未知符号、仅有一条无害告警；
     less_v3 库已由 esp-sr CMake 的 `PATCH<3` 自动选中，符号有无都走 less_v3）。
5. **`components/wake_word/fridge_wake_word.c`**：init 时 `esp_log_level_set("AFE", ESP_LOG_ERROR)`，
   抑制 AFE 内部增强线程在两帧之间打印的 `Ringbuffer of AFE is empty` 警告（节拍差异下的正常提示，
   不影响检测；不抑制会以约 20 条/秒刷爆 USB JSON 控制台）。

### 4.1 C6 Hosted 在 5.5.1 的关键修复（SDIO mempool 崩溃）

- 现象：首次 5.5.1 真机启动 hosted 初始化即 `assert failed: sdio_mempool_create sdio_drv.c:255 (buf_mp_g)`，
  日志 `E HS_MP: mempool create failed: no mem`。
- 根因：SDIO 收发缓冲池需约 48KB **片内 DMA** 内存（`INTERNAL|DMA`）。IDF 6 给 hosted 额外挂了约
  120KB 的 `RETENT_RAM` 堆区，5.5.1 不再暴露该区（首个内部堆区仅 93KB 且起始更高），启动早期片内
  DMA 内存不足以分配整个缓冲池。
- 修复：开启 hosted 官方为 P4 提供的 `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`——SDIO 传输缓冲
  优先从 DMA 可达的 PSRAM 分配（P4 的 GDMA 经 cache 可达 PSRAM），失败再回退片内。既修复崩溃又
  保住紧张的片内 RAM。**未改 C6 Slave 固件、未改分区、未改 SDIO 引脚/LDO4。**

## 5. 构建与镜像（干净构建，未复用 IDF6 build 目录）

- 干净流程：删 `build/`、`managed_components/`、`dependencies.lock`、`sdkconfig`；
  `idf.py set-target esp32p4` → `reconfigure` → `build`。
- 依赖解析结果：esp-sr 2.4.5 / esp_hosted 2.12.11 / esp_wifi_remote 0.15.0 / esp-dsp 1.8.0 /
  dl_fft 0.6.0 / cjson 1.7.19 / esp_codec_dev 1.5.4 / idf 5.5.1。
- 编译器：riscv32-esp-elf **GCC 14.2.0**。**无 `R_RISCV` 重定位错误；非 Kconfig 的编译告警 0。**
- 唯一配置告警：`unknown kconfig symbol 'ESP32P4_SELECTS_REV_LESS_V3'`（无害，见 §4.5）。

| 镜像 | 大小(字节) | SHA-256 | 烧录偏移 |
| --- | --- | --- | --- |
| bootloader.bin | 22,672 | `a062ef576dba123031a590afca7265a066353414fd53be4bac8937bee19d8536` | 0x2000 |
| partition-table.bin | 3,072 | `f2c96736488b2fb84b9ae0d2236365c53666f38ff7a7fcaa1497d76db8df7742` | 0x8000 |
| ota_data_initial.bin | — | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` | 0x19000 |
| **fridge_spirit.bin** | **1,873,728** | `496c8179657694e8b08a81b1397d93b457e6ac167f672e9b8354c65ab6bfd29e` | 0x30000 |
| **srmodels.bin** | **579,120** | `baab09bb3822c45bb25eb7cd4889fdb526d033b957b4610a226569557fe96443` | 0xd30000 |

- 主固件 1,873,728B 装入 ota_0(6MiB)，2MiB app 分区尺寸口径下剩余约 11%。
- **srmodels.bin 579,120B（565KB）< 2MiB model 分区**，含 `wn9_xiaobinxiaobin_tts` + `vadnet1_medium`，无 MultiNet。
- bootloader 剩 0x770 字节（8%）。
- 静态内存：DIRAM 用 145,588B（25%），剩 430,876B。

## 6. 烧录命令（COM15，CH343P，ROM loader）

CH343P 在 esptool RAM stub 阶段会出现 `Invalid head of packet (0xE0)`，固定用 ROM loader：

```
python -m esptool --chip esp32p4 -p COM15 --no-stub -b 115200 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x19000 build/ota_data_initial.bin \
  0x30000 build/fridge_spirit.bin \
  0xd30000 build/srmodels/srmodels.bin
```

五个区域全部 Hash 校验通过。

## 7. 真机验证结果（COM15，ESP32-P4 rev1.3，MAC 80:f1:b2:d2:04:85）

### 已验证通过
- 启动无 brownout / watchdog / panic / PSRAM / I2S 致命错误。
- **PSRAM**：Found 32MB @ **200MHz**，memory test OK；运行中 PSRAM 空闲约 31,750 KB。
- **CPU 360MHz**，芯片 rev v1.3，最低支持 rev v1.0，ESP-IDF v5.5.1。
- **C6 Hosted**：SDIO Slot1 握手成功，Identified slave [esp32c6]，`hostedReady=true`，
  **Host/Slave 版本均 2.12.11**。（mempool 崩溃已修复）
- **Wi-Fi**：已连接 `eric_2G`，IP 192.168.31.228，RSSI ≈ -25，`internet=true`；`scan_wifi` 返回真实 AP 列表。
- **TF 卡**：`mounted=true`，`/sdcard`，容量 59,638 MB、空闲 59,623 MB，`formatRequired=false`，**未格式化**。
- **FT6336U 触摸**：地址 0x48、chip 0x64、vendor 0x11，读取成功（无回归）。
- **ES8311 麦克风**：16kHz/16-bit/mono ready，PA 关闭；唤醒监听时 `rms≈20-31`，`timeoutCount=0`，
  `largestInternalDmaBlock=278528`，麦克风持续实时喂帧。
- **WakeNet**：`Successfully load srmodels`，加载 `wn9_xiaobinxiaobin_tts`，AFE 流水线
  `[input]->VAD(vadnet1_medium)->WakeNet(wn9_xiaobinxiaobin_tts)->AGC->[output]` 建立，
  `wake word ready: 小冰小冰`；`Ringbuffer empty` 刷屏已消除（0 条）。
- **NVS 持久唤醒**：`wake_set_enabled(true)` 保存后重启自动恢复 `state=listening`（重启后自动监听）。
- **唤醒开关幂等**：disable→idle→enable→starting→listening，反复切换正常，无资源泄漏迹象。
- **ASR 编排就绪**：`get_asr_config` 返回 model=`TeleAI/TeleSpeechASR`；缺 Key 时 `ready=false`
  （无 Key/网络时应返回明确错误并恢复监听，链路已接好）。
- 唤醒后端命令齐全：`wake_status`/`wake_set_enabled`/`wake_reset_stats`/`wake_start`/`wake_stop`；
  事件 `wake_word_detected`/`wake_asr_state`（recording/transcribing/done/error）代码在位。

### 仍未验证（需用户参与）
- 实际说“小冰小冰”触发 `wake_word_detected`（需人声，无法由脚本产生）。
- 唤醒后 5 秒录音 → SiliconFlow `TeleAI/TeleSpeechASR` 转写出结果（需用户提供 ASR API Key）。
- 连续 20 次唤醒的成功率、误触发（普通中文/电视背景）、任务栈高水位与长稳。
- **屏幕（TR230S QSPI）本次上电失败**：`WAIT# stayed low ... ESP_ERR_TIMEOUT`。这是**硬件接线问题**
  （QSPI D0-D3 飞线/WAIT#/屏供电/FPC 方向），与 IDF 迁移无关——显示驱动代码未改动，且 2026-07-11
  IDF6 下同代码曾点亮成功。需断电检查屏幕飞线后再复测。

## 8. 回退到 IDF 6.0.1 的方法

- 把 `tools/idf.ps1` 里 `export.ps1` 路径改回 `D:\esp-IDF\.espressif\v6.0.1\esp-idf\export.ps1`；
- 恢复 `tmp/backups/manual/20260713-131500_idf55_wakenet_migration/` 下改动的文件；
- 删 `build/`、`managed_components/`、`dependencies.lock` 重新 set-target。
- 注意 IDF6 下 WakeNet less-v3 链接仍会失败——回退 IDF6 意味着放弃本地 WakeNet 唤醒。
