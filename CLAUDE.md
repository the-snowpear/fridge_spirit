# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

> # ⚠️ 必读 / MUST READ BEFORE ANY TASK ⚠️
>
> **在动手做任何任务之前，必须先完整阅读仓库根目录的 `AGENTS.md`。**
>
> `AGENTS.md` 与本文件 **同等权威 (equal authority)**，并不是"补充材料"。两份文件构成一个整体：
>
> - `CLAUDE.md` 偏向"怎么构建、怎么烧录、代码在哪里、架构是什么"。
> - `AGENTS.md` 偏向"项目硬约束、硬件安全红线、备份工作流、中文注释规范、进度记录规则、踩坑经验"。
>
> 当两份文件出现冲突时：
> 1. 涉及 **硬件安全 / 备份流程 / 中文注释 / 进度记录格式** 的条款，以 `AGENTS.md` 为准。
> 2. 涉及 **构建/烧录命令、组件清单、协议字段、目录结构** 的条款，以 `CLAUDE.md` 为准。
> 3. 仍有歧义时，停下来问用户，不要凭推断动工。
>
> 同样必读的还有 `doc/memory.md`，它记录了已经验证过的接线、踩过的坑、不能再犯的错误。
> **注意：`doc/memory.md` 中 2026-05 的条目都是旧 ESP32-S3 板的经验，其中的 GPIO 号、COM16、
> INMP441/MAX98357A 接线在新 P4 板上不再直接成立，只能作为"问题模式"参考，不能照抄引脚。**
>
> **跳过这些文件直接开工 = 极大概率违反项目规则、损坏硬件、或重复已经踩过的坑。**

---

## ⚠️ Board migration in progress: ESP32-S3 → ESP32-P4 (since 2026-07-02)

The project is being ported from the ESP32-S3-DevKitC-1 N8R8 to the **慧勤智远 (WKS) ESP32-P4 CB V1.2** system board. Until the port is complete, treat every pin number, sdkconfig value, and partition offset in older docs/comments as **S3-era legacy that must be re-verified**.

- The final S3 state is archived in git: branch **`s3-legacy`**, tag **`s3-final-20260702`** (commit `ea8d0e8`). Do not develop on that branch; it exists for reference and rollback.
- `main` is the P4 porting line going forward.
- Vendor docs, schematics, and example code for the new board live in `doc/慧勤智远 ESP32-P4 CB V1.2小系统板/` (**not tracked by git** — 2.7 GB, see `.gitignore`; do not delete it). Key entry points:
  - `1. 用户手册/ESP32-P4 CB V1.2_用户手册_V1.4.pdf` — pin allocation table (§1.5) and circuit notes (§2) are the authoritative wiring reference.
  - `3. 程序源码/1，基础例程/basic__routines.zip` — per-peripheral examples (`09_mipilcd`, `13_mipilcd_touch_screen`, `17_mipicamera`, `23_music`, `24_recoding`, `30_spilcd`, ...).
  - `3. 程序源码/3，WIFI例程/wifi__routines.zip` — Wi-Fi via `esp_hosted` + `esp_wifi_remote` over SDIO to the onboard ESP32-C6.
  - Vendor examples target ESP-IDF 5.5.x; the main line is pinned to **5.5.1** because P4 rev1.3 WakeNet less-v3 libraries do not link with IDF6/GCC15. IDF6.0.1 remains only as a rollback environment.

### Hardware decisions already made (2026-07-02, by the user)

