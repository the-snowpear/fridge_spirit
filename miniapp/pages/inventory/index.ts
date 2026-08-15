/**
 * 库存三级浏览页：区域（zone）→ 层（layer）→ 3×3 格位（cell）。
 * 历史 zone + slot 数据会由服务层自动落到该区域默认层，避免升级后丢位置。
 */
import type { MiniAppInstance } from "../../app";
import { getInventory, refreshInventory } from "../../services/api";
import type { InventoryItem } from "../../types/models";
import { RequestError } from "../../utils/request";
import {
  displayInventoryPlace,
  getFridgeZones,
  getInventoryLayers,
} from "../../utils/fridgeZones";
import { cellToSlot } from "../../utils/inventoryLocation";
import { updateOfflineSnapshot } from "../../utils/localFeatures";
import { formatInventoryQuantity } from "../../utils/quantity";

type Level = "zone" | "layer" | "cell";
type ItemView = InventoryItem & { locationText: string; quantityText: string };
type SummaryView = {
  key: string;
  label: string;
  hint: string;
  count: number;
  warnCount: number;
};
type CellView = {
  cell: number;
  code: string;
  occupied: boolean;
  item?: ItemView;
  level?: "normal" | "warn" | "danger";
  expireText?: string;
};

function freshness(item: InventoryItem): "normal" | "warn" | "danger" {
  if (!item.expireDate) return "normal";
  const now = new Date();
  now.setHours(0, 0, 0, 0);
  const days = Math.round(
    (new Date(`${item.expireDate}T00:00:00`).getTime() - now.getTime()) / 86400000,
  );
  return days <= 1 ? "danger" : days <= 3 ? "warn" : "normal";
}

function expiry(item: InventoryItem): string {
  if (!item.expireDate) return "";
  const now = new Date();
  now.setHours(0, 0, 0, 0);
  const days = Math.round(
    (new Date(`${item.expireDate}T00:00:00`).getTime() - now.getTime()) / 86400000,
  );
  if (days <= 0) return "今天";
  if (days === 1) return "明天";
  return `${days}天`;
}

