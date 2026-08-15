"""P4 设备预置、二维码绑定和设备令牌校验。"""

from __future__ import annotations

import hashlib
import hmac
import secrets
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from uuid import UUID

from fastapi import HTTPException
from sqlalchemy import select, update
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.models.device import Device, DeviceBinding
from app.models.home import Home, HomeMember
from app.models.p4_sync import P4BindingSession, P4DeviceCredential
from app.models.user import User


def _now() -> datetime:
    return datetime.now(timezone.utc)


def secret_hash(value: str) -> str:
    """统一摘要格式；日志和响应均不得输出原始 secret。"""
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def secret_matches(value: str, expected_hash: str) -> bool:
    return hmac.compare_digest(secret_hash(value), expected_hash)


@dataclass(frozen=True)
class DevicePrincipal:
    device: Device
    credential: P4DeviceCredential
    binding: DeviceBinding


def credential_needs_rotation(
    credential: P4DeviceCredential,
    *,
    has_active_binding: bool,
) -> bool:
    """判断 claimed 会话是否必须交付一套新的完整设备凭据。"""
    return (
        not has_active_binding
        or not credential.http_token_hash
        or not credential.mqtt_username
        or not credential.mqtt_password
    )


async def authenticate_bootstrap(
    db: AsyncSession,
    *,
    device_sn: str,
    key_id: str,
    secret: str,
) -> tuple[Device, P4DeviceCredential]:
    row = (
        await db.execute(
            select(Device, P4DeviceCredential)
            .join(P4DeviceCredential, P4DeviceCredential.device_id == Device.id)
            .where(
                Device.device_sn == device_sn,
                P4DeviceCredential.bootstrap_key_id == key_id,
                P4DeviceCredential.active.is_(True),
            )
        )
    ).one_or_none()
    if row is None or not secret_matches(secret, row[1].bootstrap_secret_hash):
        raise HTTPException(status_code=401, detail="invalid device bootstrap credential")
    return row[0], row[1]


async def authenticate_device_token(
    db: AsyncSession,
    *,
    token: str,
) -> DevicePrincipal:
    token_digest = secret_hash(token)
    row = (
        await db.execute(
            select(Device, P4DeviceCredential, DeviceBinding)
            .join(P4DeviceCredential, P4DeviceCredential.device_id == Device.id)
            .join(
                DeviceBinding,
                (DeviceBinding.device_id == Device.id) & (DeviceBinding.status == "active"),
            )
            .where(
                P4DeviceCredential.http_token_hash == token_digest,
                P4DeviceCredential.active.is_(True),
            )
            .order_by(DeviceBinding.created_at.asc())
            .limit(1)
        )
    ).one_or_none()
    if row is None:
        raise HTTPException(status_code=401, detail="invalid device token")
    return DevicePrincipal(device=row[0], credential=row[1], binding=row[2])


async def create_binding_session(
    db: AsyncSession,
    *,
    device: Device,
) -> tuple[P4BindingSession, str]:
    now = _now()
    # 清除超时会话中的一次性明文交付数据，避免长期残留。
    await db.execute(
        update(P4BindingSession)
        .where(
            P4BindingSession.expires_at <= now,
            P4BindingSession.status.in_(["pending", "claimed"]),
        )
        .values(status="expired", delivery={})
    )
    await db.execute(
        update(P4BindingSession)
        .where(
            P4BindingSession.device_id == device.id,
            P4BindingSession.status == "pending",
        )
        .values(status="expired")
    )
    token = secrets.token_urlsafe(32)
    session = P4BindingSession(
        device_id=device.id,
        token_hash=secret_hash(token),
        expires_at=now + timedelta(seconds=settings.p4_binding_ttl_seconds),
        status="pending",
        delivery={},
    )
    db.add(session)
    await db.commit()
    await db.refresh(session)
    return session, token


async def cleanup_expired_binding_sessions(db: AsyncSession) -> int:
    """周期清除超时二维码及其中的一次性明文交付数据。"""
    result = await db.execute(
        update(P4BindingSession)
        .where(
            P4BindingSession.expires_at <= _now(),
            P4BindingSession.status.in_(["pending", "claimed"]),
        )
        .values(status="expired", delivery={})
    )
    await db.commit()
    return int(result.rowcount or 0)


