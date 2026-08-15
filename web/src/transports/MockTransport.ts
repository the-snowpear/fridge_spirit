import {
  createMockAIConfig,
  createMockASRConfig,
  createMockCameraStatus,
  createMockDiagnostics,
  createMockLogs,
  createMockMQTTConfig,
  createMockMq5Sensor,
  createMockNetwork,
  createMockPins,
  createMockRadar,
  createMockSensors,
  createMockStatus,
  createMockTTSConfig,
  createMockWakeStatus,
  createMockWifiNetworks,
} from "../data/mockData";
import type {
  AIAssistantChatRequest,
  AIAssistantChatResponse,
  AgentChatResponse,
  AgentRunStatus,
  AgentSession,
  AgentSessionRecord,
  AgentSubmitResponse,
  AIConfig,
  AIContextPreview,
  AIProfilesResponse,
  AsrTranscribeResponse,
  ASRConfig,
  CameraAnalyzeResponse,
  CameraCaptureResponse,
  CameraProbeResponse,
  CameraRgb565DiagResponse,
  CameraStatus,
  DeviceChatHistory,
  DeviceCommand,
  DeviceResponse,
  KitchenAIGenerateRequest,
  KitchenAIPage,
  KitchenAIStatus,
  KitchenToolsStatus,
  MQTTConfig,
  MicSource,
  NetworkConfig,
  ProjectAITaskRequest,
  ProjectAITaskResponse,
  RadarSnapshot,
  SensorSnapshot,
  StateMachineConfig,
  StateMachineStatus,
  TTSConfig,
  TTSStatus,
  TfFileEntry,
  TfTransferSession,
  UiStatus,
  WakeStatus,
  VoiceChatResponse,
  VoiceChatStatus,
} from "../types";
import { BaseTransport } from "./DeviceTransport";

// MockTransport 与固件共享竖屏白名单，避免测试面板把已移植设置页错误折回横屏。
const portraitUiPages = new Set([
  "home",
  "home_detail",
  "plan",
  "chat",
  "chat_history",
  "chat_tool_detail",
  "chat_pending_actions",
  "more",
  "timer",
  "alarm",
  "stopwatch",
  "safety",
  "settings",
  "maintenance",
]);

