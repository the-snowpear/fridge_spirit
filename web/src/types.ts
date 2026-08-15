import type { LucideIcon } from "lucide-react";

export type TransportMode = "mock" | "serial";
export type ConnectionState = "disconnected" | "connecting" | "connected";
export type HealthState = "ok" | "warn" | "danger" | "offline";
export type SafetyLevel = "safe" | "caution" | "danger";
export type LogLevel = "debug" | "info" | "warn" | "error";

export interface DeviceRequest {
  type: "request";
  request_id: string;
  command: DeviceCommand;
  payload?: unknown;
  tool?: string;
  arguments?: Record<string, unknown>;
}

export interface DeviceResponse<TPayload = unknown> {
  type: "response";
  request_id: string;
  ok: boolean;
  command: DeviceCommand;
  payload: TPayload;
  error?: string;
}

export interface DeviceEvent<TPayload = unknown> {
  type: "event";
  event: string;
  payload: TPayload;
}

export type DeviceMessage = DeviceResponse | DeviceEvent;

export type DeviceCommand =
  | "get_status"
  | "get_network"
  | "scan_wifi"
  | "set_network"
  | "get_mqtt_config"
  | "get_mqtt_status"
  | "set_mqtt_config"
  | "clear_mqtt_secret"
  | "mqtt_publish_state"
  | "get_ai_config"
  | "get_ai_profiles"
  | "set_ai_config"
  | "clear_ai_key"
  | "get_asr_config"
  | "set_asr_config"
  | "clear_asr_key"
  | "asr_transcribe"
  | "get_tts_config"
  | "set_tts_config"
  | "clear_tts_key"
  | "get_state_machine_config"
  | "set_state_machine_config"
  | "get_state_machine_status"
  | "create_ai_profile"
  | "select_ai_profile"
  | "delete_ai_profile"
  | "test_ai_chat"
  | "agent_submit"
  | "agent_run_status"
  | "agent_cancel"
  | "agent_confirm"
  | "agent_reject"
  | "agent_session_read"
  | "agent_session_clear"
  | "ai_assistant_chat"
  | "get_kitchen_ai_status"
  | "kitchen_ai_generate"
  | "get_kitchen_tools"
  | "timer_start"
  | "timer_pause"
  | "timer_resume"
  | "timer_cancel"
  | "stopwatch_start"
  | "stopwatch_pause"
  | "stopwatch_reset"
  | "alarm_set"
  | "alarm_cancel"
  | "alarm_dismiss"
  | "wake_start"
  | "wake_stop"
  | "wake_status"
  | "wake_set_enabled"
  | "wake_reset_stats"
  | "mic_record_start"
  | "mic_record_status"
  | "mic_record_stop"
  | "mic_record_wav"
  | "mic_record_wav_info"
  | "mic_record_wav_chunk"
  | "mic_record_wav_end"
  | "voice_chat_start"
  | "voice_chat_stop"
  | "voice_chat_status"
  | "tts_play"
  | "tts_status"
  | "tts_stop"
  | "speaker_beep"
  | "radar_test_start"
  | "radar_test_status"
  | "radar_test_stop"
  | "get_camera_status"
  | "camera_probe"
  | "camera_capture"
  | "camera_jpeg_diag"
  | "camera_rgb565_diag"
  | "camera_analyze"
  | "clear_camera_frame"
  | "camera_reset"
  | "get_ai_context_preview"
  | "test_ai_task"
  | "get_chat_history"
  | "clear_chat_history"
  | "get_pins"
  | "get_sensors"
  | "get_diagnostics"
  | "get_logs"
  | "tool_call"
  | "ui_status"
  | "screenshot_capture"
  | "get_tfcard_status"
  | "tf_list"
  | "tf_stat"
  | "tf_mkdir"
  | "tf_move"
  | "tf_delete"
  | "tf_restore"
  | "tf_trash_empty"
  | "tf_text_read"
  | "tf_text_write"
  | "tf_transfer_begin"
  | "tf_transfer_status"
  | "tf_transfer_chunk"
  | "tf_transfer_end"
  | "tf_transfer_abort"
  | "tf_mount"
  | "tf_unmount"
  | "tf_health_test"
  | "tf_benchmark"
  | "tf_get_access_mode"
  | "tf_set_access_mode"
  | "tf_http_enable"
  | "tf_http_status"
  | "tf_http_disable"
  | "tf_export_diagnostics"
  | "format_tfcard"
  | "tfcard_selftest"
  | "get_realtime_config"
  | "set_realtime_config"
  | "clear_realtime_key"
  | "realtime_start"
  | "realtime_stop"
  | "realtime_status"
  | "realtime_send_text"
  | "realtime_cancel"
  | "mq5_confirm_burn_in"
  | "mq5_capture_baseline"
  | "mq5_reset_calibration";

