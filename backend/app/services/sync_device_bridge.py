"""同步快照到设备的 MQTT 桥接层。

同步服务本身只负责落库和生成 serverRevision；本模块负责把云端当前快照转换为
固件已经支持的 MQTT 命令，避免小程序“同步成功”但设备端没有任何变化。
"""

from __future__ import annotations

import asyncio
import logging
from datetime import date, datetime, timezone
from typing import Any
from uuid import UUID

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.device import Device, DeviceBinding, DeviceStatusEvent
from app.models.inventory import InventoryItem
from app.models.reminder import Reminder
from app.models.system_config import SystemConfig
from app.schemas.fridge_zone import FridgeZoneConfig
from app.schemas.sync import SyncDevicePushData
from app.services.ai_config_service import build_device_payload, get_ai_config_full
from app.services.fridge_zone_service import get_fridge_zones
from app.services.mqtt_client import mqtt_client

logger = logging.getLogger(__name__)

# 等待设备 completed ACK 的超时（秒）。设备文档写入是本地 LittleFS 操作，通常很快；
# 给到 8 秒兼顾 MQTT 往返和设备忙时排队，超时则按"已下发未确认"处理，不阻塞同步主流程。
DEVICE_ACK_TIMEOUT_SECONDS = 8.0

DEVICE_SYNC_DOMAINS = {
    "inventory",
    "fridge_zones",
    "ai_config",
    "asr_config",
    "tts_config",
    "shopping_list",
    "recipe_cache",
    "nutrition_cache",
    "reminder",
    "settings",
}

PENDING_COMMAND_EVENT = "pending_command"
PENDING_COMMAND_STATUS = "pending"
SENT_COMMAND_STATUS = "sent"
FAILED_COMMAND_STATUS = "failed"

_SLOT_TO_CELL = {
    "A1": 0,
    "A2": 1,
    "A3": 2,
    "B1": 3,
    "B2": 4,
    "B3": 5,
    "C1": 6,
    "C2": 7,
    "C3": 8,
}


def _collapse_repeated_unit(unit: str) -> str:
    known_units = ["克", "个", "份", "盒", "瓶", "袋", "根", "把", "颗", "杯", "块"]
    if unit and set(unit) == {"g"}:
        return "g"
    for known in known_units:
        if unit == known * (len(unit) // len(known)) and len(unit) > len(known):
            return known
    return unit


def _clean_device_quantity_text(value: object) -> str:
    text = str(value or "").strip()
    if not text:
        return ""
    import re

    match = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)(.*)$", text)
    if not match:
        return text[:32]
    number = match.group(1)
    unit = _collapse_repeated_unit(match.group(2).strip())
    if "." not in number and number.isdigit():
        gram_like = unit in {"g", "克", "ml", "毫升"}
        max_digits = 3 if gram_like else 2
        polluted_prefix = len(number) > max_digits and number.startswith("11")
        while polluted_prefix and len(number) > max_digits and number.startswith("1"):
            number = number[1:]
        if len(number) > 1 and set(number) == {"1"}:
            number = "1"
    return f"{number}{unit}"[:32]


def _now_ms() -> int:
    return int(datetime.now(timezone.utc).timestamp() * 1000)


def _days_left(expire_date: date | None) -> int | None:
    if expire_date is None:
        return None
    return (expire_date - datetime.now(timezone.utc).date()).days


async def _active_devices(db: AsyncSession, home_id: UUID) -> list[Device]:
    result = await db.execute(
        select(Device)
        .join(DeviceBinding, DeviceBinding.device_id == Device.id)
        .where(DeviceBinding.home_id == home_id, DeviceBinding.status == "active")
        .order_by(DeviceBinding.created_at.asc())
    )
    return list(result.scalars().all())


