import type {
  AIConfig,
  ASRConfig,
  CameraStatus,
  DeviceLog,
  DeviceStatus,
  DiagnosticSnapshot,
  MQTTConfig,
  NetworkConfig,
  PinInfo,
  RadarSnapshot,
  SensorSnapshot,
  TTSConfig,
  WakeStatus,
  WifiNetwork,
} from "../types";

const now = () =>
  new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date());

// Mock 状态数据：贴近当前硬件架构，避免前端展示脱离真实约束。
export function createMockStatus(): DeviceStatus {
  return {
    model: "冰箱小精灵 · P4 CB V1.2",
    chip: "ESP32-P4NRW32 + ESP32-C6",
    firmware: "p4-minimal-20260711",
    uptime: "02:17:43",
    flash: "16 MB W25Q128",
    psram: "32 MB detected",
    freeHeapKb: 184,
    minHeapKb: 132,
    freePsramKb: 30240,
    temperatureC: null,
    wifi: "ok",
    mqtt: "warn",
    usb: "ok",
    ota: "warn",
    page: "IDLE / local summary",
    powerNote: "屏幕 5V 供电与 ESP32-P4 3.3V GPIO 必须隔离确认；C6 Hosted 与 TF 共享 SDMMC/LDO4。",
    tasks: [
      { name: "gui_task", priority: "高", state: "running", heartbeat: "120 ms" },
      { name: "sensor_task", priority: "中高", state: "running", heartbeat: "42 ms" },
      { name: "net_task", priority: "中", state: "reconnect", heartbeat: "3.2 s" },
      { name: "storage_task", priority: "中低", state: "idle", heartbeat: "1.0 s" },
    ],
  };
}

export function createMockNetwork(): NetworkConfig {
  return {
    ssid: "KitchenLab-2.4G",
    wifiPassword: "",
    mqttHost: "mqtt://192.168.1.20:1883",
    apiBaseUrl: "https://api.fridge-spirit.local",
    ntpServer: "ntp.aliyun.com",
    save: true,
    saveAiKey: false,
    connected: true,
    saved: true,
    internet: true,
    status: "mock_online",
    ip: "192.168.1.88",
    rssi: -48,
    lastError: "",
  };
}

export function createMockMQTTConfig(): MQTTConfig {
  return {
    brokerUri: "mqtts://example.com:8883",
    homeId: "home_demo",
    deviceId: "s3_demo_001",
    username: "device_s3_demo_001",
    hasPassword: true,
    enabled: true,
    configured: true,
    connected: false,
    reconnectCount: 0,
    publishedCount: 0,
    receivedCount: 0,
    lastError: 0,
    statusText: "mock configured",
  };
}

export function createMockAIConfig(): AIConfig {
  return {
    profileId: 0,
    profileName: "默认配置",
    apiBaseUrl: "https://api.openai.com/v1",
    model: "gpt-4o-mini",
    systemPrompt: "你是冰箱小精灵的开发测试助手，请用简短中文回答。",
    timeoutMs: 30000,
    apiFormat: 0,
    hasApiKey: false,
    apiKeyPreview: "",
    lastError: "",
    ready: false,
  };
}

export function createMockASRConfig(): ASRConfig {
  return {
    apiBaseUrl: "https://api.siliconflow.cn/v1/audio/transcriptions",
    model: "TeleAI/TeleSpeechASR",
    timeoutMs: 45000,
    hasApiKey: false,
    apiKeyPreview: "",
    lastError: "",
    ready: false,
  };
}

export function createMockTTSConfig(): TTSConfig {
  return {
    apiBaseUrl: "https://api.siliconflow.cn/v1/audio/speech",
    model: "fnlp/MOSS-TTSD-v0.5",
    voice: "fnlp/MOSS-TTSD-v0.5:alex",
    timeoutMs: 45000,
    hasApiKey: false,
    apiKeyPreview: "",
    lastError: "",
    ready: false,
  };
}

export function createMockWakeStatus(overrides: Partial<WakeStatus> = {}): WakeStatus {
  return {
    enabled: false,
    state: "idle",
    wakeWord: "小冰小冰",
    model: "wn9_xiaobinxiaobin_tts",
    audioSource: "fy_sp703",
    triggerCount: 0,
    lastTriggerMs: 0,
    vadState: -1,
    rms: 0,
    peakAbs: 0,
    timeoutCount: 0,
    error: "",
    ...overrides,
  };
}

