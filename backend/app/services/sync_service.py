"""三端数据备份与同步服务层。

第一版采用后写覆盖策略：每个被接受的事件都会获得新的 server_revision，并立即
投影到当前业务表或 SystemConfig 文档。固件库存回流使用整份快照，后端负责把
设备 UI JSON 转成云端 InventoryItem。
"""

from __future__ import annotations

import re
from datetime import date, datetime, timedelta, timezone
from decimal import Decimal, InvalidOperation
from typing import Any
from uuid import UUID, uuid4

from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.ai_chat import AiChatMessage
from app.models.device import Device
from app.models.home import Home
from app.models.inventory import InventoryItem
from app.models.reminder import Reminder
from app.models.system_config import SystemConfig
from app.models.sync import SyncEvent, SyncState
from app.models.user import User
from app.schemas.ai_config import AiConfigUpdateRequest
from app.schemas.common import JsonDict
from app.schemas.fridge_zone import FridgeZoneConfig
from app.schemas.inventory import InventoryItemSchema, InventoryUpdateRequest
from app.schemas.reminder import ReminderSchema
from app.schemas.sync import (
    AiHistorySnapshot,
    NutritionCacheData,
    RecipeCacheData,
    ShoppingListData,
    SyncEventSchema,
    SyncPullData,
    SyncPushData,
    SyncPushEvent,
    SyncSnapshotData,
    SyncStatusData,
)
from app.services.ai_chat_history_service import DEFAULT_SESSION_ID, list_history
from app.services.ai_config_service import get_ai_config, should_accept_device_config, upsert_ai_config
from app.services.fridge_zone_service import DEFAULT_ZONES, FRIDGE_ZONES_KEY, get_fridge_zones, upsert_fridge_zones

SHOPPING_LIST_KEY = "shopping_list"
RECIPE_CACHE_KEY = "recipe_cache"
NUTRITION_CACHE_KEY = "nutrition_cache"
SETTINGS_KEY = "settings"


def _now() -> datetime:
    return datetime.now(timezone.utc)


async def _get_state(db: AsyncSession, home_id: UUID, *, create: bool = True) -> SyncState:
    """读取或创建家庭同步游标。"""
    result = await db.execute(select(SyncState).where(SyncState.home_id == home_id))
    state = result.scalar_one_or_none()
    if state is None and create:
        state = SyncState(home_id=home_id, current_revision=0)
        db.add(state)
        await db.flush()
    if state is None:
        raise RuntimeError("sync state not found")
    return state


async def _next_revision(db: AsyncSession, home_id: UUID, source: str, client_event_id: str) -> int:
    """生成家庭内单调递增 revision。"""
    state = await _get_state(db, home_id)
    state.current_revision += 1
    state.last_source = source
    state.last_client_event_id = client_event_id
    await db.flush()
    return int(state.current_revision)


def _event_to_schema(event: SyncEvent) -> SyncEventSchema:
    return SyncEventSchema(
        id=event.id,
        client_event_id=event.client_event_id,
        domain=event.domain,
        op=event.op,
        source=event.source,
        server_revision=int(event.server_revision),
        client_revision=int(event.client_revision) if event.client_revision is not None else None,
        device_sn=event.device_sn,
        payload=event.payload or {},
        created_at=event.created_at,
    )


def _derived_client_event_id(parent_id: str, suffix: str) -> str:
    """生成派生事件 id，保持幂等同时避免超过数据库字段长度。"""
    tail = f":{suffix}"
    max_parent_len = 128 - len(tail)
    return f"{parent_id[:max_parent_len]}{tail}"


def _item_to_schema(item: InventoryItem) -> InventoryItemSchema:
    extra = item.extra if isinstance(item.extra, dict) else {}
    return InventoryItemSchema(
        id=item.id,
        name=item.name,
        category=item.category,
        quantity=float(item.quantity) if item.quantity is not None else 0.0,
        unit=item.unit,
        zone=item.zone,
        slot=item.slot,
        zone_id=item.zone,
        layer_id=extra.get("layerId"),
        cell=extra.get("cell"),
        location=item.location,
        expire_date=item.expire_date,
        status=item.status,
        source=item.source,
        confidence=item.confidence,
        updated_at=item.updated_at,
        extra=extra,
    )


def _reminder_to_schema(item: Reminder) -> ReminderSchema:
    return ReminderSchema(
        id=item.id,
        reminder_type=item.reminder_type,
        title=item.title,
        content=item.content,
        status=item.status,
        due_at=item.due_at,
        acked_at=item.acked_at,
    )


async def _resolve_device(db: AsyncSession, device_sn: str | None) -> Device | None:
    if not device_sn:
        return None
    return (await db.execute(select(Device).where(Device.device_sn == device_sn))).scalar_one_or_none()


