/**
 * 库存详情 / 编辑页。两种模式：
 *   - mode=create：新增条目；可从 query 预填 name/quantity/unit/category/zone/slot/source/confidence；
 *   - mode=edit （默认）：根据 id 取 detail，编辑保存。
 *
 * 字段对齐 backend `InventoryWritePayload`：name/quantity/unit/category/zone/slot/location/expireDate/source。
 * 删除按钮仅 edit 模式可见。
 */

import {
  createInventoryItem,
  deleteInventoryItem,
  getInventory,
  updateInventoryItem,
} from "../../services/api";
import { syncNow } from "../../services/sync";
import type {
  FridgeZoneConfig,
  FridgeLayerConfig,
  InventoryItem,
  InventoryWritePayload,
} from "../../types/models";
import { RequestError } from "../../utils/request";
import {
  displayInventoryPlace,
  getFridgeZones,
  getInventoryLayers,
} from "../../utils/fridgeZones";
import {
  cellToSlot,
  defaultLayerId,
  inventoryPositionPayload,
  slotToCell,
} from "../../utils/inventoryLocation";

type ZoneOption = {
  value: string;
  label: string;
  hint: string;
  custom: boolean;
  spanClass: string;
  zoneClass: string;
  count: number;
};
type LayerOption = FridgeLayerConfig;
type CellOption = { value: number; label: string };
type PlaceMode = "form" | "zone" | "layer" | "cell";

interface DetailData {
  mode: "create" | "edit";
  itemId: string;
  loading: boolean;
  saving: boolean;
  errorText: string;

  name: string;
  category: string;
  quantity: string; // input 用 string，提交时转 number
  unit: string;
  zoneIndex: number;
  layerIndex: number;
  cellIndex: number;
  zoneOptions: ZoneOption[];
  layerOptions: LayerOption[];
  cellOptions: CellOption[];
  selectedZoneValue: string;
  selectedLayerValue: string;
  selectedCellValue: number;
  placeText: string;
  placeMode: PlaceMode;
  choosingZoneIndex: number;
  choosingZoneLabel: string;
  choosingLayerIndex: number;
  choosingLayerLabel: string;
  expireDate: string;
  storedAt: string;
  note: string;
  source: string;
  confidence: string;

  // 仅 edit 模式显示
  origin: InventoryItem | null;
}

function makeZoneOptions(): ZoneOption[] {
  return [
    { value: "", label: "未指定", hint: "", custom: false, spanClass: "", zoneClass: "", count: 0 },
    ...getFridgeZones().map((z: FridgeZoneConfig) => ({
      value: z.key,
      label: z.label,
      hint: z.hint || "选择此空间",
      custom: !!z.custom,
      spanClass: `span-w${z.width || 1}-h${z.height || 1}`,
      zoneClass: z.custom ? "zone-custom" : `zone-${z.key}`,
      count: 0,
    })),
  ];
}

function findZoneIndex(options: ZoneOption[], zone?: string): number {
  if (!zone) return 0;
  const i = options.findIndex((o) => o.value === zone);
  return i >= 0 ? i : 0;
}
function findLayerIndex(options: LayerOption[], layerId?: string): number {
  if (!layerId) return -1;
  const i = options.findIndex((o) => o.id === layerId);
  return i >= 0 ? i : -1;
}

function formatDateTimeInput(value: Date): string {
  const year = value.getFullYear();
  const month = String(value.getMonth() + 1).padStart(2, "0");
  const day = String(value.getDate()).padStart(2, "0");
  const hour = String(value.getHours()).padStart(2, "0");
  const minute = String(value.getMinutes()).padStart(2, "0");
  return `${year}-${month}-${day} ${hour}:${minute}`;
}

function getExtraString(extra: Record<string, unknown> | undefined, key: string): string {
  const value = extra?.[key];
  return typeof value === "string" ? value : "";
}

function getStoredAtText(item: InventoryItem): string {
  const storedAt = getExtraString(item.extra, "storedAt");
  if (storedAt) return storedAt;
  const fallback = item.createdAt || item.updatedAt;
  if (!fallback) return formatDateTimeInput(new Date());
  const parsed = new Date(fallback);
  if (Number.isNaN(parsed.getTime())) return fallback.slice(0, 16).replace("T", " ");
  return formatDateTimeInput(parsed);
}