/** MQ-5 燃气模块运行状态（与固件 fridge_mq5_state_t 一一对应；unknown 为防御性 fallback） */
export type Mq5State =
  | "disabled"
  | "error"
  | "warming_up"
  | "needs_burn_in"
  | "ready_to_calibrate"
  | "monitoring"
  | "unknown";

/** MQ-5 校准操作命令（均需携带精确 confirm 口令） */
export type Mq5Command = "mq5_confirm_burn_in" | "mq5_capture_baseline" | "mq5_reset_calibration";

/** 智谱 GLM Realtime 配置视图（设备端 NVS；不含明文 Key） */
export interface RealtimeConfig {
  url: string;
  model: string;
  voice: string;
  instructions: string;
  vadMode: string;
  timeoutMs: number;
  enabled: boolean;
  hasApiKey: boolean;
  ready: boolean;
  apiKeyPreview: string;
  lastError: string;
}

export interface RealtimeStatus {
  ready: boolean;
  sessionActive: boolean;
  networkOk: boolean;
  internetReady: boolean;
  audioUpActive: boolean;
  audioDownActive: boolean;
  toolsReady: boolean;
  state: string;
  model: string;
  voice: string;
  lastError: string;
  toolsExported: number;
  audioUpBytes: number;
  audioDownBytes: number;
  toolCallsOk: number;
  toolCallsFail: number;
  lastEventAgeMs: number;
}

export interface RealtimeEventPayload {
  state?: string;
  role?: string;
  text?: string;
  tool?: string;
  callId?: string;
  arguments?: string;
  summary?: string;
  code?: string;
  status?: string;
  isFinal?: boolean;
  latencyMs?: number;
}

export interface TfCardStatus {
  mounted: boolean;
  filesystem?: string;
  mountPoint?: string;
  capacityMb?: number;
  freeMb?: number;
  totalBytes?: number;
  freeBytes?: number;
  usedBytes?: number;
  writable?: boolean;
  formatRequired?: boolean;
  formattedThisBoot?: boolean;
  expertMode?: boolean;
  transferActive?: boolean;
  cardName?: string;
  lastError?: string;
}

export interface TfFileEntry {
  name: string;
  path: string;
  type: "file" | "directory";
  size: number;
  modifiedAt?: number | string;
  mtime?: number;
  isDir?: boolean;
  hidden?: boolean;
  protected?: boolean;
  originalPath?: string;
}

export interface TfListResponse {
  path: string;
  entries: TfFileEntry[];
  cursor?: number;
  nextCursor?: number;
  hasMore: boolean;
}

export interface TfAccessMode {
  expertMode: boolean;
  protectedRoot?: string;
}

export interface TfTransferSession {
  session: number;
  active: boolean;
  direction: "read" | "write";
  path: string;
  offset: number;
  totalSize: number;
  chunkSize: number;
}

export interface TfHttpSession {
  serverRunning: boolean;
  authorized: boolean;
  session: number;
  port: number;
  expiresInSeconds: number;
  url: string;
  token?: string;
  lastError?: string;
}

export interface DeviceStatus {
  model: string;
  chip: string;
  firmware: string;
  uptime: string;
  flash: string;
  psram: string;
  freeHeapKb: number;
  minHeapKb: number;
  freePsramKb: number;
  temperatureC: number | null;
  wifi: HealthState;
  mqtt: HealthState;
  usb: HealthState;
  ota: HealthState;
  page: string;
  powerNote: string;
  tasks: TaskStatus[];
}

export interface TaskStatus {
  name: string;
  priority: string;
  state: string;
  heartbeat: string;
}