function crc32Mock(bytes: Uint8Array) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function createMockWavBase64(durationMs: number) {
  const sampleRate = 16000;
  const pcmBytes = Math.max(32000, Math.round((sampleRate * 2 * durationMs) / 1000));
  const wavBytes = pcmBytes + 44;
  const buffer = new ArrayBuffer(wavBytes);
  const view = new DataView(buffer);
  const writeAscii = (offset: number, text: string) => {
    for (let index = 0; index < text.length; index += 1) {
      view.setUint8(offset + index, text.charCodeAt(index));
    }
  };

  writeAscii(0, "RIFF");
  view.setUint32(4, wavBytes - 8, true);
  writeAscii(8, "WAVEfmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeAscii(36, "data");
  view.setUint32(40, pcmBytes, true);

  for (let offset = 44; offset < wavBytes; offset += 2) {
    const sampleIndex = (offset - 44) / 2;
    const tone = Math.sin((2 * Math.PI * 440 * sampleIndex) / sampleRate);
    const noise = (Math.random() - 0.5) * 0.12;
    view.setInt16(offset, Math.round((tone * 0.32 + noise) * 32767), true);
  }

  let binary = "";
  const bytes = new Uint8Array(buffer);
  for (let offset = 0; offset < bytes.length; offset += 8192) {
    binary += String.fromCharCode(...bytes.slice(offset, offset + 8192));
  }
  return {
    sampleRate,
    channels: 1,
    bitsPerSample: 16,
    durationMs,
    pcmBytes,
    wavBytes,
    wavBase64: window.btoa(binary),
  };
}

function decodeMockBase64(base64: string) {
  const binary = window.atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

// Mock 截屏使用一个很小的合法 BMP，覆盖首页的 TF 读回、CRC 校验、预览和下载链路。
function createMockScreenshotBmp() {
  const width = 2;
  const height = 2;
  const rowBytes = 8;
  const pixelOffset = 54;
  const bytes = new Uint8Array(pixelOffset + rowBytes * height);
  const view = new DataView(bytes.buffer);
  const writeAscii = (offset: number, text: string) => {
    for (let index = 0; index < text.length; index += 1) view.setUint8(offset + index, text.charCodeAt(index));
  };
  writeAscii(0, "BM");
  view.setUint32(2, bytes.length, true);
  view.setUint32(10, pixelOffset, true);
  view.setUint32(14, 40, true);
  view.setInt32(18, width, true);
  view.setInt32(22, height, true);
  view.setUint16(26, 1, true);
  view.setUint16(28, 24, true);
  view.setUint32(34, rowBytes * height, true);
  const pixels = [
    [0x22, 0x91, 0xff], [0x31, 0xb4, 0x77],
    [0xee, 0xc3, 0x44], [0xc9, 0x55, 0x2c],
  ];
  pixels.forEach(([blue, green, red], index) => {
    const row = Math.floor(index / width);
    const column = index % width;
    const offset = pixelOffset + row * rowBytes + column * 3;
    view.setUint8(offset, blue);
    view.setUint8(offset + 1, green);
    view.setUint8(offset + 2, red);
  });
  return bytes;
}

export class MockTransport extends BaseTransport {
  private timer: number | undefined;
  private wakeTimer: number | undefined;
  private connectedAt = Date.now();
  private connected = false;
  private uiPage = "home";
  private uiOrientation: UiStatus["orientation"] = "landscape";
  private network = createMockNetwork();
  private mqttConfig: MQTTConfig = createMockMQTTConfig();
  private aiConfig = createMockAIConfig();
  private aiProfiles: AIConfig[] = [this.aiConfig];
  private asrConfig: ASRConfig = createMockASRConfig();
  private ttsConfig: TTSConfig = createMockTTSConfig();
  private stateMachineConfig: StateMachineConfig = {
    nightLightThreshold: 250,
    dayLightThreshold: 450,
    radarTwoMeterRaw: 200,
    radarTwoMeterGate: 8,
    sleepEnabled: false,
    autoVoiceAfterClose: true,
    autoVoiceRecordSeconds: 6,
    closeStableMs: 2500,
  };
  private ttsStatus: TTSStatus = {
    state: "idle",
    sampleRate: 24000,
    audioBytes: 0,
    playedBytes: 0,
    durationMs: 0,
    latencyMs: 0,
    httpStatus: 0,
    model: "",
    voice: "",
    error: "",
  };
  private kitchenTools: KitchenToolsStatus = {
    timeReady: true,
    timer: {
      state: "idle",
      durationSeconds: 0,
      remainingSeconds: 0,
      label: "",
    },
    stopwatch: {
      state: "idle",
      elapsedSeconds: 0,
    },
    alarms: [],
    lastAlert: "",
    lastError: "",
  };
  private kitchenAiStatus: KitchenAIStatus = {
    running: false,
    lastOk: false,
    state: "idle",
    lastTrigger: "manual",
    currentPage: "all",
    lastPage: "all",
    lastGeneratedAt: 0,
    lastAutoDay: 0,
    httpStatus: 0,
    latencyMs: 0,
    lastReplyLen: 0,
    lastError: "",
    lastReplyPreview: "",
  };
  private kitchenTimerStartedAt = 0;
  private kitchenTimerEndsAt = 0;
  private stopwatchStartedAt = 0;
  private nextAlarmId = 1;
  private wakeStatus: WakeStatus = createMockWakeStatus();
  private cameraStatus: CameraStatus = createMockCameraStatus(false);
  private cameraPreviewDataUrl: string | null = null;
  private radar: RadarSnapshot = createMockRadar(null);
  // MQ-5 燃气模块 Mock 状态（相对 ADC，非 ppm）；与 get_sensors 快照共用。
  private mq5 = createMockMq5Sensor();
  private voiceStatus: VoiceChatStatus = {
    state: "idle",
    durationMs: 0,
    pcmBytes: 0,
    rms: 0,
    error: "",
  };
  private micWavCache: ReturnType<typeof createMockWavBase64> | null = null;
  private micWavBytes: Uint8Array | null = null;
  private micWavSession = 0;
  private tfMounted = true;
  private tfExpertMode = false;
  private tfFiles = new Map<string, { entry: TfFileEntry; bytes: Uint8Array }>([
    ["食材清单.json", { entry: { name: "食材清单.json", path: "食材清单.json", type: "file", size: 92, modifiedAt: Math.floor(Date.now() / 1000) }, bytes: new TextEncoder().encode('{\n  "items": ["番茄", "鸡蛋", "牛奶"],\n  "updatedBy": "冰箱小精灵"\n}') }],
    ["照片", { entry: { name: "照片", path: "照片", type: "directory", size: 0, modifiedAt: Math.floor(Date.now() / 1000) }, bytes: new Uint8Array() }],
    [".fridge_spirit", { entry: { name: ".fridge_spirit", path: ".fridge_spirit", type: "directory", size: 0, protected: true }, bytes: new Uint8Array() }],
    [".fridge_spirit/trash", { entry: { name: "trash", path: ".fridge_spirit/trash", type: "directory", size: 0, protected: true }, bytes: new Uint8Array() }],
    [".fridge_spirit/uploads", { entry: { name: "uploads", path: ".fridge_spirit/uploads", type: "directory", size: 0, protected: true }, bytes: new Uint8Array() }],
    [".fridge_spirit/exports", { entry: { name: "exports", path: ".fridge_spirit/exports", type: "directory", size: 0, protected: true }, bytes: new Uint8Array() }],
  ]);
  private tfTransfers = new Map<number, TfTransferSession & { bytes: Uint8Array }>();
  private tfNextSession = 1;
  private chatHistory: DeviceChatHistory = {
    schemaVersion: 1,
    updatedAt: Math.floor(Date.now() / 1000),
    ttlSeconds: 48 * 60 * 60,
    maxMessages: 15,
    timeReady: true,
    count: 0,
    prunedCount: 0,
    messages: [],
  };
  private agentSession: AgentSession = {
    schema_version: 1,
    foreground: {
      next_sequence: 1,
      messages: [],
    },
  };
  private agentRun: AgentRunStatus = {
    active: false,
    runId: "",
    state: "COMPLETED",
    intent: "",
    reply: "",
    error: "",
    httpStatus: 0,
    modelLatencyMs: 0,
    contextBytes: 0,
    toolSchemaCount: 18,
    modelSteps: 0,
    cloudStackHighWater: 12288,
    freePsramBytes: 28 * 1024 * 1024,
    internalDmaLargestBytes: 96 * 1024,
    cloudPending: false,
    confirmation: null,
  };
  private agentPollCount = 0;
  private agentPendingReply = "";
  private realtimeConfig = {
    url: "wss://open.bigmodel.cn/api/paas/v4/realtime",
    model: "glm-realtime-flash",
    voice: "tongtong",
    instructions: "你是冰箱小精灵，厨房语音助手。用简短中文回答。",
    vadMode: "server_vad",
    timeoutMs: 30000,
    enabled: true,
    hasApiKey: false,
    ready: false,
    apiKeyPreview: "",
    lastError: "",
  };
  private realtimeStatus = {
    ready: true,
    sessionActive: false,
    networkOk: true,
    internetReady: true,
    audioUpActive: false,
    audioDownActive: false,
    toolsReady: true,
    state: "idle",
    model: "glm-realtime-flash",
    voice: "tongtong",
    lastError: "",
    toolsExported: 10,
    audioUpBytes: 0,
    audioDownBytes: 0,
    toolCallsOk: 0,
    toolCallsFail: 0,
    lastEventAgeMs: 0,
  };
  private realtimeHasKey = false;
  private realtimeSessionGeneration = 0;
  private realtimeResponseGeneration = 0;

  // 模拟 MQ-5 每次 get_sensors 的轻微读数波动；基线固定，差值随当前读数更新。
  private buildMq5Snapshot(): Partial<SensorSnapshot> {
    const jitter = Math.floor(Math.random() * 41) - 20; // -20..+20
    const raw12bit = Math.max(0, Math.min(4095, (this.mq5.mq5Raw12bit ?? 1024) + jitter));
    this.mq5 = {
      ...this.mq5,
      mq5Raw12bit: raw12bit,
      mq5Value10bit: Math.round((raw12bit * 1023) / 4095),
      mq5AdcPercent: Math.round((raw12bit * 100) / 4095),
      mq5SampleCount: (this.mq5.mq5SampleCount ?? 0) + 1,
      mq5UpdatedAtMs: Date.now(),
      mq5DeltaFromBaseline: this.mq5.mq5BaselineReady
        ? raw12bit - (this.mq5.mq5BaselineRaw12bit ?? 0)
        : 0,
    };
    return { ...this.mq5 };
  }

  private createStateMachineStatus(): StateMachineStatus {
    const sensors = createMockSensors();
    // P4 MPU 快照不再伪造未接入的光敏字段；状态机 Mock 使用自己的测试输入。
    const mockLightValue10bit = 600;
    const mockLightDelta = 0;
    return {
      state: sensors.stateMachine?.state ?? "SLEEP",
      doorState: sensors.stateMachine?.doorState ?? "CLOSED",
      offline: false,
      isNight: mockLightValue10bit < this.stateMachineConfig.nightLightThreshold,
      radarSoftwarePaused: mockLightValue10bit < this.stateMachineConfig.nightLightThreshold,
      radarPresenceReliable: Boolean(sensors.radar?.stablePresence),
      radarWithin2m: false,
      radarWithin1m: false,
      radarApproaching: Boolean(sensors.radar?.approaching),
      imuMotionStrength: 0.08,
      lightValue10bit: mockLightValue10bit,
      lightDelta: mockLightDelta,
      radarDistanceRaw: sensors.radar?.smoothedDistanceRaw ?? 0,
      radarGate: sensors.radar?.stableGate ?? 0,
      lastReason: "Mock 状态机：亮度与雷达阈值模拟",
      autoVoiceState: "IDLE",
      autoVoiceError: "",
      updatedAtMs: Date.now(),
      stateSinceMs: Date.now() - 5000,
    };
  }

  private withVoiceDiagnostics(status: VoiceChatStatus): VoiceChatStatus {
    const sampleCount = Math.floor(status.pcmBytes / 2);
    const peakAbs = Math.max(status.rms * 3, status.rms ? 500 : 0);
    const clipCount = peakAbs > 32000 ? 4 : 0;
    return {
      ...status,
      sampleCount,
      peakAbs,
      minSample: -peakAbs,
      maxSample: peakAbs,
      meanSample: Math.round(status.rms / 18),
      clipCount,
      timeoutCount: 0,
      qualityHint: sampleCount < 8000 ? "too_short" : clipCount > 0 ? "clipping" : status.rms < 80 ? "silent" : "ok",
    };
  }

  async connect() {
    this.connected = true;
    this.connectedAt = Date.now();
    this.emitLog("info", "Mock 设备已连接，当前使用模拟数据。", "mock");
    this.timer = window.setInterval(() => {
      this.emitMessage({
        type: "event",
        event: "log",
        payload: {
          level: "debug",
          source: "sensor_task",
          message: "PIR hold=0 brightness_delta=12.4 angle_delta=0.8",
        },
      });
    }, 6500);
  }

  async disconnect() {
    this.connected = false;
    this.realtimeSessionGeneration += 1;
    this.realtimeResponseGeneration += 1;
    if (this.timer) {
      window.clearInterval(this.timer);
    }
    if (this.wakeTimer) {
      window.clearInterval(this.wakeTimer);
      this.wakeTimer = undefined;
    }
    this.emitLog("info", "Mock 设备已断开。", "mock");
  }

  // Mock 只模拟 P4 唯一 foreground Session，记录结构与 TF sessions.json 保持一致。
  private appendAgentRecord(record: Partial<AgentSessionRecord> & Pick<AgentSessionRecord, "kind" | "run_id" | "text">) {
    const item: AgentSessionRecord = {
      kind: record.kind,
      run_id: record.run_id,
      call_id: record.call_id ?? "",
      time: record.time ?? new Date().toLocaleTimeString("zh-CN", { hour12: false }).slice(0, 5),
      text: record.text,
      tool_name: record.tool_name ?? "",
      detail: record.detail ?? "",
      latency: record.latency ?? "",
      risk: record.risk ?? 0,
      tool_status: record.tool_status ?? 0,
      requires_confirmation: record.requires_confirmation ?? false,
    };
    this.agentSession = {
      ...this.agentSession,
      foreground: {
        next_sequence: this.agentSession.foreground.next_sequence + 1,
        messages: [...this.agentSession.foreground.messages, item].slice(-48),
      },
    };
  }

  // 云 Worker 在第二次状态轮询时结束，便于 WebUI 验证等待态、完成态和诊断字段。
  private advanceMockAgentRun() {
    if (!this.agentRun.active || this.agentRun.state !== "WAITING_MODEL") return;
    this.agentPollCount += 1;
    if (this.agentPollCount < 2) return;
    const reply = this.agentPendingReply || "Mock P4 小精灵已完成本次请求。";
    this.agentRun = {
      ...this.agentRun,
      active: false,
      state: "COMPLETED",
      reply,
      httpStatus: 200,
      modelLatencyMs: 420,
      modelSteps: Math.max(1, this.agentRun.modelSteps),
      cloudPending: false,
      confirmation: null,
    };
    this.appendAgentRecord({ kind: "assistant", run_id: this.agentRun.runId, text: reply, latency: "420" });
  }

  async sendCommand<TPayload = unknown>(command: DeviceCommand, payload?: unknown): Promise<DeviceResponse<TPayload>> {
    if (!this.connected) {
      throw new Error("Mock 设备未连接");
    }

    await new Promise((resolve) => window.setTimeout(resolve, 180));
    const requestId = crypto.randomUUID();
    let responsePayload: unknown;

    switch (command) {
      case "get_status":
        responsePayload = createMockStatus();
        break;
      case "get_network":
        responsePayload = this.network;
        break;
      case "get_mqtt_config":
      case "get_mqtt_status":
        responsePayload = this.mqttConfig;
        break;
      case "set_mqtt_config": {
        const update = payload as Partial<MQTTConfig> & { password?: string; token?: string };
        const secret = update.password?.trim() || update.token?.trim();
        this.mqttConfig = {
          ...this.mqttConfig,
          ...update,
          password: undefined,
          token: undefined,
          hasPassword: secret ? true : this.mqttConfig.hasPassword,
          configured: Boolean((update.brokerUri ?? this.mqttConfig.brokerUri) && (update.homeId ?? this.mqttConfig.homeId) && (update.deviceId ?? this.mqttConfig.deviceId)),
          statusText: "mock configured",
        };
        this.emitLog("info", "MQTT 云端绑定配置已写入 Mock 设备缓存，token 不会回显。", "mqtt");
        responsePayload = this.mqttConfig;
        break;
      }
      case "clear_mqtt_secret":
        this.mqttConfig = {
          ...this.mqttConfig,
          hasPassword: false,
          connected: false,
          statusText: "secret cleared",
        };
        this.emitLog("warn", "Mock MQTT token 已清除。", "mqtt");
        responsePayload = this.mqttConfig;
        break;
      case "mqtt_publish_state":
        this.mqttConfig = {
          ...this.mqttConfig,
          connected: true,
          publishedCount: this.mqttConfig.publishedCount + 1,
          statusText: "mock state published",
        };
        this.emitLog("info", "Mock 设备状态已发布到 MQTT。", "mqtt");
        responsePayload = this.mqttConfig;
        break;
      case "scan_wifi":
        responsePayload = createMockWifiNetworks();
        break;
      case "set_network":
        this.network = {
          ...(payload as NetworkConfig),
          connected: true,
          saved: true,
          internet: true,
          status: "mock_online",
          ip: "192.168.1.88",
          rssi: -48,
          lastError: "",
          saveAiKey: false,
        };
        this.emitLog("info", "网络配置已写入 Mock 设备缓存。", "network");
        responsePayload = this.network;
        break;
      case "get_ai_config":
        responsePayload = this.aiConfig;
        break;
      case "get_ai_profiles":
        responsePayload = this.getAiProfilesPayload();
        break;
      case "set_ai_config": {
        const update = payload as Partial<AIConfig> & { apiKey?: string };
        const apiKey = update.apiKey?.trim();
        const profileId = update.profileId ?? this.aiConfig.profileId ?? 0;
        this.aiConfig = {
          ...this.aiConfig,
          ...update,
          profileId,
          profileName: update.profileName?.trim() || this.aiConfig.profileName || "默认配置",
          apiKey: undefined,
          hasApiKey: apiKey ? true : this.aiConfig.hasApiKey,
          apiKeyPreview: apiKey ? `${apiKey.slice(0, 3)}...${apiKey.slice(-4)}` : this.aiConfig.apiKeyPreview,
          ready: Boolean((update.apiBaseUrl ?? this.aiConfig.apiBaseUrl) && (update.model ?? this.aiConfig.model) && (apiKey || this.aiConfig.hasApiKey)),
          lastError: "",
        };
        this.aiProfiles = this.aiProfiles
          .filter((item) => item.profileId !== profileId)
          .concat(this.aiConfig)
          .sort((a, b) => (a.profileId ?? 0) - (b.profileId ?? 0));
        this.emitLog("info", "AI API 配置已写入 Mock 设备缓存，Key 不会回显。", "ai");
        responsePayload = this.aiConfig;
        break;
      }
      case "clear_ai_key":
        this.aiConfig = {
          ...this.aiConfig,
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
          lastError: "API Key 已清除",
        };
        this.aiProfiles = this.aiProfiles.map((item) => (item.profileId === this.aiConfig.profileId ? this.aiConfig : item));
        this.emitLog("warn", "Mock AI API Key 已清除。", "ai");
        responsePayload = this.aiConfig;
        break;
      case "get_asr_config":
        responsePayload = this.asrConfig;
        break;
      case "set_asr_config": {
        const update = payload as Partial<ASRConfig> & { apiKey?: string };
        const apiKey = update.apiKey?.trim();
        this.asrConfig = {
          ...this.asrConfig,
          ...update,
          apiKey: undefined,
          apiBaseUrl: update.apiBaseUrl?.trim() || this.asrConfig.apiBaseUrl,
          model: update.model?.trim() || this.asrConfig.model,
          hasApiKey: apiKey ? true : this.asrConfig.hasApiKey,
          apiKeyPreview: apiKey ? `${apiKey.slice(0, 3)}...${apiKey.slice(-4)}` : this.asrConfig.apiKeyPreview,
          ready: Boolean((update.apiBaseUrl ?? this.asrConfig.apiBaseUrl) && (update.model ?? this.asrConfig.model) && (apiKey || this.asrConfig.hasApiKey)),
          lastError: "",
        };
        this.emitLog("info", "Mock ASR 配置已保存，Key 不会回显。", "asr");
        responsePayload = this.asrConfig;
        break;
      }
      case "clear_asr_key":
        this.asrConfig = {
          ...this.asrConfig,
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
          lastError: "ASR Key 已清除",
        };
        responsePayload = this.asrConfig;
        break;
      case "get_tts_config":
        responsePayload = this.ttsConfig;
        break;
      case "set_tts_config": {
        const update = payload as Partial<TTSConfig> & { apiKey?: string };
        const apiKey = update.apiKey?.trim();
        this.ttsConfig = {
          ...this.ttsConfig,
          ...update,
          apiKey: undefined,
          apiBaseUrl: update.apiBaseUrl?.trim() || this.ttsConfig.apiBaseUrl,
          model: update.model?.trim() || this.ttsConfig.model,
          voice: update.voice?.trim() || this.ttsConfig.voice,
          hasApiKey: apiKey ? true : this.ttsConfig.hasApiKey,
          apiKeyPreview: apiKey ? `${apiKey.slice(0, 3)}...${apiKey.slice(-4)}` : this.ttsConfig.apiKeyPreview,
          ready: Boolean((update.apiBaseUrl ?? this.ttsConfig.apiBaseUrl) && (update.model ?? this.ttsConfig.model) && (update.voice ?? this.ttsConfig.voice) && (apiKey || this.ttsConfig.hasApiKey)),
          lastError: "",
        };
        this.emitLog("info", "Mock TTS 配置已保存，Key 不会回显。", "tts");
        responsePayload = this.ttsConfig;
        break;
      }
      case "clear_tts_key":
        this.ttsConfig = {
          ...this.ttsConfig,
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
          lastError: "TTS Key 已清除",
        };
        responsePayload = this.ttsConfig;
        break;
      case "get_state_machine_config":
        responsePayload = this.stateMachineConfig;
        break;
      case "set_state_machine_config":
        this.stateMachineConfig = {
          ...this.stateMachineConfig,
          ...(payload as Partial<StateMachineConfig>),
        };
        this.emitLog("info", "Mock 状态机配置已保存。", "state");
        responsePayload = this.stateMachineConfig;
        break;
      case "get_state_machine_status":
        responsePayload = this.createStateMachineStatus();
        break;
      case "create_ai_profile": {
        const nextId = Math.max(...this.aiProfiles.map((item) => item.profileId ?? 0)) + 1;
        this.aiConfig = {
          ...createMockAIConfig(),
          profileId: nextId,
          profileName: (payload as { profileName?: string })?.profileName || `备用配置 ${nextId}`,
          apiBaseUrl: "",
          hasApiKey: false,
          apiKeyPreview: "",
          ready: false,
        };
        this.aiProfiles = [...this.aiProfiles, this.aiConfig];
        responsePayload = this.aiConfig;
        break;
      }
      case "select_ai_profile": {
        const profileId = (payload as { profileId?: number })?.profileId ?? 0;
        const found = this.aiProfiles.find((item) => item.profileId === profileId);
        if (!found) {
          throw new Error("Mock AI 配置不存在");
        }
        this.aiConfig = found;
        responsePayload = this.aiConfig;
        break;
      }
      case "delete_ai_profile": {
        const profileId = (payload as { profileId?: number })?.profileId ?? 0;
        if (profileId === 0) {
          throw new Error("默认配置不能删除");
        }
        this.aiProfiles = this.aiProfiles.filter((item) => item.profileId !== profileId);
        this.aiConfig = this.aiProfiles[0] ?? createMockAIConfig();
        responsePayload = this.aiConfig;
        break;
      }
      case "agent_submit": {
        const message = String((payload as { message?: string } | undefined)?.message ?? "").trim().slice(0, 512);
        if (!message) throw new Error("Mock 小精灵需要 message");
        if (this.agentRun.active) throw new Error("Mock 小精灵已有任务正在执行");
        const runId = `mock-run-${Date.now()}`;
        const needsConfirmation = /删除|清空|写入|添加|设置|修改/.test(message);
        this.appendAgentRecord({ kind: "user", run_id: runId, text: message });
        this.agentPollCount = 0;
        this.agentPendingReply = needsConfirmation
          ? "Mock P4 小精灵已收到工具处理结果，并完成后续推理。"
          : `Mock P4 小精灵：已收到“${message.slice(0, 80)}”。真实设备会构建完整上下文并进入最多 6 步的处理流程。`;
        this.agentRun = {
          active: true,
          runId,
          state: needsConfirmation ? "WAITING_CONFIRMATION" : "WAITING_MODEL",
          intent: needsConfirmation ? "tool_write" : "open_chat",
          reply: "",
          error: "",
          httpStatus: 0,
          modelLatencyMs: 0,
          contextBytes: 32768,
          toolSchemaCount: 18,
          modelSteps: 1,
          cloudStackHighWater: 12288,
          freePsramBytes: 28 * 1024 * 1024,
          internalDmaLargestBytes: 96 * 1024,
          cloudPending: !needsConfirmation,
          confirmation: needsConfirmation ? {
            callId: `mock-call-${Date.now()}`,
            toolName: "shopping.add_item",
            arguments: { name: "牛奶", quantity: "1盒", source: "web_mock" },
            permission: "WRITE",
          } : null,
        };
        if (this.agentRun.confirmation) {
          this.appendAgentRecord({
            kind: "tool",
            run_id: runId,
            call_id: this.agentRun.confirmation.callId,
            text: "等待用户确认",
            tool_name: this.agentRun.confirmation.toolName,
            detail: JSON.stringify(this.agentRun.confirmation.arguments),
            risk: 1,
            requires_confirmation: true,
          });
        }
        responsePayload = { queued: true, runId, state: "ROUTING" } satisfies AgentSubmitResponse;
        break;
      }
      case "agent_run_status":
        this.advanceMockAgentRun();
        responsePayload = this.agentRun;
        break;
      case "agent_cancel": {
        if (!this.agentRun.active) throw new Error("没有可取消的 Mock 小精灵任务");
        this.agentRun = {
          ...this.agentRun,
          active: false,
          state: "CANCELLED",
          error: "",
          cloudPending: false,
          confirmation: null,
        };
        responsePayload = this.agentRun;
        break;
      }
      case "agent_confirm":
      case "agent_reject": {
        const request = payload as { runId?: string; callId?: string } | undefined;
        const confirmation = this.agentRun.confirmation;
        if (!this.agentRun.active || this.agentRun.state !== "WAITING_CONFIRMATION" || !confirmation) {
          throw new Error("当前没有待确认的 Mock 工具");
        }
        if (request?.runId !== this.agentRun.runId || request?.callId !== confirmation.callId) {
          throw new Error("Mock 小精灵任务或 callId 不匹配");
        }
        const confirmed = command === "agent_confirm";
        this.appendAgentRecord({
          kind: "result",
          run_id: this.agentRun.runId,
          call_id: confirmation.callId,
          text: confirmed ? "用户确认，工具执行成功" : "用户拒绝，工具未执行",
          tool_name: confirmation.toolName,
          detail: confirmed ? "ok" : "rejected",
          tool_status: confirmed ? 1 : 2,
        });
        this.agentPendingReply = confirmed
          ? "Mock P4 小精灵：工具已经执行，我已把牛奶加入购物清单。"
          : "Mock P4 小精灵：已拒绝该工具调用，没有修改购物清单。";
        this.agentPollCount = 0;
        this.agentRun = {
          ...this.agentRun,
          state: "WAITING_MODEL",
          modelSteps: this.agentRun.modelSteps + 1,
          cloudPending: true,
          confirmation: null,
        };
        responsePayload = { accepted: true, confirmed, runId: this.agentRun.runId, callId: confirmation.callId };
        break;
      }
      case "agent_session_read":
        responsePayload = this.agentSession;
        break;
      case "agent_session_clear":
        if (this.agentRun.active) throw new Error("小精灵运行中，不能清空会话");
        this.agentSession = {
          schema_version: 1,
          foreground: {
            next_sequence: this.agentSession.foreground.next_sequence,
            messages: [],
          },
        };
        responsePayload = this.agentSession;
        break;
      case "test_ai_chat": {
        const message = (payload as { message?: string })?.message ?? "";
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存一个测试 Key。");
        }
        responsePayload = {
          reply: `Mock P4 小精灵：已收到“${message.slice(0, 80)}”。真实设备会构建完整上下文并进入处理流程。`,
          latencyMs: 180,
          runId: `mock-run-${Date.now()}`,
          status: "COMPLETED",
          httpStatus: 200,
        } satisfies AgentChatResponse;
        break;
      }
      case "ai_assistant_chat": {
        const request = this.normalizeAssistantRequest(payload);
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存一个测试 Key。");
        }
        const tool = this.tryExecuteKitchenToolFromText(request.message);
        const reply = tool?.message ?? this.createAssistantReply(request);
        const historyCount = this.chatHistory.messages.length;
        const historyPrunedCount = this.appendMockChatHistory(request, reply);
        responsePayload = {
          reply,
          model: this.aiConfig.model,
          latencyMs: 260,
          status: "mock_ok",
          httpStatus: 200,
          taskType: request.taskType,
          contextInjected: true,
          localSnapshotVersion: 1,
          needsConfirmation: request.taskType !== "chat_assist",
          historyInjected: historyCount > 0,
          historyCount: Math.min(historyCount, this.chatHistory.maxMessages),
          historyPersisted: true,
          historyPrunedCount,
          toolExecuted: Boolean(tool),
          toolMessage: tool?.message ?? "",
        } satisfies AIAssistantChatResponse;
        this.emitLog("info", `Mock AI 助手已注入上下文并完成：${request.taskType}`, "ai");
        break;
      }
      case "get_kitchen_ai_status":
        responsePayload = this.kitchenAiStatus;
        break;
      case "kitchen_ai_generate": {
        const request = payload as Partial<KitchenAIGenerateRequest> | undefined;
        const page = this.normalizeKitchenAiPage(request?.page);
        const mockReply = page === "nutrition"
          ? "{\"schema_version\":1,\"generated_at\":0,\"nutrition\":{\"score\":82,\"summary\":\"蛋白质够用，蔬菜可再补一点\",\"focus\":[\"补绿叶菜\"],\"suggestions\":[\"晚餐加一份青菜\"],\"restrict_note\":\"仅供日常参考，非医疗建议\"}}"
          : page === "shopping"
            ? "{\"schema_version\":1,\"generated_at\":0,\"items\":[{\"name\":\"青菜\",\"quantity\":\"1把\",\"reason\":\"补充蔬菜\",\"priority\":\"high\",\"checked\":false}]}"
            : "{\"schema_version\":1,\"generated_at\":0,\"items\":[{\"title\":\"菠菜虾仁豆腐汤\",\"tag\":\"清淡临期\",\"time_minutes\":15,\"use_inventory\":[\"菠菜\",\"虾仁\",\"豆腐\"],\"missing\":[],\"steps\":[\"洗净菠菜\",\"豆腐切块\",\"煮开调味\"],\"reason\":\"优先消耗临期食材\"}]}";
        this.kitchenAiStatus = {
          running: false,
          lastOk: true,
          state: "ok",
          lastTrigger: "manual",
          currentPage: "all",
          lastPage: page,
          lastGeneratedAt: Math.floor(Date.now() / 1000),
          lastAutoDay: this.createTodayKey(),
          httpStatus: 200,
          latencyMs: 420 + Math.round(Math.random() * 260),
          lastReplyLen: mockReply.length,
          lastError: "",
          lastReplyPreview: mockReply,
        };
        this.emitLog("info", `Mock 厨房 AI 已生成 ${page} JSON 缓存。`, "kitchen_ai");
        responsePayload = this.kitchenAiStatus;
        break;
      }
      case "get_kitchen_tools":
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "timer_start": {
        const request = payload as { durationSeconds?: number; duration_seconds?: number; label?: string } | undefined;
        const seconds = Math.max(1, request?.durationSeconds ?? request?.duration_seconds ?? 480);
        this.startKitchenTimer(seconds, request?.label || "厨房");
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      }
      case "timer_pause":
        this.pauseKitchenTimer();
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "timer_resume":
        this.resumeKitchenTimer();
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "timer_cancel":
        this.kitchenTools.timer = { state: "idle", durationSeconds: 0, remainingSeconds: 0, label: "" };
        this.kitchenTimerStartedAt = 0;
        this.kitchenTimerEndsAt = 0;
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "stopwatch_start":
        this.startStopwatch();
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "stopwatch_pause":
        this.pauseStopwatch();
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "stopwatch_reset":
        this.kitchenTools.stopwatch = { state: "idle", elapsedSeconds: 0 };
        this.stopwatchStartedAt = 0;
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      case "alarm_set": {
        const request = payload as { hour?: number; minute?: number; label?: string } | undefined;
        const hour = request?.hour ?? 7;
        const minute = request?.minute ?? 0;
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
          throw new Error("闹钟时间无效");
        }
        if (this.kitchenTools.alarms.length >= 5) {
          throw new Error("最多只能设置 5 个闹钟");
        }
        this.kitchenTools.alarms = [
          ...this.kitchenTools.alarms,
          { id: this.nextAlarmId++, enabled: true, ringing: false, hour, minute, label: request?.label || "厨房提醒" },
        ];
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      }
      case "alarm_cancel":
      case "alarm_dismiss": {
        const request = payload as { id?: number } | undefined;
        const id = request?.id ?? this.kitchenTools.alarms[0]?.id;
        if (!id) {
          throw new Error("没有可操作的闹钟");
        }
        if (command === "alarm_cancel") {
          this.kitchenTools.alarms = this.kitchenTools.alarms.filter((alarm) => alarm.id !== id);
        } else {
          this.kitchenTools.alarms = this.kitchenTools.alarms.map((alarm) => (alarm.id === id ? { ...alarm, ringing: false } : alarm));
        }
        responsePayload = this.getKitchenToolsSnapshot();
        break;
      }
      case "wake_start":
      case "wake_set_enabled": {
        const enabled = command === "wake_start" || Boolean((payload as { enabled?: boolean } | undefined)?.enabled);
        if (!enabled) {
          this.stopMockWakeEvents();
          this.wakeStatus = createMockWakeStatus({
            ...this.wakeStatus,
            enabled: false,
            state: "idle",
            error: "",
          });
          this.emitLog("info", "Mock WakeNet 已持久关闭。", "wake");
          responsePayload = this.wakeStatus;
          break;
        }
        this.wakeStatus = createMockWakeStatus({
          ...this.wakeStatus,
          enabled: true,
          state: "listening",
          error: "",
          rms: 120 + Math.round(Math.random() * 80),
          peakAbs: 900 + Math.round(Math.random() * 600),
        });
        this.startMockWakeEvents();
        this.emitLog("info", "Mock WakeNet 已持久启用。", "wake");
        responsePayload = this.wakeStatus;
        break;
      }
      case "wake_stop":
        this.stopMockWakeEvents();
        this.wakeStatus = createMockWakeStatus({
          ...this.wakeStatus,
          enabled: false,
          state: "idle",
          error: "",
        });
        this.emitLog("info", "Mock WakeNet 监听已停止。", "wake");
        responsePayload = this.wakeStatus;
        break;
      case "wake_status":
        if (this.wakeStatus.enabled) {
          this.wakeStatus = createMockWakeStatus({
            ...this.wakeStatus,
            state: "listening",
            vadState: Math.random() > 0.72 ? 1 : 0,
            rms: 110 + Math.round(Math.random() * 260),
            peakAbs: 600 + Math.round(Math.random() * 1800),
          });
        }
        responsePayload = this.wakeStatus;
        break;
      case "wake_reset_stats":
        this.wakeStatus = createMockWakeStatus({
          ...this.wakeStatus,
          triggerCount: 0,
          lastTriggerMs: 0,
          timeoutCount: 0,
          error: "",
        });
        responsePayload = this.wakeStatus;
        break;
      case "mic_record_start":
      case "voice_chat_start": {
        if (this.wakeStatus.enabled) {
          throw new Error("audio busy");
        }
        // 读取请求 payload 的 source：缺省时默认 FY-SP703，显式 es8311 仍可用作回退；
        // 非法值与真机协议保持一致，直接拒绝而不是静默切换音源。
        const request = payload as { source?: MicSource } | undefined;
        if (request?.source && request.source !== "es8311" && request.source !== "fy_sp703") {
          throw new Error("unsupported audio source");
        }
        const source: MicSource = request?.source ?? "fy_sp703";
        this.voiceStatus = this.withVoiceDiagnostics({
          state: "recording",
          durationMs: 0,
          pcmBytes: 0,
          rms: 0,
          error: "",
          source,
        });
        this.emitLog("info", `Mock ${source} voice recording started.`, "asr");
        responsePayload = this.voiceStatus;
        break;
      }
      case "mic_record_status":
      case "voice_chat_status":
        if (this.voiceStatus.state === "recording") {
          this.voiceStatus = this.withVoiceDiagnostics({
            ...this.voiceStatus,
            durationMs: Math.min(this.voiceStatus.durationMs + 500, 6000),
            pcmBytes: this.voiceStatus.pcmBytes + 16000,
            rms: Math.round(300 + Math.random() * 1200),
          });
        }
        responsePayload = this.voiceStatus;
        break;
      case "mic_record_stop":
        this.voiceStatus = this.withVoiceDiagnostics({
          ...this.voiceStatus,
          state: "ready",
          durationMs: Math.max(this.voiceStatus.durationMs, 500),
        });
        this.emitLog("info", "Mock microphone recording stopped without ASR.", "asr");
        responsePayload = this.voiceStatus;
        break;
      case "mic_record_wav":
        if (this.voiceStatus.state !== "ready" || this.voiceStatus.pcmBytes <= 0) {
          throw new Error("no recorded PCM ready");
        }
        responsePayload = createMockWavBase64(Math.max(this.voiceStatus.durationMs, 1000));
        break;
      case "mic_record_wav_info": {
        if (this.voiceStatus.state !== "ready" || this.voiceStatus.pcmBytes <= 0) {
          throw new Error("no recorded PCM ready");
        }
        this.micWavCache = createMockWavBase64(Math.min(2000, Math.max(this.voiceStatus.durationMs, 1000)));
        this.micWavBytes = decodeMockBase64(this.micWavCache.wavBase64);
        this.micWavSession += 1;
        responsePayload = {
          sampleRate: this.micWavCache.sampleRate,
          channels: this.micWavCache.channels,
          bitsPerSample: this.micWavCache.bitsPerSample,
          durationMs: this.micWavCache.durationMs,
          pcmBytes: this.micWavCache.pcmBytes,
          wavBytes: this.micWavCache.wavBytes,
          chunkBytes: 1024,
          session: this.micWavSession,
          fullCrc32: crc32Mock(this.micWavBytes),
        };
        break;
      }
      case "mic_record_wav_chunk": {
        if (!this.micWavCache || !this.micWavBytes) {
          throw new Error("mic_record_wav_info is required first");
        }
        const request = payload as { session?: number; offset?: number } | undefined;
        if (request?.session !== this.micWavSession) {
          throw new Error("WAV transfer session is not active");
        }
        const offset = request?.offset ?? -1;
        if (!Number.isInteger(offset) || offset < 0 || offset >= this.micWavBytes.length) {
          throw new Error("WAV offset out of range");
        }
        const chunk = this.micWavBytes.slice(offset, Math.min(offset + 1024, this.micWavBytes.length));
        let binary = "";
        for (const byte of chunk) binary += String.fromCharCode(byte);
        responsePayload = {
          session: this.micWavSession,
          offset,
          length: chunk.length,
          crc32: crc32Mock(chunk),
          eof: offset + chunk.length === this.micWavBytes.length,
          data_b64: window.btoa(binary),
        };
        break;
      }
      case "mic_record_wav_end": {
        const request = payload as { session?: number } | undefined;
        if (!this.micWavCache || request?.session !== this.micWavSession) {
          throw new Error("WAV transfer session is not active");
        }
        this.micWavCache = null;
        this.micWavBytes = null;
        responsePayload = { session: this.micWavSession, released: true };
        break;
      }
      case "asr_transcribe": {
        if (!this.asrConfig.hasApiKey) {
          throw new Error("Mock ASR 缺少 API Key，请先保存 ASR Key。");
        }
        if (this.voiceStatus.state !== "ready" || this.voiceStatus.pcmBytes <= 0) {
          throw new Error("no recorded PCM ready");
        }
        responsePayload = {
          text: "冰箱里还有哪些食材",
          model: this.asrConfig.model,
          httpStatus: 200,
          latencyMs: 680,
          audioBytes: this.voiceStatus.pcmBytes + 44,
        } satisfies AsrTranscribeResponse;
        break;
      }
      case "radar_test_start":
        this.radar = createMockRadar(this.radar);
        this.radar.mode = "report";
        this.emitLog("info", "Mock radar report mode started.", "radar");
        responsePayload = this.radar;
        break;
      case "radar_test_status":
        this.radar = createMockRadar(this.radar);
        responsePayload = this.radar;
        break;
      case "radar_test_stop":
        this.radar = {
          ...createMockRadar(this.radar),
          mode: "normal",
          lastText: "normal mode",
        };
        this.emitLog("info", "Mock radar returned to normal mode.", "radar");
        responsePayload = this.radar;
        break;
      case "voice_chat_stop": {
        if (!this.asrConfig.hasApiKey) {
          throw new Error("Mock ASR 缺少 API Key，请先保存 ASR Key。");
        }
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存 AI Key。");
        }
        this.voiceStatus = this.withVoiceDiagnostics({
          ...this.voiceStatus,
          state: "ready",
          durationMs: Math.max(this.voiceStatus.durationMs, 1800),
          pcmBytes: Math.max(this.voiceStatus.pcmBytes, 57600),
          rms: Math.max(this.voiceStatus.rms, 620),
        });
        const transcript = "帮我看看今晚可以吃什么";
        const request: AIAssistantChatRequest = {
          ...this.normalizeProjectAiRequest({ taskType: "voice_intent_parse", userText: transcript }),
          taskType: "voice_intent_parse",
          message: transcript,
          userText: transcript,
        };
        const reply = this.createAssistantReply(request);
        this.appendMockChatHistory(request, reply);
        responsePayload = {
          transcript,
          reply,
          asrModel: this.asrConfig.model,
          aiModel: this.aiConfig.model,
          asrLatencyMs: 680,
          aiLatencyMs: 930,
          asrHttpStatus: 200,
          aiHttpStatus: 200,
          audioBytes: this.voiceStatus.pcmBytes + 44,
          historyPrunedCount: 0,
        } satisfies VoiceChatResponse;
        break;
      }
      case "tts_play": {
        const text = (payload as { text?: string })?.text ?? "";
        if (!this.ttsConfig.hasApiKey) {
          throw new Error("Mock TTS 缺少 API Key，请先保存 TTS Key。");
        }
        this.stopMockWakeEvents();
        this.wakeStatus = createMockWakeStatus({
          ...this.wakeStatus,
          enabled: false,
          state: "idle",
        });
        const audioBytes = Math.max(24000, Math.min(240000, text.length * 1800));
        this.ttsStatus = {
          state: "playing",
          sampleRate: 24000,
          audioBytes,
          playedBytes: Math.round(audioBytes * 0.18),
          durationMs: Math.round(audioBytes / 48),
          latencyMs: 520,
          httpStatus: 200,
          model: this.ttsConfig.model,
          voice: this.ttsConfig.voice,
          error: "",
        };
        this.emitLog("info", `Mock TTS 已合成并开始播放：${text.slice(0, 24)}`, "tts");
        responsePayload = this.ttsStatus;
        break;
      }
      case "tts_status":
        if (this.ttsStatus.state === "playing") {
          const playedBytes = Math.min(this.ttsStatus.audioBytes, this.ttsStatus.playedBytes + 24000);
          this.ttsStatus = {
            ...this.ttsStatus,
            playedBytes,
            state: playedBytes >= this.ttsStatus.audioBytes ? "done" : "playing",
          };
        }
        responsePayload = this.ttsStatus;
        break;
      case "tts_stop":
        this.ttsStatus = {
          ...this.ttsStatus,
          state: "idle",
        };
        this.emitLog("info", "Mock TTS 播放已停止。", "tts");
        responsePayload = this.ttsStatus;
        break;
      case "speaker_beep":
        this.emitLog("info", "Mock 板载扬声器自检：1000 Hz / 500 ms。", "tts");
        responsePayload = { played: true, frequencyHz: 1000, durationMs: 500 };
        break;
      case "get_camera_status":
        responsePayload = this.cameraStatus;
        break;
      case "camera_probe":
        responsePayload = {
          ok: true,
          xclkEnabled: false,
          sccbReady: true,
          address: 0x3c,
          pidHigh: 0x36,
          pidLow: 0x60,
          pid: 0x3660,
          expectedPid: 0x3660,
          durationMs: 42,
          espErr: 0,
          espErrName: "ESP_OK",
          lastError: "",
        } satisfies CameraProbeResponse;
        this.emitLog("info", "Mock OV3660 probe ok: PID=0x3660.", "camera");
        break;
      case "camera_capture":
        this.cameraStatus = {
          ...createMockCameraStatus(true),
          frameId: this.cameraStatus.frameId + 1,
          jpegBytes: 18000 + Math.round(Math.random() * 9000),
          captureMs: 240 + Math.round(Math.random() * 180),
          freePsramKb: 6100 - Math.round(Math.random() * 120),
        };
        this.cameraPreviewDataUrl = this.createMockCameraPreview();
        responsePayload = {
          status: this.cameraStatus,
          previewDataUrl: this.cameraPreviewDataUrl,
          previewOmitted: false,
          previewMaxBytes: 65536,
        } satisfies CameraCaptureResponse;
        this.emitLog("info", `Mock OV3660 captured frame ${this.cameraStatus.frameId}.`, "camera");
        break;
      case "camera_jpeg_diag":
        this.cameraStatus = {
          ...createMockCameraStatus(true),
          frameId: this.cameraStatus.frameId + 1,
          jpegBytes: 12000 + Math.round(Math.random() * 8000),
          captureMs: 120 + Math.round(Math.random() * 100),
          pixelFormat: "JPEG",
          frameSize: "QVGA",
          freePsramKb: 6150 - Math.round(Math.random() * 80),
        };
        this.cameraPreviewDataUrl = this.createMockCameraPreview();
        responsePayload = {
          status: this.cameraStatus,
          previewDataUrl: this.cameraPreviewDataUrl,
          previewOmitted: false,
          previewMaxBytes: 65536,
        } satisfies CameraCaptureResponse;
        this.emitLog("info", `Mock OV3660 hardware JPEG diag frame ${this.cameraStatus.frameId}.`, "camera");
        break;
      case "camera_rgb565_diag":
        responsePayload = {
          ok: true,
          width: 160,
          height: 120,
          bytes: 38400,
          captureMs: 180,
          checksum: 0x36a5c021,
          firstBytes: "102030405060708090a0b0c0d0e0f000",
          espErr: 0,
          espErrName: "ESP_OK",
          lastError: "",
        } satisfies CameraRgb565DiagResponse;
        this.cameraStatus = {
          ...this.cameraStatus,
          initialized: true,
          pixelFormat: "RGB565",
          frameSize: "QQVGA",
        };
        this.emitLog("info", "Mock RGB565 诊断成帧。", "camera");
        break;
      case "camera_analyze":
        if (!this.cameraStatus.hasFrame) {
          throw new Error("没有最近照片，请先拍照。");
        }
        if (!this.aiConfig.hasApiKey) {
          throw new Error("Mock AI 缺少 API Key，请先保存一个测试 Key。");
        }
        responsePayload = {
          taskType: "recognize_ingredients",
          reply: JSON.stringify({
            schema_version: 1,
            type: "recognize_ingredients",
            candidates: [
              { name: "番茄", quantity: "约2个", confidence: 0.82, doubt: "Mock 预览图，仅验证流程" },
              { name: "鸡蛋", quantity: "可能有1枚", confidence: 0.54, doubt: "边缘区域不清晰，需要用户确认" },
            ],
            needs_confirmation: true,
            confirm_fields: ["名称", "数量", "保质期", "存放位置"],
            safety_note: "识别结果不会直接入库，请人工确认。",
          }, null, 2),
          model: this.aiConfig.model,
          status: "mock_ok",
          httpStatus: 200,
          latencyMs: 980,
          width: this.cameraStatus.width,
          height: this.cameraStatus.height,
          jpegBytes: this.cameraStatus.jpegBytes,
          needsConfirmation: true,
        } satisfies CameraAnalyzeResponse;
        this.emitLog("info", "Mock 摄像头图片识别已返回候选结果。", "camera");
        break;
      case "clear_camera_frame":
        this.cameraStatus = createMockCameraStatus(false);
        this.cameraPreviewDataUrl = null;
        responsePayload = this.cameraStatus;
        break;
      case "camera_reset":
        this.cameraStatus = createMockCameraStatus(false);
        this.cameraPreviewDataUrl = null;
        responsePayload = this.cameraStatus;
        this.emitLog("info", "Mock 摄像头驱动已重置。", "camera");
        break;
      case "get_ai_context_preview": {
        const request = this.normalizeProjectAiRequest(payload);
        responsePayload = this.createContextPreview(request);
        break;
      }
      case "test_ai_task": {
        const request = this.normalizeProjectAiRequest(payload);
        responsePayload = this.createProjectAiResult(request);
        this.emitLog("info", `Mock 项目 AI 任务完成：${request.taskType}`, "ai_context");
        break;
      }
      case "get_chat_history":
        responsePayload = this.chatHistory;
        break;
      case "clear_chat_history":
        this.chatHistory = {
          ...this.chatHistory,
          updatedAt: Math.floor(Date.now() / 1000),
          count: 0,
          prunedCount: 0,
          messages: [],
        };
        responsePayload = this.chatHistory;
        break;
      case "get_tfcard_status": {
        const usedBytes = [...this.tfFiles.values()].reduce((sum, item) => sum + item.bytes.length, 0);
        responsePayload = { mounted: this.tfMounted, filesystem: "FAT32", mountPoint: "/sdcard", capacityMb: Math.floor(64_026_652_672 / 1024 / 1024), freeMb: Math.floor((64_026_652_672 - usedBytes) / 1024 / 1024), expertMode: this.tfExpertMode, transferActive: this.tfTransfers.size > 0, formatRequired: false, lastError: "" };
        break;
      }
      case "screenshot_capture": {
        const path = `.fridge_spirit/exports/shot_mock_${Date.now()}.bmp`;
        const bytes = createMockScreenshotBmp();
        this.tfFiles.set(path, {
          entry: { name: path.split("/").at(-1) || "shot_mock.bmp", path, type: "file", size: bytes.length, modifiedAt: Math.floor(Date.now() / 1000) },
          bytes,
        });
        responsePayload = { path, format: "bmp" };
        this.emitLog("info", "Mock 截屏已保存并可读回。", "screenshot");
        break;
      }
      case "tf_get_access_mode":
        responsePayload = { expertMode: this.tfExpertMode, protectedRoot: "/.fridge_spirit" };
        break;
      case "tf_set_access_mode":
        this.tfExpertMode = Boolean((payload as { enabled?: boolean })?.enabled);
        responsePayload = { expertMode: this.tfExpertMode, protectedRoot: ".fridge_spirit" };
        break;
      case "tf_list": {
        if (!this.tfMounted) throw new Error("TF 卡未挂载");
        const request = payload as { path?: string; cursor?: number; limit?: number } | undefined;
        const parent = request?.path || "";
        const prefix = parent ? `${parent}/` : "";
        const all = [...this.tfFiles.values()].map((item) => item.entry).filter((entry) => entry.path.startsWith(prefix) && !entry.path.slice(prefix.length).includes("/") && entry.path !== parent);
        const start = Number(request?.cursor || 0); const limit = request?.limit || 80; const page = all.slice(start, start + limit);
        responsePayload = { path: parent, entries: page.map((entry) => ({ ...entry, isDir: entry.type === "directory", mtime: entry.modifiedAt })), nextCursor: start + page.length, hasMore: start + page.length < all.length };
        break;
      }
      case "tf_stat": {
        const target = (payload as { path?: string })?.path || "/";
        responsePayload = this.tfFiles.get(target)?.entry ?? { name: "TF", path: "/", type: "directory", size: 0 };
        break;
      }
      case "tf_mkdir": {
        const target = (payload as { path: string }).path; const name = target.split("/").filter(Boolean).at(-1) || "新目录";
        this.tfFiles.set(target, { entry: { name, path: target, type: "directory", size: 0, modifiedAt: Math.floor(Date.now() / 1000) }, bytes: new Uint8Array() });
        responsePayload = this.tfFiles.get(target)?.entry;
        break;
      }
      case "tf_move": {
        const request = payload as { from: string; to: string }; const found = this.tfFiles.get(request.from);
        if (!found) throw new Error("文件不存在");
        this.tfFiles.delete(request.from); const name = request.to.split("/").filter(Boolean).at(-1) || found.entry.name;
        found.entry = { ...found.entry, name, path: request.to, modifiedAt: Math.floor(Date.now() / 1000) }; this.tfFiles.set(request.to, found); responsePayload = { path: request.to };
        break;
      }
      case "tf_delete": {
        const target = (payload as { path: string }).path;
        if (target.startsWith(".fridge_spirit") && !this.tfExpertMode) throw new Error("保护模式禁止删除系统目录");
        const found = this.tfFiles.get(target); if (!found) throw new Error("文件不存在");
        const trashId = String(Date.now()); const trashPath = `.fridge_spirit/trash/${trashId}.data`; this.tfFiles.delete(target);
        found.entry = { ...found.entry, name: `${trashId}.data`, path: trashPath, originalPath: target }; this.tfFiles.set(trashPath, found); responsePayload = { trashId };
        break;
      }
      case "tf_restore": {
        const trashId = (payload as { trashId: string }).trashId; const target = `.fridge_spirit/trash/${trashId}.data`; const found = this.tfFiles.get(target); if (!found) throw new Error("回收项目不存在");
        const original = found.entry.originalPath || found.entry.name; this.tfFiles.delete(target); found.entry = { ...found.entry, name: original.split("/").at(-1) || original, path: original, originalPath: undefined }; this.tfFiles.set(original, found); responsePayload = { path: original };
        break;
      }
      case "tf_trash_empty":
        for (const key of [...this.tfFiles.keys()]) if (key.startsWith(".fridge_spirit/trash/")) this.tfFiles.delete(key);
        responsePayload = { jobId: crypto.randomUUID(), state: "done", deleted: true };
        break;
      case "tf_text_read": {
        const request = payload as { path: string; maxBytes?: number; encoding?: string }; const found = this.tfFiles.get(request.path); if (!found) throw new Error("文件不存在");
        const bytes = found.bytes.slice(0, request.maxBytes || found.bytes.length); let binary = ""; bytes.forEach((byte) => { binary += String.fromCharCode(byte); });
        responsePayload = request.encoding === "base64" ? { base64: btoa(binary) } : { text: new TextDecoder().decode(bytes) };
        break;
      }
      case "tf_text_write": {
        const request = payload as { path: string; text: string }; const bytes = new TextEncoder().encode(request.text); const old = this.tfFiles.get(request.path);
        this.tfFiles.set(request.path, { entry: { ...(old?.entry ?? { name: request.path.split("/").at(-1) || "file", path: request.path, type: "file" as const }), size: bytes.length, modifiedAt: Math.floor(Date.now() / 1000) }, bytes }); responsePayload = { size: bytes.length };
        break;
      }
      case "tf_transfer_begin": {
        const request = payload as { direction: "read" | "write"; path: string; totalSize: number }; const id = this.tfNextSession++; const existing = this.tfFiles.get(request.path)?.bytes ?? new Uint8Array();
        const totalSize = request.direction === "read" ? existing.length : request.totalSize;
        const session: TfTransferSession & { bytes: Uint8Array } = { session: id, active: true, direction: request.direction, path: request.path, offset: 0, totalSize, chunkSize: 1024, bytes: request.direction === "write" ? new Uint8Array(totalSize) : existing };
        this.tfTransfers.set(id, session); responsePayload = session;
        break;
      }
      case "tf_transfer_status": {
        const requested = (payload as { session?: number } | undefined)?.session;
        if (!requested && this.tfTransfers.size > 1) throw new Error("存在多个传输会话，必须指定 session");
        const session = requested ? this.tfTransfers.get(requested) : [...this.tfTransfers.values()][0];
        responsePayload = session ?? { active: false, session: 0, direction: "read", path: "", offset: 0, totalSize: 0, chunkSize: 1024 }; break;
      }
      case "tf_transfer_chunk": {
        const request = payload as { session: number; offset: number; data_b64?: string }; const session = this.tfTransfers.get(request.session); if (!session) throw new Error("传输会话不存在");
        if (request.data_b64 !== undefined) { const binary = atob(request.data_b64); const bytes = Uint8Array.from(binary, (char) => char.charCodeAt(0)); session.bytes.set(bytes, request.offset); session.offset = request.offset + bytes.length; responsePayload = { nextOffset: session.offset }; }
        else { const bytes = session.bytes.slice(request.offset, request.offset + 1024); let binary = ""; bytes.forEach((byte) => { binary += String.fromCharCode(byte); }); session.offset = request.offset + bytes.length; responsePayload = { data_b64: btoa(binary), crc32: crc32Mock(bytes), length: bytes.length, offset: request.offset, eof: session.offset >= session.totalSize }; }
        break;
      }
      case "tf_transfer_end": {
        const id = (payload as { session: number }).session; const session = this.tfTransfers.get(id); if (!session) throw new Error("传输会话不存在");
        if (session.direction === "write") { const name = session.path.split("/").at(-1) || "file"; this.tfFiles.set(session.path, { entry: { name, path: session.path, type: "file", size: session.bytes.length, modifiedAt: Math.floor(Date.now() / 1000) }, bytes: session.bytes }); }
        this.tfTransfers.delete(id); responsePayload = { completed: true }; break;
      }
      case "tf_transfer_abort":
        this.tfTransfers.delete((payload as { session: number }).session); responsePayload = { aborted: true }; break;
      case "tf_mount": this.tfMounted = true; responsePayload = { mounted: true }; break;
      case "tf_unmount": this.tfMounted = false; responsePayload = { mounted: false }; break;
      case "tfcard_selftest": responsePayload = { jobId: crypto.randomUUID(), state: "done", readOk: true, writeOk: true }; break;
      case "tf_health_test": responsePayload = { jobId: crypto.randomUUID(), state: "done", ioHealthy: true, smartAvailable: false }; break;
      case "tf_benchmark": responsePayload = { jobId: crypto.randomUUID(), state: "done", readKbps: 18400, writeKbps: 11200 }; break;
      case "tf_http_enable": responsePayload = { serverRunning: false, authorized: false, session: 0, port: 8080, expiresInSeconds: 0, url: "", lastError: "Mock 自动使用串口通道" }; break;
      case "tf_http_status": responsePayload = { serverRunning: false, authorized: false, session: 0, port: 8080, expiresInSeconds: 0, url: "" }; break;
      case "tf_http_disable": responsePayload = { serverRunning: false, authorized: false, session: 0, port: 8080, expiresInSeconds: 0, url: "" }; break;
      case "tf_export_diagnostics": responsePayload = { path: ".fridge_spirit/exports/diagnostics-mock.json", bytes: 512 }; break;
      case "format_tfcard":
        this.tfFiles = new Map([...this.tfFiles].filter(([key]) => key === ".fridge_spirit" || key.startsWith(".fridge_spirit/"))); responsePayload = { mounted: true, formattedThisBoot: true }; break;
      case "get_pins":
        responsePayload = createMockPins();
        break;
      case "get_sensors":
        responsePayload = {
          ...createMockSensors(),
          ...this.buildMq5Snapshot(),
          stateMachine: this.createStateMachineStatus(),
        };
        break;
      case "mq5_confirm_burn_in": {
        const confirm = (payload as { confirm?: string } | undefined)?.confirm;
        if (confirm !== "MQ5_BURN_IN_COMPLETE") {
          throw new Error("confirm=MQ5_BURN_IN_COMPLETE 是必需的，拒绝执行首次老化确认");
        }
        if (!this.mq5.mq5DailyWarmupComplete) {
          throw new Error("日常预热尚未完成，无法确认首次老化");
        }
        if (this.mq5.mq5BurnInConfirmed) {
          throw new Error("首次老化已确认，无需重复确认");
        }
        this.mq5 = {
          ...this.mq5,
          mq5BurnInConfirmed: true,
          mq5State: this.mq5.mq5BaselineReady ? "monitoring" : "ready_to_calibrate",
        };
        this.emitLog("info", "MQ-5 首次老化确认完成。", "sensors");
        responsePayload = this.buildMq5Snapshot();
        break;
      }
      case "mq5_capture_baseline": {
        const confirm = (payload as { confirm?: string } | undefined)?.confirm;
        if (confirm !== "CAPTURE_MQ5_CLEAN_AIR") {
          throw new Error("confirm=CAPTURE_MQ5_CLEAN_AIR 是必需的，拒绝采集洁净空气基线");
        }
        if (!this.mq5.mq5DailyWarmupComplete || !this.mq5.mq5BurnInConfirmed ||
            (this.mq5.mq5SampleCount ?? 0) < 64) {
          throw new Error("预热/老化/64 样本窗口前置未满足，无法采集洁净空气基线");
        }
        const raw12bit = this.mq5.mq5Raw12bit ?? 0;
        this.mq5 = {
          ...this.mq5,
          mq5BaselineRaw12bit: raw12bit,
          mq5BaselineReady: true,
          mq5DeltaFromBaseline: 0,
          mq5State: "monitoring",
        };
        this.emitLog("info", "MQ-5 洁净空气基线已采集（相对 ADC，非 ppm）。", "sensors");
        responsePayload = this.buildMq5Snapshot();
        break;
      }
      case "mq5_reset_calibration": {
        const confirm = (payload as { confirm?: string } | undefined)?.confirm;
        if (confirm !== "RESET_MQ5_CALIBRATION") {
          throw new Error("confirm=RESET_MQ5_CALIBRATION 是必需的，拒绝重置老化与基线");
        }
        if (!this.mq5.mq5BurnInConfirmed && !this.mq5.mq5BaselineReady) {
          throw new Error("尚无老化确认或基线标定，无需重置校准");
        }
        this.mq5 = {
          ...this.mq5,
          mq5BurnInConfirmed: false,
          mq5BaselineReady: false,
          mq5BaselineRaw12bit: 0,
          mq5DeltaFromBaseline: 0,
          mq5SampleCount: 0,
          mq5State: "needs_burn_in",
        };
        this.emitLog("warn", "MQ-5 老化与基线已重置，需重新完成首次老化。", "sensors");
        // 复位后返回当前快照（样本计数保持 0，与固件 reset 后立即读快照一致），
        // 下一次 get_sensors 起重新累计样本，直到满 64 才能再次采集基线。
        responsePayload = { ...this.mq5 };
        break;
      }
      case "get_diagnostics":
        responsePayload = createMockDiagnostics();
        break;
      case "get_logs":
        responsePayload = createMockLogs();
        break;
      case "ui_status":
        responsePayload = {
          ready: true,
          resourceReady: true,
          framebuffersSynced: true,
          pageFrameReady: true,
          page: this.uiPage,
          orientation: this.uiOrientation,
          privacyMode: this.uiOrientation === "portrait_privacy",
        } satisfies UiStatus;
        break;
      case "tool_call": {
        const call = (payload ?? {}) as {
          tool?: string;
          arguments?: Record<string, unknown>;
        };
        if (call.tool === "ui.switch_page") {
          const page = String(call.arguments?.page ?? "");
          if (!page) throw new Error("ui.switch_page 需要 page");
          this.uiPage = page;
          if (!portraitUiPages.has(page)) this.uiOrientation = "landscape";
        } else if (call.tool === "ui.set_orientation") {
          const orientation = call.arguments?.orientation;
          if (orientation !== "landscape" && orientation !== "portrait_privacy") {
            throw new Error("ui.set_orientation 方向无效");
          }
          if (
            orientation === "portrait_privacy" &&
            !portraitUiPages.has(this.uiPage)
          ) {
            throw new Error("该页面尚未移植竖屏，请先进入首页、计划、对话、更多或设置类页面");
          }
          this.uiOrientation = orientation;
        } else {
          throw new Error(`Mock 不支持 Tool：${call.tool || "(empty)"}`);
        }
        responsePayload = {
          tool: call.tool,
          confirmed: false,
          status: "ok",
          code: "ok",
          summary: "Mock UI 请求已入队",
          latencyMs: 1,
          data: { queued: true },
        };
        break;
      }
      case "get_realtime_config":
        responsePayload = {
          ...this.realtimeConfig,
          hasApiKey: this.realtimeHasKey,
          ready: this.realtimeHasKey,
          apiKeyPreview: this.realtimeHasKey ? "sk-m...demo" : "",
        };
        break;
      case "set_realtime_config": {
        const body = (payload ?? {}) as Record<string, unknown>;
        if (typeof body.url === "string" && body.url) this.realtimeConfig.url = body.url;
        if (typeof body.model === "string" && body.model) this.realtimeConfig.model = body.model;
        if (typeof body.voice === "string" && body.voice) this.realtimeConfig.voice = body.voice;
        if (typeof body.instructions === "string" && body.instructions) {
          this.realtimeConfig.instructions = body.instructions;
        }
        if (typeof body.timeoutMs === "number" && body.timeoutMs > 0) {
          this.realtimeConfig.timeoutMs = body.timeoutMs;
        }
        if (typeof body.enabled === "boolean") this.realtimeConfig.enabled = body.enabled;
        if (typeof body.apiKey === "string" && body.apiKey) {
          this.realtimeHasKey = true;
          this.realtimeConfig.apiKeyPreview = "sk-m...demo";
        }
        this.realtimeConfig.hasApiKey = this.realtimeHasKey;
        this.realtimeConfig.ready = this.realtimeHasKey;
        this.realtimeStatus.model = this.realtimeConfig.model;
        this.realtimeStatus.voice = this.realtimeConfig.voice;
        responsePayload = { ...this.realtimeConfig };
        break;
      }
      case "clear_realtime_key":
        this.realtimeHasKey = false;
        this.realtimeConfig.hasApiKey = false;
        this.realtimeConfig.ready = false;
        this.realtimeConfig.apiKeyPreview = "";
        responsePayload = { ...this.realtimeConfig };
        break;
      case "realtime_status":
        responsePayload = { ...this.realtimeStatus };
        break;
      case "realtime_start":
        if (!this.realtimeHasKey) throw new Error("Mock Realtime 缺少 API Key");
        this.realtimeResponseGeneration += 1;
        const sessionGeneration = ++this.realtimeSessionGeneration;
        this.realtimeStatus.sessionActive = true;
        this.realtimeStatus.state = "listening";
        this.realtimeStatus.audioUpActive = true;
        this.realtimeStatus.toolsExported = 10;
        window.setTimeout(() => {
          if (!this.connected ||
              !this.realtimeStatus.sessionActive ||
              sessionGeneration !== this.realtimeSessionGeneration) return;
          this.emitMessage({
            type: "event",
            event: "realtime_state",
            payload: { state: "session_ready", summary: "mock session ready" },
          });
          this.emitMessage({
            type: "event",
            event: "realtime_state",
            payload: { state: "listening", summary: "mock listening" },
          });
        }, 200);
        responsePayload = { accepted: true };
        break;
      case "realtime_stop":
        this.realtimeSessionGeneration += 1;
        this.realtimeResponseGeneration += 1;
        this.realtimeStatus.sessionActive = false;
        this.realtimeStatus.state = "idle";
        this.realtimeStatus.audioUpActive = false;
        this.realtimeStatus.audioDownActive = false;
        responsePayload = { accepted: true };
        break;
      case "realtime_cancel":
        this.realtimeResponseGeneration += 1;
        this.realtimeStatus.audioDownActive = false;
        this.realtimeStatus.state = this.realtimeStatus.sessionActive ? "listening" : "idle";
        responsePayload = { accepted: true };
        break;
      case "realtime_send_text": {
        const text =
          typeof (payload as { text?: string } | undefined)?.text === "string"
            ? (payload as { text: string }).text
            : "还有什么食材？";
        if (!this.realtimeStatus.sessionActive) throw new Error("session not ready");
        const responseGeneration = ++this.realtimeResponseGeneration;
        this.realtimeStatus.state = "tool_call";
        window.setTimeout(() => {
          if (!this.connected ||
              !this.realtimeStatus.sessionActive ||
              responseGeneration !== this.realtimeResponseGeneration) return;
          this.emitMessage({
            type: "event",
            event: "realtime_state",
            payload: { state: "responding", summary: "response.created" },
          });
          this.emitMessage({
            type: "event",
            event: "realtime_tool_call",
            payload: {
              tool: "inventory.list",
              callId: "mock_1",
              arguments: "{}",
            },
          });
          this.realtimeStatus.toolCallsOk += 1;
          this.emitMessage({
            type: "event",
            event: "realtime_tool_result",
            payload: {
              tool: "inventory.list",
              callId: "mock_1",
              status: "ok",
              summary: "已读取库存",
              latencyMs: 12,
            },
          });
          this.emitMessage({
            type: "event",
            event: "realtime_transcript",
            payload: {
              role: "assistant",
              text: `Mock 回复：收到「${text}」。`,
              isFinal: false,
            },
          });
          this.emitMessage({
            type: "event",
            event: "realtime_transcript",
            payload: {
              role: "assistant",
              text: `当前模拟库存有番茄和牛奶。`,
              isFinal: false,
            },
          });
          this.emitMessage({
            type: "event",
            event: "realtime_transcript",
            payload: {
              role: "assistant",
              text: `Mock 回复：收到「${text}」。当前模拟库存有番茄和牛奶。`,
              isFinal: true,
            },
          });
          this.emitMessage({
            type: "event",
            event: "realtime_done",
            payload: { status: "response.done" },
          });
          this.realtimeStatus.state = "listening";
        }, 350);
        responsePayload = { accepted: true };
        break;
      }
      default:
        responsePayload = {};
    }

    const response: DeviceResponse<TPayload> = {
      type: "response",
      request_id: requestId,
      ok: true,
      command,
      payload: responsePayload as TPayload,
    };
    this.emitMessage(response);
    return response;
  }

  private getAiProfilesPayload(): AIProfilesResponse {
    return {
      activeProfileId: this.aiConfig.profileId ?? 0,
      profiles: this.aiProfiles,
    };
  }

  private startMockWakeEvents() {
    if (this.wakeTimer) {
      return;
    }
    this.wakeTimer = window.setInterval(() => {
      if (!this.connected || !this.wakeStatus.enabled) {
        return;
      }
      const elapsedMs = Date.now() - this.connectedAt;
      this.wakeStatus = createMockWakeStatus({
        ...this.wakeStatus,
        enabled: true,
        state: "listening",
        triggerCount: this.wakeStatus.triggerCount + 1,
        lastTriggerMs: elapsedMs,
        vadState: 1,
        rms: 420 + Math.round(Math.random() * 220),
        peakAbs: 2600 + Math.round(Math.random() * 1800),
      });
      this.emitMessage({
        type: "event",
        event: "wake_word_detected",
        payload: this.wakeStatus,
      });
      const triggerId = this.wakeStatus.triggerCount;
      this.emitMessage({
        type: "event",
        event: "wake_asr_state",
        payload: { state: "recording", triggerId },
      });
      window.setTimeout(() => {
        if (!this.connected || !this.wakeStatus.enabled) {
          return;
        }
        this.emitMessage({
          type: "event",
          event: "wake_asr_state",
          payload: { state: "transcribing", triggerId, audioBytes: 160044 },
        });
      }, 650);
      window.setTimeout(() => {
        if (!this.connected || !this.wakeStatus.enabled) {
          return;
        }
        this.emitMessage({
          type: "event",
          event: "wake_asr_state",
          payload: {
            state: "done",
            triggerId,
            text: "小冰小冰，帮我看看冰箱里的食材",
            model: this.asrConfig.model,
            httpStatus: 200,
            latencyMs: 720,
            audioBytes: 160044,
          },
        });
      }, 1450);
    }, 5200);
  }

  private stopMockWakeEvents() {
    if (this.wakeTimer) {
      window.clearInterval(this.wakeTimer);
      this.wakeTimer = undefined;
    }
  }

  private normalizeKitchenAiPage(page: unknown): KitchenAIPage {
    return page === "recipe" || page === "nutrition" || page === "shopping" || page === "all" ? page : "all";
  }

  private createTodayKey() {
    const now = new Date();
    return (now.getFullYear() * 10000) + ((now.getMonth() + 1) * 100) + now.getDate();
  }

  private getKitchenToolsSnapshot(): KitchenToolsStatus {
    const now = Date.now();
    const timer = { ...this.kitchenTools.timer };
    if (timer.state === "running") {
      timer.remainingSeconds = Math.max(0, Math.ceil((this.kitchenTimerEndsAt - now) / 1000));
      if (timer.remainingSeconds <= 0) {
        timer.state = "ringing";
        this.kitchenTools.lastAlert = `${timer.label || "厨房"}定时器到了`;
        this.kitchenTools.timer = timer;
      }
    }
    const stopwatch = { ...this.kitchenTools.stopwatch };
    if (stopwatch.state === "running") {
      stopwatch.elapsedSeconds = Math.max(0, Math.floor((now - this.stopwatchStartedAt) / 1000) + this.kitchenTools.stopwatch.elapsedSeconds);
    }
    return {
      ...this.kitchenTools,
      timer,
      stopwatch,
      alarms: this.kitchenTools.alarms.map((alarm) => ({ ...alarm })),
    };
  }

  private startKitchenTimer(seconds: number, label: string) {
    const now = Date.now();
    this.kitchenTimerStartedAt = now;
    this.kitchenTimerEndsAt = now + seconds * 1000;
    this.kitchenTools.timer = {
      state: "running",
      durationSeconds: seconds,
      remainingSeconds: seconds,
      label,
    };
    this.kitchenTools.lastError = "";
    this.emitLog("info", `Mock 定时器已启动：${label} ${seconds}s`, "kitchen_tools");
  }

  private pauseKitchenTimer() {
    const snap = this.getKitchenToolsSnapshot();
    if (snap.timer.state !== "running") {
      throw new Error("定时器未在运行");
    }
    this.kitchenTools.timer = { ...snap.timer, state: "paused" };
  }

  private resumeKitchenTimer() {
    if (this.kitchenTools.timer.state !== "paused" || this.kitchenTools.timer.remainingSeconds <= 0) {
      throw new Error("定时器不能继续");
    }
    const now = Date.now();
    this.kitchenTimerStartedAt = now;
    this.kitchenTimerEndsAt = now + this.kitchenTools.timer.remainingSeconds * 1000;
    this.kitchenTools.timer = { ...this.kitchenTools.timer, state: "running" };
  }

  private startStopwatch() {
    if (this.kitchenTools.stopwatch.state !== "running") {
      this.stopwatchStartedAt = Date.now();
      this.kitchenTools.stopwatch = { ...this.kitchenTools.stopwatch, state: "running" };
    }
  }

  private pauseStopwatch() {
    const snap = this.getKitchenToolsSnapshot();
    if (snap.stopwatch.state !== "running") {
      throw new Error("秒表未在运行");
    }
    this.kitchenTools.stopwatch = { ...snap.stopwatch, state: "paused" };
  }

  private tryExecuteKitchenToolFromText(text: string) {
    if (/定时器|分钟|秒钟/.test(text)) {
      const minuteMatch = text.match(/(\d+)\s*分钟/);
      const secondMatch = text.match(/(\d+)\s*秒/);
      const seconds = (minuteMatch ? Number(minuteMatch[1]) * 60 : 0) + (secondMatch ? Number(secondMatch[1]) : 0);
      if (seconds > 0) {
        const label = text.includes("煮蛋") ? "煮蛋" : "厨房";
        this.startKitchenTimer(seconds, label);
        return { message: `已启动${label}定时器，${Math.floor(seconds / 60)}分钟${String(seconds % 60).padStart(2, "0")}秒` };
      }
    }
    if (/开始秒表/.test(text)) {
      this.startStopwatch();
      return { message: "秒表已开始" };
    }
    if (/暂停秒表/.test(text)) {
      this.pauseStopwatch();
      return { message: "秒表已暂停" };
    }
    if (/重置秒表|清零秒表/.test(text)) {
      this.kitchenTools.stopwatch = { state: "idle", elapsedSeconds: 0 };
      this.stopwatchStartedAt = 0;
      return { message: "秒表已重置" };
    }
    return null;
  }

  private createMockCameraPreview() {
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240">
      <rect width="320" height="240" fill="#f5faf9"/>
      <rect x="18" y="24" width="284" height="192" rx="8" fill="#dfeceb" stroke="#9db8ba"/>
      <circle cx="110" cy="126" r="42" fill="#d9473f"/>
      <circle cx="118" cy="116" r="10" fill="#ef7267"/>
      <ellipse cx="206" cy="126" rx="46" ry="34" fill="#f1d073"/>
      <circle cx="206" cy="126" r="18" fill="#fff4c7"/>
      <text x="22" y="34" font-size="13" fill="#173236">Mock OV3660 QVGA JPEG</text>
    </svg>`;
    return `data:image/svg+xml;base64,${window.btoa(unescape(encodeURIComponent(svg)))}`;
  }

  private normalizeProjectAiRequest(payload: unknown): ProjectAITaskRequest {
    const request = payload as Partial<ProjectAITaskRequest> | undefined;
    return {
      taskType: request?.taskType ?? "chat_assist",
      userText: request?.userText ?? "",
      includeInventory: request?.includeInventory ?? true,
      includeReminders: request?.includeReminders ?? true,
      includePreferences: request?.includePreferences ?? true,
    };
  }

  private normalizeAssistantRequest(payload: unknown): AIAssistantChatRequest {
    const request = payload as Partial<AIAssistantChatRequest> | undefined;
    const base = this.normalizeProjectAiRequest(payload);
    return {
      ...base,
      message: request?.message ?? request?.userText ?? "",
      userText: request?.userText ?? request?.message ?? "",
    };
  }

  private appendMockChatHistory(request: AIAssistantChatRequest, reply: string) {
    const now = Math.floor(Date.now() / 1000);
    const cutoff = now - this.chatHistory.ttlSeconds;
    const merged = [
      ...this.chatHistory.messages,
      {
        id: crypto.randomUUID(),
        role: "user" as const,
        content: request.message.slice(0, 512),
        taskType: request.taskType,
        createdAt: now,
      },
      {
        id: crypto.randomUUID(),
        role: "assistant" as const,
        content: reply.slice(0, 2048),
        taskType: request.taskType,
        createdAt: now,
      },
    ]
      .filter((item) => item.createdAt >= cutoff)
      .slice(-this.chatHistory.maxMessages);
    const prunedCount = Math.max(0, this.chatHistory.messages.length + 2 - merged.length);
    this.chatHistory = {
      ...this.chatHistory,
      updatedAt: now,
      count: merged.length,
      prunedCount,
      messages: merged,
    };
    return prunedCount;
  }

  private createAssistantReply(request: AIAssistantChatRequest) {
    const contextParts = [
      request.includeInventory ? "库存" : "",
      request.includeReminders ? "临期提醒" : "",
      request.includePreferences ? "偏好" : "",
      "长期记忆",
    ].filter(Boolean);
    if (request.taskType === "recipe_generate") {
      return `Mock 真实助手：我已注入${contextParts.join("、") || "无额外上下文"}。按当前测试库存，建议先处理牛奶和番茄，可以做番茄鸡蛋汤；如果牛奶有异味、胀包或冷链异常，请不要食用。`;
    }
    if (request.taskType === "shopping_list_generate") {
      return `Mock 真实助手：根据已注入的${contextParts.join("、") || "上下文"}，建议购买绿叶菜和主食，葱花、酸奶属于可选补充，避免一次买太多。`;
    }
    return `Mock 真实助手：已收到“${request.message.slice(0, 80)}”。我会优先依据项目上下文回答，不会编造库存或把结果直接入库。`;
  }

  private createContextPreview(request: ProjectAITaskRequest): AIContextPreview {
    return {
      taskType: request.taskType,
      localSnapshotVersion: 1,
      needsConfirmation: request.taskType !== "chat_assist",
      context: {
        schema_version: 1,
        task_type: request.taskType,
        request_id: "mock_preview",
        persona_template: "你是冰箱小精灵的智能厨房助手。回答要实用、保守、可确认、少打扰。",
        task_template: "按任务类型输出结构化 JSON，AI 结果不能直接入库。",
        user_text: request.userText,
        dynamic_context: {
          inventory: request.includeInventory ? { items: [{ name: "牛奶", days_left: 1, location: "门架中层" }, { name: "番茄", days_left: 2, location: "冷藏主仓 B2" }] } : null,
          reminders: request.includeReminders ? { reminders: [{ name: "牛奶", suggestion: "今天优先处理" }] } : null,
          preferences: request.includePreferences ? { people: 2, taste: ["清淡", "少油"], avoid: ["香菜"] } : null,
          user_memory: {
            schema_version: 1,
            kind: "user_memory",
            items: [{ id: "mock-memory-1", type: "preference", text: "偏好清淡少油" }],
          },
          offline_queue: { pending_count: 0 },
          conversation_history: this.chatHistory,
        },
        output_policy: {
          ai_result_must_be_confirmed: true,
          do_not_fabricate_inventory: true,
          do_not_store_full_chat: true,
        },
      },
    };
  }

  private createProjectAiResult(request: ProjectAITaskRequest): ProjectAITaskResponse {
    const base = {
      taskType: request.taskType,
      confidence: 82,
      needsConfirmation: request.taskType !== "chat_assist",
      safetyNote: "Mock 结果仅验证上下文注入和结构化输出，不会直接写入库存。",
    };

    if (request.taskType === "recipe_generate") {
      return {
        ...base,
        needsConfirmation: false,
        result: {
          schema_version: 1,
          type: "recipe_generate",
          recipe: {
            name: "番茄鸡蛋汤",
            use_inventory: ["番茄", "鸡蛋"],
            missing: ["葱花，可选"],
            time_minutes: 15,
            steps: ["番茄切块，鸡蛋打散", "少油炒番茄出汁", "加水煮开后淋入蛋液", "按口味少量加盐"],
          },
        },
      };
    }

    if (request.taskType === "shopping_list_generate") {
      return {
        ...base,
        result: {
          schema_version: 1,
          type: "shopping_list_generate",
          suggested: ["绿叶菜", "面条"],
          optional: ["葱花", "低脂酸奶"],
          basis: "根据当前库存和快手晚餐偏好生成，避免过量购买",
        },
      };
    }

    if (request.taskType === "recognize_ingredients") {
      return {
        ...base,
        result: {
          schema_version: 1,
          type: "recognize_ingredients",
          candidates: [{ name: "番茄", quantity: "约2-3个", confidence: 0.82, doubt: "Mock 未接入真实图片" }],
          needs_confirmation: true,
          confirm_fields: ["名称", "数量", "保质期", "位置"],
        },
      };
    }

    return {
      ...base,
      needsConfirmation: request.taskType !== "chat_assist",
      result: {
        schema_version: 1,
        type: request.taskType,
        reply: "我会优先依据当前库存、临期提醒和你的偏好给出建议；信息不足时会请你确认，不会编造库存。",
      },
    };
  }
}