async def _store_pending_command(
    db: AsyncSession,
    *,
    device: Device,
    home_id: UUID,
    command: str,
    payload: dict[str, Any],
    reason: str,
) -> None:
    """把离线/未确认的设备命令写入轻量 outbox。

    项目当前没有独立设备命令表，复用 DeviceStatusEvent 记录 pending_command。
    payload 内保存完整命令，设备上线后由 drain_pending_device_commands() 补发。
    """
    request_id = str(payload.get("request_id") or "")
    existing = None
    if request_id:
        existing = (
            await db.execute(
                select(DeviceStatusEvent).where(
                    DeviceStatusEvent.device_id == device.id,
                    DeviceStatusEvent.device_sn == device.device_sn,
                    DeviceStatusEvent.event_type == PENDING_COMMAND_EVENT,
                    DeviceStatusEvent.payload["request_id"].astext == request_id,
                    DeviceStatusEvent.payload["status"].astext == PENDING_COMMAND_STATUS,
                )
            )
        ).scalar_one_or_none()
    body = {
        "status": PENDING_COMMAND_STATUS,
        "homeId": str(home_id),
        "deviceSn": device.device_sn,
        "command": command,
        "payload": payload,
        "request_id": request_id,
        "reason": reason,
        "attempts": 0,
        "queuedAt": datetime.now(timezone.utc).isoformat(),
    }
    if existing is None:
        db.add(
            DeviceStatusEvent(
                device_id=device.id,
                device_sn=device.device_sn,
                event_type=PENDING_COMMAND_EVENT,
                payload=body,
            )
        )
    else:
        merged = dict(existing.payload or {})
        merged.update(body)
        merged["attempts"] = int(merged.get("attempts") or 0)
        existing.payload = merged


async def drain_pending_device_commands(
    db: AsyncSession,
    *,
    device_sn: str,
    limit: int = 20,
) -> tuple[int, int]:
    """设备上线后补发此前因为离线/未确认而缓存的命令。

    返回 (sent, failed)。这里不等待 completed ACK，避免 MQTT state 回调链阻塞；
    真正手动同步路径仍可通过 wait_for_ack=True 得到执行确认。
    """
    rows = (
        await db.execute(
            select(DeviceStatusEvent)
            .where(
                DeviceStatusEvent.device_sn == device_sn,
                DeviceStatusEvent.event_type == PENDING_COMMAND_EVENT,
                DeviceStatusEvent.payload["status"].astext == PENDING_COMMAND_STATUS,
            )
            .order_by(DeviceStatusEvent.created_at.asc())
            .limit(limit)
        )
    ).scalars().all()
    sent = 0
    failed = 0
    for row in rows:
        payload = dict(row.payload or {})
        command = str(payload.get("command") or "")
        command_payload = payload.get("payload") if isinstance(payload.get("payload"), dict) else {}
        attempts = int(payload.get("attempts") or 0) + 1
        payload["attempts"] = attempts
        payload["lastAttemptAt"] = datetime.now(timezone.utc).isoformat()
        try:
            await mqtt_client.publish_command(device_sn, command, command_payload)
        except Exception as exc:  # noqa: BLE001
            failed += 1
            payload["lastError"] = str(exc)
            if attempts >= 5:
                payload["status"] = FAILED_COMMAND_STATUS
            row.payload = payload
            continue
        sent += 1
        payload["status"] = SENT_COMMAND_STATUS
        payload["sentAt"] = datetime.now(timezone.utc).isoformat()
        row.payload = payload
    if rows:
        await db.commit()
    return sent, failed


async def _publish_and_wait_ack(
    device_sn: str,
    command: str,
    payload: dict[str, Any],
) -> bool:
    """发布一条设备命令并等待 completed ACK。

    返回 True 表示设备回了 completed 且 ok=true（命令真正落地）；
    返回 False 表示 publish 失败、等待超时、或设备回了失败 ACK。
    依赖 payload['request_id'] 作为 ACK 关联键（push_command 会原样透传）。
    超时只代表"未在窗口内确认"，命令可能仍已送达；调用方据此区分 queued / acked。
    """
    request_id = str(payload.get("request_id") or "")
    waiter = mqtt_client.register_ack_waiter(request_id) if request_id else None
    try:
        await mqtt_client.publish_command(device_sn, command, payload)
    except Exception:
        if request_id:
            mqtt_client.unregister_ack_waiter(request_id)
        raise
    if waiter is None:
        return False
    try:
        ack = await asyncio.wait_for(waiter, timeout=DEVICE_ACK_TIMEOUT_SECONDS)
    except asyncio.TimeoutError:
        mqtt_client.unregister_ack_waiter(request_id)
        return False
    except asyncio.CancelledError:
        mqtt_client.unregister_ack_waiter(request_id)
        raise
    return bool(ack.get("ok", True)) and ack.get("stage") == "completed"


def _device_zone_id(zone_key: str | None, zones: list[FridgeZoneConfig]) -> int:
    if not zone_key:
        return 1
    standard = {"freezer": 0, "left": 1, "right": 2, "door": 3}
    if zone_key in standard:
        return standard[zone_key]
    for idx, zone in enumerate(zones[:6]):
        if zone.key == zone_key:
            return idx
    return 4