async def _user_home(db: AsyncSession, user: User) -> Home:
    """v2 不使用旧比赛 demo 强制共享家庭逻辑。"""
    home = (
        await db.execute(
            select(Home)
            .join(HomeMember, HomeMember.home_id == Home.id)
            .join(DeviceBinding, DeviceBinding.home_id == Home.id)
            .join(P4DeviceCredential, P4DeviceCredential.device_id == DeviceBinding.device_id)
            .where(
                HomeMember.user_id == user.id,
                DeviceBinding.status == "active",
                P4DeviceCredential.active.is_(True),
            )
            .order_by(Home.created_at.asc())
            .limit(1)
        )
    ).scalar_one_or_none()
    if home is not None:
        return home
    home = Home(name="我的 P4 Agent 家庭", owner_user_id=user.id)
    db.add(home)
    await db.flush()
    db.add(HomeMember(home_id=home.id, user_id=user.id, role="member"))
    return home


async def claim_binding_session(
    db: AsyncSession,
    *,
    session_id: UUID,
    token: str,
    user: User,
) -> tuple[P4BindingSession, Device, P4DeviceCredential]:
    session = (
        await db.execute(
            select(P4BindingSession)
            .where(P4BindingSession.id == session_id)
            .with_for_update()
        )
    ).scalar_one_or_none()
    if session is None:
        raise HTTPException(status_code=404, detail="binding session not found")
    if not secret_matches(token, session.token_hash):
        raise HTTPException(status_code=401, detail="invalid binding token")
    now = _now()
    if session.status in {"claimed", "acked"}:
        if session.claimed_by_user_id == user.id:
            device = await db.get(Device, session.device_id)
            credential = (
                await db.execute(
                    select(P4DeviceCredential).where(P4DeviceCredential.device_id == session.device_id)
                )
            ).scalar_one()
            return session, device, credential
        raise HTTPException(status_code=409, detail="binding token already used")
    if session.expires_at <= now:
        session.status = "expired"
        session.delivery = {}
        await db.commit()
        raise HTTPException(status_code=410, detail="binding token expired")
    if session.status != "pending":
        raise HTTPException(status_code=409, detail=f"binding session is {session.status}")

    device = await db.get(Device, session.device_id)
    credential = (
        await db.execute(
            select(P4DeviceCredential).where(P4DeviceCredential.device_id == session.device_id)
        )
    ).scalar_one()
    active_binding = (
        await db.execute(
            select(DeviceBinding)
            .where(DeviceBinding.device_id == device.id, DeviceBinding.status == "active")
            .order_by(DeviceBinding.created_at.asc())
            .limit(1)
        )
    ).scalar_one_or_none()
    if active_binding is None:
        home = await _user_home(db, user)
        existing = (
            await db.execute(
                select(DeviceBinding).where(
                    DeviceBinding.device_id == device.id,
                    DeviceBinding.home_id == home.id,
                )
            )
        ).scalar_one_or_none()
        if existing is None:
            active_binding = DeviceBinding(
                device_id=device.id,
                home_id=home.id,
                bound_by_user_id=user.id,
                status="active",
            )
            db.add(active_binding)
        else:
            existing.status = "active"
            existing.unbound_at = None
            existing.bound_by_user_id = user.id
            active_binding = existing
    else:
        home = await db.get(Home, active_binding.home_id)

    member = (
        await db.execute(
            select(HomeMember).where(
                HomeMember.home_id == home.id,
                HomeMember.user_id == user.id,
            )
        )
    ).scalar_one_or_none()
    if member is None:
        db.add(HomeMember(home_id=home.id, user_id=user.id, role="member"))

    # 首次绑定签发设备凭据；后续扫码仅为家庭增加成员，不打断设备现有连接。
    http_token: str | None = None
    # 老数据可能只签发过 HTTP token，却缺 MQTT 用户名或密码。任何一项缺失都必须
    # 重新生成完整凭据，并通过本次 claimed 会话一次性交付给设备。
    if credential_needs_rotation(
        credential, has_active_binding=active_binding is not None
    ):
        http_token = secrets.token_urlsafe(40)
        credential.http_token_hash = secret_hash(http_token)
        credential.mqtt_username = f"p4-{device.device_sn}"
        credential.mqtt_password = secrets.token_urlsafe(32)
        credential.binding_epoch += 1
    session.status = "claimed"
    session.home_id = home.id
    session.claimed_by_user_id = user.id
    session.claimed_at = now
    # 当前比赛开发模式走明文 HTTP/MQTT：一次性凭据暂存在会话，设备 ACK 后清空。
    session.delivery = {
        "mqtt": {
            "brokerUri": settings.mqtt_public_broker_uri,
            "clientId": f"p4-{device.device_sn}",
            "username": credential.mqtt_username,
            "password": credential.mqtt_password,
        },
    }
    if http_token:
        session.delivery["deviceToken"] = http_token
    await db.commit()
    await db.refresh(session)
    return session, device, credential