export interface NetworkConfig {
  ssid: string;
  wifiPassword?: string;
  mqttHost: string;
  apiBaseUrl: string;
  ntpServer: string;
  save?: boolean;
  saveAiKey?: false;
  connected?: boolean;
  connecting?: boolean;
  saved?: boolean;
  internet?: boolean;
  hostedReady?: boolean;
  c6OtaGuard?: boolean;
  hostedHostVersion?: string;
  hostedSlaveVersion?: string;
  hostedLastError?: string;
  status?: string;
  ip?: string;
  rssi?: number;
  lastError?: string;
}

export interface MQTTConfig {
  brokerUri: string;
  homeId: string;
  deviceId: string;
  username: string;
  password?: string;
  token?: string;
  keepaliveSeconds?: number;
  hasPassword: boolean;
  enabled: boolean;
  configured: boolean;
  connected: boolean;
  reconnectCount: number;
  publishedCount: number;
  receivedCount: number;
  lastError: number;
  statusText: string;
}

export interface AIConfig {
  profileId?: number;
  profileName?: string;
  apiBaseUrl: string;
  apiKey?: string;
  model: string;
  systemPrompt: string;
  timeoutMs: number;
  apiFormat?: number; // 接口格式：0=OpenAI 兼容，1=Google Gemini 原生
  hasApiKey: boolean;
  apiKeyPreview: string;
  lastError: string;
  ready: boolean;
}

export interface ASRConfig {
  apiBaseUrl: string;
  apiKey?: string;
  model: string;
  timeoutMs: number;
  hasApiKey: boolean;
  apiKeyPreview: string;
  lastError: string;
  ready: boolean;
}

export interface TTSConfig {
  apiBaseUrl: string;
  apiKey?: string;
  model: string;
  voice: string;
  timeoutMs: number;
  hasApiKey: boolean;
  apiKeyPreview: string;
  lastError: string;
  ready: boolean;
}

export type MicSource = "es8311" | "fy_sp703";

export interface VoiceChatStatus {
  state: "idle" | "recording" | "wake_listening" | "ready" | "error";
  source?: MicSource;
  nativeSampleRate?: number;
  nativeBitsPerSample?: number;
  nativeChannels?: number;
  selectedChannel?: "left" | "right" | "mix" | string;
  leftRms?: number;
  rightRms?: number;
  overflowCount?: number;
  durationMs: number;
  pcmBytes: number;
  rms: number;
  sampleCount?: number;
  peakAbs?: number;
  minSample?: number;
  maxSample?: number;
  meanSample?: number;
  clipCount?: number;
  timeoutCount?: number;
  qualityHint?: "ok" | "silent" | "clipping" | "i2s_timeout" | "too_short" | string;
  error: string;
}

export interface WakeStatus {
  enabled: boolean;
  state: "idle" | "listening" | "error" | string;
  wakeWord: string;
  model: string;
  audioSource?: MicSource;
  triggerCount: number;
  lastTriggerMs: number;
  vadState: number;
  rms: number;
  peakAbs: number;
  timeoutCount: number;
  error: string;
}

export type WakeWordDetectedEventPayload = WakeStatus;

export interface WakeAsrStateEventPayload {
  state: "recording" | "transcribing" | "done" | "error";
  triggerId: number;
  text?: string;
  model?: string;
  httpStatus?: number;
  latencyMs?: number;
  audioBytes?: number;
  error?: string;
}

export interface AsrTranscribeResponse {
  text: string;
  model: string;
  httpStatus: number;
  latencyMs: number;
  audioBytes: number;
}

export interface VoiceChatResponse {
  transcript: string;
  reply: string;
  asrModel: string;
  aiModel: string;
  asrLatencyMs: number;
  aiLatencyMs: number;
  asrHttpStatus: number;
  aiHttpStatus: number;
  audioBytes: number;
  historyPrunedCount: number;
}

export interface MicRecordWavResponse {
  source?: MicSource;
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  durationMs: number;
  pcmBytes: number;
  wavBytes: number;
  wavBase64: string;
}

export interface MicRecordWavInfoResponse {
  source?: MicSource;
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  durationMs: number;
  pcmBytes: number;
  wavBytes: number;
  chunkBytes: number;
  session: number;
  fullCrc32: number;
}

