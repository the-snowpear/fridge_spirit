"""P4 Agent 公网绑定与 v2 同步模型。

这些表只服务当前 ESP32-P4 Agent 数据，不读取或迁移 S3 旧同步记录。
"""

from datetime import datetime
from uuid import UUID

from sqlalchemy import BigInteger, Boolean, DateTime, ForeignKey, Integer, String, UniqueConstraint
from sqlalchemy.orm import Mapped, mapped_column

from app.db.base import Base
from app.models.common import JsonDict, TimestampMixin, UuidPkMixin, json_dict_column


class P4DeviceCredential(TimestampMixin, UuidPkMixin, Base):
    """设备预置凭据和绑定后凭据；摘要用于校验，明文 MQTT 密码仅限当前比赛开发模式。"""

    __tablename__ = "p4_device_credentials"

    device_id: Mapped[UUID] = mapped_column(ForeignKey("devices.id"), unique=True, index=True)
    bootstrap_key_id: Mapped[str] = mapped_column(String(80), unique=True, index=True)
    bootstrap_secret_hash: Mapped[str] = mapped_column(String(128))
    http_token_hash: Mapped[str | None] = mapped_column(String(128), unique=True)
    mqtt_username: Mapped[str | None] = mapped_column(String(128), unique=True)
    mqtt_password: Mapped[str | None] = mapped_column(String(256))
    binding_epoch: Mapped[int] = mapped_column(Integer, default=0)
    active: Mapped[bool] = mapped_column(Boolean, default=True)


class P4BindingSession(TimestampMixin, UuidPkMixin, Base):
    """设备屏幕二维码对应的一次性、短时绑定会话。"""

    __tablename__ = "p4_binding_sessions"

    device_id: Mapped[UUID] = mapped_column(ForeignKey("devices.id"), index=True)
    token_hash: Mapped[str] = mapped_column(String(128), unique=True, index=True)
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), index=True)
    status: Mapped[str] = mapped_column(String(32), default="pending", index=True)
    claimed_by_user_id: Mapped[UUID | None] = mapped_column(ForeignKey("users.id"), index=True)
    home_id: Mapped[UUID | None] = mapped_column(ForeignKey("homes.id"), index=True)
    claimed_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    acked_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    # 当前公网比赛开发模式下用于设备轮询领取一次性凭据；ACK 后立即清空。
    delivery: Mapped[JsonDict] = json_dict_column()


class P4SyncState(TimestampMixin, UuidPkMixin, Base):
    """家庭级 P4 v2 单调递增游标。"""

    __tablename__ = "p4_sync_states"

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), unique=True, index=True)
    current_revision: Mapped[int] = mapped_column(BigInteger, default=0)


class P4SyncEntity(TimestampMixin, UuidPkMixin, Base):
    """业务实体最新投影；entity_revision 用于显式冲突检测。"""

    __tablename__ = "p4_sync_entities"
    __table_args__ = (
        UniqueConstraint("home_id", "domain", "entity_id", name="uq_p4_sync_entity"),
    )

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    domain: Mapped[str] = mapped_column(String(64), index=True)
    entity_id: Mapped[str] = mapped_column(String(128), index=True)
    entity_revision: Mapped[int] = mapped_column(BigInteger, default=0)
    server_revision: Mapped[int] = mapped_column(BigInteger, index=True)
    deleted: Mapped[bool] = mapped_column(Boolean, default=False)
    data: Mapped[JsonDict] = json_dict_column()


class P4SyncEvent(TimestampMixin, UuidPkMixin, Base):
    """P4 v2 增量事件；同一家庭内 event_id 幂等。"""

    __tablename__ = "p4_sync_events"
    __table_args__ = (
        UniqueConstraint("home_id", "event_id", name="uq_p4_sync_event"),
    )

    home_id: Mapped[UUID] = mapped_column(ForeignKey("homes.id"), index=True)
    event_id: Mapped[str] = mapped_column(String(128), index=True)
    source: Mapped[str] = mapped_column(String(32), index=True)
    user_id: Mapped[UUID | None] = mapped_column(ForeignKey("users.id"), index=True)
    device_id: Mapped[UUID | None] = mapped_column(ForeignKey("devices.id"), index=True)
    domain: Mapped[str] = mapped_column(String(64), index=True)
    entity_id: Mapped[str] = mapped_column(String(128), index=True)
    operation: Mapped[str] = mapped_column(String(32))
    base_entity_revision: Mapped[int] = mapped_column(BigInteger, default=0)
    entity_revision: Mapped[int] = mapped_column(BigInteger)
    server_revision: Mapped[int] = mapped_column(BigInteger, index=True)
    schema_version: Mapped[int] = mapped_column(Integer, default=2)
    data: Mapped[JsonDict] = json_dict_column()