async def revoke_device_binding(
    db: AsyncSession,
    *,
    device_id: UUID,
    user: User,
) -> tuple[Device, P4DeviceCredential, UUID]:
    """由家庭 owner 撤销 P4 绑定，并让旧 HTTP/MQTT 凭据立即失效。"""
    row = (
        await db.execute(
            select(Device, DeviceBinding, P4DeviceCredential)
            .join(
                DeviceBinding,
                (DeviceBinding.device_id == Device.id)
                & (DeviceBinding.status == "active"),
            )
            .join(P4DeviceCredential, P4DeviceCredential.device_id == Device.id)
            .join(
                HomeMember,
                (HomeMember.home_id == DeviceBinding.home_id)
                & (HomeMember.user_id == user.id),
            )
            .where(
                Device.id == device_id,
                HomeMember.role == "owner",
            )
            .with_for_update()
        )
    ).one_or_none()
    if row is None:
        raise HTTPException(status_code=404, detail="active device binding not found")
    device, binding, credential = row
    now = _now()
    binding.status = "inactive"
    binding.unbound_at = now
    # 保留出厂 Bootstrap 凭据，使设备解绑后仍能重新生成二维码；只撤销绑定后
    # 的 HTTP/MQTT 凭据。旧 token 因摘要被清空而立即失效。
    credential.active = True
    credential.http_token_hash = None
    credential.mqtt_username = None
    credential.mqtt_password = None
    credential.binding_epoch += 1
    device.status = "offline"
    await db.execute(
        update(P4BindingSession)
        .where(
            P4BindingSession.device_id == device.id,
            P4BindingSession.status.in_(["pending", "claimed"]),
        )
        .values(status="revoked", delivery={})
    )
    await db.commit()
    return device, credential, binding.home_id


async def binding_session_for_device(
    db: AsyncSession,
    *,
    session_id: UUID,
    device: Device,
) -> tuple[P4BindingSession, P4DeviceCredential]:
    session = await db.get(P4BindingSession, session_id)
    if session is None or session.device_id != device.id:
        raise HTTPException(status_code=404, detail="binding session not found")
    if session.status in {"pending", "claimed"} and session.expires_at <= _now():
        session.status = "expired"
        session.delivery = {}
        await db.commit()
    credential = (
        await db.execute(
            select(P4DeviceCredential).where(P4DeviceCredential.device_id == device.id)
        )
    ).scalar_one()
    return session, credential


async def ack_binding_session(
    db: AsyncSession,
    *,
    session_id: UUID,
    principal: DevicePrincipal,
) -> P4BindingSession:
    session = await db.get(P4BindingSession, session_id)
    if session is None or session.device_id != principal.device.id:
        raise HTTPException(status_code=404, detail="binding session not found")
    if session.status == "acked":
        return session
    if session.status != "claimed":
        raise HTTPException(status_code=409, detail="binding session has not been claimed")
    session.status = "acked"
    session.acked_at = _now()
    session.delivery = {}
    await db.commit()
    await db.refresh(session)
    return session