export interface MicRecordWavChunkResponse {
  session: number;
  offset: number;
  length: number;
  crc32: number;
  eof: boolean;
  data_b64: string;
}

export interface TTSStatus {
  state: "idle" | "synthesizing" | "playing" | "done" | "error";
  sampleRate: number;
  audioBytes: number;
  playedBytes: number;
  durationMs: number;
  latencyMs: number;
  httpStatus: number;
  model: string;
  voice: string;
  error: string;
}

export type KitchenTimerState = "idle" | "running" | "paused" | "ringing";
export type KitchenStopwatchState = "idle" | "running" | "paused";

export interface KitchenTimerStatus {
  state: KitchenTimerState | string;
  durationSeconds: number;
  remainingSeconds: number;
  label: string;
}

export interface KitchenStopwatchStatus {
  state: KitchenStopwatchState | string;
  elapsedSeconds: number;
}

export interface KitchenAlarmStatus {
  id: number;
  enabled: boolean;
  ringing: boolean;
  hour: number;
  minute: number;
  label: string;
}

export interface KitchenToolsStatus {
  timeReady: boolean;
  timer: KitchenTimerStatus;
  stopwatch: KitchenStopwatchStatus;
  alarms: KitchenAlarmStatus[];
  lastAlert: string;
  lastError: string;
}

export type KitchenAIPage = "all" | "recipe" | "nutrition" | "shopping";

export interface KitchenAIGenerateRequest {
  page: KitchenAIPage;
}

export interface KitchenAIStatus {
  running: boolean;
  lastOk: boolean;
  state: "idle" | "running" | "ok" | "error" | string;
  lastTrigger: "auto" | "manual" | string;
  currentPage: KitchenAIPage | string;
  lastPage: KitchenAIPage | string;
  lastGeneratedAt: number;
  lastAutoDay: number;
  httpStatus: number;
  latencyMs: number;
  lastReplyLen?: number;
  lastError: string;
  lastReplyPreview?: string;
}

export interface CameraStatus {
  initialized: boolean;
  hasFrame: boolean;
  width: number;
  height: number;
  jpegBytes: number;
  captureMs: number;
  frameId: number;
  freeHeapKb: number;
  freePsramKb: number;
  pixelFormat: string;
  frameSize: string;
  lastError: string;
}

export interface CameraProbeResponse {
  ok: boolean;
  xclkEnabled: boolean;
  sccbReady: boolean;
  address: number;
  pidHigh: number;
  pidLow: number;
  pid: number;
  expectedPid: number;
  durationMs: number;
  espErr: number;
  espErrName: string;
  lastError: string;
}

export interface CameraCaptureResponse {
  status: CameraStatus;
  previewDataUrl: string | null;
  previewOmitted: boolean;
  previewMaxBytes: number;
}

export interface CameraRgb565DiagResponse {
  ok: boolean;
  width: number;
  height: number;
  bytes: number;
  captureMs: number;
  checksum: number;
  firstBytes: string;
  espErr: number;
  espErrName: string;
  lastError: string;
}

export interface CameraAnalyzeResponse {
  taskType: ProjectAITaskType;
  reply: string;
  model: string;
  status: string;
  httpStatus: number;
  latencyMs: number;
  width: number;
  height: number;
  jpegBytes: number;
  needsConfirmation: boolean;
}

export interface AIProfilesResponse {
  activeProfileId: number;
  profiles: AIConfig[];
}

export interface AIChatMessage {
  id: string;
  role: "user" | "assistant";
  content: string;
  at: string;
  status?: "ok" | "error" | "pending";
  latencyMs?: number;
}

export interface AIChatResponse {
  reply: string;
  model: string;
  latencyMs: number;
  status: string;
  httpStatus?: number;
}

// P4 唯一 Agent Session 的同步调试返回；上下文和历史均由设备 Runtime 管理。
export interface AgentChatResponse {
  reply: string;
  latencyMs: number;
  httpStatus: number;
  runId: string;
  status: string;
}

export interface AgentSubmitResponse {
  queued: boolean;
  runId: string;
  state: string;
}

export interface AgentConfirmation {
  callId: string;
  toolName: string;
  arguments: Record<string, unknown>;
  permission: string;
}

