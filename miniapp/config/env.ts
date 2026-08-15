/**
 * miniapp 默认环境配置。
 *
 * - baseUrl 默认指向当前联调服务器；正式发布前需要配置合法域名 + HTTPS。
 * - timeoutMs 是"普通请求"的默认值；AI / scan 类长接口由 request 层临时放大。
 * - mockEnabled 默认关：backend 不可达时，由各页面 catch 后展示骨架占位，
 *   不再静默落回 mock 数据，避免开发者误以为后端在跑。
 */

import type { ApiConfig } from "../types/models";

export const DEFAULT_API_CONFIG: ApiConfig = {
  baseUrl: "http://165.154.23.36:6005",
  timeoutMs: 15000,
  mockEnabled: false,
};

/** P4 Agent 对外接口统一使用 v2；旧 S3 `/api/v1` 不再兼容。 */
export const API_PREFIX = "/api/v2";

/** AI / Scan 类长接口的临时超时；超过普通 15s，避免设备转发或视觉模型还没回就被掐。 */
export const LONG_REQUEST_TIMEOUT_MS = 60_000;

/** 默认家庭名（首屏占位用）。 */
export const DEFAULT_HOME_NAME = "我的冰箱";
