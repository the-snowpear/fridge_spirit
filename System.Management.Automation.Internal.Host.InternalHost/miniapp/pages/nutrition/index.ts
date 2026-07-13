/**
 * 营养助手页：对齐固件 kitchen_nutrition 缓存结构。
 *
 * 页面优先展示云端同步的 nutrition_cache；用户也可以基于当前库存即时生成，
 * 生成结果会写入本地缓存并通过三端同步队列推送到云端和设备。
 */

import { getInventory, sendAiChat } from "../../services/api";
import { enqueueSyncOp, syncNow } from "../../services/sync";
import type { InventoryItem, NutritionCache } from "../../types/models";
import { getNutritionCache, saveNutritionCache } from "../../utils/localFeatures";
import { formatInventoryQuantity } from "../../utils/quantity";

type NutritionView = {
  score: number;
  summary: string;
  focus: string[];
  suggestions: string[];
  restrictNote: string;
  generatedText: string;
};

Page({
  data: {
    loading: false,
    errorText: "",
    view: buildFallbackView(null as NutritionCache | null),
    rawReply: "",
  },

  onShow() {
    this.applyCache(getNutritionCache());
  },

  onCloudSyncApplied() {
    if (this.data.loading) return;
    this.applyCache(getNutritionCache());
  },

  applyCache(cache: NutritionCache | null) {
    this.setData({ view: buildFallbackView(cache) });
  },

  async onTapRefresh() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "", rawReply: "" });
    try {
      const inventory = await getInventory();
      const prompt = buildNutritionPrompt(inventory.items || []);
      const reply = await sendAiChat(prompt, "miniapp-nutrition");
      const cache = parseNutritionReply(reply.reply);
      saveNutritionCache(cache);
      this.queueNutritionSync(cache);
      this.applyCache(cache);
      this.setData({ rawReply: reply.reply });
      wx.showToast({ title: "已生成建议", icon: "success" });
    } catch (err) {
      const message = err instanceof Error ? err.message : "生成失败";
      this.setData({ errorText: message });
      wx.showToast({ title: "生成失败", icon: "none" });
    } finally {
      this.setData({ loading: false });
    }
  },

  queueNutritionSync(cache: NutritionCache) {
    enqueueSyncOp("nutrition_cache", "replace", {
      schema_version: cache.schemaVersion || 1,
      source: "miniapp_ai",
      generated_at: cache.generatedAt || Math.floor(Date.now() / 1000),
      nutrition: {
        score: cache.nutrition.score || 0,
        summary: cache.nutrition.summary || "",
        focus: cache.nutrition.focus || [],
        suggestions: cache.nutrition.suggestions || [],
        restrict_note: cache.nutrition.restrictNote || cache.nutrition.restrict_note || "仅供日常膳食参考，非医疗建议。",
      },
      updatedAt: new Date().toISOString(),
    });
    void syncNow().catch(() => {
      // 营养建议本地可用，同步失败时保留队列，下一轮前台同步继续推送。
    });
  },
});

function buildNutritionPrompt(items: InventoryItem[]): string {
  const today = new Date().toISOString().slice(0, 10);
  const inventoryText = items.length
    ? items
        .slice(0, 40)
        .map((it) => `${it.name} ${formatInventoryQuantity(it)}${it.expireDate ? ` 到期:${it.expireDate}` : ""}${it.category ? ` 类别:${it.category}` : ""}`)
        .join("\n")
    : "暂无库存";
  return [
    "你是冰箱小精灵的小程序营养助手，只给日常膳食建议，不做医疗诊断。",
    `今天是 ${today}。请基于库存生成营养助手页 JSON。`,
    "请只输出 JSON object，不要 markdown，不要额外解释。",
    "根对象字段固定：schema_version, generated_at, nutrition。",
    "nutrition 字段：score(0-100), summary, focus(string[] 1-3条), suggestions(string[] 1-3条), restrict_note。",
    "summary 控制在 32 个中文字符内；restrict_note 必须说明仅供日常参考，非医疗建议。",
    "库存：",
    inventoryText,
  ].join("\n");
}

function parseNutritionReply(reply: string): NutritionCache {
  const jsonText = extractJsonObject(reply);
  if (jsonText) {
    try {
      const parsed = JSON.parse(jsonText) as Partial<NutritionCache> & {
        schema_version?: number;
        generated_at?: number;
        nutrition?: Record<string, unknown>;
      };
      const nutrition = parsed.nutrition || {};
      return {
        schemaVersion: Number(parsed.schemaVersion || parsed.schema_version || 1),
        source: "miniapp_ai",
        generatedAt: Number(parsed.generatedAt || parsed.generated_at || Math.floor(Date.now() / 1000)),
        nutrition: {
          score: clampScore(nutrition.score),
          summary: String(nutrition.summary || "按当前库存生成了今日营养建议。"),
          focus: normalizeStringArray(nutrition.focus),
          suggestions: normalizeStringArray(nutrition.suggestions),
          restrictNote: String(nutrition.restrictNote || nutrition.restrict_note || "仅供日常膳食参考，非医疗建议。"),
        },
        updatedAt: new Date().toISOString(),
      };
    } catch {
      // 非严格 JSON 时落入文本兜底。
    }
  }
  return {
    schemaVersion: 1,
    source: "miniapp_ai",
    generatedAt: Math.floor(Date.now() / 1000),
    nutrition: {
      score: 60,
      summary: reply.slice(0, 48) || "暂未生成有效建议。",
      focus: ["补齐蔬果"],
      suggestions: reply ? reply.split(/\n+/).filter(Boolean).slice(0, 3) : ["请稍后重试。"],
      restrictNote: "仅供日常膳食参考，非医疗建议。",
    },
    updatedAt: new Date().toISOString(),
  };
}

function buildFallbackView(cache: NutritionCache | null): NutritionView {
  const nutrition = cache?.nutrition || {};
  const generated = cache?.generatedAt ? new Date(cache.generatedAt * 1000).toLocaleString() : "等待生成";
  return {
    score: clampScore(nutrition.score),
    summary: nutrition.summary || "按库存、临期和偏好生成每日膳食提醒。",
    focus: normalizeStringArray(nutrition.focus).length ? normalizeStringArray(nutrition.focus) : ["登记食材后更准确"],
    suggestions: normalizeStringArray(nutrition.suggestions).length ? normalizeStringArray(nutrition.suggestions) : ["优先消耗 3 天内到期食材", "每餐尽量补一份蔬果"],
    restrictNote: nutrition.restrictNote || nutrition.restrict_note || "仅供日常膳食参考，非医疗建议。",
    generatedText: generated,
  };
}

function extractJsonObject(text: string): string {
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  return start >= 0 && end > start ? text.slice(start, end + 1) : "";
}

function normalizeStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value.map(String).map((it) => it.trim()).filter(Boolean).slice(0, 4);
}

function clampScore(value: unknown): number {
  const parsed = Number(value || 0);
  if (!Number.isFinite(parsed)) return 0;
  return Math.max(0, Math.min(100, Math.round(parsed)));
}
