/** P4 二维码绑定：只接受 fridgespirit://bind?v=2 的一次性 claim URI。 */
import type { MiniAppInstance } from "../../app";
import { bindDevice, getPrimaryDevice } from "../../services/api";
import type { BindDevicePayload, DeviceSummary } from "../../types/models";
import { RequestError } from "../../utils/request";

interface BindPageData { loading: boolean; device: DeviceSummary | null; errorText: string; }

function parseBindingUri(raw: string): BindDevicePayload {
  const match = raw.trim().match(/^fridgespirit:\/\/bind\?(.+)$/i);
  if (!match) throw new Error("不是冰箱小精灵 P4 绑定二维码");
  const params: Record<string, string> = {};
  for (const pair of match[1].split("&")) {
    const [key, value = ""] = pair.split("=");
    params[decodeURIComponent(key)] = decodeURIComponent(value);
  }
  if (params.v !== "2" || !params.sessionId || !params.claimToken || params.claimToken.length < 32) {
    throw new Error("二维码版本或一次性凭据无效");
  }
  return { sessionId: params.sessionId, claimToken: params.claimToken };
}

Page({
  data: { loading: false, device: null, errorText: "" } as BindPageData,
  async onShow() {
    const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
    this.setData({ device: app.globalData.activeDevice });
    try {
      const device = await getPrimaryDevice();
      if (device) { app.setActiveDevice(device); this.setData({ device }); }
    } catch { /* 未绑定或离线时保持空态。 */ }
  },
  scanBindingQr() {
    wx.scanCode({
      onlyFromCamera: true,
      scanType: ["qrCode"],
      success: (res) => { void this.submit(res.result); },
      fail: () => wx.showToast({ title: "扫码已取消", icon: "none" }),
    });
  },
  async submit(raw: string) {
    if (this.data.loading) return;
    this.setData({ loading: true, errorText: "" });
    try {
      const device = await bindDevice(parseBindingUri(raw));
      const app = getApp<MiniAppInstance["globalData"]>() as unknown as MiniAppInstance;
      app.setActiveDevice(device);
      void app.bootstrap(true);
      this.setData({ device });
      wx.showToast({ title: "绑定成功", icon: "success" });
      setTimeout(() => wx.switchTab({ url: "/pages/home/index" }), 600);
    } catch (err) {
      const message = err instanceof RequestError || err instanceof Error ? err.message : "绑定失败";
      this.setData({ errorText: message });
      wx.showToast({ title: "绑定失败", icon: "none" });
    } finally { this.setData({ loading: false }); }
  },
});
