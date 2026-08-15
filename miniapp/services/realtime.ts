/** P4 v2 WebSocket：JWT 首帧鉴权、心跳、指数退避重连和 REST revision 补偿。 */
import { API_PREFIX, DEFAULT_API_CONFIG } from "../config/env";
import type { ApiConfig, AuthSession } from "../types/models";
import { getStorage } from "../utils/storage";
import { syncNow } from "./sync";

let socket: WechatMiniprogram.SocketTask | null = null;
let reconnectTimer = 0;
let heartbeatTimer = 0;
let retries = 0;
let stopped = true;
let onChanged: (() => void) | null = null;

function wsUrl(baseUrl: string): string {
  return `${baseUrl.replace(/^http/, "ws").replace(/\/+$/, "")}${API_PREFIX}/ws`;
}
function clearTimers(): void {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  if (heartbeatTimer) clearInterval(heartbeatTimer);
  reconnectTimer = 0;
  heartbeatTimer = 0;
}
function scheduleReconnect(): void {
  if (stopped || reconnectTimer) return;
  const delay = Math.min(30_000, 1000 * 2 ** Math.min(retries++, 5));
  reconnectTimer = setTimeout(() => { reconnectTimer = 0; connect(); }, delay) as unknown as number;
}
function connect(): void {
  const app = getApp<any>();
  const config = getStorage<ApiConfig>("apiConfig") || app?.globalData?.apiConfig || DEFAULT_API_CONFIG;
  const session = getStorage<AuthSession>("authSession") || app?.globalData?.session;
  if (stopped || !config?.baseUrl || !session?.token) return;
  socket = wx.connectSocket({ url: wsUrl(config.baseUrl) });
  socket.onOpen(() => socket?.send({ data: JSON.stringify({ type: "auth", token: session.token }) }));
  socket.onMessage((message) => {
    try {
      const frame = JSON.parse(String(message.data || "{}")) as { type?: string };
      if (frame.type === "auth.ok") {
        retries = 0;
        heartbeatTimer = setInterval(() => socket?.send({ data: JSON.stringify({ type: "ping" }) }), 25_000) as unknown as number;
        void syncNow().then(() => onChanged?.());
      } else if (frame.type === "sync.event" || frame.type === "sync.conflict" || frame.type === "binding.changed") {
        void syncNow().then(() => onChanged?.());
      }
    } catch { /* 忽略非 JSON 帧，等待下一条协议消息。 */ }
  });
  socket.onClose(() => { socket = null; clearTimers(); scheduleReconnect(); });
  socket.onError(() => { socket?.close({}); });
}

export function startRealtime(callback?: () => void): void {
  stopped = false;
  onChanged = callback || null;
  if (!socket) connect();
}
export function stopRealtime(): void {
  stopped = true;
  clearTimers();
  socket?.close({});
  socket = null;
}
