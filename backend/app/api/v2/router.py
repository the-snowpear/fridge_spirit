"""P4 Agent v2 路由总入口。"""

from fastapi import APIRouter

from app.api.v2.auth import router as auth_router
from app.api.v2.routes import router as p4_router

api_v2_router = APIRouter()
api_v2_router.include_router(auth_router, prefix="/auth", tags=["p4-auth"])
api_v2_router.include_router(p4_router)
