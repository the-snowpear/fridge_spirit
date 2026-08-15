"""P4 Agent v2 对外协议模型。"""

from datetime import datetime
from typing import Any, Literal
from uuid import UUID

from pydantic import Field, field_validator

from app.schemas.common import CamelModel, JsonDict


P4_DOMAINS = {
    "inventory",
    "reminders",
    "timers",
    "alarms",
    "stopwatch",
    "shopping",
    "recipes",
    "nutrition",
    "agent_sessions",
    "agent_memory",
    "device_settings",
    "ai_config",
}


class BindingSessionCreate(CamelModel):
    device_sn: str = Field(min_length=4, max_length=80)
    key_id: str | None = Field(default=None, min_length=4, max_length=80)
    bootstrap_secret: str | None = Field(default=None, min_length=16, max_length=256)
    device_token: str | None = Field(default=None, min_length=32, max_length=512)


class BindingSessionData(CamelModel):
    session_id: UUID
    status: str
    expires_at: datetime
    expires_in_seconds: int
    qr_payload: str | None = None
    poll_after_ms: int = 1500
    home_id: UUID | None = None
    binding_epoch: int | None = None
    device_token: str | None = None
    mqtt: dict[str, Any] | None = None


class BindingClaim(CamelModel):
    session_id: UUID
    claim_token: str = Field(min_length=32, max_length=512)


class BindingAck(CamelModel):
    committed: bool = True


class P4SyncEventIn(CamelModel):
    event_id: str = Field(min_length=8, max_length=128)
    domain: str = Field(min_length=1, max_length=64)
    entity_id: str = Field(min_length=1, max_length=128)
    operation: Literal["upsert", "delete", "append"]
    base_entity_revision: int = Field(default=0, ge=0)
    data: dict[str, Any] = Field(default_factory=dict)

    @field_validator("domain")
    @classmethod
    def validate_domain(cls, value: str) -> str:
        if value not in P4_DOMAINS:
            raise ValueError(f"unsupported P4 domain: {value}")
        return value


class P4SyncPush(CamelModel):
    events: list[P4SyncEventIn] = Field(min_length=1, max_length=50)


class P4SyncEventOut(CamelModel):
    event_id: str
    domain: str
    entity_id: str
    operation: str
    entity_revision: int
    server_revision: int
    source: str
    data: dict[str, Any]
    created_at: datetime | None = None


class P4SyncConflict(CamelModel):
    event_id: str
    domain: str
    entity_id: str
    expected_revision: int
    actual_revision: int
    server_entity: dict[str, Any] | None = None


class P4SyncPushData(CamelModel):
    server_revision: int
    accepted: list[P4SyncEventOut] = Field(default_factory=list)
    duplicates: list[str] = Field(default_factory=list)
    conflicts: list[P4SyncConflict] = Field(default_factory=list)


class P4SyncPullData(CamelModel):
    # server_revision 是本页最后游标；latest_server_revision 是云端最新游标。
    server_revision: int
    latest_server_revision: int
    has_more: bool
    events: list[P4SyncEventOut] = Field(default_factory=list)


class P4SyncSnapshotData(CamelModel):
    server_revision: int
    generated_at: datetime
    domains: dict[str, list[dict[str, Any]]]


class P4SeedRequest(CamelModel):
    domains: dict[str, list[dict[str, Any]]] = Field(default_factory=dict)

    @field_validator("domains")
    @classmethod
    def validate_domains(cls, value: dict[str, list[dict[str, Any]]]) -> dict[str, list[dict[str, Any]]]:
        unknown = set(value) - P4_DOMAINS
        if unknown:
            raise ValueError(f"unsupported P4 domains: {sorted(unknown)}")
        if sum(len(items) for items in value.values()) > 1000:
            raise ValueError("P4 seed contains more than 1000 entities")
        return value


class DeviceListItem(CamelModel):
    id: UUID
    device_sn: str
    name: str | None = None
    status: str
    last_seen_at: datetime | None = None


class AgentConfigUpdate(CamelModel):
    event_id: str = Field(min_length=8, max_length=128)
    base_entity_revision: int = Field(default=0, ge=0)
    data: dict[str, Any]


class P4NotificationSubscribe(CamelModel):
    """P4 小程序通知订阅；用户和家庭均由 JWT/查询参数确定。"""

    channel: str = Field(default="wechat", min_length=1, max_length=32)
    target: str = Field(min_length=1, max_length=200)
    extra: JsonDict = Field(default_factory=dict)


class P4NotificationSubscribeData(CamelModel):
    subscription_id: UUID
    status: str


class P4DeviceUnbindData(CamelModel):
    device_id: UUID
    device_sn: str
    revoked: bool
    binding_epoch: int
