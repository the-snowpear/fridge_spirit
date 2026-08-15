"""P4 Agent v2：二维码绑定、同步、设备接口和实时 WebSocket。"""

from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timedelta, timezone
from uuid import UUID

from fastapi import APIRouter, Depends, File, Form, Header, HTTPException, Query, UploadFile, WebSocket, WebSocketDisconnect
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import settings
from app.core.deps import get_current_user
from app.db.session import AsyncSessionLocal, get_db
from app.models.device import Device, DeviceBinding
from app.models.home import HomeMember
from app.models.notification import NotificationSubscription
from app.models.p4_sync import P4DeviceCredential, P4SyncEntity
from app.models.user import User
from app.schemas.ai_chat import AiChatRequest, AiChatResponseData
from app.schemas.ai_chat_history import AiChatHistoryClearData, AiChatHistoryData
from app.schemas.common import ApiResponse
from app.schemas.inventory import RefreshData
from app.schemas.scan import ScanResult
from app.schemas.v2 import (
    BindingAck,
    BindingClaim,
    BindingSessionCreate,
    BindingSessionData,
    AgentConfigUpdate,
    DeviceListItem,
    P4SeedRequest,
    P4SyncEventIn,
    P4SyncPush,
    P4SyncPushData,
    P4SyncPullData,
    P4SyncSnapshotData,
    P4DeviceUnbindData,
    P4NotificationSubscribe,
    P4NotificationSubscribeData,
)
from app.services.p4_binding import (
    DevicePrincipal,
    ack_binding_session,
    authenticate_bootstrap,
    authenticate_device_token,
    binding_session_for_device,
    claim_binding_session,
    create_binding_session,
    revoke_device_binding,
)
from app.services.p4_facade import (
    analyze_p4_food_image,
    chat_with_p4_context,
    clear_agent_history,
    list_agent_history,
    request_inventory_refresh,
)
from app.services.p4_realtime import p4_realtime_hub
from app.services.p4_sync import apply_events, build_snapshot, pull_events, seed_device
from app.services.token import JWTError, decode_access_token

router = APIRouter()
logger = logging.getLogger(__name__)


def _device_token(authorization: str | None) -> str | None:
    if not authorization:
        return None
    scheme, _, value = authorization.partition(" ")
    if scheme.lower() != "device" or not value:
        return None
    return value.strip()


async def _device_principal(
    authorization: str | None = Header(default=None, alias="Authorization"),
    db: AsyncSession = Depends(get_db),
) -> DevicePrincipal:
    token = _device_token(authorization)
    if token is None:
        raise HTTPException(status_code=401, detail="missing device token")
    return await authenticate_device_token(db, token=token)


def _session_data(
    session,
    *,
    token: str | None = None,
    include_delivery: bool = False,
    binding_epoch: int | None = None,
) -> BindingSessionData:
    delivery = dict(session.delivery or {}) if include_delivery else {}
    return BindingSessionData(
        session_id=session.id,
        status=session.status,
        expires_at=session.expires_at,
        expires_in_seconds=max(
            0,
            int(
                (
                    session.expires_at
                    - datetime.now(timezone.utc)
                ).total_seconds()
            ),
        ),
        qr_payload=(
            f"fridgespirit://bind?v=2&sessionId={session.id}&claimToken={token}"
            if token
            else None
        ),
        poll_after_ms=1500,
        home_id=session.home_id,
        binding_epoch=binding_epoch,
        device_token=delivery.get("deviceToken"),
        mqtt=delivery.get("mqtt"),
    )