def _device_zones(zones: list[FridgeZoneConfig]) -> list[dict[str, Any]]:
    device_zones: list[dict[str, Any]] = []
    for idx, zone in enumerate(zones[:6]):
        device_zones.append(
            {
                "id": idx,
                "key": zone.key,
                "name": zone.label,
                "note": zone.hint,
                "custom": zone.custom,
                "width": zone.width,
                "height": zone.height,
            }
        )
    return device_zones


async def build_device_inventory_document(
    db: AsyncSession,
    *,
    home_id: UUID,
    server_revision: int,
) -> dict[str, Any]:
    """把云端库存表转换为固件 UI 库存整快照。

    固件屏幕模型目前使用 numeric zone + 0..8 cell，因此这里保留云端 zone key，
    同时生成设备可直接应用的 zone/cell 字段。
    """
    zones_data = await get_fridge_zones(db, home_id)
    zones = list(zones_data.zones)
    items = (
        await db.execute(
            select(InventoryItem)
            .where(InventoryItem.home_id == home_id, InventoryItem.status != "deleted")
            .order_by(InventoryItem.updated_at.desc())
        )
    ).scalars().all()

    device_items: list[dict[str, Any]] = []
    data_updated_at_ms = 0
    for item in items:
        if item.updated_at is not None:
            data_updated_at_ms = max(data_updated_at_ms, int(item.updated_at.timestamp() * 1000))
        zone_id = _device_zone_id(item.zone, zones)
        cell = _SLOT_TO_CELL.get(str(item.slot or "").upper(), 4)
        quantity = float(item.quantity) if item.quantity is not None else 1.0
        extra = item.extra if isinstance(item.extra, dict) else {}
        device_quantity_text = extra.get("deviceQuantityText")
        if isinstance(device_quantity_text, str) and device_quantity_text.strip():
            quantity_text = _clean_device_quantity_text(device_quantity_text)
        elif quantity.is_integer():
            quantity_text = _clean_device_quantity_text(f"{int(quantity)}{item.unit or '份'}")
        else:
            quantity_text = _clean_device_quantity_text(f"{quantity:g}{item.unit or '份'}")
        parsed_quantity = quantity
        parsed_unit = item.unit
        import re

        quantity_match = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*([^\d\s].*)?\s*$", quantity_text)
        if quantity_match:
            parsed_quantity = float(quantity_match.group(1))
            parsed_unit = (quantity_match.group(2) or item.unit or "份").strip()
        days_left = _days_left(item.expire_date)
        stored_at = extra.get("storedAt") or extra.get("stored_at")
        device_items.append(
            {
                "id": str(item.id),
                "name": item.name,
                "category": item.category,
                "quantity": quantity_text,
                "quantityValue": parsed_quantity,
                "unit": parsed_unit,
                "expire_date": item.expire_date.isoformat() if item.expire_date else "",
                "days_left": days_left,
                "location": item.location or "",
                "storedAt": stored_at if isinstance(stored_at, str) else "",
                "zone": zone_id,
                "zoneKey": item.zone,
                "slot": item.slot,
                "cell": cell,
                "source": item.source,
            }
        )

    return {
        "schema_version": 1,
        "snapshotVersion": server_revision,
        "serverRevision": server_revision,
        "updatedAtMs": data_updated_at_ms or _now_ms(),
        "source": "cloud_snapshot",
        "zones": _device_zones(zones),
        "items": device_items,
    }


def _asr_payload(full_config: dict[str, Any]) -> dict[str, Any]:
    return {
        "asrApiBaseUrl": full_config.get("asrApiBaseUrl"),
        "asrModel": full_config.get("asrModel"),
        "asrApiKey": full_config.get("asrApiKey", ""),
        "asrTimeoutMs": full_config.get("asrTimeoutMs"),
        "configUpdatedAt": full_config.get("configUpdatedAt", 0),
    }


def _tts_payload(full_config: dict[str, Any]) -> dict[str, Any]:
    return {
        "ttsApiBaseUrl": full_config.get("ttsApiBaseUrl"),
        "ttsModel": full_config.get("ttsModel"),
        "ttsVoice": full_config.get("ttsVoice"),
        "ttsApiKey": full_config.get("ttsApiKey", ""),
        "ttsTimeoutMs": full_config.get("ttsTimeoutMs"),
        "configUpdatedAt": full_config.get("configUpdatedAt", 0),
    }


async def _load_config_doc(db: AsyncSession, home_id: UUID, key: str) -> dict[str, Any]:
    row = (
        await db.execute(
            select(SystemConfig).where(SystemConfig.home_id == home_id, SystemConfig.config_key == key)
        )
    ).scalar_one_or_none()
    value = dict(row.value or {}) if row is not None else {}
    value.setdefault("items", [])
    value["source"] = value.get("source") or "cloud_snapshot"
    return value