1. **Screen**: undecided long-term. Bring-up plan: connect the existing TR230S 720x720 panel through an **adapter cable to the board's 20-pin SPI screen socket**, SPI 4-wire mode only (the socket has no QSPI data lines). Later the project may switch to a WKS MIPI-DSI panel on the 26-pin DSI socket. See the direct-plug prohibition below.
2. **Camera**: switch to **OV5647 over MIPI-CSI** (the board has no DVP interface; OV3660 is retired).
3. **Audio**: use the onboard **electret MIC + ES8311 codec + NS4150B PA** (speaker via the board's Speaker terminal) first; keep the ability to fall back to external INMP441 / MAX98357A on header pins.
4. **ESP-IDF**: use 5.5.1 (`tools\idf.ps1 set-target esp32p4`); keep IDF6 only for the documented fallback path.

### Porting workstreams (status as of 2026-07-02)

| Area | What changes | Status |
| --- | --- | --- |
| `sdkconfig.defaults` / partition table | Regenerate for `esp32p4`: 16 MB flash (W25Q128), 32 MB in-package PSRAM (no octal-SPI PSRAM options), larger `ota_0`/`assets` possible | not started |
| `network` | ESP32-P4 has **no on-chip Wi-Fi**. Add managed components `espressif/esp_hosted` + `espressif/esp_wifi_remote` (SDIO to onboard C6, host data pins GPIO47–52, `C6_WKUP`=GPIO53, `C6_EN`=GPIO54) and configure SDIO power management per the user manual §2.2 | not started |
| `camera` | Replace `esp32-camera`/OV3660 DVP with `esp_video` / MIPI-CSI OV5647 (`CSI_RST`=GPIO26, `CSI_PWDN`=GPIO27, SCCB on shared I2C GPIO28/29); vendor `17_mipicamera` is the reference | not started |
| `audio` + `speaker` | Rebuild both on ES8311 codec: I2S on GPIO2 (MCLK) / GPIO3 (DSDIN) / GPIO4 (SCLK) / GPIO5 (ASDOUT) / GPIO6 (LRCK), control via shared I2C GPIO28/29, PA enable `PA_CTRL`=GPIO11; vendor `23_music` / `24_recoding` are the references. Keep the public headers (`fridge_audio.h`, `fridge_speaker.h`) stable so `asr`/`voice_session`/`wake_word` don't churn | not started |
| `display` + `touch` | Short term: TR230S via adapter on SPI2 (CS=GPIO7, MOSI=GPIO8, SCK=GPIO9, MISO=GPIO10, DC=GPIO20, RST=GPIO22, BL=GPIO23), touch I2C on GPIO16/17 (INT=GPIO15, RST=GPIO18). Long term: possible MIPI-DSI panel | not started |
| `sensors` / `radar` | MPU6050 now uses the GT911-owned shared I2C0 on SCL GPIO16 / SDA GPIO17 (3.3V, AD0=GND/0x68, INT unused) through the IDF new master driver; light sensor and radar remain separate migration/calibration work | MPU6050 code integrated; real-hardware validation pending |
| `wake_word` | Verify ESP-SR WakeNet model support/partition layout on esp32p4 | not started |
| `usb_protocol` / web panel | UART0 (GPIO37/38) goes through the onboard CH343P; JSON Lines protocol itself is unchanged | not started |

When a workstream lands and is verified on real hardware, update this table and `doc/progress.md`.

## Project overview

"冰箱小精灵" (Fridge Spirit) is an ESP-IDF firmware project for a smart fridge-magnet AIoT device, now targeting the **慧勤智远 ESP32-P4 CB V1.2** board: ESP32-P4NRW32 (dual-core RISC-V @ 360 MHz, **32 MB in-package PSRAM**), **16 MB W25Q128 QSPI flash**, onboard **ESP32-C6-MINI-1** (Wi-Fi 6 / BT5 via SDIO + esp-hosted), MIPI-DSI (26-pin) and SPI (20-pin) screen sockets, MIPI-CSI (15-pin) camera socket, ES8311 audio codec + electret MIC + NS4150A PA, TF card, CAN, RS485, USB-C serial (CH343P) + USB-C JTAG + USB OTG.

The firmware drives the screen + touch, an OV5647 MIPI camera, MPU6050 IMU, BH1750/photoresistor, and a 24 GHz HLK presence radar; calls **OpenAI-compatible HTTPS APIs** directly from the device for chat / TTS / ASR (SiliconFlow is the current default backend); and exposes a USB JSON Lines console for a `web/` React+Vite operator panel.

This is a **hardware project** — GPIO, power, clock, and bus decisions can damage the board, screen, camera, or sensors. Review the hardware-safety section of `AGENTS.md` before touching any pin assignment, sdkconfig, or driver init.

## Build, flash, and monitor (ESP-IDF 5.5.1 on Windows)

The main ESP-IDF source lives at `D:\esp-IDF\v5.5.1\esp-idf`, while shared tools live under `D:\esp-IDF\.espressif`. Always use the project wrapper so it selects 5.5.1, clears inherited MSYS variables, validates the environment/profile, and forwards the common serial/build arguments to the real IDF script:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM15 flash
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf.ps1 -p COM15 monitor
```

- Target chip: `esp32p4` (run `tools\\idf.ps1 set-target esp32p4` once after the sdkconfig rework; this regenerates `sdkconfig` — reconcile against the new `sdkconfig.defaults`, never keep S3 leftovers).
- **COM port: `COM15`.** It is the P4 board's CH343P UART0 path (`USB VID_1A86&PID_55D3`) for flashing/monitor/JSON-Lines. `COM16` is the retired S3 board. The default P4 configuration sets `CONFIG_ESPTOOLPY_NO_STUB=y` because this board's RAM stub transfer is unreliable; keep the ROM-loader path at 115200.
- The COM port may be held by `idf_monitor`, VS Code Serial Monitor, Web Serial in the browser, or another terminal. If `Access to the port ... is denied` appears, close the holding process — do **not** retry-flash in a loop. When killing leftover monitor processes, only kill those whose command line contains the target COM port and `idf_monitor`/`idf.py monitor`.
- After toggling Kconfig options (notably `CONFIG_FRIDGE_SCREEN_TEST` / `CONFIG_FRIDGE_CAMERA_TEST`), run `idf.py reconfigure build` and check `build/config/sdkconfig.h` before reflashing.
- The main image must fit the large `ota_0` slot, while the small recovery image must fit `ota_1` (partition CSV is being reworked for 16 MB flash). Use `idf.py size` / `idf.py size-components` to debug overflow.
- Recovery is currently blocked on the main P4 line: `sdkconfig.recovery` is still the archived S3/8MB configuration. Do not run a recovery build or flash command until a P4-specific recovery sdkconfig and an explicit `ota_1` flash procedure are created.

### Web operator panel (`web/`)

```bash
cd web
npm install
npm run dev      # vite dev server (host 0.0.0.0)
npm run build    # tsc -b && vite build  — REQUIRED before claiming a Web change works
npm run preview
```

`start_web_panel.bat` from the repo root will install deps, pick the first free port in `5173-5199`, start `npm run dev`, and open the browser. The panel talks to the firmware over **Web Serial** (Chrome/Edge only) using the JSON Lines protocol described below.

There is no test runner configured for either firmware or web — validation is real-board: build, flash, send USB JSON commands, watch the monitor log.

## ESP32-P4 CB V1.2 board constraints (must check before any pin work)

Authoritative source: user manual §1.5 pin table + §2 circuits. Summary of what is **already occupied on the board** (do not reassign without checking the schematic):

- **ES8311 I2S**: GPIO2 (MCLK), GPIO3 (DSDIN), GPIO4 (SCLK), GPIO5 (ASDOUT), GPIO6 (LRCK); `PA_CTRL`=GPIO11 (NS4150B enable).
- **SPI screen socket (20-pin)**: GPIO7 (CS), GPIO8 (MOSI), GPIO9 (SCK), GPIO10 (MISO), GPIO20 (LCD_DC), GPIO22 (LCD_RST), GPIO23 (BL_CTR); touch: GPIO15 (INT/PEN), GPIO16 (SCL/SCK), GPIO17 (SDA/MOSI), GPIO18 (RST/CS), GPIO19 (RTP_MISO). The DSI socket shares GPIO15–18, GPIO22, GPIO23 and adds `LCD_ID`=GPIO46.
- **Shared I2C (GPIO28 SDA / GPIO29 SCL)**: wired to **both** the MIPI-CSI camera SCCB **and** the ES8311 control port. The manual requires **time-multiplexing — the camera and codec must not be used simultaneously on this bus**. This directly affects the "photo + voice" scenario: any code touching camera init/capture or codec config must serialize access to this bus (single owner/mutex), and this constraint must be documented in the component headers.
- **MIPI-CSI**: dedicated CSI lanes + `CSI_RST`=GPIO26, `CSI_PWDN`=GPIO27.
- **ESP32-C6 (Wi-Fi) SDIO**: GPIO47 (CMD), GPIO48 (CLK), GPIO49–52 (D0–D3), GPIO53 (`C6_WKUP`), GPIO54 (`C6_EN`). SDIO power management must be configured per manual §2.2 or Wi-Fi will be unstable.
- **TF card SDIO**: GPIO39–44 + `SD1_PWR_EN`=GPIO45.
- **RS485**: GPIO30–32; **CAN**: GPIO33/34; **UART0 (CH343P)**: GPIO37 (TXD), GPIO38 (RXD).
- **Buttons/LED**: KEY0=GPIO12, BOOT=GPIO35 (**strapping** — boot-mode control), LED green=GPIO14, LED blue=GPIO13; GPIO36 is a boot-mode strap with a 10 K pull-up — leave it alone. GPIO0/GPIO1 carry the 32.768 kHz crystal.
- Free-for-external-modules pins are what remains of the 33 header IOs after the above; **P4 ADC channels do not map like S3** — verify the ADC-capable GPIO list in the P4 datasheet before assigning the light sensor.

**Screen direct-plug prohibition**: the board's 20-pin SPI socket puts **5 V on pins 1–2 and GND on pins 3–4**; the TR230S 20-pin FPC has **GND on pin 1 and 5 V VCC on pin 2**. Plugging the TR230S FPC straight into the socket applies 5 V to the panel's GND — **it will damage the panel**. The TR230S may only be connected through a pin-by-pin verified adapter cable, in SPI 4-wire mode (the socket has single MOSI/MISO — QSPI-4SDA is physically impossible there), and only after the user confirms the adapter mapping. Other DSI-socket note: the DSI reset line is level-shifted to 1.8 V by a resistor divider on the board.

## Architecture

### Build entry & components

- Root `CMakeLists.txt` previously added `example/ov3660_reference/esp32-camera` via `EXTRA_COMPONENT_DIRS` — this is retired with OV3660; the P4 camera stack uses managed components (`esp_video` etc.). Everything else lives under `components/`. Do not put feature code in `example/` — that directory is reference material pulled from upstream projects.
- `main/main.c` is **only** startup orchestration. It picks one of three modes from Kconfig and wires component init in a fixed order:
  - `CONFIG_FRIDGE_SCREEN_TEST=y` — standalone screen test task, **no** NVS / Wi-Fi / USB protocol. If this stays on by accident, Web Serial appears to time out on every command (see `doc/memory.md` 2026-05-23 entry).
  - `CONFIG_FRIDGE_CAMERA_TEST=y` — camera-only USB console; skips screen/sensors/audio. Mutually exclusive with screen test (enforced by `#error`).
  - `CONFIG_FRIDGE_RECOVERY_APP=y` — small local recovery shell for `ota_1`; starts NVS, diagnostics, Wi-Fi, and JSON Lines commands (`get_status`, `set_network`, `restore_main`, `boot_main`) only.
  - Default — full stack: `nvs → diagnostics → network → ai_client → mqtt_protocol → asr → sensors → radar → audio → speaker → wake_word → camera → usb_protocol`.
- Each subsystem under `components/` owns its own `CMakeLists.txt`, `include/`, NVS namespace, and FreeRTOS tasks. Cross-component access goes through the published `include/` header, never via internal statics. Hardware-touching components must document GPIO / voltage / clock / DMA / PSRAM constraints in their headers.
- Component map (kept in sync with `main/CMakeLists.txt` `REQUIRES`); *italic notes = P4 porting impact*:
  - `network` — Wi-Fi STA, SNTP, NVS-backed credentials. *Rebase onto `esp_wifi_remote` + `esp_hosted` (SDIO to onboard C6); the esp_wifi API surface mostly survives, but init and Kconfig change.*
  - `ai_client` — HTTPS chat to OpenAI-compatible APIs; UTF-8-cleans every system/context/history/user string before serialization (see "AI HTTP 400 root cause" below). *No hardware dependency — should port unchanged once network is up.*
  - `ai_context` — assembles project context + structured memory + recent dialog history before each AI call. *Ports unchanged.*
  - `storage` — NVS, LittleFS (`joltwallet/littlefs`), inventory snapshots, offline event queue. *Ports unchanged; partition names must match the new CSV.*
  - `sensors` — P4 mainline currently exposes the MPU6050 through `fridge_sensors_get_snapshot()`: shared I2C0, SCL GPIO16 / SDA GPIO17, 100kHz, address 0x68. The S3 GPIO1 light ADC and GPIO4/5 legacy-I2C implementation are removed; light sensing remains unported and must not be represented as zero-valued live data.
  - `radar` — HLK 24 GHz UART presence radar; both `Range xx` text frames and binary report frames appear, and the trailer is **not** a single fixed value (`08070605` and `F8F7F6F5` both seen). *UART pins re-assigned on P4; protocol handling unchanged.*
  - `audio` — mic capture, 16 kHz / mono PCM for ASR; multipart bodies for ASR must be allocated in PSRAM. *Rebuilt on ES8311 ADC path (onboard electret MIC) instead of INMP441 raw I2S; keep the public API stable; external INMP441 fallback stays possible via header pins.*
  - `asr` — SiliconFlow `TeleAI/TeleSpeechASR` by default; NVS namespace `fridge_asr`, **separate** from AI config. *Ports unchanged.*
  - `wake_word` — ESP-SR WakeNet, model `wn9_xiaobinxiaobin` (see `sdkconfig.defaults`); model image lives in the `model` partition (subtype `0x40`). *Verify WakeNet/esp-sr support and model partition flow on esp32p4.*
  - `speaker` — TTS playback. SiliconFlow TTS model is `fnlp/MOSS-TTSD-v0.5`; **do not** use voice `alloy` (see `components/speaker/README.md`). *Rebuilt on ES8311 DAC + NS4150B PA (`PA_CTRL`=GPIO11) instead of MAX98357A.*
  - `camera` — *OV5647 over MIPI-CSI via `esp_video`; OV3660/`esp32-camera` retired. Must share the GPIO28/29 I2C bus with ES8311 — serialize access.*
  - `display_test`, `diagnostics`, `mqtt_protocol`, `usb_protocol` — self-explanatory; `usb_protocol` is the JSON Lines console below. *`usb_protocol` stays on UART0 (now GPIO37/38 through CH343P); `display_test` needs the new panel driver.*

### USB JSON Lines protocol (firmware ↔ web/CLI)

- 115200 baud, one JSON object per line. Logs and responses share the same TX, so debug scripts must read line-by-line and only match `{...}` lines that contain the expected `request_id`.
- AI / ASR / Wi-Fi commands (`ai_assistant_chat`, `test_ai_chat`, `scan_wifi`, `set_network`, `voice_chat_stop`, etc.) can run 10–60 s. Use **90–130 s** timeouts; do not apply a generic short timeout.
- API keys must never appear in logs or responses. Log only HTTP status, response length, latency, task type, and history count.
- `WebSerialTransport` (in `web/src/transports/`) must resolve `response → pending` **before** dispatching to UI handlers, with try/catch around dispatch. Reversing this order causes spurious `get_status` timeouts (see memory.md 2026-05-23).

### Flash layout & sdkconfig

- `sdkconfig.defaults` is being reworked for the P4 target and is the source of truth once regenerated. The S3-era layout (`partitions_recovery.csv`: `nvs(64K) | otadata | phy_init | ota_0(3584K) | ota_1(1024K) | assets(0x83, 1536K) | cache(0x83, 704K) | model(0x40, 768K) | coredump(64K)`) was sized for 8 MB flash; the P4 board has **16 MB**, so `ota_0`, `assets`, and `model` can grow — keep the same partition roles and names.
- Custom `0x83` subtypes are project-defined data partitions (LittleFS / cache). `0x40` is the ESP-SR model image.
- PSRAM on P4 is 32 MB **in-package** — the S3 octal-SPI PSRAM options do not apply; use the P4 PSRAM Kconfig and re-verify the boot-time memory test in the monitor log.
- `mbedtls` uses the bundled certificate store (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`); do **not** hard-code per-vendor certs in firmware.

## Project rules you must follow

These come from `AGENTS.md` and `doc/memory.md` and override any general defaults.

1. **Hardware safety first.** Screen VCC is 5 V but GPIO logic is 3.3 V — never feed 5 V into a GPIO. On the P4 board, avoid strapping pins (GPIO35 BOOT, GPIO36) and the 32 kHz crystal pins (GPIO0/GPIO1); never plug the TR230S FPC directly into the 20-pin SPI socket (pinout mismatch puts 5 V on the panel's GND). Do not bump SPI clocks, camera clocks, backlight duty, or GPIO drive strength "just to try". Stop and flag risk if wiring/power is unclear.
2. **Chinese comments are mandatory.** File headers, init functions, task functions, state-machine branches, and every hardware-driver step must have concise Chinese comments explaining what the block does and what hardware/timing constraints apply. Public interfaces, message structs, and state enums document field meanings in Chinese.
3. **Component-first.** New features go under `components/<name>/` with their own `CMakeLists.txt` and `include/`. `main/main.c` only orchestrates startup. Never let UI/network/storage/state code reach into another component's internals.
4. **Backups before edits.** Any in-repo edit needs a prior backup under `tmp/backups/{manual,before_run,archive}/YYYYMMDD-HHMMSS_短事项名/`, preserving the original relative path and filename. Do not scatter `.bak-*` next to source files, do not pile backups directly under `tmp/`. `tmp/logs/`, `tmp/notes/`, `tmp/cache/` are the only other approved categories. Do **not** delete historical backups without user confirmation.
5. **UTF-8 only.** All Chinese docs / comments / backups stay UTF-8. `doc/progress.md` has been observed with non-UTF-8 bytes — do not blind-append; verify encoding first or ask the user.
6. **Progress log.** Milestone-sized completions get a new `----`-separated block in `doc/progress.md` with a date, what changed, modules touched, current result, and risks/next steps. Small fixes attach as bullets under the most recent relevant block, not as a new block.
7. **AI request hygiene.** Every system/context/history/user text crossing the network must be UTF-8 validated/cleaned — a half-truncated multibyte character produced past `400 invalid unicode code point` errors. Empty/very-short ASR transcripts must be rejected before calling the AI to avoid 400s on empty `messages[0].content`. The voice chat path reuses the text chat history-trimming logic (`convert_storage_history_to_ai_history()`) — keep them in lockstep.
8. **Radar is context, not a door sensor.** "Presence=true" only means the module reported a target. Door state is derived from light delta + IMU.
9. **AI output is never auto-applied to inventory.** It must go through rule validation and explicit user confirmation; low-confidence results enter a pending queue.

## Subagents and skills

`AGENTS.md` authorizes automatic use of the `esp32-firmware-engineer` skill for ESP32 / ESP-IDF / FreeRTOS / peripheral driver / partition table / OTA / serial log / crash triage / LVGL / build-flash-monitor work, and authorizes spawning subagents to parallelize. Use them where they help.
