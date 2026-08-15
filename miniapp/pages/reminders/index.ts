/**
 * 提醒页：列出当前 home 待处理提醒；点击确认（POST /reminders/{id}/confirm，status=acked）。
 *
 * - 数据来源：getReminders() 返回 ReminderListData.items；
 * - 展示 pending / triggered；完成写 completed，忽略写 cancelled，与 P4 域一致；
 * - 删除后用 setData 局部更新，不重新整页拉。
 */

import { confirmReminder, getReminders } from "../../services/api";
import { syncNow } from "../../services/sync";
import type { ReminderItem } from "../../types/models";
import { RequestError } from "../../utils/request";
import { updateOfflineSnapshot } from "../../utils/localFeatures";

type ReminderViewItem = ReminderItem & {
  dueText: string;
  levelText: string;
  tagClass: string;
  confirming?: boolean;
};

function toViewItem(item: ReminderItem): ReminderViewItem {
  let levelText = "提示";
  let tagClass = "mint";
  if (item.reminderType === "expire_soon") {
    levelText = "临期";
    tagClass = "danger";
  } else if (item.reminderType === "low_stock") {
    levelText = "低库存";
    tagClass = "warn";
  } else if (item.reminderType === "device_offline") {
    levelText = "设备离线";
    tagClass = "warn";
  } else if (item.reminderType === "scan_pending") {
    levelText = "待处理";
    tagClass = "warn";
  }
  return {
    ...item,
    dueText: item.dueAt ? item.dueAt.slice(0, 16).replace("T", " ") : "无截止",
    levelText,
    tagClass,
  };
}

Page({
  data: {
    items: [] as ReminderViewItem[],
    loading: false,
    errorText: "",
  },

  onShow() {
    this.load();
  },

  onPullDownRefresh() {
    this.load().finally(() => wx.stopPullDownRefresh());
  },

  onCloudSyncApplied() {
    if (this.data.loading) return;
    void this.load();
  },

  async load() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const data = await getReminders();
      updateOfflineSnapshot({ reminderItems: data.items || [] });
      this.setData({
        items: (data.items || [])
          .filter((it) => it.status === "pending" || it.status === "triggered")
          .map(toViewItem),
      });
    } catch (err) {
      const message =
        err instanceof RequestError
          ? err.message
          : err instanceof Error
            ? err.message
            : "加载失败";
      this.setData({ errorText: message, items: [] });
    } finally {
      this.setData({ loading: false });
    }
  },

  async onConfirm(event: WechatMiniprogram.BaseEvent) {
    await this.actOn(event, "completed");
  },

  async onDismiss(event: WechatMiniprogram.BaseEvent) {
    await this.actOn(event, "cancelled");
  },

  async actOn(event: WechatMiniprogram.BaseEvent, status: "completed" | "cancelled") {
    const id = event.currentTarget.dataset.id as string;
    this.markConfirming(id, true);
    try {
      await confirmReminder(id, status);
      // 在线 REST 已写库 + 记同步事件 + 推设备，不再入队，避免重复投影。
      void syncNow().catch(() => {
        // 提醒操作已提交；同步失败不影响本次确认结果。
      });
      this.setData({ items: this.data.items.filter((x) => x.id !== id) });
      wx.showToast({
        title: status === "completed" ? "已完成" : "已忽略",
        icon: "success",
      });
    } catch (err) {
      // v2 API 已用原 eventId 持久化 outbox。
      this.setData({ items: this.data.items.filter((x) => x.id !== id) });
      wx.showToast({ title: "已加入待同步", icon: "none" });
    }
  },

  markConfirming(id: string, on: boolean) {
    this.setData({
      items: this.data.items.map((it) =>
        it.id === id ? { ...it, confirming: on } : it,
      ),
    });
  },
});
