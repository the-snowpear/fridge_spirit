/**
 * P4 v2 离线同步：稳定 eventId、逐实体 revision、显式冲突和 REST 游标补偿。
 */
import { getSyncSnapshot, pullSyncChanges, pushSyncChanges } from "./api";
import type {
  InventoryItem, LocalSyncState, RecipeRecommendation, ReminderItem, ShoppingItem,
  SyncDomain, SyncEvent, SyncSnapshotData,
} from "../types/models";
import { getStorage, setStorage } from "../utils/storage";
import { NUTRITION_CACHE_KEY, OFFLINE_SNAPSHOT_KEY, RECIPE_RECOMMENDATIONS_KEY, SHOPPING_ITEMS_KEY, updateOfflineSnapshot } from "../utils/localFeatures";
import { setAllFridgeZones } from "../utils/fridgeZones";
import { normalizeInventoryItem } from "../utils/inventoryLocation";

export const SYNC_QUEUE_KEY = "syncQueueV2";
export const SYNC_STATE_KEY = "syncStateV2";
const ENTITY_REVISION_KEY = "syncEntityRevisionsV2";
const DEFAULT_SYNC_STATE: LocalSyncState = { serverRevision: 0, dirtyDomains: [], conflicts: [] };
let syncInFlight: Promise<SyncSnapshotData | null> | null = null;

const DOMAIN_MAP: Record<string, SyncDomain> = {
  reminder: "reminders", shopping_list: "shopping", recipe_cache: "recipes",
  nutrition_cache: "nutrition", settings: "device_settings", fridge_zones: "device_settings",
  ai_history: "agent_sessions", asr_config: "ai_config", tts_config: "ai_config",
};

function uuidV4(): string {
  return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, (char) => {
    const value = Math.floor(Math.random() * 16);
    return (char === "x" ? value : (value & 0x3) | 0x8).toString(16);
  });
}

function revisions(): Record<string, number> {
  return getStorage<Record<string, number>>(ENTITY_REVISION_KEY) || {};
}
function setRevision(domain: string, entityId: string, revision: number): void {
  setStorage(ENTITY_REVISION_KEY, { ...revisions(), [`${domain}:${entityId}`]: revision });
}
function normalizeEntityId(domain: string, payload: Record<string, unknown>): string {
  return String(payload.entityId || payload.itemId || payload.reminderId || payload.id ||
    (domain === "device_settings" ? "primary" : domain === "ai_config" ? "active-profile" : "default"));
}
function normalizeData(payload: Record<string, unknown>): Record<string, unknown> {
  const nested = payload.payload;
  return nested && typeof nested === "object" ? { ...(nested as Record<string, unknown>) } : { ...payload };
}

export function getLocalSyncState(): LocalSyncState {
  return { ...DEFAULT_SYNC_STATE, ...(getStorage<LocalSyncState>(SYNC_STATE_KEY) || {}) };
}
export function saveLocalSyncState(patch: Partial<LocalSyncState>): LocalSyncState {
  const current = getLocalSyncState();
  const next = { ...current, ...patch, dirtyDomains: patch.dirtyDomains || current.dirtyDomains || [] };
  setStorage(SYNC_STATE_KEY, next);
  return next;
}
export function getSyncQueue(): SyncEvent[] { return getStorage<SyncEvent[]>(SYNC_QUEUE_KEY) || []; }
export function markDomainDirty(domain: SyncDomain): void {
  saveLocalSyncState({ dirtyDomains: Array.from(new Set([...getLocalSyncState().dirtyDomains, domain])) });
}

