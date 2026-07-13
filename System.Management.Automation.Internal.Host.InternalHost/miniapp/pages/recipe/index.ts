/**
 * AI 菜谱页：复用现有 /ai/chat，根据库存生成推荐。
 *
 * 后端暂未提供 recipe 表，因此本页把模型结果缓存到本地；缺少食材可写入本地购物清单。
 */

import { getInventory, sendAiChat } from "../../services/api";
import { enqueueSyncOp, syncNow } from "../../services/sync";
import type { InventoryItem, RecipeRecommendation, ShoppingItem } from "../../types/models";
import { addShoppingItems, getRecipeRecommendations, saveRecipeRecommendations, makeLocalId } from "../../utils/localFeatures";
import { formatInventoryQuantity } from "../../utils/quantity";

type RecipeView = RecipeRecommendation & {
  missingCount: number;
  ingredientText: string;
};

Page({
  data: {
    loading: false,
    errorText: "",
    summary: {
      kicker: "今晚吃什么",
      title: "按库存生成",
      hint: "优先使用临期食材，减少浪费。",
      duration: "AI 推荐",
    },
    cards: [] as RecipeView[],
    selected: null as RecipeView | null,
    rawReply: "",
  },

  onShow() {
    const cached = getRecipeRecommendations();
    if (cached.length) this.applyCards(cached);
  },

  onCloudSyncApplied() {
    if (this.data.loading) return;
    const cached = getRecipeRecommendations();
    this.applyCards(cached);
  },

  async onTapRefresh() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "", rawReply: "" });
    try {
      const inventory = await getInventory();
      const prompt = buildRecipePrompt(inventory.items || []);
      const reply = await sendAiChat(prompt, "miniapp-recipe");
      const cards = parseRecipeReply(reply.reply);
      saveRecipeRecommendations(cards);
      this.queueRecipeSync(cards);
      this.applyCards(cards);
      this.setData({ rawReply: reply.reply });
      wx.showToast({ title: "已生成菜谱", icon: "success" });
    } catch (err) {
      const message = err instanceof Error ? err.message : "生成失败";
      this.setData({ errorText: message });
      wx.showToast({ title: "生成失败", icon: "none" });
    } finally {
      this.setData({ loading: false });
    }
  },

  applyCards(cards: RecipeRecommendation[]) {
    const normalized = cards.map((card, index) => normalizeRecipe(card, index));
    const views: RecipeView[] = normalized.map((card) => ({
      ...card,
      missingCount: card.ingredients.filter((it) => it.missing).length,
      ingredientText: card.ingredients.map((it) => `${it.name}${it.missing ? "（缺）" : ""}`).join("、"),
    }));
    const first = views[0];
    this.setData({
      cards: views,
      summary: first
        ? {
            kicker: "今晚吃什么",
            title: first.title,
            hint: first.desc,
            duration: first.durationText,
          }
        : this.data.summary,
    });
  },

  onTapCard(e: WechatMiniprogram.BaseEvent) {
    const id = e.currentTarget.dataset.id as string;
    const selected = this.data.cards.find((card) => card.id === id) || null;
    this.setData({ selected });
  },

  closeDetail() {
    this.setData({ selected: null });
  },

  addMissingToShopping(e: WechatMiniprogram.BaseEvent) {
    const id = (e.currentTarget.dataset.id as string) || this.data.selected?.id || "";
    const card = this.data.cards.find((it) => it.id === id) || this.data.selected;
    if (!card) return;
    const missing = card.ingredients.filter((it) => it.missing);
    if (!missing.length) {
      wx.showToast({ title: "没有缺少食材", icon: "none" });
      return;
    }
    const shoppingItems = addShoppingItems(
      missing.map((it) => ({
        name: it.name,
        quantityText: it.quantityText || "1 份",
        source: "recipe",
        sourceText: `菜谱缺料 · ${card.title}`,
      })),
    );
    this.queueShoppingSync(shoppingItems);
    wx.showToast({ title: `已加入 ${missing.length} 项`, icon: "success" });
  },

  queueRecipeSync(cards: RecipeRecommendation[]) {
    enqueueSyncOp("recipe_cache", "replace", {
      schema_version: 1,
      source: "miniapp_ai",
      generated_at: Math.floor(Date.now() / 1000),
      items: cards.map(toDeviceRecipeItem),
      updatedAt: new Date().toISOString(),
    });
    void syncNow().catch(() => {
      // 菜谱缓存保留本地可用，同步失败时队列继续等待。
    });
  },

  queueShoppingSync(items: ShoppingItem[]) {
    enqueueSyncOp("shopping_list", "recipe_add", {
      items,
      updatedAt: new Date().toISOString(),
    });
    void syncNow().catch(() => {
      // 缺料清单已经写入本地，同步失败不阻塞用户。
    });
  },

  noop() {
    // 用于阻止 sheet 点击冒泡。
  },
});