Page({
  data: {
    loading: false,
    refreshing: false,
    errorText: "",
    items: [] as InventoryItem[],
    level: "zone" as Level,
    activeZone: "",
    activeZoneLabel: "",
    activeLayer: "",
    activeLayerLabel: "",
    summaries: [] as SummaryView[],
    cells: [] as CellView[],
    unplaced: [] as ItemView[],
  },

  onShow() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    const pending = (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string })
      .pendingZone;
    if (pending) {
      const zone = getFridgeZones().find((item) => item.key === pending);
      if (zone) {
        this.setData({ level: "layer", activeZone: zone.key, activeZoneLabel: zone.label });
      }
      (app.globalData as MiniAppInstance["globalData"] & { pendingZone?: string }).pendingZone =
        undefined;
    }
    void this.load();
  },

  onPullDownRefresh() {
    void this.load().finally(() => wx.stopPullDownRefresh());
  },

  onCloudSyncApplied() {
    if (!this.data.loading) void this.load();
  },

  async load() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const data = await getInventory();
      this.setData({ items: data.items || [] });
      updateOfflineSnapshot({ inventoryItems: data.items || [] });
      this.renderLevel();
    } catch (error) {
      const message =
        error instanceof RequestError
          ? error.message
          : error instanceof Error
            ? error.message
            : "加载失败";
      this.setData({ errorText: message, summaries: [], cells: [], unplaced: [] });
    } finally {
      this.setData({ loading: false });
    }
  },

  renderLevel() {
    if (this.data.level === "zone") {
      const summaries = getFridgeZones().map((zone) => {
        const items = this.data.items.filter((item) => item.zoneId === zone.key);
        return {
          key: zone.key,
          label: zone.label,
          hint: zone.hint || "查看各层",
          count: items.length,
          warnCount: items.filter((item) => freshness(item) !== "normal").length,
        };
      });
      this.setData({
        summaries,
        cells: [],
        unplaced: this.toItemViews(this.data.items.filter((item) => !item.zoneId)),
      });
      return;
    }

    if (this.data.level === "layer") {
      const summaries = getInventoryLayers(this.data.activeZone).map((layer) => {
        const items = this.data.items.filter(
          (item) => item.zoneId === this.data.activeZone && item.layerId === layer.id,
        );
        return {
          key: layer.id,
          label: layer.name,
          hint: "3 × 3 格位",
          count: items.length,
          warnCount: items.filter((item) => freshness(item) !== "normal").length,
        };
      });
      this.setData({
        summaries,
        cells: [],
        unplaced: this.toItemViews(
          this.data.items.filter(
            (item) => item.zoneId === this.data.activeZone && !item.layerId,
          ),
        ),
      });
      return;
    }

    const items = this.data.items.filter(
      (item) =>
        item.zoneId === this.data.activeZone && item.layerId === this.data.activeLayer,
    );
    const occupied = new Map(items.filter((item) => item.cell != null).map((item) => [item.cell!, item]));
    this.setData({
      summaries: [],
      cells: Array.from({ length: 9 }, (_, cell): CellView => {
        const item = occupied.get(cell);
        return item
          ? {
              cell,
              code: cellToSlot(cell) || String(cell + 1),
              occupied: true,
              item: this.toItemViews([item])[0],
              level: freshness(item),
              expireText: expiry(item),
            }
          : { cell, code: cellToSlot(cell) || String(cell + 1), occupied: false };
      }),
      unplaced: this.toItemViews(items.filter((item) => item.cell == null)),
    });
  },

  toItemViews(items: InventoryItem[]): ItemView[] {
    return items.map((item) => ({
      ...item,
      locationText:
        item.location || displayInventoryPlace(item.zoneId, item.layerId, item.cell),
      quantityText: formatInventoryQuantity(item),
    }));
  },

  onTapSummary(event: WechatMiniprogram.BaseEvent) {
    const key = String(event.currentTarget.dataset.key || "");
    if (this.data.level === "zone") {
      const zone = getFridgeZones().find((item) => item.key === key);
      if (!zone) return;
      this.setData({
        level: "layer",
        activeZone: key,
        activeZoneLabel: zone.label,
        activeLayer: "",
        activeLayerLabel: "",
      });
    } else {
      const layer = getInventoryLayers(this.data.activeZone).find((item) => item.id === key);
      if (!layer) return;
      this.setData({
        level: "cell",
        activeLayer: key,
        activeLayerLabel: layer.name,
      });
    }
    this.renderLevel();
  },

  onTapBackLevel() {
    if (this.data.level === "cell") {
      this.setData({ level: "layer", activeLayer: "", activeLayerLabel: "" });
    } else if (this.data.level === "layer") {
      this.setData({ level: "zone", activeZone: "", activeZoneLabel: "" });
    }
    this.renderLevel();
  },

  onTapRootLevel() {
    if (this.data.level === "zone") return;
    this.setData({
      level: "zone",
      activeZone: "",
      activeZoneLabel: "",
      activeLayer: "",
      activeLayerLabel: "",
    });
    this.renderLevel();
  },

  onTapZoneLevel() {
    if (!this.data.activeZone || this.data.level !== "cell") return;
    this.setData({ level: "layer", activeLayer: "", activeLayerLabel: "" });
    this.renderLevel();
  },

  onTapCell(event: WechatMiniprogram.BaseEvent) {
    const cell = Number(event.currentTarget.dataset.cell);
    const item = this.data.cells.find((entry) => entry.cell === cell)?.item;
    const url = item
      ? `/pages/inventory-detail/index?id=${encodeURIComponent(item.id)}`
      : `/pages/inventory-detail/index?mode=create&zoneId=${encodeURIComponent(this.data.activeZone)}&layerId=${encodeURIComponent(this.data.activeLayer)}&cell=${cell}`;
    wx.navigateTo({ url });
  },

  onTapItem(event: WechatMiniprogram.BaseEvent) {
    wx.navigateTo({
      url: `/pages/inventory-detail/index?id=${encodeURIComponent(String(event.currentTarget.dataset.id || ""))}`,
    });
  },

  onTapScan() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    app.globalData.pendingScanZone = this.data.activeZone || undefined;
    wx.switchTab({ url: "/pages/scan/index" });
  },

  async onTapRefreshDevice() {
    if (this.data.refreshing) return;
    this.setData({ refreshing: true });
    try {
      const data = await refreshInventory();
      wx.showToast({ title: data.queued ? "已发起刷新" : "设备未在线", icon: "none" });
    } catch {
      wx.showToast({ title: "刷新失败", icon: "none" });
    } finally {
      this.setData({ refreshing: false });
    }
  },
});