export function createMockCameraStatus(hasFrame = false): CameraStatus {
  return {
    initialized: true,
    hasFrame,
    width: hasFrame ? 320 : 0,
    height: hasFrame ? 240 : 0,
    jpegBytes: hasFrame ? 18432 : 0,
    captureMs: hasFrame ? 310 : 0,
    frameId: hasFrame ? 1 : 0,
    freeHeapKb: 152,
    freePsramKb: 6210,
    pixelFormat: "JPEG",
    frameSize: "QVGA",
    lastError: "",
  };
}

export function createMockWifiNetworks(): WifiNetwork[] {
  return [
    { ssid: "KitchenLab-2.4G", band: "2.4G", signal: 92, rssi: -54, channel: 6, secured: true, authmode: "WPA2", note: "Mock 扫描结果，ESP32 兼容" },
    { ssid: "Home-IoT", band: "2.4G", signal: 78, rssi: -61, channel: 1, secured: true, authmode: "WPA2/WPA3", note: "Mock 扫描结果" },
    { ssid: "Phone-Hotspot", band: "2.4G", signal: 64, rssi: -68, channel: 11, secured: true, authmode: "WPA2", note: "Mock 热点" },
  ];
}

export function createMockPins(): PinInfo[] {
  return [
    { gpio: "GPIO15/18", signal: "TOUCH_GT911_CTRL", usage: "WKS70 GT911", level: "caution", note: "INT=15、RST=18；仅 GT911 使用。", readonly: true },
    { gpio: "GPIO16/17", signal: "I2C0_SHARED", usage: "GT911 + SHT40 + MPU6050", level: "caution", note: "SCL=16、SDA=17；MPU6050 地址 0x68、100kHz、AD0 接地、INT 悬空；所有上拉只能到 3.3V。", readonly: true },
    { gpio: "GPIO2/3/4/5/6/11/28/29", signal: "ES8311_AUDIO", usage: "板载音频", level: "caution", note: "GPIO28/29 与 OV5647 SCCB 分时复用。", readonly: true },
    { gpio: "GPIO39-45", signal: "TF_SDMMC", usage: "板载 TF 卡", level: "caution", note: "D0-D3=39-42、CLK=43、CMD=44、PWR_EN=45 低有效。", readonly: true },
    { gpio: "GPIO47-54", signal: "C6_SDIO", usage: "板载 ESP32-C6", level: "danger", note: "板载专用，不接外设。", readonly: true },
    { gpio: "GPIO0/1/35/36", signal: "RESERVED_STRAP_XTAL", usage: "晶振/启动敏感", level: "danger", note: "GPIO0/1 为 32.768kHz 晶振，GPIO35/36 为启动敏感脚。", readonly: true },
  ];
}

