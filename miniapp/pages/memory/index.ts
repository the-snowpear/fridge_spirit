/**
 * Agent Memory 只读同步页。
 * 家庭记忆属于共享敏感数据，本页不提供无确认删除或批量覆盖操作。
 */
import { getAgentMemories } from "../../services/api";
import type { AgentMemoryItem } from "../../types/models";

type MemoryView = AgentMemoryItem & {
  kindText: string;
  createdText: string;
  tagText: string;
};

Page({
  data: {
    loading: false,
    errorText: "",
    filter: "all" as "all" | "structured" | "markdown",
    items: [] as MemoryView[],
    visibleItems: [] as MemoryView[],
  },

  onShow() {
    void this.load();
  },

  onPullDownRefresh() {
    void this.load().finally(() => wx.stopPullDownRefresh());
  },

  onCloudSyncApplied() {
    if (!this.data.loading) void this.load();
  },

  onTapFilter(event: WechatMiniprogram.BaseEvent) {
    const filter = String(event.currentTarget.dataset.filter || "all") as
      | "all"
      | "structured"
      | "markdown";
    this.setData({ filter });
    this.applyFilter(this.data.items, filter);
  },

  async load() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const memories = await getAgentMemories();
      const items = memories
        .filter((item) => item.text.trim())
        .map((item) => ({
          ...item,
          kindText: item.kind === "markdown" ? "长期笔记" : "结构化偏好",
          createdText: item.createdAt
            ? new Date(item.createdAt).toLocaleString()
            : "同步时间未记录",
          tagText: item.tags.length ? item.tags.join(" · ") : item.category || "未分类",
        }));
      this.setData({ items });
      this.applyFilter(items, this.data.filter);
    } catch (error) {
      this.setData({
        errorText: error instanceof Error ? error.message : "读取家庭记忆失败",
      });
    } finally {
      this.setData({ loading: false });
    }
  },

  applyFilter(items: MemoryView[], filter: "all" | "structured" | "markdown") {
    this.setData({
      visibleItems: filter === "all" ? items : items.filter((item) => item.kind === filter),
    });
  },
});