async def _build_reminder_document(db: AsyncSession, home_id: UUID, server_revision: int) -> dict[str, Any]:
    reminders = (
        await db.execute(select(Reminder).where(Reminder.home_id == home_id).order_by(Reminder.updated_at.desc()))
    ).scalars().all()
    return {
        "schema_version": 1,
        "serverRevision": server_revision,
        "updatedAtMs": _now_ms(),
        "source": "cloud_snapshot",
        "reminders": [
            {
                "id": str(item.id),
                "reminderType": item.reminder_type,
                "title": item.title,
                "content": item.content,
                "status": item.status,
                "dueAt": item.due_at.isoformat() if item.due_at else None,
                "ackedAt": item.acked_at.isoformat() if item.acked_at else None,
            }
            for item in reminders
        ],
    }


async def push_cloud_snapshot_to_devices(
    db: AsyncSession,
    *,
    home_id: UUID,
    server_revision: int,
    domains: set[str] | None = None,
    request_device_inventory: bool = False,
    accept_clean_device_snapshot: bool = False,
    push_cloud_snapshot: bool = True,
    wait_for_ack: bool = False,
) -> SyncDevicePushData:
    """按 domain 把云端当前快照下发给该家庭所有 active 设备。

    投递策略（轻量方案）：
    - 只向当前 online 的设备实际发送；离线设备的命令计入 skipped_offline，依赖固件
      重连时的 publish_reconnect_sync_seed() 自行补发，后端不维护持久 outbox。
    - wait_for_ack=True 时，每条命令发布后等待设备 completed ACK：成功计 acked_count，
      超时/失败只计 queued_count，让"立即同步"能区分"已下发"与"设备已确认写入"。
    - wait_for_ack=False（默认）时保持原 fire-and-forget 行为，仅计 queued_count。

    重要：从 MQTT on_message 回调链里调用本函数时**必须**保持 wait_for_ack=False。
    gmqtt 会 await on_message 以决定 PUBACK，而 cmd_ack 走另一条 on_message；在 state
    处理链里阻塞等 ACK 会与 ACK 自身的投递串行死锁，直到超时。只有 HTTP /sync/device-push
    这种独立请求任务里才允许 wait_for_ack=True。
    """
    selected = set(domains or DEVICE_SYNC_DOMAINS) & DEVICE_SYNC_DOMAINS
    devices = await _active_devices(db, home_id)
    errors: list[str] = []
    command_count = 0
    queued_count = 0
    acked_count = 0
    skipped_offline = 0

    # 这些文档跨设备共享，按需懒加载一次即可。
    inventory_doc: dict[str, Any] | None = None
    full_ai_config: dict[str, Any] | None = None
    shopping_doc: dict[str, Any] | None = None
    recipe_doc: dict[str, Any] | None = None
    nutrition_doc: dict[str, Any] | None = None
    reminder_doc: dict[str, Any] | None = None
    settings_doc: dict[str, Any] | None = None

    for device in devices:
        online = device.status == "online"
        # 先组装这台设备要发的命令列表，再统一过 online 过滤 + publish + 等 ACK。
        commands: list[tuple[str, dict[str, Any]]] = []

        if request_device_inventory:
            commands.append((
                "inventory_refresh",
                {
                    "homeId": str(home_id),
                    "request_id": f"sync-refresh:{server_revision}:{device.device_sn}",
                    "acceptCleanSnapshot": accept_clean_device_snapshot,
                },
            ))
            # 手动同步库存/分区时只请求 ui_inventory 快照：固件的 zones 就在该快照里，
            # 后端会投影成 fridge_zones。只有 AI/购物清单/菜谱等独立文档域才额外请求
            # sync_documents_refresh，避免普通“同步设备”触发大 JSON 打包导致设备重启。
            if selected - {"inventory", "fridge_zones"}:
                commands.append((
                    "sync_documents_refresh",
                    {
                        "homeId": str(home_id),
                        "request_id": f"sync-docs:{server_revision}:{device.device_sn}",
                        "acceptCleanSnapshot": accept_clean_device_snapshot,
                    },
                ))

        if push_cloud_snapshot:
            if server_revision > 0 and selected & {"inventory", "fridge_zones"}:
                if inventory_doc is None:
                    inventory_doc = await build_device_inventory_document(
                        db, home_id=home_id, server_revision=server_revision
                    )
                commands.append((
                    "inventory_replace",
                    {
                        "request_id": f"sync-inventory:{server_revision}:{device.device_sn}",
                        "serverRevision": server_revision,
                        "inventory": inventory_doc,
                    },
                ))

            if "ai_config" in selected:
                if full_ai_config is None:
                    full_ai_config = await get_ai_config_full(db, home_id)
                commands.append((
                    "ai_config_update",
                    {
                        "request_id": f"sync-ai:{server_revision}:{device.device_sn}",
                        **build_device_payload(full_ai_config),
                    },
                ))

            if "asr_config" in selected:
                if full_ai_config is None:
                    full_ai_config = await get_ai_config_full(db, home_id)
                commands.append((
                    "asr_config_update",
                    {
                        "request_id": f"sync-asr:{server_revision}:{device.device_sn}",
                        **_asr_payload(full_ai_config),
                    },
                ))

            if "tts_config" in selected:
                if full_ai_config is None:
                    full_ai_config = await get_ai_config_full(db, home_id)
                commands.append((
                    "tts_config_update",
                    {
                        "request_id": f"sync-tts:{server_revision}:{device.device_sn}",
                        **_tts_payload(full_ai_config),
                    },
                ))

            if "shopping_list" in selected:
                if shopping_doc is None:
                    shopping_doc = await _load_config_doc(db, home_id, "shopping_list")
                    shopping_doc["serverRevision"] = server_revision
                    shopping_doc["updatedAtMs"] = _now_ms()
                commands.append((
                    "shopping_list_update",
                    {"request_id": f"sync-shopping:{server_revision}:{device.device_sn}", **shopping_doc},
                ))

            if "recipe_cache" in selected:
                if recipe_doc is None:
                    recipe_doc = await _load_config_doc(db, home_id, "recipe_cache")
                    recipe_doc["serverRevision"] = server_revision
                    recipe_doc["updatedAtMs"] = _now_ms()
                commands.append((
                    "recipe_cache_update",
                    {"request_id": f"sync-recipe:{server_revision}:{device.device_sn}", **recipe_doc},
                ))

            if "nutrition_cache" in selected:
                if nutrition_doc is None:
                    nutrition_doc = await _load_config_doc(db, home_id, "nutrition_cache")
                    nutrition_doc["serverRevision"] = server_revision
                    nutrition_doc["updatedAtMs"] = _now_ms()
                commands.append((
                    "nutrition_cache_update",
                    {"request_id": f"sync-nutrition:{server_revision}:{device.device_sn}", **nutrition_doc},
                ))

            if "reminder" in selected:
                if reminder_doc is None:
                    reminder_doc = await _build_reminder_document(db, home_id, server_revision)
                commands.append((
                    "reminder_update",
                    {"request_id": f"sync-reminder:{server_revision}:{device.device_sn}", **reminder_doc},
                ))

            if "settings" in selected:
                if settings_doc is None:
                    settings_doc = await _load_config_doc(db, home_id, "settings")
                    settings_doc["serverRevision"] = server_revision
                    settings_doc["updatedAtMs"] = _now_ms()
                commands.append((
                    "preferences_update",
                    {"request_id": f"sync-settings:{server_revision}:{device.device_sn}", **settings_doc},
                ))

        for command, payload in commands:
            command_count += 1
            if not online:
                # 离线设备不实际下发，靠固件重连补发；只统计跳过数，便于前端提示。
                skipped_offline += 1
                await _store_pending_command(
                    db,
                    device=device,
                    home_id=home_id,
                    command=command,
                    payload=payload,
                    reason="device_offline",
                )
                continue
            try:
                if wait_for_ack:
                    acked = await _publish_and_wait_ack(device.device_sn, command, payload)
                    queued_count += 1
                    if acked:
                        acked_count += 1
                    else:
                        errors.append(f"{device.device_sn}: {command} not acked (timeout or failed)")
                        await _store_pending_command(
                            db,
                            device=device,
                            home_id=home_id,
                            command=command,
                            payload=payload,
                            reason="ack_timeout_or_failed",
                        )
                else:
                    await mqtt_client.publish_command(device.device_sn, command, payload)
                    queued_count += 1
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{device.device_sn}: {command} failed: {exc}")

    if errors:
        logger.warning("cloud snapshot push completed with errors: home=%s errors=%s", home_id, errors)

    return SyncDevicePushData(
        device_count=len(devices),
        command_count=command_count,
        queued_count=queued_count,
        acked_count=acked_count,
        skipped_offline=skipped_offline,
        domains=sorted(selected),
        errors=errors,
    )


__all__ = [
    "DEVICE_SYNC_DOMAINS",
    "build_device_inventory_document",
    "drain_pending_device_commands",
    "push_cloud_snapshot_to_devices",
]
