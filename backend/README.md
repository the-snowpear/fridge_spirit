# 冰箱小精灵 P4 Agent Backend

当前公网服务以 `/api/v2` 为 P4 Agent 唯一业务协议，提供微信登录、设备二维码绑定、12 个结构化业务域同步、MQTT v2 和小程序 WebSocket。旧 `/api/v1` 代码仍保留给仓库内既有调试页面，但不得作为 P4 固件或新版小程序的数据源。

完整接口与小程序接入示例见：

- `doc/P4_Agent_MQTT公网多端同步技术文档.md`

## 技术栈

- Python 3.11+、FastAPI、SQLAlchemy async
- PostgreSQL 16
- EMQX 5
- Nginx

## 公网部署

```bash
cd backend
cp .env.example .env
# 修改 JWT_SECRET、WX_APPID、WX_SECRET、PUBLIC_API_BASE_URL、
# MQTT_PUBLIC_BROKER_URI 和数据库口令。
docker compose up -d --build
docker compose ps
curl http://<server>:6005/healthz
```

`api` 容器会先运行 `scripts/migrate_p4_v2.py`，幂等创建 P4 v2 表，再启动 Uvicorn。

## 预置 P4 设备

```bash
docker compose exec api python scripts/provision_p4_device.py \
  --device-sn FS-P4-000001 --name "比赛设备"
```

原始 `bootstrapSecret` 只显示一次；数据库只保存摘要。把脚本输出通过固件 USB `device_provision_set` 写入设备。

## P4 v2 入口

- `POST /api/v2/auth/wechat-login`
- `POST /api/v2/device-binding/sessions`
- `GET /api/v2/device-binding/sessions/{sessionId}`
- `POST /api/v2/bindings/claim`
- `POST /api/v2/device-binding/sessions/{sessionId}/ack`
- `GET /api/v2/devices`
- `GET /api/v2/sync/snapshot`
- `GET|POST /api/v2/sync/events`
- `GET|PUT /api/v2/agent-config`
- `GET /api/v2/device-sync/snapshot`
- `POST /api/v2/device-sync/seed`
- `WS /api/v2/ws`

## 当前安全边界

比赛联调配置允许 HTTP、WS、MQTT 1883 和明文 AI Key 同步。公网正式发布前必须切换 HTTPS/WSS/MQTTS、配置 EMQX 鉴权与 ACL、关闭匿名访问、启用 NVS/Flash 安全能力，并轮换全部开发凭据。
