/**
 * 库存三级位置兼容层。
 *
 * P4 权威模型为 zone_id + layer_id + cell(0~8)。小程序历史数据使用
 * zone + slot(A1~C3)，这里统一归一化，确保旧数据可读、新写入可被 P4 识别。
 */
import type {
  FridgeLayerConfig,
  InventoryItem,
  InventoryWritePayload,
  StandardSlot,
} from "../types/models";
import { STANDARD_SLOTS } from "../types/models";

export const DEFAULT_INVENTORY_LAYERS: FridgeLayerConfig[] = [
  { id: "f1", zoneId: "freezer", name: "第1层", order: 0 },
  { id: "f2", zoneId: "freezer", name: "第2层", order: 1 },
  { id: "l1", zoneId: "left", name: "第1层", order: 0 },
  { id: "l2", zoneId: "left", name: "第2层", order: 1 },
  { id: "l3", zoneId: "left", name: "第3层", order: 2 },
  { id: "r1", zoneId: "right", name: "第1层", order: 0 },
  { id: "r2", zoneId: "right", name: "第2层", order: 1 },
  { id: "d1", zoneId: "door", name: "上格", order: 0 },
  { id: "d2", zoneId: "door", name: "下格", order: 1 },
  { id: "v1", zoneId: "drawer", name: "抽屉", order: 0 },
];

export function layersForZone(zoneId?: string): FridgeLayerConfig[] {
  if (!zoneId) return [];
  const defaults = DEFAULT_INVENTORY_LAYERS
    .filter((layer) => layer.zoneId === zoneId)
    .sort((a, b) => a.order - b.order);
  return defaults.length
    ? defaults
    : [{ id: `${zoneId}-layer-1`, zoneId, name: "第1层", order: 0 }];
}

export function defaultLayerId(zoneId?: string): string {
  return layersForZone(zoneId)[0]?.id || "";
}

export function slotToCell(slot?: string): number | undefined {
  if (!slot) return undefined;
  const index = STANDARD_SLOTS.indexOf(slot as StandardSlot);
  return index >= 0 ? index : undefined;
}

export function cellToSlot(cell?: number): StandardSlot | undefined {
  return Number.isInteger(cell) && cell! >= 0 && cell! < STANDARD_SLOTS.length
    ? STANDARD_SLOTS[cell!]
    : undefined;
}

function splitQuantity(value: unknown, unitValue: unknown): { quantity: number; unit: string } {
  if (typeof value === "number") {
    return { quantity: value, unit: String(unitValue || "份") };
  }
  const text = String(value || "").trim();
  const match = text.match(/^([0-9]+(?:\.[0-9]+)?)(.*)$/);
  if (!match) return { quantity: 1, unit: String(unitValue || text || "份") };
  return {
    quantity: Number(match[1]) || 1,
    unit: String(unitValue || match[2].trim() || "份"),
  };
}

export function normalizeInventoryItem(
  raw: Record<string, unknown>,
  entityId?: string,
): InventoryItem {
  const zoneId = String(raw.zoneId || raw.zone_id || raw.zone || "");
  const explicitLayer = String(raw.layerId || raw.layer_id || "");
  const rawCell = raw.cell;
  const numericCell = typeof rawCell === "number" && rawCell >= 0 && rawCell <= 8
    ? Math.floor(rawCell)
    : slotToCell(String(raw.slot || ""));
  const layerId = explicitLayer || (zoneId ? defaultLayerId(zoneId) : "");
  const quantity = splitQuantity(raw.quantity, raw.unit);
  const cell = zoneId && layerId ? numericCell : undefined;
  const slot = cellToSlot(cell);
  return {
    ...(raw as unknown as InventoryItem),
    id: String(raw.id || entityId || ""),
    name: String(raw.name || ""),
    quantity: quantity.quantity,
    unit: quantity.unit,
    zoneId: zoneId || undefined,
    layerId: layerId || undefined,
    cell,
    // 旧页面字段继续保留，逐步迁移期间不会让离线快照失效。
    zone: zoneId || undefined,
    slot,
    expireDate: String(raw.expireDate || raw.expire_date || "") || undefined,
    status: String(raw.status || "active"),
    source: String(raw.source || "device"),
    extra: {
      ...((raw.extra as Record<string, unknown> | undefined) || {}),
      note: raw.note || (raw.extra as Record<string, unknown> | undefined)?.note,
    },
  };
}

export function inventoryPositionPayload(
  zoneId?: string,
  layerId?: string,
  cell?: number,
): Pick<
  InventoryWritePayload,
  "zoneId" | "layerId" | "cell" | "zone" | "slot"
> {
  const validCell = Number.isInteger(cell) && cell! >= 0 && cell! <= 8 ? cell : undefined;
  const resolvedLayerId = zoneId ? layerId || defaultLayerId(zoneId) : undefined;
  return {
    zoneId: zoneId || undefined,
    layerId: resolvedLayerId,
    cell: zoneId && resolvedLayerId ? validCell : undefined,
    zone: zoneId || undefined,
    slot: cellToSlot(validCell),
  };
}
