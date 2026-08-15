"""P4 v2 微信登录。

该入口只签发用户 JWT，不调用旧 v1 的 DEMO 家庭自动归并逻辑；用户与 P4 家庭的
关系只能由二维码 claim 建立，避免旧 S3/DEMO 数据污染 P4 Agent 同步域。
"""

from datetime import datetime, timedelta, timezone

import httpx
from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.db.session import get_db
from app.models.device import DeviceBinding
from app.models.home import HomeMember
from app.models.p4_sync import P4DeviceCredential
from app.models.user import User, WxSession
from app.schemas.auth import LoginData, WechatLoginRequest
from app.schemas.common import ApiResponse
from app.services.token import issue_access_token
from app.services.wx_auth import wx_auth_service


router = APIRouter()


@router.post("/wechat-login", response_model=ApiResponse[LoginData])
async def p4_wechat_login(
    payload: WechatLoginRequest,
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[LoginData]:
    """微信 code2Session 后签发只面向 P4 v2 的 JWT。"""
    try:
        wx_session = await wx_auth_service.code2session(payload.code)
    except (httpx.HTTPError, ValueError) as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    user = (
        await db.execute(
            select(User).where(User.primary_openid == wx_session.openid)
        )
    ).scalar_one_or_none()
    if user is None:
        user = User(
            primary_openid=wx_session.openid,
            display_name=payload.nickname,
            avatar_url=payload.avatar_url,
        )
        db.add(user)
        await db.flush()

    expires_at = datetime.now(timezone.utc) + timedelta(
        seconds=settings.access_token_ttl_seconds
    )
    db.add(
        WxSession(
            user_id=user.id,
            openid=wx_session.openid,
            unionid=wx_session.unionid,
            session_key_ciphertext=None,
            expires_at=expires_at,
        )
    )

    has_bound_device = (
        await db.execute(
            select(DeviceBinding.id)
            .join(HomeMember, HomeMember.home_id == DeviceBinding.home_id)
            .join(
                P4DeviceCredential,
                P4DeviceCredential.device_id == DeviceBinding.device_id,
            )
            .where(
                HomeMember.user_id == user.id,
                DeviceBinding.status == "active",
                P4DeviceCredential.active.is_(True),
            )
            .limit(1)
        )
    ).scalar_one_or_none() is not None

    token = issue_access_token(user.id, wx_session.openid)
    await db.commit()
    return ApiResponse(
        data=LoginData(
            user_id=user.id,
            openid=wx_session.openid,
            unionid=wx_session.unionid,
            access_token=token,
            expires_in=settings.access_token_ttl_seconds,
            is_placeholder_session=wx_session.is_placeholder,
            has_bound_device=has_bound_device,
        )
    )

