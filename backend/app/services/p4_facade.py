"""P4 v2 小程序命令 facade。

库存和 AI 会话始终以 ``p4_sync_entities`` 为事实源；本模块不会读写旧版
``inventory_items`` 或 ``ai_chat_messages``。
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any
from uuid import UUID, uuid4

from fastapi import HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.device import Device, DeviceBinding
from app.models.p4_sync import P4SyncEntity
from app.schemas.ai_chat_history import AiChatHistoryMessage
from app.schemas.scan import ScanCandidate, ScanResult
from app.schemas.v2 import P4SyncEventIn
from app.services.ai_service import chat_via_cloud
from app.services.p4_sync import apply_events


DEFAULT_AGENT_SESSION_ID = "miniapp-default"
MAX_AGENT_MESSAGES = 80
CELL_SLOTS = ("A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3")
DEFAULT_ZONE_LAYERS: dict[str, tuple[str, ...]] = {
    "freezer": ("f1", "f2"),
    "left": ("l1", "l2", "l3"),
    "right": ("r1", "r2"),
    "door": ("d1", "d2"),
    "drawer": ("v1",),
}
CATEGORY_ZONE_MAP = {
    "蔬菜": "drawer",
    "水果": "drawer",
    "熟食": "left",
    "肉类": "right",
    "海鲜": "right",
    "乳制品": "right",
    "速冻": "freezer",
    "饮品": "door",
    "调料": "door",
}


def _slot_to_cell(slot: Any) -> int | None:
    try:
        return CELL_SLOTS.index(str(slot))
    except ValueError:
        return None


def normalize_inventory_location(data: dict[str, Any]) -> tuple[str | None, str | None, int | None]:
    """兼容 P4 snake_case、新小程序 camelCase 与旧 zone/slot。"""
    zone_id = data.get("zoneId") or data.get("zone_id") or data.get("zone")
    if not zone_id:
        return None, None, None
    zone_id = str(zone_id)
    layer_id = data.get("layerId") or data.get("layer_id")
    if not layer_id:
        layer_id = DEFAULT_ZONE_LAYERS.get(zone_id, (f"{zone_id}-layer-1",))[0]
    raw_cell = data.get("cell")
    cell = int(raw_cell) if isinstance(raw_cell, (int, float)) and 0 <= int(raw_cell) <= 8 else _slot_to_cell(data.get("slot"))
    return zone_id, str(layer_id), cell


def _recommend_three_level_location(
    *,
    name: str,
    category: str | None,
    items: list[dict[str, Any]],
) -> tuple[str | None, str | None, int | None, str | None]:
    """先同类合并，再按类别选择首个层和空格；不跨层错误判满。"""
    occupied: set[tuple[str, str, int]] = set()
    for item in items:
        zone_id, layer_id, cell = normalize_inventory_location(item)
        if zone_id and layer_id and cell is not None:
            if str(item.get("name") or "") == name:
                return zone_id, layer_id, cell, "同类食材优先合并"
            occupied.add((zone_id, layer_id, cell))

    zone_id = "right"
    for key, candidate_zone in CATEGORY_ZONE_MAP.items():
        if category and key in category:
            zone_id = candidate_zone
            break
    zone_order = [zone_id, *[zone for zone in DEFAULT_ZONE_LAYERS if zone != zone_id]]
    for candidate_zone in zone_order:
        for layer_id in DEFAULT_ZONE_LAYERS.get(candidate_zone, (f"{candidate_zone}-layer-1",)):
            for cell in range(9):
                if (candidate_zone, layer_id, cell) not in occupied:
                    reason = "按类别推荐空位" if candidate_zone == zone_id else "原分区已满，建议放其它分区"
                    return candidate_zone, layer_id, cell, reason
    return None, None, None, "所有标准层格位已满，请手动指定位置"


def normalize_agent_session_id(session_id: str | None) -> str:
    value = (session_id or DEFAULT_AGENT_SESSION_ID).strip()
    return (value or DEFAULT_AGENT_SESSION_ID)[:128]


async def _entity(
    db: AsyncSession,
    *,
    home_id: UUID,
    domain: str,
    entity_id: str,
) -> P4SyncEntity | None:
    return (
        await db.execute(
            select(P4SyncEntity).where(
                P4SyncEntity.home_id == home_id,
                P4SyncEntity.domain == domain,
                P4SyncEntity.entity_id == entity_id,
                P4SyncEntity.deleted.is_(False),
            )
        )
    ).scalar_one_or_none()


async def _ai_config(db: AsyncSession, home_id: UUID) -> dict[str, Any]:
    row = await _entity(
        db, home_id=home_id, domain="ai_config", entity_id="active-profile"
    )
    return dict(row.data or {}) if row else {}


async def build_inventory_context(
    db: AsyncSession, home_id: UUID
) -> dict[str, Any]:
    """从 v2 inventory 投影构造 AI 上下文。"""
    rows = (
        await db.execute(
            select(P4SyncEntity)
            .where(
                P4SyncEntity.home_id == home_id,
                P4SyncEntity.domain == "inventory",
                P4SyncEntity.deleted.is_(False),
            )
            .order_by(P4SyncEntity.server_revision.desc())
            .limit(50)
        )
    ).scalars().all()
    items = [
        dict(row.data or {})
        for row in rows
        if str((row.data or {}).get("status", "active")) != "deleted"
    ]
    public_items = [
        (
        lambda location: {
            "name": item.get("name"),
            "quantity": item.get("quantity"),
            "unit": item.get("unit"),
            "zoneId": location[0],
            "layerId": location[1],
            "cell": location[2],
            "expireDate": item.get("expireDate") or item.get("expire_date"),
        })(normalize_inventory_location(item))
        for item in items[:20]
    ]
    return {
        "recentItems": public_items,
        "expiringSoon": [
            item for item in public_items if item.get("expireDate")
        ][:10],
        "totalCount": len(items),
    }


async def analyze_p4_food_image(
    *,
    image_bytes: bytes,
    hint_zone: str | None,
    db: AsyncSession,
    home_id: UUID,
) -> ScanResult:
    """复用视觉模型与推荐算法，但库存占用只来自 v2 投影。"""
    from app.services.vision_service import (
        _call_vision_model,
        _compress_image,
        _normalize_candidate,
        _parse_candidates,
    )

    try:
        compressed = _compress_image(image_bytes)
    except Exception as exc:  # noqa: BLE001
        raise RuntimeError(f"invalid image: {exc}") from exc
    raw_text, model_used = await _call_vision_model(
        compressed, ai_config=await _ai_config(db, home_id)
    )
    candidates = [
        candidate
        for raw in _parse_candidates(raw_text)
        if (candidate := _normalize_candidate(raw)) is not None
    ]

    rows = (
        await db.execute(
            select(P4SyncEntity)
            .where(
                P4SyncEntity.home_id == home_id,
                P4SyncEntity.domain == "inventory",
                P4SyncEntity.deleted.is_(False),
            )
            .order_by(P4SyncEntity.server_revision.desc())
        )
    ).scalars().all()
    inventory = [
        dict(row.data or {})
        for row in rows
        if str((row.data or {}).get("status", "active")) != "deleted"
    ]
    enriched: list[ScanCandidate] = []
    for candidate in candidates:
        zone_id, layer_id, cell, reason = _recommend_three_level_location(
            name=candidate.name,
            category=candidate.category,
            items=inventory,
        )
        if zone_id and layer_id and cell is not None:
            inventory.append(
                {"name": candidate.name, "zoneId": zone_id, "layerId": layer_id, "cell": cell}
            )
        enriched.append(
            candidate.model_copy(
                update={
                    "suggested_zone": zone_id,
                    "suggested_layer_id": layer_id,
                    "suggested_cell": cell,
                    "suggested_slot": CELL_SLOTS[cell] if cell is not None else None,
                    "suggested_reason": reason,
                }
            )
        )
    return ScanResult(
        candidates=enriched,
        raw_text=raw_text[:200],
        model_used=model_used,
    )


def _history_messages(data: dict[str, Any]) -> list[AiChatHistoryMessage]:
    result: list[AiChatHistoryMessage] = []
    for item in data.get("messages", [])[-MAX_AGENT_MESSAGES:]:
        if not isinstance(item, dict):
            continue
        try:
            result.append(AiChatHistoryMessage.model_validate(item))
        except (TypeError, ValueError):
            continue
    return result


async def list_agent_history(
    db: AsyncSession, *, home_id: UUID, session_id: str | None
) -> tuple[str, list[AiChatHistoryMessage], P4SyncEntity | None]:
    normalized = normalize_agent_session_id(session_id)
    row = await _entity(
        db, home_id=home_id, domain="agent_sessions", entity_id=normalized
    )
    return normalized, _history_messages(dict(row.data or {}) if row else {}), row


async def chat_with_p4_context(
    db: AsyncSession,
    *,
    home_id: UUID,
    user_id: UUID,
    prompt: str,
    session_id: str | None,
) -> tuple[dict[str, Any], str]:
    """调用云端兼容模型，并把一轮历史写回 agent_sessions。"""
    normalized, messages, row = await list_agent_history(
        db, home_id=home_id, session_id=session_id
    )
    result = await chat_via_cloud(
        prompt,
        await build_inventory_context(db, home_id),
        "p4_backend",
        ai_config=await _ai_config(db, home_id),
    )
    now = datetime.now(timezone.utc)
    next_messages = [
        *[item.model_dump(mode="json", by_alias=True) for item in messages],
        {
            "id": str(uuid4()),
            "role": "user",
            "content": prompt,
            "source": "miniapp",
            "sentAt": now.isoformat(),
        },
        {
            "id": str(uuid4()),
            "role": "assistant",
            "content": result.get("reply") or "",
            "source": result.get("source"),
            "fallbackReason": result.get("fallbackReason"),
            "modelUsed": result.get("modelUsed"),
            "deviceSn": result.get("deviceSn"),
            "sentAt": now.isoformat(),
        },
    ][-MAX_AGENT_MESSAGES:]
    event = P4SyncEventIn(
        event_id=f"chat:{uuid4()}",
        domain="agent_sessions",
        entity_id=normalized,
        operation="upsert",
        base_entity_revision=int(row.entity_revision) if row else 0,
        data={
            "sessionId": normalized,
            "messages": next_messages,
            "updatedAt": now.isoformat(),
            "updatedBy": str(user_id),
        },
    )
    applied = await apply_events(
        db,
        home_id=home_id,
        events=[event],
        source="miniapp",
        user_id=user_id,
    )
    if applied.conflicts:
        raise HTTPException(status_code=409, detail="agent session changed; retry request")
    return result, normalized


async def clear_agent_history(
    db: AsyncSession,
    *,
    home_id: UUID,
    user_id: UUID,
    session_id: str | None,
) -> tuple[str, int]:
    normalized, messages, row = await list_agent_history(
        db, home_id=home_id, session_id=session_id
    )
    if row is None:
        return normalized, 0
    applied = await apply_events(
        db,
        home_id=home_id,
        events=[
            P4SyncEventIn(
                event_id=f"chat-clear:{uuid4()}",
                domain="agent_sessions",
                entity_id=normalized,
                operation="delete",
                base_entity_revision=int(row.entity_revision),
                data={},
            )
        ],
        source="miniapp",
        user_id=user_id,
    )
    if applied.conflicts:
        raise HTTPException(status_code=409, detail="agent session changed; retry request")
    return normalized, len(messages)


async def request_inventory_refresh(
    db: AsyncSession, *, home_id: UUID
) -> bool:
    """向当前家庭全部 P4 设备发送轻量刷新命令。"""
    from app.services.mqtt_client import mqtt_client
    from app.services.p4_mqtt import p4_mqtt_bridge

    if not mqtt_client.is_connected():
        return False
    device_sns = (
        await db.execute(
            select(Device.device_sn)
            .join(DeviceBinding, DeviceBinding.device_id == Device.id)
            .where(
                DeviceBinding.home_id == home_id,
                DeviceBinding.status == "active",
            )
        )
    ).scalars().all()
    queued = False
    for device_sn in device_sns:
        await p4_mqtt_bridge.publish_command(
            home_id,
            device_sn,
            "sync.request_snapshot",
            {
                "domain": "inventory",
                "requestedAt": int(datetime.now(timezone.utc).timestamp() * 1000),
            },
        )
        queued = True
    return queued