Page({
  data: {
    mode: "edit",
    itemId: "",
    loading: false,
    saving: false,
    errorText: "",
    name: "",
    category: "",
    quantity: "1",
    unit: "份",
    zoneIndex: 0,
    layerIndex: -1,
    cellIndex: -1,
    zoneOptions: makeZoneOptions(),
    layerOptions: [] as LayerOption[],
    cellOptions: Array.from({ length: 9 }, (_, value) => ({
      value,
      label: cellToSlot(value) || String(value + 1),
    })),
    selectedZoneValue: "",
    selectedLayerValue: "",
    selectedCellValue: -1,
    placeText: "未指定位置",
    placeMode: "form",
    choosingZoneIndex: -1,
    choosingZoneLabel: "",
    choosingLayerIndex: -1,
    choosingLayerLabel: "",
    expireDate: "",
    storedAt: "",
    note: "",
    source: "manual",
    confidence: "",
    origin: null,
  } as DetailData,

  onLoad(query: Record<string, string | undefined>) {
    const mode = query.mode === "create" ? "create" : "edit";
    const zoneOptions = makeZoneOptions();
    this.setData({ mode, zoneOptions });
    if (mode === "create") {
      const zoneId = query.zoneId || query.zone || "";
      const layers = getInventoryLayers(zoneId);
      const layerId = query.layerId || (zoneId ? defaultLayerId(zoneId) : "");
      const cell =
        query.cell != null ? Number(query.cell) : slotToCell(query.slot);
      // 预填 query 字段
      this.setData({
        name: query.name || "",
        category: query.category || "",
        quantity: query.quantity || "1",
        unit: query.unit || "份",
        zoneIndex: findZoneIndex(zoneOptions, zoneId),
        layerOptions: layers,
        layerIndex: findLayerIndex(layers, layerId),
        cellIndex: Number.isInteger(cell) && cell! >= 0 && cell! <= 8 ? cell! : -1,
        storedAt: formatDateTimeInput(new Date()),
        note: query.note || "",
        source: query.source || "manual",
        confidence: query.confidence || "",
      });
      this.updatePlaceText();
      void this.refreshZoneCounts();
    } else {
      const id = query.id || "";
      this.setData({ itemId: id });
      if (id) {
        void this.loadDetail(id);
      } else {
        this.setData({ errorText: "缺少 id 参数" });
      }
      void this.refreshZoneCounts();
    }
  },

  async refreshZoneCounts() {
    try {
      const data = await getInventory();
      const counts: Record<string, number> = {};
      for (const item of data.items || []) {
        if (!item.zoneId) continue;
        counts[item.zoneId] = (counts[item.zoneId] || 0) + 1;
      }
      this.setData({
        zoneOptions: this.data.zoneOptions.map((zone) => ({
          ...zone,
          count: zone.value ? counts[zone.value] || 0 : 0,
        })),
      });
    } catch {
      // 计数只影响位置选择展示，不阻断表单编辑。
    }
  },

  async loadDetail(id: string) {
    this.setData({ loading: true, errorText: "" });
    try {
      const data = await getInventory();
      const target = (data.items || []).find((x) => x.id === id);
      if (!target) {
        this.setData({ errorText: "条目不存在或已删除" });
        return;
      }
      const layers = getInventoryLayers(target.zoneId);
      this.setData({
        origin: target,
        name: target.name,
        category: target.category || "",
        quantity: String(target.quantity || 1),
        unit: target.unit || "份",
        zoneIndex: findZoneIndex(this.data.zoneOptions, target.zoneId),
        layerOptions: layers,
        layerIndex: findLayerIndex(layers, target.layerId),
        cellIndex: target.cell == null ? -1 : target.cell,
        expireDate: target.expireDate || "",
        storedAt: getStoredAtText(target),
        note: getExtraString(target.extra, "note"),
        source: target.source || "manual",
        confidence: target.confidence == null ? "" : String(target.confidence),
      });
      this.updatePlaceText();
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "加载失败";
      this.setData({ errorText: message });
    } finally {
      this.setData({ loading: false });
    }
  },

  onFieldInput(event: WechatMiniprogram.Input) {
    const field = (event.currentTarget.dataset.field as string) || "";
    const value = String(event.detail.value || "");
    if (!field) return;
    this.setData({ [field]: value });
  },

  beginPlaceSelection() {
    this.setData({
      placeMode: "zone",
      choosingZoneIndex: this.data.zoneIndex > 0 ? this.data.zoneIndex : -1,
      choosingZoneLabel: this.data.zoneIndex > 0 ? this.data.zoneOptions[this.data.zoneIndex]?.label || "" : "",
    });
  },

  onTapPlaceZone(event: WechatMiniprogram.BaseEvent) {
    const value = String(event.currentTarget.dataset.value || "");
    const zoneIndex = findZoneIndex(this.data.zoneOptions, value);
    if (zoneIndex <= 0) return;
    this.setData({
      choosingZoneIndex: zoneIndex,
      choosingZoneLabel: this.data.zoneOptions[zoneIndex]?.label || "",
      layerOptions: getInventoryLayers(value),
      choosingLayerIndex: -1,
      choosingLayerLabel: "",
      placeMode: "layer",
    });
  },

  onTapPlaceLayer(event: WechatMiniprogram.BaseEvent) {
    const value = String(event.currentTarget.dataset.value || "");
    const index = findLayerIndex(this.data.layerOptions, value);
    if (index < 0) return;
    this.setData({
      choosingLayerIndex: index,
      choosingLayerLabel: this.data.layerOptions[index]?.name || "",
      placeMode: "cell",
    });
  },

  onTapPlaceCell(event: WechatMiniprogram.BaseEvent) {
    const value = Number(event.currentTarget.dataset.value);
    if (!Number.isInteger(value) || value < 0 || value > 8) return;
    this.setData({
      zoneIndex: this.data.choosingZoneIndex > 0 ? this.data.choosingZoneIndex : this.data.zoneIndex,
      layerIndex: this.data.choosingLayerIndex,
      cellIndex: value,
      placeMode: "form",
    });
    this.updatePlaceText();
  },

  onClearPlace() {
    this.setData({
      zoneIndex: 0,
      layerIndex: -1,
      cellIndex: -1,
      layerOptions: [],
      choosingZoneIndex: -1,
      choosingZoneLabel: "",
      choosingLayerIndex: -1,
      choosingLayerLabel: "",
      placeMode: "form",
    });
    this.updatePlaceText();
  },

  backToPlaceZones() {
    this.setData({ placeMode: "zone" });
  },

  backToPlaceLayers() {
    this.setData({ placeMode: "layer" });
  },

  cancelPlaceSelection() {
    this.setData({
      placeMode: "form",
      choosingZoneIndex: -1,
      choosingZoneLabel: "",
      choosingLayerIndex: -1,
      choosingLayerLabel: "",
    });
  },

  onDateChange(event: WechatMiniprogram.PickerChange) {
    this.setData({ expireDate: String(event.detail.value || "") });
  },

  buildPayload(): InventoryWritePayload {
    const zone = this.data.zoneOptions[this.data.zoneIndex]?.value || "";
    const layer = this.data.layerOptions[this.data.layerIndex]?.id || "";
    const cell = this.data.cellIndex >= 0 ? this.data.cellIndex : undefined;
    const payload: InventoryWritePayload = {
      name: this.data.name.trim(),
      category: this.data.category.trim() || undefined,
      quantity: parseFloat(this.data.quantity) || 1,
      unit: this.data.unit || "份",
      ...inventoryPositionPayload(zone || undefined, layer || undefined, cell),
      expireDate: this.data.expireDate || undefined,
      source: this.data.source || "manual",
      extra: {
        ...(this.data.origin?.extra || {}),
        storedAt: this.data.storedAt.trim() || undefined,
        note: this.data.note.trim() || undefined,
      },
    };
    if (this.data.confidence) {
      const c = parseInt(this.data.confidence, 10);
      if (!Number.isNaN(c)) payload.confidence = c;
    }
    return payload;
  },

  updatePlaceText() {
    const zone = this.data.zoneOptions[this.data.zoneIndex]?.value || "";
    const layer = this.data.layerOptions[this.data.layerIndex]?.id || "";
    const cell = this.data.cellIndex >= 0 ? this.data.cellIndex : undefined;
    this.setData({
      placeText: displayInventoryPlace(zone, layer, cell),
      selectedZoneValue: zone,
      selectedLayerValue: layer,
      selectedCellValue: cell == null ? -1 : cell,
    });
  },

  async onSave() {
    if (this.data.saving) return;
    if (!this.data.name.trim()) {
      wx.showToast({ title: "请填写名称", icon: "none" });
      return;
    }
    this.setData({ saving: true });
    try {
      const payload = this.buildPayload();
      if (this.data.mode === "create") {
        await createInventoryItem(payload);
        wx.showToast({ title: "已入库", icon: "success" });
      } else {
        await updateInventoryItem(this.data.itemId, payload);
        wx.showToast({ title: "已保存", icon: "success" });
      }
      // 在线 REST 已写库 + 记同步事件 + 推设备，这里不再入队，避免重复投影和旧 payload 回放。
      // 仅刷新本地快照并触发一次设备下发即可。
      void syncNow().catch(() => {
        // 页面写入已经完成；同步失败不影响本次保存。
      });
      setTimeout(() => wx.navigateBack(), 400);
    } catch (err) {
      // v2 API 在发请求前已用同一 eventId 持久化 outbox。
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "保存失败";
      wx.showToast({ title: `${message}，已加入待同步`, icon: "none" });
    } finally {
      this.setData({ saving: false });
    }
  },

  onDelete() {
    if (this.data.mode !== "edit" || !this.data.itemId) return;
    wx.showModal({
      title: "删除条目",
      content: `确定要删除"${this.data.name}"吗？`,
      confirmText: "删除",
      confirmColor: "#d95745",
      success: async (res) => {
        if (!res.confirm) return;
        try {
          await deleteInventoryItem(this.data.itemId);
          // 在线 REST 已软删 + 记同步事件 + 推设备，不再入队，避免重复投影。
          void syncNow().catch(() => {
            // 删除后的同步失败不影响本次删除结果。
          });
          wx.showToast({ title: "已删除", icon: "success" });
          setTimeout(() => wx.navigateBack(), 400);
        } catch (err) {
          // v2 API 在发请求前已持久化删除事件。
          wx.showToast({ title: "删除失败，已加入待同步", icon: "none" });
        }
      },
    });
  },
});