export function createMockRadar(previous?: RadarSnapshot | null): RadarSnapshot {
  const triggerThresholds = [59979, 29992, 2999, 2000, 500, 400, 400, 300, 300, 300, 300, 250, 250, 200, 200, 200];
  const holdThresholds = [39994, 19999, 400, 300, 300, 200, 200, 150, 150, 100, 100, 100, 100, 100, 100, 100];
  const presence = Math.random() > 0.42;
  const wallLike = presence && Math.random() > 0.76;
  const previousGate = previous?.stableGate ?? previous?.estimatedGate ?? 4;
  const targetGate = presence ? Math.max(0, Math.min(15, wallLike ? previousGate : previousGate + Math.round(Math.random() * 2 - 1))) : -1;
  const gateEnergy = Array.from({ length: 16 }, (_, index) => {
    const base = Math.round(12 + Math.random() * 18);
    if (!presence || targetGate < 0) {
      return base;
    }
    const nearClutter = index <= 2 ? Math.round(80 + Math.random() * 80) : 0;
    const spread = Math.max(0, (wallLike ? 980 : 780) - Math.abs(index - targetGate) * 230);
    return Math.round(base + nearClutter + spread + Math.random() * (wallLike ? 8 : 55));
  });
  const totalEnergy = gateEnergy.reduce((sum, value) => sum + value, 0);
  const peakGate = gateEnergy.reduce((bestIndex, value, index, values) => (value > values[bestIndex] ? index : bestIndex), 0);
  const remotePeakGate = gateEnergy.slice(3).reduce((bestIndex, value, index, values) => (value > values[bestIndex] ? index : bestIndex), 0) + 3;
  const remotePeakEnergy = gateEnergy[remotePeakGate] ?? 0;
  const wasHolding = Boolean(previous?.thresholdPresence);
  const thresholdGate = gateEnergy.reduce((bestIndex, value, index) => {
    if (index > 12) {
      return bestIndex;
    }
    const threshold = wasHolding ? holdThresholds[index] : triggerThresholds[index];
    const bestThreshold = wasHolding ? holdThresholds[bestIndex] : triggerThresholds[bestIndex];
    const score = value >= threshold ? value / threshold : 0;
    const bestScore = (gateEnergy[bestIndex] ?? 0) >= bestThreshold ? (gateEnergy[bestIndex] ?? 0) / bestThreshold : 0;
    return score > bestScore ? index : bestIndex;
  }, 0);
  const threshold = wasHolding ? holdThresholds[thresholdGate] : triggerThresholds[thresholdGate];
  const thresholdPresence = presence && thresholdGate <= 12 && (gateEnergy[thresholdGate] ?? 0) >= threshold;
  const thresholdScore = thresholdPresence ? Math.min(100, Math.round((((gateEnergy[thresholdGate] ?? 0) - threshold) * 100) / threshold)) : 0;
  const estimatedGate = thresholdPresence ? thresholdGate : peakGate <= 2 && remotePeakEnergy > 120 ? remotePeakGate : peakGate;
  const stability = presence ? Math.min(100, (previous?.stability ?? 35) + Math.round(Math.random() * 20 - 4)) : 0;
  const stablePresence = presence && stability >= 55 && (gateEnergy[estimatedGate] ?? 0) > 120;
  const stableZone = !stablePresence ? "unknown" : estimatedGate <= 3 ? "near" : estimatedGate <= 8 ? "mid" : "far";
  const confidence = stablePresence ? Math.min(100, Math.round(45 + stability * 0.45 + Math.random() * 10)) : Math.round(Math.random() * 30);
  const distanceRaw = presence ? 20 + estimatedGate * 75 + Math.round(Math.random() * 30 - 15) : 0;
  const nearClutter = presence && peakGate <= 2 && distanceRaw <= 80 && remotePeakEnergy < (gateEnergy[peakGate] ?? 1) * 0.08;
  const previousSemanticGate = previous?.stableGate ?? previous?.estimatedGate ?? estimatedGate;
  const smoothedDistanceRaw = presence ? Math.round(((previous?.smoothedDistanceRaw ?? distanceRaw) * 2 + distanceRaw) / 3) : 0;
  const approachScore = presence && previousSemanticGate > estimatedGate ? Math.min(100, (previousSemanticGate - estimatedGate) * 40 + Math.round(Math.random() * 24)) : Math.round(Math.random() * 24);
  const distanceSpan = wallLike ? Math.round(Math.random() * 5) : Math.round(10 + Math.random() * 45);
  const gateSpan = wallLike ? 0 : Math.round(Math.random() * 2);
  const energyChangeScore = wallLike ? Math.round(Math.random() * 10) : Math.round(16 + Math.random() * 44);
  const motionScore = Math.min(100, Math.round(distanceSpan * 0.4 + gateSpan * 22 + energyChangeScore));
  const staticScore = Math.min(100, Math.round((stability > 55 ? (stability - 55) * 2 : 0) + (40 - Math.min(40, motionScore)) + (distanceSpan <= 10 ? 18 : 0) + (gateSpan === 0 ? 18 : 0) + (energyChangeScore <= 12 ? 16 : 0)));
  const humanScore = Math.min(100, Math.round((thresholdPresence ? 20 : 0) + thresholdScore * 0.22 + Math.min(50, (gateEnergy[peakGate] ?? 0) * 100 / Math.max(1, totalEnergy)) * 0.32 + stability * 0.12 + Math.min(50, motionScore) * 0.64 + Math.min(70, approachScore) * 0.26));
  const approachFrames = presence && previousSemanticGate > estimatedGate ? 2 + Math.round(Math.random() * 2) : Math.round(Math.random());
  const approachDistanceDelta = presence && previousSemanticGate > estimatedGate ? Math.max(25, (previous?.smoothedDistanceRaw ?? distanceRaw) - smoothedDistanceRaw) : Math.round(Math.random() * 16);
  const staticClutter = Boolean(stablePresence && !nearClutter && (wallLike || staticScore >= 70) && motionScore <= 22);
  const humanCandidate = stablePresence && !nearClutter && !staticClutter && motionScore >= 28 && humanScore >= 30;
  const semanticPresence = humanCandidate && (humanScore >= 45 || (approachScore >= 60 && approachFrames >= 2 && approachDistanceDelta >= 25));
  const semanticConfidence = nearClutter || staticClutter ? Math.min(55, confidence) : confidence;
  const targetClass = semanticPresence && approachScore >= 60 && approachFrames >= 2 && approachDistanceDelta >= 25
    ? "reliable_approaching"
    : semanticPresence && estimatedGate <= 1
      ? "reliable_within_1m"
      : semanticPresence
        ? "reliable_human"
        : humanCandidate
          ? "human_candidate"
          : staticClutter
            ? "static_reflection"
            : nearClutter
              ? "near_clutter"
              : presence
                ? "raw_target"
                : "idle";
  return {
    ready: true,
    mode: "report",
    presence,
    nearClutter,
    staticClutter,
    humanCandidate,
    stablePresence: semanticPresence,
    thresholdPresence,
    within1m: semanticPresence && estimatedGate <= 1 && smoothedDistanceRaw > 0 && smoothedDistanceRaw <= 120 && semanticConfidence >= 60,
    approaching: semanticPresence && semanticConfidence >= 55 && approachScore >= 60,
    distanceRaw,
    smoothedDistanceRaw,
    gateEnergy,
    peakGate,
    peakEnergy: gateEnergy[peakGate] ?? 0,
    estimatedGate,
    stableGate: stablePresence ? estimatedGate : previous?.stableGate ?? estimatedGate,
    thresholdGate,
    stableZone: nearClutter ? "unknown" : stableZone,
    confidence: semanticConfidence,
    stability,
    approachScore,
    approachFrames,
    approachDistanceDelta,
    motionScore,
    distanceSpan,
    gateSpan,
    energyChangeScore,
    staticScore,
    humanScore,
    thresholdScore,
    holdFramesRemaining: thresholdPresence ? 30 : Math.max(0, (previous?.holdFramesRemaining ?? 0) - 1),
    nearEnergy: gateEnergy.slice(0, 4).reduce((sum, value) => sum + value, 0),
    midEnergy: gateEnergy.slice(4, 9).reduce((sum, value) => sum + value, 0),
    farEnergy: gateEnergy.slice(9).reduce((sum, value) => sum + value, 0),
    frameCount: (previous?.frameCount ?? 0) + 1,
    parseErrorCount: previous?.parseErrorCount ?? 0,
    timeoutCount: previous?.timeoutCount ?? 0,
    ot2Level: presence ? 1 : 0,
    lastText: nearClutter ? "near clutter" : staticClutter ? `static clutter gate=${estimatedGate} motion=${motionScore}` : semanticPresence && estimatedGate <= 1 ? `likely within 1m gate=${estimatedGate} confidence=${semanticConfidence}` : semanticPresence ? `${stableZone} gate=${estimatedGate} confidence=${semanticConfidence}` : presence ? "unstable target" : "idle",
    targetClass,
    rejectionReason: targetClass === "static_reflection" ? "stable low-motion reflection" : targetClass === "raw_target" ? "waiting for stable history" : targetClass === "human_candidate" ? "motion/history not strong enough" : "",
    lastError: "",
    updatedAtMs: Date.now(),
  };
}

