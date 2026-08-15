"""P4 Agent v2 实体同步、冲突和快照服务。"""

from __future__ import annotations

from datetime import datetime, timezone
import json
from typing import Iterable
from uuid import UUID, uuid4

from fastapi import HTTPException
from sqlalchemy import select, text
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.p4_sync import P4SyncEntity, P4SyncEvent, P4SyncState
from app.schemas.v2 import (
    P4_DOMAINS,
    P4SeedRequest,
    P4SyncConflict,
    P4SyncEventIn,
    P4SyncEventOut,
    P4SyncPushData,
    P4SyncPullData,
    P4SyncSnapshotData,
)


SECRET_KEYS = {
    "apikey",
    "api_key",
    "asrapikey",
    "asr_api_key",
    "ttsapikey",
    "tts_api_key",
    "password",
    "secret",
    "token",
}


def _redact(value: object) -> object:
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            normalized = key.replace("-", "").lower()
            sensitive = (
                normalized in SECRET_KEYS
                or normalized.endswith("apikey")
                or normalized.endswith("password")
                or normalized.endswith("secret")
                or normalized.endswith("token")
            )
            result[key] = "***" if sensitive else _redact(item)
        return result
    if isinstance(value, list):
        return [_redact(item) for item in value]
    return value


def event_out(row: P4SyncEvent, *, redact: bool = True) -> P4SyncEventOut:
    data = dict(row.data or {})
    if redact and row.domain == "ai_config":
        data = _redact(data)  # type: ignore[assignment]
    return P4SyncEventOut(
        event_id=row.event_id,
        domain=row.domain,
        entity_id=row.entity_id,
        operation=row.operation,
        entity_revision=int(row.entity_revision),
        server_revision=int(row.server_revision),
        source=row.source,
        data=data,
        created_at=row.created_at,
    )


async def _state(db: AsyncSession, home_id: UUID) -> P4SyncState:
    # PostgreSQL 事务级 advisory lock 保证家庭首个事件并发时只创建一个游标。
    await db.execute(
        text("SELECT pg_advisory_xact_lock(hashtext(:home_id))"),
        {"home_id": str(home_id)},
    )
    state = (
        await db.execute(
            select(P4SyncState).where(P4SyncState.home_id == home_id).with_for_update()
        )
    ).scalar_one_or_none()
    if state is None:
        state = P4SyncState(home_id=home_id, current_revision=0)
        db.add(state)
        await db.flush()
    return state


async def apply_events(
    db: AsyncSession,
    *,
    home_id: UUID,
    events: Iterable[P4SyncEventIn],
    source: str,
    user_id: UUID | None = None,
    device_id: UUID | None = None,
) -> P4SyncPushData:
    """逐实体乐观并发；重复 eventId 幂等，冲突事件不写入。"""
    state = await _state(db, home_id)
    accepted: list[P4SyncEventOut] = []
    duplicates: list[str] = []
    conflicts: list[P4SyncConflict] = []
    for incoming in events:
        if len(json.dumps(incoming.data, ensure_ascii=False).encode("utf-8")) > 48 * 1024:
            raise HTTPException(status_code=413, detail=f"event {incoming.event_id} exceeds 48 KiB")
        duplicate = (
            await db.execute(
                select(P4SyncEvent).where(
                    P4SyncEvent.home_id == home_id,
                    P4SyncEvent.event_id == incoming.event_id,
                )
            )
        ).scalar_one_or_none()
        if duplicate is not None:
            duplicates.append(incoming.event_id)
            continue

        entity = (
            await db.execute(
                select(P4SyncEntity)
                .where(
                    P4SyncEntity.home_id == home_id,
                    P4SyncEntity.domain == incoming.domain,
                    P4SyncEntity.entity_id == incoming.entity_id,
                )
                .with_for_update()
            )
        ).scalar_one_or_none()
        actual_revision = int(entity.entity_revision) if entity else 0
        if incoming.base_entity_revision != actual_revision:
            conflicts.append(
                P4SyncConflict(
                    event_id=incoming.event_id,
                    domain=incoming.domain,
                    entity_id=incoming.entity_id,
                    expected_revision=incoming.base_entity_revision,
                    actual_revision=actual_revision,
                    server_entity=None
                    if entity is None
                    else {
                        "entityId": entity.entity_id,
                        "entityRevision": actual_revision,
                        "deleted": entity.deleted,
                        "data": _redact(entity.data)
                        if incoming.domain == "ai_config"
                        else entity.data,
                    },
                )
            )
            continue

        state.current_revision += 1
        server_revision = int(state.current_revision)
        next_entity_revision = actual_revision + 1
        if entity is None:
            entity = P4SyncEntity(
                home_id=home_id,
                domain=incoming.domain,
                entity_id=incoming.entity_id,
                entity_revision=next_entity_revision,
                server_revision=server_revision,
                deleted=incoming.operation == "delete",
                data={} if incoming.operation == "delete" else dict(incoming.data),
            )
            db.add(entity)
        else:
            entity.entity_revision = next_entity_revision
            entity.server_revision = server_revision
            entity.deleted = incoming.operation == "delete"
            if incoming.operation == "delete":
                entity.data = {}
            elif incoming.operation == "append" and isinstance(entity.data.get("items"), list):
                entity.data = {
                    **entity.data,
                    "items": [*entity.data["items"], incoming.data],
                }
            else:
                entity.data = dict(incoming.data)

        stored = P4SyncEvent(
            home_id=home_id,
            event_id=incoming.event_id,
            source=source,
            user_id=user_id,
            device_id=device_id,
            domain=incoming.domain,
            entity_id=incoming.entity_id,
            operation=incoming.operation,
            base_entity_revision=incoming.base_entity_revision,
            entity_revision=next_entity_revision,
            server_revision=server_revision,
            schema_version=2,
            data=dict(incoming.data),
        )
        db.add(stored)
        await db.flush()
        accepted.append(event_out(stored))
    await db.commit()
    return P4SyncPushData(
        server_revision=int(state.current_revision),
        accepted=accepted,
        duplicates=duplicates,
        conflicts=conflicts,
    )