def _slot_from_cell(cell: Any) -> str | None:
    slots = ["A1", "A2", "A3", "B1", "B2", "B3", "C1", "C2", "C3"]
    try:
        idx = int(cell)
    except (TypeError, ValueError):
        return None
    if idx < 0 or idx >= len(slots):
        return None
    return slots[idx]


def _parse_date(value: Any) -> date | None:
    """兼容小程序 ISO 日期和设备空值；无法解析时不覆盖原值。"""
    if value is None or value == "":
        return None
    if isinstance(value, date) and not isinstance(value, datetime):
        return value
    if isinstance(value, datetime):
        return value.date()
    if isinstance(value, str):
        try:
            return date.fromisoformat(value[:10])
        except ValueError:
            return None
    return None


def _parse_device_expire_date(raw: dict[str, Any], fallback: date | None = None) -> date | None:
    """把设备 UI 的“今天/明天/N天后/days_left”恢复成云端日期。

    设备库存快照是演示 UI 友好的相对文案；如果无法解析，不清空云端旧日期，
    避免一次设备回流把小程序首页的临期食品全部抹掉。
    """
    parsed = _parse_date(raw.get("expireDate") or raw.get("expire_date"))
    if parsed is not None:
        return parsed

    today = _now().date()
    days_raw = raw.get("days_left")
    if days_raw is None:
        days_raw = raw.get("daysLeft")
    try:
        days = int(days_raw)
        return today + timedelta(days=max(days, 0))
    except (TypeError, ValueError):
        pass

    text = str(raw.get("expire_date") or raw.get("expireDate") or "").strip()
    if "今天" in text:
        return today
    if "明天" in text:
        return today + timedelta(days=1)
    match = re.search(r"(\d+)\s*天后", text)
    if match:
        return today + timedelta(days=int(match.group(1)))
    return fallback


def _parse_quantity(value: Any, fallback: Any) -> Decimal:
    """库存数量写入 Numeric 列前统一收敛，避免字符串直接落库。"""
    raw = fallback if value is None else value
    try:
        return Decimal(str(raw if raw is not None else 1))
    except (InvalidOperation, ValueError, TypeError):
        return Decimal("1")


def _coerce_updated_at_ms(value: Any) -> int:
    """把毫秒数 / ISO 字符串 / datetime 统一成毫秒整数；无法解析返回 0。"""
    if value is None or value == "":
        return 0
    if isinstance(value, bool):  # bool 是 int 子类，先排除避免 True->1
        return 0
    if isinstance(value, (int, float)):
        return int(value)
    if isinstance(value, datetime):
        return int(value.timestamp() * 1000)
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return 0
        if text.isdigit():
            return int(text)
        try:
            dt = datetime.fromisoformat(text.replace("Z", "+00:00"))
        except ValueError:
            return 0
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp() * 1000)
    return 0


def _event_updated_at_ms(payload: dict[str, Any]) -> int:
    """从事件 payload 推断"真实编辑时间"（毫秒），用于后写覆盖判定。

    兼容多种来源字段：小程序 updatedAtMs/updatedAt、AI 配置 configUpdatedAt、
    以及被 item/payload 包裹的在线 REST 入队结构。无任何时间字段时返回 0，
    调用方应把 0 视为"无时间信息"，不参与"较旧则跳过"的判断（保持旧行为）。
    """
    if not isinstance(payload, dict):
        return 0
    candidates = (
        payload.get("updatedAtMs"),
        payload.get("updated_at_ms"),
        payload.get("updatedAt"),
        payload.get("updated_at"),
        payload.get("configUpdatedAt"),
        payload.get("config_updated_at"),
    )
    best = 0
    for raw in candidates:
        best = max(best, _coerce_updated_at_ms(raw))
    # 在线 REST 入队会把服务端返回 item 一并塞入；item 内也可能带 updatedAt。
    for nested_key in ("item", "payload"):
        nested = payload.get(nested_key)
        if isinstance(nested, dict):
            best = max(best, _event_updated_at_ms(nested))
    return best


def _item_effective_updated_at_ms(item: InventoryItem) -> int:
    """库存条目的有效编辑时间（毫秒）。

    优先用 extra.syncUpdatedAtMs（同步通道写入时记录），缺省时回退到 DB 自动维护的
    updated_at 列。这样在线 REST 直接写表、未经同步通道的条目也有可靠的"较新"判据，
    避免设备旧快照把这些条目当成孤儿删掉。
    """
    extra = item.extra if isinstance(item.extra, dict) else {}
    extra_ms = _coerce_updated_at_ms(extra.get("syncUpdatedAtMs"))
    column_ms = _coerce_updated_at_ms(item.updated_at) if item.updated_at is not None else 0
    return max(extra_ms, column_ms)


