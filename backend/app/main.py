"""FastAPI application entrypoint."""

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy import text

from app.api.v1.router import api_router
from app.api.v2.router import api_v2_router
from app.core.config import settings
from app.core.lifespan import lifespan
from app.db.session import AsyncSessionLocal
from app.services.mqtt_client import mqtt_client
from app.services.p4_realtime import p4_realtime_hub


def create_app() -> FastAPI:
    """创建 FastAPI 应用，并集中挂载中间件、健康检查和 v1 路由。"""
    app = FastAPI(
        title=settings.app_name,
        version="0.1.0",
        debug=settings.app_debug,
        lifespan=lifespan,
    )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=settings.cors_origins,
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    @app.get("/healthz", tags=["health"])
    async def healthz() -> dict:
        """检查 API、PostgreSQL、MQTT 与 P4 v2 schema。"""
        database_ok = False
        schema_ok = False
        try:
            async with AsyncSessionLocal() as db:
                await db.execute(text("SELECT 1"))
                database_ok = True
                schema_ok = bool(
                    (
                        await db.execute(
                            text("SELECT to_regclass('public.p4_sync_events') IS NOT NULL")
                        )
                    ).scalar_one()
                )
        except Exception:  # noqa: BLE001 - 健康响应必须可用
            pass
        mqtt_ok = mqtt_client.is_connected()
        return {
            "status": "ok" if database_ok and schema_ok and mqtt_ok else "degraded",
            "service": settings.app_name,
            "env": settings.app_env,
            "database": database_ok,
            "mqtt": mqtt_ok,
            "p4Schema": schema_ok,
            "websocketConnections": p4_realtime_hub.connection_count(),
        }

    app.include_router(api_router, prefix=settings.api_v1_prefix)
    app.include_router(api_v2_router, prefix=settings.api_v2_prefix)
    return app


app = create_app()