async def build_snapshot(
    db: AsyncSession,
    *,
    home_id: UUID,
    include_secrets: bool = False,
) -> P4SyncSnapshotData:
    state = (
        await db.execute(select(P4SyncState).where(P4SyncState.home_id == home_id))
    ).scalar_one_or_none()
    entities = (
        await db.execute(
            select(P4SyncEntity)
            .where(P4SyncEntity.home_id == home_id, P4SyncEntity.deleted.is_(False))
            .order_by(P4SyncEntity.domain, P4SyncEntity.entity_id)
        )
    ).scalars().all()
    domains: dict[str, list[dict]] = {domain: [] for domain in sorted(P4_DOMAINS)}
    for entity in entities:
        data = dict(entity.data or {})
        if entity.domain == "ai_config" and not include_secrets:
            data = _redact(data)  # type: ignore[assignment]
        domains[entity.domain].append(
            {
                "entityId": entity.entity_id,
                "entityRevision": int(entity.entity_revision),
                "serverRevision": int(entity.server_revision),
                "data": data,
            }
        )
    return P4SyncSnapshotData(
        server_revision=int(state.current_revision) if state else 0,
        generated_at=datetime.now(timezone.utc),
        domains=domains,
    )


async def pull_events(
    db: AsyncSession,
    *,
    home_id: UUID,
    since_revision: int,
    limit: int = 200,
) -> P4SyncPullData:
    state = (
        await db.execute(select(P4SyncState).where(P4SyncState.home_id == home_id))
    ).scalar_one_or_none()
    rows = (
        await db.execute(
            select(P4SyncEvent)
            .where(
                P4SyncEvent.home_id == home_id,
                P4SyncEvent.server_revision > since_revision,
            )
            .order_by(P4SyncEvent.server_revision.asc())
            .limit(limit)
        )
    ).scalars().all()
    latest = int(state.current_revision) if state else 0
    cursor = int(rows[-1].server_revision) if rows else min(since_revision, latest)
    return P4SyncPullData(
        server_revision=cursor,
        latest_server_revision=latest,
        has_more=cursor < latest,
        events=[event_out(row) for row in rows],
    )


async def seed_device(
    db: AsyncSession,
    *,
    home_id: UUID,
    device_id: UUID,
    request: P4SeedRequest,
) -> P4SyncPushData:
    state = await _state(db, home_id)
    if state.current_revision != 0:
        raise HTTPException(status_code=409, detail="cloud already has P4 Agent data")
    events: list[P4SyncEventIn] = []
    for domain, items in request.domains.items():
        for item in items:
            entity_id = str(item.get("entityId") or item.get("id") or uuid4())
            raw_data = item.get("data") if isinstance(item.get("data"), dict) else item
            events.append(
                P4SyncEventIn(
                    event_id=f"seed:{device_id}:{domain}:{entity_id}"[:128],
                    domain=domain,
                    entity_id=entity_id,
                    operation="upsert",
                    base_entity_revision=0,
                    data=dict(raw_data),
                )
            )
    return await apply_events(
        db,
        home_id=home_id,
        events=events,
        source="device",
        device_id=device_id,
    )
