"""P4 Agent MQTT v2 bridge。

MQTT 只传轻量事件、命令、状态和 ACK；大型全量快照由设备使用 HTTP 获取。
"""

from __future__ import annotations

import json
import logging
from datetime import datetime, timezone
from typing import Any
from uuid import UUID, uuid4

from sqlalchemy import select

from app.core.config import settings
from app.db.session import AsyncSessionLocal
from app.models.device import Device, DeviceBinding, DeviceStatusEvent
from app.models.p4_sync import P4DeviceCredential, P4SyncEvent
from app.schemas.v2 import P4SyncEventIn, P4SyncEventOut
from app.services.p4_realtime import p4_realtime_hub
from app.services.p4_sync import _redact, apply_events

logger = logging.getLogger(__name__)
MAX_ENVELOPE_BYTES = 64 * 1024


class P4MqttBridge:
    def topics(self) -> list[tuple[str, int]]:
        prefix = settings.mqtt_topic_prefix
        return [(f"{prefix}/v2/+/+/up/+", 1)]

    async def handle(self, topic: str, payload: bytes) -> bool:
        parts = topic.split("/")
        if len(parts) != 6 or parts[1] != "v2" or parts[4] != "up":
            return False
        if len(payload) > MAX_ENVELOPE_BYTES:
            logger.warning("P4 MQTT payload rejected: topic=%s bytes=%s", topic, len(payload))
            return True
        try:
            envelope = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            logger.warning("P4 MQTT invalid JSON: topic=%s", topic)
            return True
        if not isinstance(envelope, dict) or int(envelope.get("schemaVersion", 0)) != 2:
            return True

        try:
            home_id = UUID(parts[2])
        except ValueError:
            return True
        device_sn = parts[3]
        if (
            str(envelope.get("homeId")) != str(home_id)
            or str(envelope.get("deviceId")) != device_sn
            or not envelope.get("messageId")
        ):
            logger.warning("P4 MQTT envelope/topic identity mismatch: topic=%s", topic)
            return True
        kind = parts[5]
        async with AsyncSessionLocal() as db:
            row = (
                await db.execute(
                    select(Device, DeviceBinding, P4DeviceCredential)
                    .join(
                        DeviceBinding,
                        (DeviceBinding.device_id == Device.id)
                        & (DeviceBinding.status == "active"),
                    )
                    .join(P4DeviceCredential, P4DeviceCredential.device_id == Device.id)
                    .where(
                        Device.device_sn == device_sn,
                        DeviceBinding.home_id == home_id,
                        P4DeviceCredential.active.is_(True),
                    )
                    .order_by(DeviceBinding.created_at.asc())
                    .limit(1)
                )
            ).one_or_none()
            if row is None:
                logger.warning("P4 MQTT rejected unknown binding: home=%s device=%s", home_id, device_sn)
                return True
            device, binding, credential = row
            if int(envelope.get("bindingEpoch", -1)) != credential.binding_epoch:
                logger.warning("P4 MQTT rejected stale binding epoch: device=%s", device_sn)
                return True

            if kind == "event":
                await self._handle_events(db, home_id, device, envelope)
            elif kind == "state":
                await self._handle_state(db, home_id, device, envelope)
            elif kind in {"ack", "error"}:
                await self._handle_status_event(db, home_id, device, kind, envelope)
        return True

    async def _handle_events(self, db, home_id: UUID, device: Device, envelope: dict) -> None:
        payload = envelope.get("payload") if isinstance(envelope.get("payload"), dict) else {}
        raw_events = payload.get("events") if isinstance(payload.get("events"), list) else [payload]
        events: list[P4SyncEventIn] = []
        for raw in raw_events[:50]:
            if not isinstance(raw, dict):
                continue
            try:
                events.append(P4SyncEventIn.model_validate(raw))
            except Exception:  # noqa: BLE001
                logger.warning("P4 MQTT event validation failed: device=%s", device.device_sn)
        if not events:
            return
        result = await apply_events(
            db,
            home_id=home_id,
            events=events,
            source="device",
            device_id=device.id,
        )
        for event in result.accepted:
            await p4_realtime_hub.publish(
                home_id,
                {
                    "type": "sync.event",
                    "homeId": str(home_id),
                    "event": event.model_dump(mode="json", by_alias=True),
                },
            )
        # 同一家庭的其它 P4 设备也通过 eventId 幂等接收本次变更。
        await self.publish_sync_events(home_id, result.accepted)
        await self.publish_command(
            home_id,
            device.device_sn,
            "sync.ack",
            {
                "requestId": envelope.get("requestId"),
                "serverRevision": result.server_revision,
                "duplicates": result.duplicates,
                "conflicts": [
                    item.model_dump(mode="json", by_alias=True)
                    for item in result.conflicts
                ],
            },
        )

    async def _handle_state(self, db, home_id: UUID, device: Device, envelope: dict) -> None:
        payload = envelope.get("payload") if isinstance(envelope.get("payload"), dict) else {}
        public_payload = _redact(payload)
        device.status = "online" if payload.get("online", True) else "offline"
        device.last_seen_at = datetime.now(timezone.utc)
        if payload.get("firmwareVersion"):
            device.firmware_version = str(payload["firmwareVersion"])[:80]
        device.extra = {**(device.extra or {}), "p4State": public_payload}
        await db.commit()
        await p4_realtime_hub.publish(
            home_id,
            {
                "type": "device.state",
                "homeId": str(home_id),
                "deviceId": device.device_sn,
                "state": public_payload,
            },
        )

    async def _handle_status_event(
        self, db, home_id: UUID, device: Device, kind: str, envelope: dict
    ) -> None:
        # ACK/error 的 messageId 与设备归属已由 topic + bindingEpoch 校验，不按裸 requestId 串线。
        db.add(
            DeviceStatusEvent(
                device_id=device.id,
                device_sn=device.device_sn,
                event_type=f"v2.{kind}",
                payload=_redact(envelope),
            )
        )
        await db.commit()
        await p4_realtime_hub.publish(
            home_id,
            {
                "type": "command.ack" if kind == "ack" else "device.error",
                "homeId": str(home_id),
                "deviceId": device.device_sn,
                "payload": _redact(envelope.get("payload", {})),
            },
        )

    async def publish_command(
        self,
        home_id: UUID,
        device_sn: str,
        command_type: str,
        payload: dict,
    ) -> None:
        from app.services.mqtt_client import mqtt_client

        topic = f"{settings.mqtt_topic_prefix}/v2/{home_id}/{device_sn}/down/command"
        body = {
            "schemaVersion": 2,
            "messageId": str(uuid4()),
            "requestId": payload.get("requestId"),
            "homeId": str(home_id),
            "deviceId": device_sn,
            "sentAt": int(datetime.now(timezone.utc).timestamp() * 1000),
            "type": command_type,
            "payload": payload,
        }
        # bindingEpoch 从数据库读取，避免调用方伪造。
        async with AsyncSessionLocal() as db:
            epoch = (
                await db.execute(
                    select(P4DeviceCredential.binding_epoch)
                    .join(Device, Device.id == P4DeviceCredential.device_id)
                    .where(Device.device_sn == device_sn)
                )
            ).scalar_one_or_none()
        body["bindingEpoch"] = int(epoch or 0)
        await mqtt_client.publish_topic(topic, body, qos=1, retain=False)

    async def publish_sync_events(
        self,
        home_id: UUID,
        accepted: list[P4SyncEventOut],
    ) -> None:
        """把已提交事件投递到家庭内设备；AI 配置从 DB 取原文，普通 API/WS 仍为脱敏值。"""
        if not accepted:
            return
        event_ids = [item.event_id for item in accepted]
        async with AsyncSessionLocal() as db:
            devices = (
                await db.execute(
                    select(Device.device_sn)
                    .join(DeviceBinding, DeviceBinding.device_id == Device.id)
                    .where(
                        DeviceBinding.home_id == home_id,
                        DeviceBinding.status == "active",
                    )
                )
            ).scalars().all()
            stored = (
                await db.execute(
                    select(P4SyncEvent)
                    .where(P4SyncEvent.home_id == home_id, P4SyncEvent.event_id.in_(event_ids))
                    .order_by(P4SyncEvent.server_revision)
                )
            ).scalars().all()
        payload_events = [
            {
                "eventId": item.event_id,
                "domain": item.domain,
                "entityId": item.entity_id,
                "operation": item.operation,
                "entityRevision": int(item.entity_revision),
                "serverRevision": int(item.server_revision),
                "data": item.data,
            }
            for item in stored
        ]
        # 每条事件独立发布，保证 envelope 始终低于设备 64 KiB 限制。
        for device_sn in devices:
            for event in payload_events:
                try:
                    await self.publish_command(
                        home_id,
                        device_sn,
                        "sync.apply",
                        {"events": [event]},
                    )
                except Exception as exc:  # noqa: BLE001
                    logger.warning(
                        "P4 MQTT sync publish deferred: device=%s event=%s error=%s",
                        device_sn,
                        event["eventId"],
                        exc,
                    )


p4_mqtt_bridge = P4MqttBridge()