export function enqueueSyncOp(domainInput: SyncDomain, op: string, payload: Record<string, unknown>): SyncEvent {
  const domain = DOMAIN_MAP[domainInput] || domainInput;
  if (domain === "ai_config" && Object.keys(payload).some((key) => key.toLowerCase().includes("apikey"))) {
    throw new Error("AI Key 只能在线提交，禁止写入离线队列");
  }
  const entityId = normalizeEntityId(domain, payload);
  const operation: SyncEvent["operation"] = op.includes("delete") ? "delete" : op.includes("append") ? "append" : "upsert";
  const event: SyncEvent = {
    // UUID 在入队时生成并随 outbox 持久化；网络重试始终复用同一个 eventId。
    eventId: uuidV4(),
    domain, entityId, operation,
    baseEntityRevision: revisions()[`${domain}:${entityId}`] || 0,
    data: operation === "delete" ? {} : normalizeData(payload),
    createdAt: new Date().toISOString(),
  };
  setStorage(SYNC_QUEUE_KEY, [...getSyncQueue(), event].slice(-100));
  markDomainDirty(domain);
  return event;
}

export async function flushSyncQueue(): Promise<void> {
  const queue = getSyncQueue();
  if (!queue.length) return;
  try {
    const result = await pushSyncChanges({ events: queue.slice(0, 50) });
    const done = new Set([...result.accepted.map((event) => event.eventId), ...result.duplicates]);
    for (const event of result.accepted) setRevision(event.domain, event.entityId, event.entityRevision || 0);
    setStorage(SYNC_QUEUE_KEY, queue.filter((event) => !done.has(event.eventId)));
    saveLocalSyncState({
      serverRevision: result.serverRevision, lastPushedAt: Date.now(),
      lastSyncError: result.conflicts.length ? "存在同步冲突，请先处理" : "",
      conflicts: result.conflicts,
      dirtyDomains: getSyncQueue().length ? getLocalSyncState().dirtyDomains : [],
    });
  } catch (err) {
    saveLocalSyncState({ lastSyncError: err instanceof Error ? err.message : "同步上传失败" });
    throw err;
  }
}

async function pullAllEvents(): Promise<void> {
  let cursor = getLocalSyncState().serverRevision || 0;
  while (true) {
    const pulled = await pullSyncChanges(cursor);
    for (const event of pulled.events) setRevision(event.domain, event.entityId, event.entityRevision || 0);
    cursor = pulled.serverRevision;
    saveLocalSyncState({ serverRevision: cursor, lastPulledAt: Date.now(), lastSyncError: "" });
    if (!pulled.hasMore) break;
  }
}

export async function pullLatestSnapshot(): Promise<SyncSnapshotData | null> {
  await pullAllEvents();
  const snapshot = await getSyncSnapshot();
  applySnapshotToLocalCache(snapshot);
  saveLocalSyncState({ serverRevision: snapshot.serverRevision, lastPulledAt: Date.now(), lastSyncError: "" });
  return snapshot;
}

export async function syncNow(_options: { forceDeviceRefresh?: boolean } = {}): Promise<SyncSnapshotData | null> {
  if (syncInFlight) return syncInFlight;
  syncInFlight = (async () => {
    await flushSyncQueue();
    return pullLatestSnapshot();
  })();
  try { return await syncInFlight; } finally { syncInFlight = null; }
}

export async function refreshSyncStatus(): Promise<LocalSyncState> {
  try { await pullAllEvents(); } catch (err) {
    saveLocalSyncState({ lastSyncError: err instanceof Error ? err.message : "同步状态读取失败" });
  }
  return getLocalSyncState();
}

function domainData<T>(snapshot: SyncSnapshotData, domain: string): T[] {
  return (snapshot.domains?.[domain] || []).map((entity) => {
    setRevision(domain, entity.entityId, entity.entityRevision);
    return { id: entity.entityId, ...entity.data } as T;
  });
}