export function createMockSensors(): SensorSnapshot {
  const accelX = Number((Math.random() * 0.08 - 0.04).toFixed(4));
  const accelY = Number((Math.random() * 0.08 - 0.04).toFixed(4));
  const accelZ = Number((1 + Math.random() * 0.05).toFixed(4));
  return {
    profile: "p4_mpu6050",
    imuReady: true,
    baselineReady: true,
    imuAddress: 0x68,
    imuWhoAmI: 0x68,
    imuError: 0,
    imuErrorName: "ESP_OK",
    consecutiveFailures: 0,
    sampleCount: Math.floor(1200 + Math.random() * 10000),
    i2cBus: 0,
    sclGpio: 16,
    sdaGpio: 17,
    sampleRateHz: 50,
    accelXG: accelX,
    accelYG: accelY,
    accelZG: accelZ,
    gyroXDps: Number((Math.random() * 2 - 1).toFixed(4)),
    gyroYDps: Number((Math.random() * 2 - 1).toFixed(4)),
    gyroZDps: Number((Math.random() * 2 - 1).toFixed(4)),
    imuTemperatureC: Number((28 + Math.random() * 2).toFixed(2)),
    pitchDeg: Number((Math.random() * 3 - 1.5).toFixed(2)),
    rollDeg: Number((Math.random() * 3 - 1.5).toFixed(2)),
    angleDelta: Number((Math.random() * 3).toFixed(1)),
    // 震动峰值与固件保持一致：去重力的动态加速度 |‖a‖ - 1g|，静止时约等于 0。
    vibrationPeak: Number(Math.abs(Math.sqrt(accelX * accelX + accelY * accelY + accelZ * accelZ) - 1).toFixed(4)),
    updatedAt: now(),
  };
}