export interface AgentRunStatus {
  active: boolean;
  runId: string;
  state: string;
  intent: string;
  reply: string;
  error: string;
  httpStatus: number;
  modelLatencyMs: number;
  contextBytes: number;
  toolSchemaCount: number;
  modelSteps: number;
  cloudStackHighWater: number;
  freePsramBytes: number;
  internalDmaLargestBytes: number;
  cloudPending: boolean;
  confirmation: AgentConfirmation | null;
}

export interface AgentSessionRecord {
  kind: "user" | "assistant" | "tool" | "result";
  run_id: string;
  call_id: string;
  time: string;
  text: string;
  tool_name: string;
  detail: string;
  latency: string;
  risk: number;
  tool_status: number;
  requires_confirmation: boolean;
}

export interface AgentSession {
  schema_version: number;
  foreground: {
    next_sequence: number;
    messages: AgentSessionRecord[];
  };
}

export type ProjectAITaskType =
  | "chat_assist"
  | "recognize_ingredients"
  | "inventory_parse"
  | "recipe_generate"
  | "shopping_list_generate"
  | "reminder_explain"
  | "voice_intent_parse"
  | "kitchen_tool_control";

export interface ProjectAITaskRequest {
  taskType: ProjectAITaskType;
  userText: string;
  includeInventory: boolean;
  includeReminders: boolean;
  includePreferences: boolean;
}

export interface AIAssistantHistoryItem {
  role: "user" | "assistant";
  content: string;
}

export interface AIAssistantChatRequest extends ProjectAITaskRequest {
  message: string;
}

export interface AIAssistantChatResponse extends AIChatResponse {
  taskType: ProjectAITaskType;
  contextInjected: boolean;
  localSnapshotVersion: number;
  needsConfirmation: boolean;
  historyInjected: boolean;
  historyCount: number;
  historyPersisted: boolean;
  historyPrunedCount: number;
  toolExecuted?: boolean;
  toolMessage?: string;
}

export interface AIContextPreview {
  taskType: ProjectAITaskType;
  localSnapshotVersion: number;
  needsConfirmation: boolean;
  context: unknown;
}

export interface ProjectAITaskResponse {
  taskType: ProjectAITaskType;
  confidence: number;
  needsConfirmation: boolean;
  safetyNote: string;
  result: unknown;
}

export interface DeviceChatHistoryMessage {
  id: string;
  role: "user" | "assistant";
  content: string;
  taskType: string;
  createdAt: number;
}

export interface DeviceChatHistory {
  schemaVersion: number;
  updatedAt: number;
  ttlSeconds: number;
  maxMessages: number;
  timeReady: boolean;
  count: number;
  prunedCount: number;
  messages: DeviceChatHistoryMessage[];
}

export interface WifiNetwork {
  ssid: string;
  band: "2.4G" | "5G";
  signal: number;
  rssi?: number;
  channel?: number;
  secured: boolean;
  authmode?: string;
  note: string;
}

export interface PinInfo {
  gpio: string;
  signal: string;
  usage: string;
  level: SafetyLevel;
  note: string;
  readonly: boolean;
}

export interface SensorSnapshot {
  profile?: string;
  pir?: boolean;
  lux?: number;
  lightRaw12bit?: number;
  lightValue10bit?: number;
  lightPercent?: number;
  lightPolarity?: "raw_high_dark" | "raw_high_bright" | string;
  lightDelta?: number;
  imuReady?: boolean;
  baselineReady?: boolean;
  imuAddress?: number;
  imuWhoAmI?: number;
  imuError?: number;
  imuErrorName?: string;
  consecutiveFailures?: number;
  sampleCount?: number;
  i2cBus?: number;
  sclGpio?: number;
  sdaGpio?: number;
  sampleRateHz?: number;
  accelXG?: number;
  accelYG?: number;
  accelZG?: number;
  gyroXDps?: number;
  gyroYDps?: number;
  gyroZDps?: number;
  imuTemperatureC?: number;
  pitchDeg?: number;
  rollDeg?: number;
  angleDelta?: number;
  vibrationPeak?: number;
  radar?: RadarSnapshot;
  touch?: string;
  display?: string;
  buzzer?: string;
  doorState?: string;
  // MQ-5 燃气模块（GPIO20 ADC1_CH4，仅 AO）：相对 ADC 读数，不是 ppm；
  // 旧固件不返回这些字段时为 undefined，前端必须显示 "--" 而不是崩溃。
  mq5State?: Mq5State;
  mq5Ready?: boolean;
  mq5Error?: number;
  mq5Raw12bit?: number;
  mq5Value10bit?: number;
  mq5AdcPercent?: number;
  mq5SampleCount?: number;
  mq5UpdatedAtMs?: number;
  mq5WarmupElapsedSec?: number;
  mq5DailyWarmupComplete?: boolean;
  mq5BurnInConfirmed?: boolean;
  mq5BaselineReady?: boolean;
  mq5BaselineRaw12bit?: number;
  mq5DeltaFromBaseline?: number;
  stateMachine?: StateMachineStatus | null;
  updatedAt: string;
}