async def _home_current_revision(db: AsyncSession, home_id: UUID) -> int:
    """云端该 home 当前 server_revision（同步游标，每次编辑都自增）。

    用于设备快照回流的结构变更门槛：设备快照里带它上次同步到的 serverRevision，
    若落后于云端当前 revision，说明云端有设备没见过的新编辑（如小程序删空库存），
    此时一份"未脏"的旧设备快照不得改变云端结构。
    """
    row = (
        await db.execute(select(SyncState.current_revision).where(SyncState.home_id == home_id))
    ).scalar_one_or_none()
    return int(row or 0)


def _collapse_repeated_unit(unit: str) -> str:
    known_units = ["克", "个", "份", "盒", "瓶", "袋", "根", "把", "颗", "杯", "块"]
    if unit and set(unit) == {"g"}:
        return "g"
    for known in known_units:
        if unit == known * (len(unit) // len(known)) and len(unit) > len(known):
            return known
    return unit


def _clean_device_quantity_text(value: Any) -> str:
    """清洗旧同步循环污染出来的 111 前缀和重复单位，结果可重复执行。"""
    text = str(value or "").strip()
    if not text:
        return ""
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


def _parse_device_quantity_text(value: Any) -> tuple[Decimal, str]:
    """把设备端 UI 数量文本拆成 Numeric 数量和单位，避免 200g 被写成 quantity=1/unit=200g。"""
    text = _clean_device_quantity_text(value)
    if not text:
        return Decimal("1"), "份"
    match = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*([^\d\s].*)?\s*$", text)
    if not match:
        return Decimal("1"), text[:24]
    try:
        quantity = Decimal(match.group(1))
    except InvalidOperation:
        quantity = Decimal("1")
    unit = (match.group(2) or "份").strip()[:24]
    return quantity, unit or "份"


def _inventory_payload_from_event(payload: dict[str, Any]) -> dict[str, Any]:
    """兼容小程序队列里 item/payload 包裹的库存事件。

    在线 REST 写入成功后，小程序会把服务端返回 item 和原始 payload 一起入队；
    离线时则只有 payload。同步服务统一摊平成 InventoryItem 可识别的字段。
    """
    raw_payload = payload.get("payload")
    raw_item = payload.get("item")
    merged: dict[str, Any] = {}
    if isinstance(raw_payload, dict):
        merged.update(raw_payload)
    if isinstance(raw_item, dict):
        merged.update(raw_item)
    for key, value in payload.items():
        if key not in {"payload", "item"}:
            merged.setdefault(key, value)
    return merged or payload


def _zone_key_from_device_zone(zone: Any, zones: list[dict[str, Any]]) -> str | None:
    try:
        zone_id = int(zone)
    except (TypeError, ValueError):
        return zone if isinstance(zone, str) else None
    if 0 <= zone_id < len(zones):
        raw = zones[zone_id]
        if isinstance(raw.get("key"), str):
            return raw["key"]
        standard = ["freezer", "left", "right", "door"]
        if zone_id < len(standard):
            return standard[zone_id]
        return f"custom_{zone_id - 3}"
    return None


def _zones_from_device_snapshot(zones: list[dict[str, Any]]) -> list[FridgeZoneConfig]:
    """把固件 ui_inventory.zones 转成家庭级 fridge_zones 配置。

    固件使用 0..5 的数字 id；云端/小程序使用 freezer/left/right/door/custom_*。
    设备新增自定义区时只会上报库存快照，所以这里必须顺手把 zones 投影到 SystemConfig，
    小程序下次 pull 才能看到自定义空间。
    """
    result: list[FridgeZoneConfig] = []
    standard_keys = ["freezer", "left", "right", "door"]
    for idx, raw in enumerate(zones[:6]):
        if not isinstance(raw, dict):
            continue
        key = raw.get("key")
        if not isinstance(key, str) or not key:
            key = standard_keys[idx] if idx < len(standard_keys) else f"custom_{idx - 3}"
        label = raw.get("label") or raw.get("name") or key
        hint = raw.get("hint") or raw.get("note") or ""
        custom = bool(raw.get("custom")) if idx >= len(standard_keys) else False
        if custom and str(label).strip() in {"自定义区", "备用区"}:
            # 固件删除自定义区后会保留 4/5 号槽位作为占位；占位不是用户真实分区，
            # 不应同步到小程序，否则删除后会再次显示出来。
            continue
        try:
            result.append(
                FridgeZoneConfig(
                    key=str(key)[:32],
                    label=str(label)[:32],
                    hint=str(hint)[:64],
                    custom=custom,
                    width=int(raw.get("width") or 1),
                    height=int(raw.get("height") or 1),
                )
            )
        except Exception:
            continue
    return result


def _zones_payload(zones: list[FridgeZoneConfig]) -> list[dict[str, Any]]:
    """把分区配置转成稳定 JSON 结构，用于变更比较和事件 payload。"""
    return [zone.model_dump(mode="json", by_alias=True) for zone in zones]


async def _upsert_inventory_item(
    db: AsyncSession,
    *,
    home_id: UUID,
    payload: dict[str, Any],
    source: str,
    device: Device | None,
    updated_at_ms: int = 0,
) -> None:
    """把小程序/同步事件中的单条库存写入 InventoryItem。

    updated_at_ms 为本次写入的"真实编辑时间"（毫秒）。当目标条目 extra 里记录的
    syncUpdatedAtMs 比本次更新，且本次确实带了时间戳时，按后写覆盖策略跳过覆盖，
    避免离线队列回放较旧 payload 把另一端的较新修改抹掉。
    """
    item_id = payload.get("id") or payload.get("itemId") or payload.get("item_id")
    item: InventoryItem | None = None
    if item_id:
        try:
            item = await db.get(InventoryItem, UUID(str(item_id)))
        except (ValueError, TypeError):
            item = None
        if item is not None and item.home_id != home_id:
            item = None

    if item is None:
        item = InventoryItem(home_id=home_id, name=str(payload.get("name") or "未命名食材"))
        db.add(item)
        await db.flush()
    else:
        # 后写覆盖：已存条目带有更新的编辑时间时，丢弃这条较旧写入（仍由调用方记录事件供审计）。
        existing_ms = _item_effective_updated_at_ms(item)
        if updated_at_ms > 0 and existing_ms > updated_at_ms:
            return

    if device is not None:
        item.device_id = device.id
    item.name = str(payload.get("name") or item.name or "未命名食材")[:120]
    item.category = payload.get("category")
    item.quantity = _parse_quantity(payload.get("quantity"), item.quantity)
    item.unit = str(payload.get("unit") or item.unit or "份")[:24]
    item.zone = payload.get("zoneId") or payload.get("zone_id") or payload.get("zone")
    item.slot = payload.get("slot")
    item.location = payload.get("location")
    parsed_expire_date = _parse_date(payload.get("expireDate") or payload.get("expire_date"))
    if parsed_expire_date is not None or "expireDate" in payload or "expire_date" in payload:
        item.expire_date = parsed_expire_date
    item.status = str(payload.get("status") or item.status or "active")[:32]
    item.source = source[:32]
    item.confidence = payload.get("confidence")
    next_extra = payload.get("extra") if isinstance(payload.get("extra"), dict) else item.extra or {}
    # 记录本次编辑时间，供后续后写覆盖比较；0 时沿用旧值，避免抹掉历史时间戳。
    next_extra = dict(next_extra)
    layer_id = payload.get("layerId") or payload.get("layer_id")
    if layer_id is not None:
        next_extra["layerId"] = layer_id
    if payload.get("cell") is not None:
        next_extra["cell"] = payload.get("cell")
    if updated_at_ms > 0:
        next_extra["syncUpdatedAtMs"] = updated_at_ms
    item.extra = next_extra


async def _replace_inventory_from_device_snapshot(
    db: AsyncSession,
    *,
    home_id: UUID,
    snapshot: dict[str, Any],
    source: str,
    device: Device | None,
    device_dirty: bool = False,
    force_import: bool = False,
    device_revision: int = 0,
    cloud_revision: int = 0,
) -> list[FridgeZoneConfig]:
    """把设备整份 ui_inventory 快照投影到云端库存表。

    P0 反向覆盖保护的核心：设备 updatedAtMs 来自 esp_timer（自启动相对毫秒），无法与
    云端 epoch 直接比较，因此结构变更门槛改用 dirty 标志 + revision 落后判断：

    - allow_structural = force_import or device_dirty or (device_revision >= cloud_revision)
      · force_import=true → 后端刚通过手动/初次同步请求设备上报，允许把干净快照作为种子导入；
      · device_dirty=true → 设备有未同步的本地编辑，允许增/删/复活；
      · device_revision >= cloud_revision → 设备已看到云端最新编辑（含首次绑定 cloud=0），
        允许整快照对齐；
      · 否则（设备 revision 落后且未脏）→ 云端有设备没见过的新编辑（如小程序删空库存），
        这份快照只作为在线握手事件记录，不再改任何库存字段，避免旧设备快照把小程序
        在设备离线期间的数量、位置、过期时间、新增或删除整体回退。

    单条数值后写覆盖（updated_at_ms 比较）不适用于设备快照（时钟不同源），这里传 0 关闭，
    交由上面的结构门槛 + 字段刷新处理。
    """
    allow_structural = bool(force_import) or bool(device_dirty) or device_revision >= cloud_revision
    if not allow_structural:
        return []
    zones_raw = snapshot.get("zones") if isinstance(snapshot.get("zones"), list) else []
    zones = [z for z in zones_raw if isinstance(z, dict)]
    device_zones = _zones_from_device_snapshot(zones)
    if device_zones:
        current_zones = await get_fridge_zones(db, home_id)
        if _zones_payload(current_zones.zones) != _zones_payload(device_zones):
            await upsert_fridge_zones(db, home_id, device_zones, source=source)
        else:
            device_zones = []
    items_raw = snapshot.get("items") if isinstance(snapshot.get("items"), list) else []
    seen_keys: set[tuple[str | None, str | None, str]] = set()

    for raw in items_raw:
        if not isinstance(raw, dict):
            continue
        zone_key = _zone_key_from_device_zone(raw.get("zone"), zones)
        slot = raw.get("slot") or _slot_from_cell(raw.get("cell"))
        name = str(raw.get("name") or "未命名食材")[:120]
        location = raw.get("location")
        quantity_text = _clean_device_quantity_text(raw.get("quantity"))
        quantity_value, unit = _parse_device_quantity_text(quantity_text)
        item_payload = {
            "name": name,
            "quantity": quantity_value,
            "unit": unit,
            "zone": zone_key,
            "zoneId": raw.get("zoneKey") or raw.get("zone_id") or zone_key,
            "layerId": raw.get("layerId") or raw.get("layer_id"),
            "cell": raw.get("cell"),
            "slot": slot,
            "location": location,
            "expireDate": None,
            "status": "active",
            "source": source,
            "extra": {
                "deviceQuantityText": quantity_text,
                "deviceExpireText": raw.get("expire_date") or raw.get("expireDate"),
                "deviceDaysLeft": raw.get("days_left") or raw.get("daysLeft"),
                "deviceCell": raw.get("cell"),
                "storedAt": raw.get("storedAt") or raw.get("stored_at") or "",
            },
        }
        seen_keys.add((zone_key, slot, name))
        existing = (
            await db.execute(
                select(InventoryItem).where(
                    InventoryItem.home_id == home_id,
                    InventoryItem.zone == zone_key,
                    InventoryItem.slot == slot,
                    InventoryItem.name == name,
                )
            )
        ).scalar_one_or_none()
        parsed_expire_date = _parse_device_expire_date(raw, existing.expire_date if existing is not None else None)
        if existing is not None:
            item_payload["id"] = str(existing.id)
        item_payload["expireDate"] = parsed_expire_date
        # 设备快照时钟与云端不同源，关闭单条数值后写覆盖（传 0），由结构门槛决定能否改结构。
        await _upsert_inventory_item(
            db,
            home_id=home_id,
            payload=item_payload,
            source=source,
            device=device,
            updated_at_ms=0,
        )

    current = (
        await db.execute(
            select(InventoryItem).where(
                InventoryItem.home_id == home_id,
                InventoryItem.status != "deleted",
            )
        )
    ).scalars().all()
    for item in current:
        key = (item.zone, item.slot, item.name)
        if key in seen_keys:
            continue
        item.status = "deleted"
    return device_zones


async def _upsert_config_doc(
    db: AsyncSession,
    *,
    home_id: UUID,
    key: str,
    value: dict[str, Any],
    source: str,
    updated_at_ms: int = 0,
    merge: bool = False,
) -> None:
    now = _now()
    row = (
        await db.execute(
            select(SystemConfig).where(SystemConfig.home_id == home_id, SystemConfig.config_key == key)
        )
    ).scalar_one_or_none()
    # 后写覆盖：已存文档带有更新的编辑时间时，丢弃这条较旧写入，避免离线队列回放旧值。
    if row is not None and updated_at_ms > 0:
        existing_ms = _coerce_updated_at_ms(
            (row.value or {}).get("updatedAtMs") if isinstance(row.value, dict) else None
        )
        if existing_ms > updated_at_ms:
            return
    # merge=True：把传入字段浅合并进已存文档（用于 settings 这类多入口分片写入，
    # 避免 {privacy} 单独写入把 {preferences} 抹掉）。merge=False：整份替换。
    if merge and row is not None and isinstance(row.value, dict):
        next_value = dict(row.value)
        next_value.update(value)
    else:
        next_value = dict(value)
    next_value["source"] = source
    # 记录编辑时间，供后续后写覆盖比较；缺省时用服务端 now。
    next_value["updatedAtMs"] = updated_at_ms or int(now.timestamp() * 1000)
    if row is None:
        db.add(SystemConfig(home_id=home_id, config_key=key, value=next_value, config_updated_at=now))
    else:
        row.value = next_value
        row.config_updated_at = now


async def _apply_event(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
    event: SyncPushEvent,
) -> tuple[Device | None, list[SyncPushEvent]]:
    """把同步事件投影到业务表。"""
    device = await _resolve_device(db, event.device_sn)
    derived_events: list[SyncPushEvent] = []
    payload = event.payload or {}
    event_updated_at_ms = _event_updated_at_ms(payload)

    if event.domain == "inventory":
        inventory_payload = _inventory_payload_from_event(payload)
        if event.op in {"snapshot", "replace"}:
            # P0：设备快照能否改云端结构取决于设备是否带未同步本地修改（dirty）或其 revision
            # 是否已追上云端，不用 updatedAtMs 比较（设备 esp_timer 相对毫秒与云端 epoch 不同源）。
            device_dirty = bool(payload.get("dirty"))
            device_force_import = bool(
                payload.get("forceImport")
                or payload.get("force_import")
                or payload.get("acceptCleanSnapshot")
            )
            device_revision = 0
            try:
                device_revision = int(payload.get("serverRevision") or payload.get("server_revision") or 0)
            except (TypeError, ValueError):
                device_revision = 0
            cloud_revision = await _home_current_revision(db, home.id)
            device_zones = await _replace_inventory_from_device_snapshot(
                db,
                home_id=home.id,
                snapshot=payload,
                source=event.source,
                device=device,
                device_dirty=device_dirty,
                force_import=device_force_import,
                device_revision=device_revision,
                cloud_revision=cloud_revision,
            )
            if device_zones:
                # 设备库存快照里带 zones 时，除了库存事件外补一条分区事件。
                # 这样 /sync/status.domains 与 /sync/pull events 都能看到 fridge_zones，
                # 不再只依赖 snapshot 的隐式投影。
                derived_events.append(
                    SyncPushEvent(
                        client_event_id=_derived_client_event_id(event.client_event_id, "fridge_zones"),
                        domain=FRIDGE_ZONES_KEY,
                        op="replace",
                        source=event.source,
                        device_sn=event.device_sn,
                        client_revision=event.client_revision,
                        payload={
                            "zones": _zones_payload(device_zones),
                            "updatedAtMs": int(_now().timestamp() * 1000),
                            "derivedFrom": event.client_event_id,
                        },
                    )
                )
        elif event.op == "delete":
            item_id = inventory_payload.get("id") or inventory_payload.get("itemId")
            if item_id:
                try:
                    item = await db.get(InventoryItem, UUID(str(item_id)))
                except (ValueError, TypeError):
                    item = None
                if item is not None and item.home_id == home.id:
                    item.status = "deleted"
        else:
            await _upsert_inventory_item(
                db,
                home_id=home.id,
                payload=inventory_payload,
                source=event.source,
                device=device,
                updated_at_ms=event_updated_at_ms,
            )
    elif event.domain == "fridge_zones":
        zones_raw = payload.get("zones") if isinstance(payload.get("zones"), list) else []
        zones = [FridgeZoneConfig.model_validate(z) for z in zones_raw if isinstance(z, dict)]
        await upsert_fridge_zones(db, home.id, zones or list(DEFAULT_ZONES), source=event.source)
    elif event.domain == "ai_config":
        if event.source == "device" and not await should_accept_device_config(db, home.id, payload):
            return device, derived_events
        config_ts_raw = payload.get("configUpdatedAt") or payload.get("config_updated_at")
        try:
            config_ts_ms = int(config_ts_raw) if config_ts_raw is not None else 0
        except (TypeError, ValueError):
            config_ts_ms = 0
        config_dt = datetime.fromtimestamp(config_ts_ms / 1000.0, tz=timezone.utc) if config_ts_ms > 0 else None
        await upsert_ai_config(
            db,
            home.id,
            AiConfigUpdateRequest.model_validate(payload),
            source=event.source,
            config_updated_at=config_dt,
            commit=False,
        )
    elif event.domain in {"shopping_list", "recipe_cache", "nutrition_cache", "settings"}:
        if event.domain == "settings":
            # settings 由 privacy/preferences/api_config 等多个入口分片写入，必须合并而非整替，
            # 否则一次 {privacy} 写入会抹掉 {preferences}。也与 REST /settings 镜像写入对齐。
            await _upsert_config_doc(db, home_id=home.id, key=SETTINGS_KEY, value=payload, source=event.source, updated_at_ms=event_updated_at_ms, merge=True)
        else:
            key = {
                "shopping_list": SHOPPING_LIST_KEY,
                "recipe_cache": RECIPE_CACHE_KEY,
                "nutrition_cache": NUTRITION_CACHE_KEY,
            }[event.domain]
            await _upsert_config_doc(db, home_id=home.id, key=key, value=payload, source=event.source, updated_at_ms=event_updated_at_ms)
    elif event.domain == "reminder":
        if event.op in {"replace", "snapshot"}:
            await _upsert_config_doc(db, home_id=home.id, key="reminder_queue", value=payload, source=event.source, updated_at_ms=event_updated_at_ms)
            return device, derived_events
        reminder_id = payload.get("id") or payload.get("reminderId")
        if reminder_id:
            try:
                reminder = await db.get(Reminder, UUID(str(reminder_id)))
            except (ValueError, TypeError):
                reminder = None
            if reminder is not None and reminder.home_id == home.id:
                reminder.status = str(payload.get("status") or reminder.status)
                reminder.acked_at = _now()
    elif event.domain == "ai_history":
        messages = payload.get("messages") if isinstance(payload.get("messages"), list) else []
        session_id = str(payload.get("sessionId") or DEFAULT_SESSION_ID)[:128]
        for msg in messages[-20:]:
            if not isinstance(msg, dict):
                continue
            role = str(msg.get("role") or "")[:16]
            content = str(msg.get("content") or "")
            if role not in {"user", "assistant"} or not content:
                continue
            db.add(
                AiChatMessage(
                    home_id=home.id,
                    user_id=user.id if user else None,
                    session_id=session_id,
                    role=role,
                    content=content[:4096],
                    source=event.source,
                    device_sn=event.device_sn,
                    extra={"synced": True},
                )
            )
    return device, derived_events


async def push_events(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
    events: list[SyncPushEvent],
) -> SyncPushData:
    """批量接收客户端事件，按 client_event_id 幂等。"""
    accepted: list[SyncEvent] = []
    duplicates = 0

    for incoming in events:
        existing = (
            await db.execute(
                select(SyncEvent).where(
                    SyncEvent.home_id == home.id,
                    SyncEvent.client_event_id == incoming.client_event_id,
                )
            )
        ).scalar_one_or_none()
        if existing is not None:
            duplicates += 1
            accepted.append(existing)
            continue

        device, derived_events = await _apply_event(db, home=home, user=user, event=incoming)
        revision = await _next_revision(db, home.id, incoming.source, incoming.client_event_id)
        row = SyncEvent(
            home_id=home.id,
            user_id=user.id if user else None,
            device_id=device.id if device else None,
            device_sn=incoming.device_sn,
            client_event_id=incoming.client_event_id,
            domain=incoming.domain,
            op=incoming.op,
            source=incoming.source,
            server_revision=revision,
            client_revision=incoming.client_revision,
            payload=incoming.payload,
        )
        db.add(row)
        await db.flush()
        accepted.append(row)
        for derived in derived_events:
            existing_derived = (
                await db.execute(
                    select(SyncEvent).where(
                        SyncEvent.home_id == home.id,
                        SyncEvent.client_event_id == derived.client_event_id,
                    )
                )
            ).scalar_one_or_none()
            if existing_derived is not None:
                accepted.append(existing_derived)
                continue
            derived_device, _ = await _apply_event(db, home=home, user=user, event=derived)
            derived_revision = await _next_revision(db, home.id, derived.source, derived.client_event_id)
            derived_row = SyncEvent(
                home_id=home.id,
                user_id=user.id if user else None,
                device_id=(derived_device.id if derived_device else device.id) if (derived_device or device) else None,
                device_sn=derived.device_sn,
                client_event_id=derived.client_event_id,
                domain=derived.domain,
                op=derived.op,
                source=derived.source,
                server_revision=derived_revision,
                client_revision=derived.client_revision,
                payload=derived.payload,
            )
            db.add(derived_row)
            await db.flush()
            accepted.append(derived_row)

    await db.commit()
    state = await _get_state(db, home.id)
    return SyncPushData(
        server_revision=int(state.current_revision),
        accepted=len(events) - duplicates,
        duplicates=duplicates,
        events=[_event_to_schema(event) for event in accepted],
    )


async def record_server_event(
    db: AsyncSession,
    *,
    home_id: UUID,
    domain: str,
    op: str,
    payload: dict[str, Any],
    user_id: UUID | None = None,
    device_id: UUID | None = None,
    device_sn: str | None = None,
    client_event_id: str | None = None,
) -> SyncEvent:
    """服务端 REST 写接口记录同步事件。

    这些接口已经完成了业务表写入，因此这里只分配 revision 并写事件日志，
    让小程序和设备后续能通过 /sync/pull 看到同一批变更。
    """
    event_id = client_event_id or f"server:{domain}:{op}:{uuid4()}"
    existing = (
        await db.execute(
            select(SyncEvent).where(SyncEvent.home_id == home_id, SyncEvent.client_event_id == event_id)
        )
    ).scalar_one_or_none()
    if existing is not None:
        return existing
    revision = await _next_revision(db, home_id, "server", event_id)
    row = SyncEvent(
        home_id=home_id,
        user_id=user_id,
        device_id=device_id,
        device_sn=device_sn,
        client_event_id=event_id,
        domain=domain,
        op=op,
        source="server",
        server_revision=revision,
        payload=payload,
    )
    db.add(row)
    await db.flush()
    return row


async def get_status(db: AsyncSession, home_id: UUID) -> SyncStatusData:
    from app.core.config import settings
    from app.services.mqtt_client import mqtt_client

    state = await _get_state(db, home_id)
    domain_rows = await db.execute(
        select(SyncEvent.domain, func.max(SyncEvent.server_revision))
        .where(SyncEvent.home_id == home_id)
        .group_by(SyncEvent.domain)
    )
    domains = {domain: int(revision or 0) for domain, revision in domain_rows.all()}
    zones_row = (
        await db.execute(
            select(SystemConfig).where(
                SystemConfig.home_id == home_id,
                SystemConfig.config_key == FRIDGE_ZONES_KEY,
            )
        )
    ).scalar_one_or_none()
    # 设备整份库存快照会顺手投影 fridge_zones 到 SystemConfig；旧版本没有为这个
    # 派生写入额外记录 SyncEvent，导致 status.domains 看起来没有分区变更。
    # 这里用当前家庭 revision 补一个摘要，真实数据仍以 /sync/pull snapshot 为准。
    if zones_row is not None and domains.get(FRIDGE_ZONES_KEY, 0) <= 0:
        domains[FRIDGE_ZONES_KEY] = int(state.current_revision)
    return SyncStatusData(
        server_revision=int(state.current_revision),
        last_synced_at=state.updated_at,
        pending_events=0,
        domains=domains,
        mqtt_connected=mqtt_client.is_connected(),
        mqtt_broker=f"{settings.mqtt_broker_host}:{settings.mqtt_broker_port}",
    )


async def _load_config_doc(db: AsyncSession, home_id: UUID, key: str) -> tuple[dict[str, Any], datetime | None]:
    row = (
        await db.execute(
            select(SystemConfig).where(SystemConfig.home_id == home_id, SystemConfig.config_key == key)
        )
    ).scalar_one_or_none()
    return (row.value if row else {}, row.config_updated_at if row else None)


async def build_snapshot(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
) -> SyncSnapshotData:
    """构建当前家庭完整备份快照。"""
    state = await _get_state(db, home.id)
    inventory = (
        await db.execute(
            select(InventoryItem)
            .where(InventoryItem.home_id == home.id, InventoryItem.status != "deleted")
            .order_by(InventoryItem.updated_at.desc())
        )
    ).scalars().all()
    reminders = (
        await db.execute(select(Reminder).where(Reminder.home_id == home.id).order_by(Reminder.updated_at.desc()))
    ).scalars().all()
    zones = await get_fridge_zones(db, home.id)
    ai_config = await get_ai_config(db, home.id)
    shopping, shopping_updated = await _load_config_doc(db, home.id, SHOPPING_LIST_KEY)
    recipes, recipes_updated = await _load_config_doc(db, home.id, RECIPE_CACHE_KEY)
    nutrition, nutrition_updated = await _load_config_doc(db, home.id, NUTRITION_CACHE_KEY)
    settings_doc, settings_updated = await _load_config_doc(db, home.id, SETTINGS_KEY)
    session_id, history = await list_history(db, home_id=home.id, session_id=DEFAULT_SESSION_ID)
    history_payload = [msg.model_dump(mode="json", by_alias=True) for msg in history]
    if settings_updated:
        settings_doc = dict(settings_doc)
        settings_doc.setdefault("updatedAt", settings_updated.isoformat())
    return SyncSnapshotData(
        server_revision=int(state.current_revision),
        generated_at=_now(),
        inventory=[_item_to_schema(item) for item in inventory],
        fridge_zones=zones.zones,
        ai_config=ai_config,
        reminders=[_reminder_to_schema(item) for item in reminders],
        shopping_list=ShoppingListData(items=shopping.get("items") or [], updated_at=shopping_updated),
        recipe_cache=RecipeCacheData(items=recipes.get("items") or [], updated_at=recipes_updated),
        nutrition_cache=NutritionCacheData(
            schema_version=int(nutrition.get("schema_version") or nutrition.get("schemaVersion") or 1),
            source=nutrition.get("source"),
            generated_at=nutrition.get("generated_at") or nutrition.get("generatedAt"),
            nutrition=nutrition.get("nutrition") if isinstance(nutrition.get("nutrition"), dict) else {},
            updated_at=nutrition_updated,
        ),
        settings=settings_doc,
        ai_history=AiHistorySnapshot(session_id=session_id, messages=history_payload),
    )


async def pull_changes(
    db: AsyncSession,
    *,
    home: Home,
    user: User | None,
    since_revision: int,
) -> SyncPullData:
    state = await _get_state(db, home.id)
    rows = (
        await db.execute(
            select(SyncEvent)
            .where(SyncEvent.home_id == home.id, SyncEvent.server_revision > since_revision)
            .order_by(SyncEvent.server_revision.asc())
            .limit(200)
        )
    ).scalars().all()
    snapshot = await build_snapshot(db, home=home, user=user)
    return SyncPullData(
        server_revision=int(state.current_revision),
        events=[_event_to_schema(row) for row in rows],
        snapshot=snapshot,
    )


__all__ = [
    "build_snapshot",
    "get_status",
    "pull_changes",
    "push_events",
    "record_server_event",
]