/** MQ-5 燃气模块稳定示例：相对 ADC 读数，不是 ppm；供 Mock 面板与状态迁移演示使用。 */
export function createMockMq5Sensor(overrides: Partial<SensorSnapshot> = {}): Partial<SensorSnapshot> {
  const raw12bit = Math.floor(900 + Math.random() * 500); // 相对 12-bit ADC 读数（约 900-1400），非 ppm
  const baselineRaw12bit = 1050;
  return {
    mq5State: "monitoring",
    mq5Ready: true,
    mq5Error: 0,
    mq5Raw12bit: raw12bit,
    mq5Value10bit: Math.round((raw12bit * 1023) / 4095),
    mq5AdcPercent: Math.round((raw12bit * 100) / 4095),
    mq5SampleCount: 2400,
    mq5UpdatedAtMs: Date.now(),
    mq5WarmupElapsedSec: 60,
    mq5DailyWarmupComplete: true,
    mq5BurnInConfirmed: true,
    mq5BaselineReady: true,
    mq5BaselineRaw12bit: baselineRaw12bit,
    mq5DeltaFromBaseline: raw12bit - baselineRaw12bit,
    ...overrides,
  };
}

export function createMockDiagnostics(): DiagnosticSnapshot {
  return {
    psram: "OK / 8MB / alloc tested",
    flashPartition: "8MB: ota_0 大主固件 + ota_1 小 recovery + assets/cache/model",
    littlefs: "planned: assets + cache",
    otaSlot: "ota_0 主固件运行，ota_1 作为本地 recovery 兜底",
    brownoutCount: 0,
    watchdogCount: 0,
    lastError: "无硬件错误，当前为 Mock 数据",
    riskItems: [
      "上电前核对屏幕 VCC 5V 与 GPIO 3.3V 逻辑隔离。",
      "I2C 上拉只允许到 3.3V。",
      "C6 Wi-Fi 峰值、屏幕背光和 OV5647 摄像头并发时建议使用稳定 5V/2A 电源。",
    ],
  };
}

export function createMockLogs(): DeviceLog[] {
  return [
    { id: crypto.randomUUID(), at: now(), level: "info", source: "boot", message: "Fridge Spirit console mock boot complete" },
    { id: crypto.randomUUID(), at: now(), level: "info", source: "partition", message: "main app uses large ota_0; ota_1 is reserved for local recovery" },
    { id: crypto.randomUUID(), at: now(), level: "info", source: "sensor_task", message: "brightness=642 delta=12 angle_delta=2.1 pir=0" },
    { id: crypto.randomUUID(), at: now(), level: "debug", source: "mqtt", message: "heartbeat queued because broker is not connected" },
  ];
}
