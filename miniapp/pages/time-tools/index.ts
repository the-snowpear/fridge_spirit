/**
 * P4 计时工具同步视图。
 * 计时器和秒表不会逐秒同步，页面只根据最近一次状态与本机时间派生显示，不回写设备。
 */
import { getTimeTools } from "../../services/api";
import type { AlarmItem, StopwatchItem, TimerItem } from "../../types/models";

type TimerView = TimerItem & { remainingText: string; stateText: string };
type AlarmView = AlarmItem & { timeText: string; repeatText: string; stateText: string };
type StopwatchView = StopwatchItem & { elapsedText: string; stateText: string; lapTexts: string[] };

function durationText(ms: number): string {
  const safe = Math.max(0, Math.floor(ms));
  const hours = Math.floor(safe / 3600000);
  const minutes = Math.floor((safe % 3600000) / 60000);
  const seconds = Math.floor((safe % 60000) / 1000);
  return hours > 0
    ? `${String(hours).padStart(2, "0")}:${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`
    : `${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`;
}

function timerRemaining(item: TimerItem): number {
  if (item.state === "done" || item.state === "triggered") return 0;
  if (item.dueAt && item.state !== "paused") return Math.max(0, item.dueAt - Date.now());
  const runningElapsed =
    item.state === "running" && item.startedAt ? Math.max(0, Date.now() - item.startedAt) : 0;
  return Math.max(0, item.durationMs - item.accumulatedMs - runningElapsed);
}

function stateText(value: string): string {
  const labels: Record<string, string> = {
    pending: "等待中",
    running: "进行中",
    paused: "已暂停",
    done: "已完成",
    triggered: "已到时",
    cancelled: "已取消",
    canceled: "已取消",
    idle: "未开始",
  };
  return labels[value] || value || "未知";
}

function repeatText(mask: number, onceAt?: number): string {
  if (!mask) {
    return onceAt ? new Date(onceAt).toLocaleString() : "仅一次";
  }
  if (mask === 31) return "工作日";
  if (mask === 127) return "每天";
  const names = ["一", "二", "三", "四", "五", "六", "日"];
  const selected = names.filter((_, index) => (mask & (1 << index)) !== 0);
  return `周${selected.join("、")}`;
}

Page({
  data: {
    loading: false,
    errorText: "",
    tab: "timers" as "timers" | "alarms" | "stopwatch",
    timers: [] as TimerView[],
    alarms: [] as AlarmView[],
    stopwatch: null as StopwatchView | null,
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

  onTapTab(event: WechatMiniprogram.BaseEvent) {
    this.setData({ tab: String(event.currentTarget.dataset.tab || "timers") });
  },

  async load() {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const data = await getTimeTools();
      this.setData({
        timers: data.timers.map((item) => ({
          ...item,
          remainingText: durationText(timerRemaining(item)),
          stateText: stateText(item.state),
        })),
        alarms: data.alarms.map((item) => ({
          ...item,
          timeText: `${String(item.hour).padStart(2, "0")}:${String(item.minute).padStart(2, "0")}`,
          repeatText: repeatText(item.weekdayMask, item.onceAt),
          stateText: item.enabled ? "已开启" : "已关闭",
        })),
        stopwatch: data.stopwatch
          ? {
              ...data.stopwatch,
              elapsedText: durationText(data.stopwatch.accumulatedMs),
              stateText: stateText(data.stopwatch.state),
              lapTexts: data.stopwatch.lapsMs.map(durationText),
            }
          : null,
      });
    } catch (error) {
      this.setData({
        errorText: error instanceof Error ? error.message : "读取同步数据失败",
      });
    } finally {
      this.setData({ loading: false });
    }
  },
});