function flattenedDomainItems(
  snapshot: SyncSnapshotData,
  domain: string,
): Array<Record<string, unknown>> {
  const result = new Map<string, { revision: number; data: Record<string, unknown> }>();
  let anonymous = 0;
  for (const entity of (snapshot.domains?.[domain] || []).slice().sort(
    (left, right) => left.serverRevision - right.serverRevision,
  )) {
    setRevision(domain, entity.entityId, entity.entityRevision);
    const nested = entity.data.items;
    if (Array.isArray(nested)) {
      for (const item of nested) {
        if (item && typeof item === "object") {
          const data = item as Record<string, unknown>;
          const key = String(data.id || `${entity.entityId}:${anonymous++}`);
          result.set(key, { revision: entity.serverRevision, data: { id: key, ...data } });
        }
      }
    } else {
      const key = String(entity.data.id || entity.entityId);
      result.set(key, {
        revision: entity.serverRevision,
        data: { id: key, ...entity.data },
      });
    }
  }
  return Array.from(result.values())
    .sort((left, right) => right.revision - left.revision)
    .map((entry) => entry.data);
}

export function applySnapshotToLocalCache(snapshot: SyncSnapshotData): void {
  const inventory = (snapshot.domains?.inventory || []).map((entity) => {
    setRevision("inventory", entity.entityId, entity.entityRevision);
    return normalizeInventoryItem(entity.data, entity.entityId);
  });
  const reminders = domainData<ReminderItem>(snapshot, "reminders");
  updateOfflineSnapshot({ inventoryItems: inventory, reminderItems: reminders });
  const settings = domainData<Record<string, unknown>>(snapshot, "device_settings")[0];
  const preferences = settings?.preferences as Record<string, unknown> | undefined;
  if (preferences?.fridgeZones) setAllFridgeZones(preferences.fridgeZones as any);
  if (settings?.privacy) setStorage("privacySettings", settings.privacy);
  const shoppingItems = flattenedDomainItems(snapshot, "shopping").map((raw, index) => {
    const quantity = raw.quantity == null ? "" : String(raw.quantity);
    const unit = String(raw.unit || "");
    const rawSource = String(raw.source || "device");
    const source: ShoppingItem["source"] =
      rawSource === "ai" ||
      rawSource === "manual" ||
      rawSource === "expire" ||
      rawSource === "recipe"
        ? rawSource
        : "device";
    return {
      id: String(raw.id || `shopping-${index}`),
      name: String(raw.name || ""),
      quantityText: String(raw.quantityText || `${quantity}${unit}` || "1 份"),
      source,
      sourceText: String(raw.sourceText || raw.note || "设备同步"),
      checked: Boolean(raw.checked),
      createdAt: Number(raw.createdAt || raw.created_at || Date.now()),
    };
  }).filter((item) => item.name);
  if ((snapshot.domains?.shopping || []).length) {
    setStorage(SHOPPING_ITEMS_KEY, shoppingItems);
  }

  const recipes = flattenedDomainItems(snapshot, "recipes").map((raw, index) => ({
    id: String(raw.id || `recipe-${index}`),
    ...raw,
  })) as unknown as RecipeRecommendation[];
  if ((snapshot.domains?.recipes || []).length) {
    setStorage(RECIPE_RECOMMENDATIONS_KEY, recipes);
  }

  const nutritionEntities = snapshot.domains?.nutrition || [];
  const nutritionCache = nutritionEntities
    .map((entity) => entity.data)
    .find((data) => data.nutrition && typeof data.nutrition === "object");
  if (nutritionCache) {
    setStorage(NUTRITION_CACHE_KEY, nutritionCache);
  }
}

/** 用户或家庭切换时必须丢弃旧家庭的游标、实体 revision 和业务缓存。 */
export function clearSyncContext(): void {
  setStorage(SYNC_QUEUE_KEY, null);
  setStorage(SYNC_STATE_KEY, null);
  setStorage(ENTITY_REVISION_KEY, null);
  setStorage(SHOPPING_ITEMS_KEY, null);
  setStorage(RECIPE_RECOMMENDATIONS_KEY, null);
  setStorage(NUTRITION_CACHE_KEY, null);
  setStorage(OFFLINE_SNAPSHOT_KEY, null);
  setStorage("privacySettings", null);
  setStorage("fridgeZonesCloudCache", null);
  setStorage("fridgeStandardZones", null);
  setStorage("fridgeCustomZones", null);
}