function buildRecipePrompt(items: InventoryItem[]): string {
  const today = new Date().toISOString().slice(0, 10);
  const inventoryText = items.length
    ? items
        .slice(0, 30)
        .map((it) => `${it.name} ${formatInventoryQuantity(it)}${it.expireDate ? ` 到期:${it.expireDate}` : ""}${it.category ? ` 类别:${it.category}` : ""}`)
        .join("\n")
    : "暂无库存";
  return [
    "你是冰箱小精灵的小程序菜谱助手。",
    `今天是 ${today}。请基于库存生成 2 到 3 个中文家常菜推荐，优先使用临期食材。`,
    "请只输出 JSON 数组，不要 markdown，不要额外解释。",
    "数组元素字段：title, desc, durationText, tags(string[]), ingredients([{name,quantityText,missing}]), steps(string[])。",
    "missing=true 表示库存中缺少但做菜需要购买的食材。",
    "库存：",
    inventoryText,
  ].join("\n");
}

function parseRecipeReply(reply: string): RecipeRecommendation[] {
  const jsonText = extractJsonArray(reply);
  if (jsonText) {
    try {
      const parsed = JSON.parse(jsonText) as Array<Partial<RecipeRecommendation>>;
      const cards = parsed.map((item, index) => normalizeRecipe(item, index)).filter((it) => it.title);
      if (cards.length) return cards;
    } catch {
      // 兼容非严格 JSON，落到纯文本卡片。
    }
  }
  return [
    {
      id: makeLocalId("recipe"),
      title: "AI 推荐结果",
      desc: reply.slice(0, 90) || "暂未生成有效菜谱。",
      durationText: "约 20 分钟",
      tags: ["AI 生成"],
      ingredients: [],
      steps: reply ? reply.split(/\n+/).filter(Boolean).slice(0, 6) : ["请稍后重试。"],
      rawText: reply,
    },
  ];
}

function extractJsonArray(text: string): string {
  const objectText = extractJsonObject(text);
  if (objectText) {
    try {
      const parsed = JSON.parse(objectText) as { items?: unknown; recipes?: unknown };
      const items = Array.isArray(parsed.items) ? parsed.items : parsed.recipes;
      if (Array.isArray(items)) {
        return JSON.stringify(items);
      }
    } catch {
      // 继续兼容直接数组输出。
    }
  }
  const start = text.indexOf("[");
  const end = text.lastIndexOf("]");
  return start >= 0 && end > start ? text.slice(start, end + 1) : "";
}

function extractJsonObject(text: string): string {
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  return start >= 0 && end > start ? text.slice(start, end + 1) : "";
}

function normalizeRecipe(item: Partial<RecipeRecommendation>, index: number): RecipeRecommendation {
  const raw = item as Partial<RecipeRecommendation> & {
    tag?: unknown;
    time_minutes?: unknown;
    timeMinutes?: unknown;
    reason?: unknown;
    use_inventory?: unknown;
    useInventory?: unknown;
    missing?: unknown;
  };
  const useInventory = normalizeStringArray(raw.use_inventory || raw.useInventory);
  const missing = normalizeStringArray(raw.missing);
  const steps = Array.isArray(item.steps) ? item.steps.map(String).filter(Boolean).slice(0, 8) : [];
  return {
    id: item.id || makeLocalId(`recipe${index}`),
    title: String(item.title || `推荐菜 ${index + 1}`),
    desc: String(item.desc || raw.reason || "基于当前库存生成。"),
    durationText: String(item.durationText || (raw.time_minutes || raw.timeMinutes ? `约 ${raw.time_minutes || raw.timeMinutes} 分钟` : "约 20 分钟")),
    tags: Array.isArray(item.tags) ? item.tags.map(String).slice(0, 4) : [String(raw.tag || "家常")],
    ingredients: Array.isArray(item.ingredients)
      ? item.ingredients.map((it) => ({
          name: String(it.name || ""),
          quantityText: String(it.quantityText || "适量"),
          missing: !!it.missing,
        })).filter((it) => it.name)
      : [
          ...useInventory.map((name) => ({ name, quantityText: "库存", missing: false })),
          ...missing.map((name) => ({ name, quantityText: "需补充", missing: true })),
        ],
    steps: steps.length ? steps : [String(raw.reason || "按日常做法完成烹饪。")],
    rawText: item.rawText,
  };
}

function normalizeStringArray(value: unknown): string[] {
  if (!Array.isArray(value)) return [];
  return value.map(String).map((it) => it.trim()).filter(Boolean).slice(0, 8);
}

function parseDurationMinutes(text: string): number {
  const match = text.match(/(\d+)/);
  return match ? Number(match[1]) : 20;
}

function toDeviceRecipeItem(card: RecipeRecommendation): Record<string, unknown> {
  const available = card.ingredients.filter((it) => !it.missing).map((it) => it.name);
  const missing = card.ingredients.filter((it) => it.missing).map((it) => it.name);
  return {
    title: card.title,
    tag: card.tags[0] || "AI 推荐",
    time_minutes: parseDurationMinutes(card.durationText),
    use_inventory: available.length ? available : ["按当前库存选择"],
    missing,
    steps: card.steps.slice(0, 4),
    reason: card.desc,
  };
}