export interface StateMachineConfig {
  nightLightThreshold: number;
  dayLightThreshold: number;
  radarTwoMeterRaw: number;
  radarTwoMeterGate: number;
  sleepEnabled?: boolean;
  autoVoiceAfterClose: boolean;
  autoVoiceRecordSeconds?: number;
  closeStableMs?: number;
}

export interface StateMachineStatus {
  state: string;
  doorState: string;
  offline: boolean;
  isNight: boolean;
  radarSoftwarePaused: boolean;
  radarPresenceReliable: boolean;
  radarWithin2m: boolean;
  radarWithin1m: boolean;
  radarApproaching: boolean;
  imuMotionStrength: number;
  lightValue10bit: number;
  lightDelta: number;
  radarDistanceRaw: number;
  radarGate: number;
  lastReason: string;
  autoVoiceState: string;
  autoVoiceError?: string;
  updatedAtMs: number;
  stateSinceMs: number;
}

export interface RadarSnapshot {
  ready: boolean;
  mode: "idle" | "normal" | "report" | "error" | string;
  presence: boolean;
  nearClutter?: boolean;
  staticClutter?: boolean;
  humanCandidate?: boolean;
  stablePresence?: boolean;
  within1m?: boolean;
  approaching?: boolean;
  thresholdPresence?: boolean;
  distanceRaw: number;
  smoothedDistanceRaw?: number;
  gateEnergy: number[];
  peakGate?: number;
  peakEnergy?: number;
  estimatedGate?: number;
  stableGate?: number;
  thresholdGate?: number;
  stableZone?: "unknown" | "near" | "mid" | "far" | string;
  confidence?: number;
  stability?: number;
  approachScore?: number;
  approachFrames?: number;
  approachDistanceDelta?: number;
  motionScore?: number;
  distanceSpan?: number;
  gateSpan?: number;
  energyChangeScore?: number;
  staticScore?: number;
  humanScore?: number;
  thresholdScore?: number;
  holdFramesRemaining?: number;
  nearEnergy?: number;
  midEnergy?: number;
  farEnergy?: number;
  frameCount: number;
  parseErrorCount: number;
  timeoutCount: number;
  ot2Level: number;
  lastText: string;
  targetClass?: string;
  rejectionReason?: string;
  lastError: string;
  updatedAtMs: number;
}

export interface DiagnosticSnapshot {
  psram: string;
  flashPartition: string;
  littlefs: string;
  otaSlot: string;
  brownoutCount: number;
  watchdogCount: number;
  lastError: string;
  riskItems: string[];
}

export interface DeviceLog {
  id: string;
  at: string;
  level: LogLevel;
  source: string;
  message: string;
}

export interface SectionDefinition {
  id:
    | "overview"
    | "usb"
    | "network"
    | "ai"
    | "realtime"
    | "camera"
    | "mic"
    | "speaker"
    | "radar"
    | "pins"
    | "sensors"
    | "logs"
    | "diagnostics"
    | "tfcard"
    | "ui_preview"
    | "settings";
  label: string;
  icon: LucideIcon;
}

export interface ScreenshotResponse {
  path: string;
  format: "jpeg" | "bmp";
  bytes?: number;
}

export interface UiStatus {
  ready: boolean;
  resourceReady: boolean;
  framebuffersSynced: boolean;
  pageFrameReady: boolean;
  page: string;
  orientation: "landscape" | "portrait_privacy";
  privacyMode: boolean;
}
