/**
 * P4 Agent v2 小程序 API 适配层。
 *
 * 页面继续消费原有 InventoryItem/Reminder 等模型；本文件把 v2 的实体快照
 * 映射为页面模型。所有实体写入都走 `/sync/events`，命令型能力走 v2 facade。
 */
import { LONG_REQUEST_TIMEOUT_MS } from "../config/env";
import type {
  AiChatHistoryClearData, AiChatHistoryData, AiChatResponseData, AiConfigData,
  AiConfigUpdateRequest, BindDevicePayload, DeviceSummary, FridgeZoneConfig,
  FridgeZoneListData, FridgeZoneUpdateRequest, HomeOverview, InventoryItem,
  InventoryListData, InventoryWritePayload, LoginData, NotificationSubscribePayload,
  NotificationSubscribeResponse, RefreshData, ReminderItem, ReminderListData,
  AgentMemoryItem, AgentSessionSummary, AlarmItem, NutritionRecord, StopwatchItem, TimerItem,
  ScanResult, SettingsUpdateRequest, SyncEvent, SyncPullData, SyncPushData,
  SyncPushRequest, SyncSnapshotData, UserSettingsData,
} from "../types/models";
import { request, uploadFile } from "../utils/request";
import { normalizeInventoryItem } from "../utils/inventoryLocation";
import { getStorage, setStorage } from "../utils/storage";

const entityRevisions = new Map<string, number>();
let cachedSnapshot: SyncSnapshotData | null = null;

function uuidV4(): string {
  return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, (char) => {
    const value = Math.floor(Math.random() * 16);
    return (char === "x" ? value : (value & 0x3) | 0x8).toString(16);
  });
}
function eventId(_domain: string, _entityId: string): string {
  return uuidV4();
}

function queryHome(path: string): string {
  const home = getApp<any>()?.globalData?.activeHome?.homeId;
  if (!home || home === "default") return path;
  return `${path}${path.includes("?") ? "&" : "?"}homeId=${encodeURIComponent(home)}`;
}

function snapshotData<T>(domain: string): T[] {
  return (cachedSnapshot?.domains?.[domain] || []).map((entity) => {
    entityRevisions.set(`${domain}:${entity.entityId}`, entity.entityRevision);
    return { id: entity.entityId, ...entity.data } as T;
  });
}