@router.post("/device-binding/sessions", response_model=ApiResponse[BindingSessionData])
async def binding_session_create(
    payload: BindingSessionCreate,
    authorization: str | None = Header(default=None, alias="Authorization"),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[BindingSessionData]:
    """设备使用预置 secret 或绑定后的 Device token 创建屏幕二维码。"""
    token = payload.device_token or _device_token(authorization)
    if token:
        principal = await authenticate_device_token(db, token=token)
        if principal.device.device_sn != payload.device_sn:
            raise HTTPException(status_code=403, detail="device token does not match deviceSn")
        device = principal.device
    else:
        if not payload.key_id or not payload.bootstrap_secret:
            raise HTTPException(status_code=400, detail="keyId and bootstrapSecret are required")
        device, _ = await authenticate_bootstrap(
            db,
            device_sn=payload.device_sn,
            key_id=payload.key_id,
            secret=payload.bootstrap_secret,
        )
    session, claim_token = await create_binding_session(db, device=device)
    return ApiResponse(data=_session_data(session, token=claim_token))


@router.get(
    "/device-binding/sessions/{session_id}",
    response_model=ApiResponse[BindingSessionData],
)
async def binding_session_get(
    session_id: UUID,
    device_sn: str = Query(alias="deviceSn"),
    authorization: str | None = Header(default=None, alias="Authorization"),
    key_id: str | None = Header(default=None, alias="X-Bootstrap-Key-Id"),
    bootstrap_secret: str | None = Header(default=None, alias="X-Bootstrap-Secret"),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[BindingSessionData]:
    """设备轮询扫码结果；claimed 时返回一次性设备/MQTT 凭据。"""
    token = _device_token(authorization)
    if token:
        principal = await authenticate_device_token(db, token=token)
        device, credential = principal.device, principal.credential
    else:
        if not key_id or not bootstrap_secret:
            raise HTTPException(status_code=401, detail="missing device credential")
        device, credential = await authenticate_bootstrap(
            db,
            device_sn=device_sn,
            key_id=key_id,
            secret=bootstrap_secret,
        )
    if device.device_sn != device_sn:
        raise HTTPException(status_code=403, detail="credential does not match deviceSn")
    session, credential = await binding_session_for_device(
        db, session_id=session_id, device=device
    )
    return ApiResponse(
        data=_session_data(
            session,
            include_delivery=session.status == "claimed",
            binding_epoch=credential.binding_epoch if session.status in {"claimed", "acked"} else None,
        )
    )


@router.post(
    "/device-binding/sessions/{session_id}/ack",
    response_model=ApiResponse[BindingSessionData],
)
async def binding_session_ack(
    session_id: UUID,
    payload: BindingAck | None = None,
    principal: DevicePrincipal = Depends(_device_principal),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[BindingSessionData]:
    if payload is not None and not payload.committed:
        raise HTTPException(status_code=400, detail="committed must be true")
    session = await ack_binding_session(db, session_id=session_id, principal=principal)
    return ApiResponse(
        data=_session_data(session, binding_epoch=principal.credential.binding_epoch)
    )


@router.post("/bindings/claim", response_model=ApiResponse[BindingSessionData])
async def binding_claim(
    payload: BindingClaim,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[BindingSessionData]:
    """小程序 JWT 用户消费一次性二维码；设备凭据绝不返回给小程序。"""
    session, _, credential = await claim_binding_session(
        db,
        session_id=payload.session_id,
        token=payload.claim_token,
        user=user,
    )
    await p4_realtime_hub.publish(
        session.home_id,
        {
            "type": "binding.changed",
            "homeId": str(session.home_id),
            "deviceId": str(session.device_id),
            "bindingEpoch": credential.binding_epoch,
        },
    )
    return ApiResponse(
        data=_session_data(session, binding_epoch=credential.binding_epoch),
        message="device binding claimed",
    )


@router.get("/devices", response_model=ApiResponse[list[DeviceListItem]])
async def devices(
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[list[DeviceListItem]]:
    rows = (
        await db.execute(
            select(Device)
            .join(DeviceBinding, DeviceBinding.device_id == Device.id)
            .join(P4DeviceCredential, P4DeviceCredential.device_id == Device.id)
            .join(HomeMember, HomeMember.home_id == DeviceBinding.home_id)
            .where(
                HomeMember.user_id == user.id,
                DeviceBinding.status == "active",
                P4DeviceCredential.active.is_(True),
            )
            .order_by(Device.created_at.asc())
        )
    ).scalars().all()
    return ApiResponse(
        data=[
            DeviceListItem(
                id=item.id,
                device_sn=item.device_sn,
                name=item.name,
                status=item.status,
                last_seen_at=item.last_seen_at,
            )
            for item in rows
        ]
    )


@router.post(
    "/devices/{device_id}/unbind",
    response_model=ApiResponse[P4DeviceUnbindData],
)
async def device_unbind(
    device_id: UUID,
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4DeviceUnbindData]:
    """撤销绑定及设备凭据；旧 token 与旧 bindingEpoch 均立即失效。"""
    device, credential, home_id = await revoke_device_binding(
        db, device_id=device_id, user=user
    )
    await p4_realtime_hub.publish(
        home_id,
        {
            "type": "binding.changed",
            "homeId": str(home_id),
            "deviceId": str(device.id),
            "bindingEpoch": credential.binding_epoch,
            "status": "revoked",
        },
    )
    return ApiResponse(
        data=P4DeviceUnbindData(
            device_id=device.id,
            device_sn=device.device_sn,
            revoked=True,
            binding_epoch=credential.binding_epoch,
        ),
        message="device credential revoked",
    )


async def _user_home_ids(db: AsyncSession, user_id: UUID) -> list[UUID]:
    return list(
        (
            await db.execute(
                select(HomeMember.home_id)
                .join(DeviceBinding, DeviceBinding.home_id == HomeMember.home_id)
                .join(P4DeviceCredential, P4DeviceCredential.device_id == DeviceBinding.device_id)
                .where(
                    HomeMember.user_id == user_id,
                    DeviceBinding.status == "active",
                    P4DeviceCredential.active.is_(True),
                )
                .distinct()
            )
        ).scalars().all()
    )


async def _selected_user_home(
    db: AsyncSession, user_id: UUID, home_id: UUID | None
) -> UUID:
    home_ids = await _user_home_ids(db, user_id)
    selected = home_id or (home_ids[0] if home_ids else None)
    if selected is None or selected not in home_ids:
        raise HTTPException(status_code=403, detail="not a member of requested home")
    return selected


def _empty_scan_result(message: str) -> ApiResponse[ScanResult]:
    return ApiResponse(
        ok=False,
        message=message,
        data=ScanResult(
            candidates=[],
            raw_text=None,
            model_used=settings.vision_model,
        ),
    )


@router.post("/inventory/scan", response_model=ApiResponse[ScanResult])
async def inventory_scan(
    file: UploadFile = File(...),
    zone: str | None = Form(default=None),
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[ScanResult]:
    """照片只做识别预览；确认入库仍通过 v2 inventory 同步事件。"""
    selected = await _selected_user_home(db, user.id, home_id)
    declared_size = getattr(file, "size", None)
    if declared_size is not None and declared_size > settings.inventory_scan_max_bytes:
        return _empty_scan_result("image too large")
    image_bytes = await file.read()
    if not image_bytes:
        return _empty_scan_result("empty image")
    if len(image_bytes) > settings.inventory_scan_max_bytes:
        return _empty_scan_result("image too large")
    try:
        result = await analyze_p4_food_image(
            image_bytes=image_bytes,
            hint_zone=zone,
            db=db,
            home_id=selected,
        )
    except RuntimeError as exc:
        logger.warning(
            "P4 inventory scan failed: user=%s home=%s error=%s",
            user.id,
            selected,
            exc,
        )
        return _empty_scan_result(f"vision failed: {exc}")
    except Exception as exc:  # noqa: BLE001
        logger.exception(
            "P4 inventory scan unexpected error: user=%s home=%s",
            user.id,
            selected,
        )
        return _empty_scan_result(f"internal error: {exc.__class__.__name__}")
    return ApiResponse(
        data=result,
        message="no food detected" if not result.candidates else "ok",
    )


@router.post("/inventory/refresh", response_model=ApiResponse[RefreshData])
async def inventory_refresh(
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[RefreshData]:
    selected = await _selected_user_home(db, user.id, home_id)
    try:
        queued = await request_inventory_refresh(db, home_id=selected)
    except Exception as exc:  # noqa: BLE001
        logger.warning("P4 inventory refresh deferred: home=%s error=%s", selected, exc)
        queued = False
    return ApiResponse(
        data=RefreshData(
            queued=queued,
            next_refresh_at=datetime.now(timezone.utc) + timedelta(seconds=60),
        ),
        message="refresh enqueued" if queued else "device unavailable",
    )


@router.post("/ai/chat", response_model=ApiResponse[AiChatResponseData])
async def ai_chat(
    payload: AiChatRequest,
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiChatResponseData]:
    selected = await _selected_user_home(db, user.id, home_id)
    result, session_id = await chat_with_p4_context(
        db,
        home_id=selected,
        user_id=user.id,
        prompt=payload.prompt,
        session_id=payload.session_id,
    )
    data = AiChatResponseData(
        source=result["source"],
        reply=result.get("reply") or "",
        fallback_reason=result.get("fallbackReason"),
        model_used=result.get("modelUsed"),
        device_sn=result.get("deviceSn"),
        session_id=session_id,
    )
    return ApiResponse(
        ok=bool(data.reply),
        data=data,
        message="ok" if data.reply else (data.fallback_reason or "no reply from ai"),
    )


@router.get("/ai/history", response_model=ApiResponse[AiChatHistoryData])
async def ai_history(
    session_id: str | None = Query(default=None, alias="sessionId", max_length=128),
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiChatHistoryData]:
    selected = await _selected_user_home(db, user.id, home_id)
    normalized, messages, _ = await list_agent_history(
        db, home_id=selected, session_id=session_id
    )
    return ApiResponse(
        data=AiChatHistoryData(session_id=normalized, messages=messages)
    )


@router.delete("/ai/history", response_model=ApiResponse[AiChatHistoryClearData])
async def ai_history_clear(
    session_id: str | None = Query(default=None, alias="sessionId", max_length=128),
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[AiChatHistoryClearData]:
    selected = await _selected_user_home(db, user.id, home_id)
    normalized, deleted_count = await clear_agent_history(
        db,
        home_id=selected,
        user_id=user.id,
        session_id=session_id,
    )
    return ApiResponse(
        data=AiChatHistoryClearData(
            session_id=normalized,
            deleted_count=deleted_count,
        )
    )


@router.post(
    "/notification/subscribe",
    response_model=ApiResponse[P4NotificationSubscribeData],
)
async def notification_subscribe(
    payload: P4NotificationSubscribe,
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4NotificationSubscribeData]:
    """登记当前 JWT 用户的通知目标，禁止请求体冒充其它 userId。"""
    selected = await _selected_user_home(db, user.id, home_id)
    subscription = (
        await db.execute(
            select(NotificationSubscription).where(
                NotificationSubscription.user_id == user.id,
                NotificationSubscription.channel == payload.channel,
                NotificationSubscription.target == payload.target,
            )
        )
    ).scalar_one_or_none()
    if subscription is None:
        subscription = NotificationSubscription(
            user_id=user.id,
            home_id=selected,
            channel=payload.channel,
            target=payload.target,
            status="active",
            extra=payload.extra,
        )
        db.add(subscription)
        await db.flush()
    else:
        subscription.home_id = selected
        subscription.status = "active"
        subscription.extra = payload.extra
    await db.commit()
    return ApiResponse(
        data=P4NotificationSubscribeData(
            subscription_id=subscription.id,
            status=subscription.status,
        )
    )


@router.get("/sync/snapshot", response_model=ApiResponse[P4SyncSnapshotData])
async def sync_snapshot(
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4SyncSnapshotData]:
    home_ids = await _user_home_ids(db, user.id)
    selected = home_id or (home_ids[0] if home_ids else None)
    if selected is None or selected not in home_ids:
        raise HTTPException(status_code=403, detail="not a member of requested home")
    return ApiResponse(data=await build_snapshot(db, home_id=selected))


@router.post("/sync/events", response_model=ApiResponse[P4SyncPushData])
async def sync_events(
    payload: P4SyncPush,
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4SyncPushData]:
    home_ids = await _user_home_ids(db, user.id)
    selected = home_id or (home_ids[0] if home_ids else None)
    if selected is None or selected not in home_ids:
        raise HTTPException(status_code=403, detail="not a member of requested home")
    result = await apply_events(
        db,
        home_id=selected,
        events=payload.events,
        source="miniapp",
        user_id=user.id,
    )
    for event in result.accepted:
        await p4_realtime_hub.publish(
            selected,
            {"type": "sync.event", "homeId": str(selected), "event": event.model_dump(mode="json", by_alias=True)},
        )
    if result.conflicts:
        await p4_realtime_hub.publish(
            selected,
            {
                "type": "sync.conflict",
                "homeId": str(selected),
                "conflicts": [item.model_dump(mode="json", by_alias=True) for item in result.conflicts],
            },
        )
    # MQTT 投递失败不回滚已提交的云端事务，设备可通过 snapshot 补齐。
    if result.accepted:
        from app.services.p4_mqtt import p4_mqtt_bridge

        await p4_mqtt_bridge.publish_sync_events(selected, result.accepted)
    return ApiResponse(data=result, message="conflict" if result.conflicts else "ok")


@router.get("/sync/events", response_model=ApiResponse[P4SyncPullData])
async def sync_events_pull(
    since_revision: int = Query(default=0, ge=0, alias="sinceRevision"),
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4SyncPullData]:
    """WebSocket 断线后按 serverRevision 补齐增量。"""
    home_ids = await _user_home_ids(db, user.id)
    selected = home_id or (home_ids[0] if home_ids else None)
    if selected is None or selected not in home_ids:
        raise HTTPException(status_code=403, detail="not a member of requested home")
    return ApiResponse(
        data=await pull_events(db, home_id=selected, since_revision=since_revision)
    )


@router.get("/agent-config", response_model=ApiResponse[dict])
async def agent_config_get(
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[dict]:
    """小程序读取脱敏后的 P4 Agent/AI 配置。"""
    home_ids = await _user_home_ids(db, user.id)
    selected = home_id or (home_ids[0] if home_ids else None)
    if selected is None or selected not in home_ids:
        raise HTTPException(status_code=403, detail="not a member of requested home")
    snapshot = await build_snapshot(db, home_id=selected)
    items = snapshot.domains.get("ai_config", [])
    return ApiResponse(data=items[0] if items else {})


@router.put("/agent-config", response_model=ApiResponse[P4SyncPushData])
async def agent_config_put(
    payload: AgentConfigUpdate,
    home_id: UUID | None = Query(default=None, alias="homeId"),
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4SyncPushData]:
    """开发模式允许写入原始 Key；响应、GET、WebSocket和日志保持脱敏。"""
    home_ids = await _user_home_ids(db, user.id)
    selected = home_id or (home_ids[0] if home_ids else None)
    if selected is None or selected not in home_ids:
        raise HTTPException(status_code=403, detail="not a member of requested home")
    current = (
        await db.execute(
            select(P4SyncEntity).where(
                P4SyncEntity.home_id == selected,
                P4SyncEntity.domain == "ai_config",
                P4SyncEntity.entity_id == "active-profile",
            )
        )
    ).scalar_one_or_none()
    merged_data = dict(current.data or {}) if current is not None else {}
    incoming_data = dict(payload.data)
    # 小程序 GET 只能拿到 "***"；原样 PUT 或省略 apiKey 都表示保留云端原 Key。
    # 显式传空字符串表示清除，传其它非空字符串表示轮换。
    incoming_key = incoming_data.get("apiKey")
    if incoming_key == "***":
        incoming_data.pop("apiKey", None)
    merged_data.update(incoming_data)
    merged_data.pop("hasApiKey", None)

    event = P4SyncEventIn(
        event_id=payload.event_id,
        domain="ai_config",
        entity_id="active-profile",
        operation="upsert",
        base_entity_revision=payload.base_entity_revision,
        data=merged_data,
    )
    result = await apply_events(
        db,
        home_id=selected,
        events=[event],
        source="miniapp",
        user_id=user.id,
    )
    if result.accepted:
        from app.services.p4_mqtt import p4_mqtt_bridge

        await p4_mqtt_bridge.publish_sync_events(selected, result.accepted)
        await p4_realtime_hub.publish(
            selected,
            {
                "type": "sync.event",
                "homeId": str(selected),
                "event": result.accepted[0].model_dump(mode="json", by_alias=True),
            },
        )
    return ApiResponse(data=result, message="conflict" if result.conflicts else "ok")


@router.post("/device-sync/seed", response_model=ApiResponse[P4SyncPushData])
async def device_seed(
    payload: P4SeedRequest,
    principal: DevicePrincipal = Depends(_device_principal),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4SyncPushData]:
    result = await seed_device(
        db,
        home_id=principal.binding.home_id,
        device_id=principal.device.id,
        request=payload,
    )
    for event in result.accepted:
        await p4_realtime_hub.publish(
            principal.binding.home_id,
            {
                "type": "sync.event",
                "homeId": str(principal.binding.home_id),
                "event": event.model_dump(mode="json", by_alias=True),
            },
        )
    return ApiResponse(data=result, message="P4 seed accepted")


@router.get("/device-sync/snapshot", response_model=ApiResponse[P4SyncSnapshotData])
async def device_snapshot(
    principal: DevicePrincipal = Depends(_device_principal),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[P4SyncSnapshotData]:
    """设备令牌读取完整快照；开发模式的 AI Key 仅在这条设备鉴权链返回原文。"""
    return ApiResponse(
        data=await build_snapshot(
            db,
            home_id=principal.binding.home_id,
            include_secrets=True,
        )
    )


@router.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    """首帧必须是 {"type":"auth","token":"<JWT>"}。"""
    await websocket.accept()
    try:
        auth = await asyncio.wait_for(websocket.receive_json(), timeout=10)
        if auth.get("type") != "auth" or not auth.get("token"):
            await websocket.close(code=4401, reason="auth frame required")
            return
        try:
            claims = decode_access_token(str(auth["token"]))
            user_id = UUID(str(claims["sub"]))
        except (JWTError, KeyError, ValueError):
            await websocket.close(code=4401, reason="invalid token")
            return
        async with AsyncSessionLocal() as db:
            if await db.get(User, user_id) is None:
                await websocket.close(code=4401, reason="user not found")
                return
            home_ids = set(await _user_home_ids(db, user_id))
        await p4_realtime_hub.add(home_ids, websocket)
        await websocket.send_json(
            {
                "type": "auth.ok",
                "homeIds": [str(item) for item in sorted(home_ids, key=str)],
                "heartbeatSeconds": 30,
            }
        )
        while True:
            message = await websocket.receive_json()
            if message.get("type") == "ping":
                await websocket.send_json({"type": "pong"})
    except (WebSocketDisconnect, asyncio.TimeoutError):
        pass
    finally:
        await p4_realtime_hub.remove(websocket)
