"""不依赖 PostgreSQL/EMQX 的 P4 v2 协议单元测试。"""

from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from uuid import uuid4

from app.api.v2.routes import _session_data
from app.schemas.v2 import (
    BindingSessionCreate,
    P4DeviceUnbindData,
    P4NotificationSubscribe,
    P4SyncEventIn,
)
from app.schemas.inventory import InventoryUpdateRequest
from app.services.p4_binding import (
    credential_needs_rotation,
    secret_hash,
    secret_matches,
)
from app.services.p4_facade import (
    _recommend_three_level_location,
    _history_messages,
    normalize_inventory_location,
    normalize_agent_session_id,
)
from app.services.p4_mqtt import p4_mqtt_bridge
from app.services.sync_device_bridge import _device_layers
from app.services.p4_sync import _redact


def test_binding_request_uses_firmware_camel_case_contract() -> None:
    value = BindingSessionCreate.model_validate(
        {
            "deviceSn": "FS-P4-001",
            "keyId": "bootstrap-FS-P4-001",
            "bootstrapSecret": "a" * 32,
        }
    )
    assert value.device_sn == "FS-P4-001"
    assert value.key_id == "bootstrap-FS-P4-001"


def test_session_response_contains_monotonic_countdown_fields() -> None:
    session = SimpleNamespace(
        id=uuid4(),
        status="pending",
        expires_at=datetime.now(timezone.utc) + timedelta(seconds=300),
        home_id=None,
        delivery={},
    )
    data = _session_data(session, token="x" * 43)
    body = data.model_dump(mode="json", by_alias=True)
    assert body["qrPayload"].startswith("fridgespirit://bind?v=2")
    assert 295 <= body["expiresInSeconds"] <= 300
    assert body["pollAfterMs"] == 1500


def test_p4_domain_and_operation_validation() -> None:
    event = P4SyncEventIn.model_validate(
        {
            "eventId": "evt-12345678",
            "domain": "timers",
            "entityId": "timer-1",
            "operation": "upsert",
            "baseEntityRevision": 0,
            "data": {"durationMs": 60_000},
        }
    )
    assert event.domain == "timers"


def test_secrets_are_hashed_and_get_payload_is_redacted() -> None:
    digest = secret_hash("example-device-secret")
    assert secret_matches("example-device-secret", digest)
    assert not secret_matches("wrong", digest)
    assert _redact({"apiKey": "sk-raw", "model": "demo"}) == {
        "apiKey": "***",
        "model": "demo",
    }


def test_mqtt_v2_subscription_is_isolated_from_legacy_topics() -> None:
    topics = [topic for topic, _ in p4_mqtt_bridge.topics()]
    assert topics == ["fridge/v2/+/+/up/+"]


def test_partial_credential_is_rotated_before_delivery() -> None:
    complete = SimpleNamespace(
        http_token_hash="digest",
        mqtt_username="p4-FS-P4-001",
        mqtt_password="secret",
    )
    partial = SimpleNamespace(
        http_token_hash="digest",
        mqtt_username=None,
        mqtt_password=None,
    )
    fresh = SimpleNamespace(
        http_token_hash=None,
        mqtt_username=None,
        mqtt_password=None,
    )
    assert not credential_needs_rotation(complete, has_active_binding=True)
    assert credential_needs_rotation(partial, has_active_binding=True)
    assert credential_needs_rotation(fresh, has_active_binding=False)


def test_v2_notification_contract_does_not_accept_identity_fields() -> None:
    payload = P4NotificationSubscribe.model_validate(
        {
            "channel": "wechat",
            "target": "template-1",
            "extra": {"scene": "expiry"},
        }
    )
    assert payload.model_dump(mode="json", by_alias=True) == {
        "channel": "wechat",
        "target": "template-1",
        "extra": {"scene": "expiry"},
    }


def test_agent_session_history_uses_v2_camel_case_payload() -> None:
    message_id = uuid4()
    sent_at = datetime.now(timezone.utc)
    messages = _history_messages(
        {
            "messages": [
                {
                    "id": str(message_id),
                    "role": "assistant",
                    "content": "库存里有牛奶",
                    "source": "cloud_fallback",
                    "fallbackReason": "p4_backend",
                    "modelUsed": "demo-model",
                    "sentAt": sent_at.isoformat(),
                }
            ]
        }
    )
    assert messages[0].id == message_id
    assert messages[0].fallback_reason == "p4_backend"
    assert normalize_agent_session_id("  kitchen  ") == "kitchen"


def test_unbind_response_is_camel_case() -> None:
    body = P4DeviceUnbindData(
        device_id=uuid4(),
        device_sn="FS-P4-001",
        revoked=True,
        binding_epoch=3,
    ).model_dump(mode="json", by_alias=True)
    assert set(body) == {"deviceId", "deviceSn", "revoked", "bindingEpoch"}


def test_v2_command_facade_routes_are_registered() -> None:
    from app.api.v2.routes import router

    method_paths = {
        (method, route.path)
        for route in router.routes
        for method in (getattr(route, "methods", None) or set())
    }
    assert {
        ("POST", "/inventory/scan"),
        ("POST", "/inventory/refresh"),
        ("POST", "/ai/chat"),
        ("GET", "/ai/history"),
        ("DELETE", "/ai/history"),
        ("POST", "/notification/subscribe"),
        ("POST", "/devices/{device_id}/unbind"),
    }.issubset(method_paths)


def test_inventory_location_normalizes_legacy_and_p4_fields() -> None:
    assert normalize_inventory_location(
        {"zone": "left", "slot": "B2"}
    ) == ("left", "l1", 4)
    assert normalize_inventory_location(
        {"zone_id": "left", "layer_id": "l2", "cell": 0}
    ) == ("left", "l2", 0)


def test_inventory_recommendation_keeps_layers_independent() -> None:
    items = [
        {"name": "第一层食材", "zoneId": "left", "layerId": "l1", "cell": 0},
        {"name": "第二层食材", "zoneId": "left", "layerId": "l2", "cell": 0},
    ]
    zone_id, layer_id, cell, _ = _recommend_three_level_location(
        name="新熟食",
        category="熟食",
        items=items,
    )
    assert (zone_id, layer_id, cell) == ("left", "l1", 1)


def test_inventory_write_contract_keeps_three_level_location() -> None:
    payload = InventoryUpdateRequest.model_validate(
        {
            "name": "牛奶",
            "zoneId": "right",
            "layerId": "r2",
            "cell": 5,
            "zone": "right",
            "slot": "B3",
        }
    )
    body = payload.model_dump(mode="json", by_alias=True)
    assert body["zoneId"] == "right"
    assert body["layerId"] == "r2"
    assert body["cell"] == 5


def test_device_snapshot_contains_p4_default_layers() -> None:
    zones = [
        SimpleNamespace(key="left"),
        SimpleNamespace(key="drawer"),
    ]
    assert _device_layers(zones) == [
        {"id": "l1", "zone_id": "left", "name": "第1层", "order": 0},
        {"id": "l2", "zone_id": "left", "name": "第2层", "order": 1},
        {"id": "l3", "zone_id": "left", "name": "第3层", "order": 2},
        {"id": "v1", "zone_id": "drawer", "name": "抽屉", "order": 0},
    ]