function numberValue(value: unknown, fallback = 0): number {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function epochMs(value: unknown): number | undefined {
  const parsed = numberValue(value, 0);
  if (parsed <= 0) return undefined;
  return parsed < 10_000_000_000 ? parsed * 1000 : parsed;
}

function isoDateTime(value: unknown): string | undefined {
  if (typeof value === "string" && value.trim()) return value;
  const parsed = epochMs(value);
  return parsed ? new Date(parsed).toISOString() : undefined;
}

async function writeEntity(
  domain: string,
  entityId: string,
  operation: "upsert" | "delete" | "append",
  data: Record<string, unknown>,
): Promise<SyncPushData> {
  const key = `${domain}:${entityId}`;
  const event: SyncEvent = {
    eventId: eventId(domain, entityId),
    domain,
    entityId,
    operation,
    baseEntityRevision: entityRevisions.get(key) || 0,
    data,
  };
  // 先持久化再发送，超时或断网后仍复用同一个 UUID，避免重试变成重复写。
  const queueKey = "syncQueueV2";
  const queued = getStorage<SyncEvent[]>(queueKey) || [];
  if (!queued.some((item) => item.eventId === event.eventId)) {
    setStorage(queueKey, [...queued, event].slice(-100));
  }
  const result = await pushSyncChanges({ events: [event] });
  const completed = new Set([...result.accepted.map((item) => item.eventId), ...result.duplicates]);
  setStorage(queueKey, (getStorage<SyncEvent[]>(queueKey) || []).filter((item) => !completed.has(item.eventId)));
  if (result.conflicts.length) throw new Error("云端数据已更新，请同步后重试");
  const accepted = result.accepted[0];
  if (accepted) entityRevisions.set(key, accepted.entityRevision || event.baseEntityRevision + 1);
  return result;
}

export function loginWithWechatCode(code: string, nickname?: string, avatarUrl?: string): Promise<LoginData> {
  return request<LoginData>({ path: "/auth/wechat-login", method: "POST", auth: false, data: { code, nickname, avatarUrl } });
}

export async function getDevices(): Promise<DeviceSummary[]> {
  return request<DeviceSummary[]>({ path: "/devices" });
}

export async function getPrimaryDevice(): Promise<DeviceSummary | null> {
  return (await getDevices())[0] || null;
}

export async function bindDevice(payload: BindDevicePayload): Promise<DeviceSummary> {
  await request({ path: "/bindings/claim", method: "POST", data: payload });
  const device = await getPrimaryDevice();
  if (!device) throw new Error("绑定已领取，但设备列表尚未更新");
  return device;
}

export function unbindDevice(deviceId: string): Promise<void> {
  return request<void>({ path: `/devices/${encodeURIComponent(deviceId)}/unbind`, method: "POST" });
}

export async function getSyncSnapshot(): Promise<SyncSnapshotData> {
  cachedSnapshot = await request<SyncSnapshotData>({ path: queryHome("/sync/snapshot") });
  for (const [domain, entities] of Object.entries(cachedSnapshot.domains || {})) {
    for (const entity of entities) entityRevisions.set(`${domain}:${entity.entityId}`, entity.entityRevision);
  }
  return cachedSnapshot;
}

async function ensureSnapshot(): Promise<SyncSnapshotData> {
  return getSyncSnapshot();
}

export async function getInventory(): Promise<InventoryListData> {
  await ensureSnapshot();
  return {
    items: (cachedSnapshot?.domains?.inventory || [])
      .map((entity) => {
        entityRevisions.set(`inventory:${entity.entityId}`, entity.entityRevision);
        return normalizeInventoryItem(entity.data, entity.entityId);
      })
      .filter((item) => item.status !== "deleted"),
  };
}

export async function getReminders(): Promise<ReminderListData> {
  await ensureSnapshot();
  return {
    items: snapshotData<Record<string, unknown>>("reminders").map((raw) => ({
      id: String(raw.id || ""),
      reminderType: String(raw.reminderType || raw.type || raw.kind || "reminder"),
      title: String(raw.title || raw.label || "未命名提醒"),
      content: String(raw.content || raw.note || "") || undefined,
      status: String(raw.status || "pending"),
      dueAt: isoDateTime(raw.dueAtEpochMs || raw.dueAt || raw.remindAt),
      ackedAt: typeof raw.ackedAt === "string" ? raw.ackedAt : undefined,
    })),
  };
}

/** 读取 P4 工具类同步域；这里只做显示适配，写操作仍由设备 Tool Broker 控制。 */
export async function getTimers(): Promise<TimerItem[]> {
  await ensureSnapshot();
  return getTimersFromCache();
}

export async function getAlarms(): Promise<AlarmItem[]> {
  await ensureSnapshot();
  return getAlarmsFromCache();
}

export async function getStopwatch(): Promise<StopwatchItem | null> {
  await ensureSnapshot();
  return getStopwatchFromCache();
}

export async function getTimeTools(): Promise<{
  timers: TimerItem[];
  alarms: AlarmItem[];
  stopwatch: StopwatchItem | null;
}> {
  await ensureSnapshot();
  const [timers, alarms, stopwatch] = await Promise.all([
    getTimersFromCache(),
    getAlarmsFromCache(),
    getStopwatchFromCache(),
  ]);
  return { timers, alarms, stopwatch };
}

function getTimersFromCache(): TimerItem[] {
  return snapshotData<Record<string, unknown>>("timers").map((raw) => {
    const durationMs =
      numberValue(raw.durationMs) || numberValue(raw.durationSeconds) * 1000;
    return {
      id: String(raw.id || ""),
      label: String(raw.label || raw.title || "计时器"),
      durationMs,
      accumulatedMs: numberValue(raw.accumulatedMs || raw.elapsedMs),
      startedAt: epochMs(raw.startedAt || raw.startedAtEpochMs || raw.createdAtEpochMs),
      dueAt: epochMs(raw.dueAt || raw.dueAtEpochMs),
      state: String(raw.state || raw.status || "paused"),
      source: String(raw.source || "device"),
    };
  });
}

function getAlarmsFromCache(): AlarmItem[] {
  return snapshotData<Record<string, unknown>>("alarms").map((raw) => ({
    id: String(raw.id || ""),
    label: String(raw.label || raw.title || "闹钟"),
    enabled: raw.enabled !== false && raw.status !== "cancelled" && raw.status !== "canceled",
    hour: numberValue(raw.hour),
    minute: numberValue(raw.minute),
    timezone: String(raw.timezone || "Asia/Shanghai"),
    weekdayMask: numberValue(raw.weekdayMask || raw.weekday_mask),
    onceAt: epochMs(raw.onceAt || raw.dueAtEpochMs || raw.dueAt),
    sound: String(raw.sound || "chime"),
    speechText: String(raw.speechText || raw.speech_text || "") || undefined,
    source: String(raw.source || "device"),
  }));
}

function getStopwatchFromCache(): StopwatchItem | null {
  const raw = snapshotData<Record<string, unknown>>("stopwatch")[0];
  if (!raw) return null;
  return {
    id: String(raw.id || "household-stopwatch"),
    state: String(raw.state || (raw.running ? "running" : "idle")),
    accumulatedMs: numberValue(raw.accumulatedMs || raw.elapsedMs),
    startedAt: epochMs(raw.startedAt || raw.startedAtEpochMs),
    lapsMs: Array.isArray(raw.lapsMs)
      ? raw.lapsMs.map((value) => numberValue(value)).filter((value) => value >= 0)
      : [],
  };
}

export async function getAgentMemories(): Promise<AgentMemoryItem[]> {
  await ensureSnapshot();
  return snapshotData<Record<string, unknown>>("agent_memory").map((raw) => {
    const createdAtMs = numberValue(raw.createdAtMs);
    return {
      id: String(raw.id || ""),
      kind: String(raw.kind || raw.type || "structured"),
      text: String(raw.text || raw.content || raw.summary || ""),
      tags: Array.isArray(raw.tags) ? raw.tags.map(String) : [],
      source: String(raw.source || "device"),
      category: String(raw.category || raw.type || "") || undefined,
      // 固件 createdAtMs 可能是开机后的单调时钟，小于 epoch 毫秒时不伪造日期。
      createdAt: raw.createdAt
        ? epochMs(raw.createdAt)
        : createdAtMs > 1_000_000_000_000
          ? createdAtMs
          : undefined,
    };
  });
}

export async function getNutritionRecords(): Promise<NutritionRecord[]> {
  await ensureSnapshot();
  return snapshotData<Record<string, unknown>>("nutrition")
    .filter((raw) => !raw.nutrition && !Array.isArray(raw.items))
    .map((raw) => ({
      id: String(raw.id || ""),
      title: String(raw.title || "饮食记录"),
      consumedAt: epochMs(raw.consumedAt),
      caloriesKcal: numberValue(raw.caloriesKcal) || undefined,
      proteinG: numberValue(raw.proteinG) || undefined,
      fatG: numberValue(raw.fatG) || undefined,
      carbohydrateG: numberValue(raw.carbohydrateG) || undefined,
      sodiumMg: numberValue(raw.sodiumMg) || undefined,
      note: String(raw.note || "") || undefined,
      source: String(raw.source || "device"),
    }));
}

export async function getHomeOverview(): Promise<HomeOverview> {
  const [device, inventory, reminders] = await Promise.all([getPrimaryDevice(), getInventory(), getReminders()]);
  const expiring = inventory.items
    .filter((item) => item.expireDate)
    .sort((a, b) => String(a.expireDate).localeCompare(String(b.expireDate)));
  return {
    device: device || undefined,
    inventoryCount: inventory.items.length,
    expiringCount: expiring.length,
    pendingReminderCount: reminders.items.filter((item) => item.status === "pending").length,
    expiringList: expiring.slice(0, 5),
    lastSyncAt: cachedSnapshot?.generatedAt,
  };
}

export async function createInventoryItem(payload: InventoryWritePayload): Promise<InventoryItem> {
  const id = `inv-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
  await writeEntity("inventory", id, "upsert", { ...payload, id });
  return { id, name: payload.name || "", quantity: payload.quantity || 0, unit: payload.unit || "份", status: payload.status || "active", source: payload.source || "manual", ...payload };
}

export async function updateInventoryItem(itemId: string, payload: InventoryWritePayload): Promise<InventoryItem> {
  await ensureSnapshot();
  const current = snapshotData<InventoryItem>("inventory").find((item) => item.id === itemId);
  const next = { ...(current || {}), ...payload, id: itemId } as InventoryItem;
  await writeEntity("inventory", itemId, "upsert", next as unknown as Record<string, unknown>);
  return next;
}

export async function deleteInventoryItem(itemId: string): Promise<void> {
  await ensureSnapshot();
  await writeEntity("inventory", itemId, "delete", {});
}

export function refreshInventory(): Promise<RefreshData> {
  return request<RefreshData>({ path: "/inventory/refresh", method: "POST" });
}

export function scanFoodImage(opts: { filePath: string; hintZone?: string }): Promise<ScanResult> {
  return uploadFile<ScanResult>({ path: "/inventory/scan", filePath: opts.filePath, fileFieldName: "file", formData: opts.hintZone ? { zone: opts.hintZone } : undefined, timeoutMs: LONG_REQUEST_TIMEOUT_MS });
}

export async function confirmReminder(
  reminderId: string,
  status: "completed" | "cancelled" = "completed",
): Promise<ReminderItem> {
  await ensureSnapshot();
  const current = snapshotData<ReminderItem>("reminders").find((item) => item.id === reminderId);
  const next = { ...(current || {}), id: reminderId, status } as ReminderItem;
  await writeEntity("reminders", reminderId, "upsert", next as unknown as Record<string, unknown>);
  return next;
}

export async function getSettings(): Promise<UserSettingsData> {
  await ensureSnapshot();
  const entries = snapshotData<Record<string, unknown>>("device_settings");
  const primary =
    entries.find((item) => item.id === "primary") ||
    entries.find((item) => item.privacy || item.preferences) ||
    {};
  const device =
    entries.find(
      (item) =>
        item.volumePercent != null ||
        item.brightnessPercent != null ||
        item.deviceSn,
    ) || {};
  return {
    privacy: (primary.privacy || {}) as UserSettingsData["privacy"],
    preferences: (primary.preferences || {}) as Record<string, unknown>,
    deviceSn: String(device.deviceSn || primary.deviceSn || "") || undefined,
    timezone: String(device.timezone || primary.timezone || "") || undefined,
    locale: String(device.locale || primary.locale || "") || undefined,
    volumePercent:
      device.volumePercent == null
        ? primary.volumePercent == null
          ? undefined
          : numberValue(primary.volumePercent)
        : numberValue(device.volumePercent),
    brightnessPercent:
      device.brightnessPercent == null
        ? primary.brightnessPercent == null
          ? undefined
          : numberValue(primary.brightnessPercent)
        : numberValue(device.brightnessPercent),
    reminderEnabled:
      typeof device.reminderEnabled === "boolean"
        ? device.reminderEnabled
        : typeof primary.reminderEnabled === "boolean"
          ? primary.reminderEnabled
          : undefined,
    agentVoiceEnabled:
      typeof device.agentVoiceEnabled === "boolean"
        ? device.agentVoiceEnabled
        : typeof primary.agentVoiceEnabled === "boolean"
          ? primary.agentVoiceEnabled
          : undefined,
    updatedAt: String(primary.updatedAt || device.updatedAt || "") || undefined,
  };
}

export async function updateSettings(payload: SettingsUpdateRequest): Promise<UserSettingsData> {
  const current = await getSettings();
  const next: UserSettingsData = {
    ...current,
    privacy: { ...current.privacy, ...(payload.privacy || {}) } as UserSettingsData["privacy"],
    preferences: { ...current.preferences, ...(payload.preferences || {}) },
    updatedAt: new Date().toISOString(),
  };
  await writeEntity("device_settings", "primary", "upsert", next as unknown as Record<string, unknown>);
  return next;
}

export function sendAiChat(prompt: string, sessionId?: string): Promise<AiChatResponseData> {
  return request<AiChatResponseData>({ path: "/ai/chat", method: "POST", data: { prompt, sessionId }, timeoutMs: 90_000 });
}
export function getAiChatHistory(sessionId?: string): Promise<AiChatHistoryData> {
  return request<AiChatHistoryData>({ path: `/ai/history${sessionId ? `?sessionId=${encodeURIComponent(sessionId)}` : ""}` });
}
export async function getAgentSessions(): Promise<AgentSessionSummary[]> {
  await ensureSnapshot();
  return snapshotData<Record<string, unknown>>("agent_sessions")
    .map((raw) => ({
      sessionId: String(raw.sessionId || raw.id || ""),
      title: String(raw.title || raw.sessionId || raw.id || "对话"),
      messageCount: Array.isArray(raw.messages) ? raw.messages.length : 0,
      updatedAt: String(raw.updatedAt || "") || undefined,
    }))
    .filter((item) => item.sessionId)
    .sort((left, right) =>
      String(right.updatedAt || "").localeCompare(String(left.updatedAt || "")),
    );
}
export async function getSyncedAgentSessionHistory(
  sessionId: string,
): Promise<AiChatHistoryData> {
  if (!cachedSnapshot) await ensureSnapshot();
  const entity = (cachedSnapshot?.domains?.agent_sessions || []).find(
    (item) => item.entityId === sessionId,
  );
  const messages = Array.isArray(entity?.data?.messages) ? entity!.data.messages : [];
  return {
    sessionId,
    messages: messages
      .filter((item): item is Record<string, unknown> => !!item && typeof item === "object")
      .map((item, index) => ({
        id: String(item.id || item.call_id || `${sessionId}-${index}`),
        role: String(item.role || item.kind || "assistant"),
        content: String(item.content || item.text || item.detail || ""),
        source: String(item.source || "device"),
        deviceSn: String(item.deviceSn || "") || undefined,
        sentAt: String(item.sentAt || item.time || new Date().toISOString()),
      })),
  };
}
export function clearAiChatHistory(sessionId?: string): Promise<AiChatHistoryClearData> {
  return request<AiChatHistoryClearData>({ path: `/ai/history${sessionId ? `?sessionId=${encodeURIComponent(sessionId)}` : ""}`, method: "DELETE" });
}
export function getAiConfig(): Promise<AiConfigData> {
  return request<{ entityId?: string; entityRevision?: number; data?: Record<string, unknown> }>({ path: queryHome("/agent-config") })
    .then((entity) => {
      if (entity.entityRevision !== undefined) entityRevisions.set("ai_config:active-profile", entity.entityRevision);
      const data = (entity.data || entity) as Record<string, unknown>;
      return {
        ...data,
        hasApiKey: data.hasApiKey === true || data.apiKey === "***",
        asrHasApiKey: data.asrHasApiKey === true || data.asrApiKey === "***",
        ttsHasApiKey: data.ttsHasApiKey === true || data.ttsApiKey === "***",
        source: String(data.source || "cloud"),
      } as AiConfigData;
    });
}
export async function updateAiConfig(payload: AiConfigUpdateRequest): Promise<AiConfigData> {
  const current = await getAiConfig();
  const baseEntityRevision = entityRevisions.get("ai_config:active-profile") || 0;
  const result = await request<SyncPushData>({ path: queryHome("/agent-config"), method: "PUT", data: { eventId: eventId("ai_config", "active-profile"), baseEntityRevision, data: payload } });
  if (result.conflicts.length) throw new Error("AI 配置已被其它终端更新，请刷新后重试");
  if (result.accepted[0]?.entityRevision !== undefined) {
    entityRevisions.set("ai_config:active-profile", result.accepted[0].entityRevision);
  }
  return { ...current, ...payload, hasApiKey: payload.apiKey === "" ? false : current.hasApiKey || Boolean(payload.apiKey), source: "miniapp" };
}

export async function getFridgeZonesRemote(): Promise<FridgeZoneListData> {
  const settings = await getSettings();
  return { zones: (settings.preferences.fridgeZones || []) as FridgeZoneConfig[], source: "cloud" };
}
export async function updateFridgeZonesRemote(payload: FridgeZoneUpdateRequest): Promise<FridgeZoneListData> {
  const settings = await getSettings();
  await updateSettings({ preferences: { ...settings.preferences, fridgeZones: payload.zones } });
  return { zones: payload.zones, source: "cloud" };
}

export function pullSyncChanges(sinceRevision: number): Promise<SyncPullData> {
  return request<SyncPullData>({ path: queryHome(`/sync/events?sinceRevision=${encodeURIComponent(String(sinceRevision || 0))}`) });
}
export function pushSyncChanges(payload: SyncPushRequest): Promise<SyncPushData> {
  return request<SyncPushData, SyncPushRequest>({ path: queryHome("/sync/events"), method: "POST", data: payload });
}
export function subscribeNotification(payload: NotificationSubscribePayload): Promise<NotificationSubscribeResponse> {
  return request<NotificationSubscribeResponse, NotificationSubscribePayload>({ path: "/notification/subscribe", method: "POST", data: payload });
}
