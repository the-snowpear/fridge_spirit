"""用户级设置（隐私 + 偏好）路由。

- GET /settings：无记录则返默认值，不会自动 insert（避免给只读访问写库）。
- PUT /settings：upsert UserSettings；privacy / preferences 任一可缺省。

写入除落 UserSettings（用户级）外，还会同步镜像到 SystemConfig('settings')（家庭级），
让 /sync/snapshot、设备下发和其它微信账号都能拿到同一份设置；否则会出现"REST 保存成功
但同步快照读不到"的双存储分叉问题。

外层统一 ApiResponse[T] 包壳。
"""

from fastapi import APIRouter, Depends
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.deps import get_active_home, get_current_user
from app.db.session import get_db
from app.models.home import Home
from app.models.user import User
from app.models.user_settings import UserSettings
from app.schemas.common import ApiResponse, JsonDict
from app.schemas.settings import (
    DEFAULT_PRIVACY,
    SettingsUpdateRequest,
    UserSettingsData,
)
from app.services.sync_device_bridge import push_cloud_snapshot_to_devices
from app.services.sync_service import _upsert_config_doc, get_status, record_server_event

router = APIRouter()


def _record_to_data(record: UserSettings | None) -> UserSettingsData:
    """无记录时返回默认值；有记录时直接渲染。

    DEFAULT_PRIVACY 用浅拷贝避免不同请求共享同一个 dict 实例。
    """
    if record is None:
        return UserSettingsData(
            privacy=dict(DEFAULT_PRIVACY),
            preferences={},
            updated_at=None,
        )
    # 把数据库里可能为 None 的 JSON 字段统一规范成 dict，避免 schema 校验失败。
    privacy: JsonDict = dict(DEFAULT_PRIVACY)
    if record.privacy:
        privacy.update(record.privacy)
    preferences: JsonDict = dict(record.preferences or {})
    return UserSettingsData(
        privacy=privacy,
        preferences=preferences,
        updated_at=record.updated_at,
    )


@router.get("", response_model=ApiResponse[UserSettingsData])
async def settings_get(
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[UserSettingsData]:
    """读取当前用户的设置；无记录时返回默认值（不写库）。"""
    row = await db.execute(select(UserSettings).where(UserSettings.user_id == user.id))
    record = row.scalar_one_or_none()
    return ApiResponse[UserSettingsData](data=_record_to_data(record))


@router.put("", response_model=ApiResponse[UserSettingsData])
async def settings_update(
    payload: SettingsUpdateRequest,
    user: User = Depends(get_current_user),
    home: Home = Depends(get_active_home),
    db: AsyncSession = Depends(get_db),
) -> ApiResponse[UserSettingsData]:
    """upsert 当前用户的设置。

    payload.privacy / preferences 缺省表示"不更新"；想清空请显式传 {}。
    写完用户级 UserSettings 后，同事务把合并结果镜像到家庭级 SystemConfig('settings')，
    记录同步事件并下发设备，保证 REST 与同步通道读到同一份设置。
    """
    row = await db.execute(select(UserSettings).where(UserSettings.user_id == user.id))
    record = row.scalar_one_or_none()

    if record is None:
        # 首次写入：先用默认值兜底，再用 payload 覆盖。
        privacy: JsonDict = dict(DEFAULT_PRIVACY)
        if payload.privacy is not None:
            privacy.update(payload.privacy)
        preferences: JsonDict = dict(payload.preferences or {})
        record = UserSettings(
            user_id=user.id,
            privacy=privacy,
            preferences=preferences,
        )
        db.add(record)
    else:
        # 浅合并而非整体替换；想"清空"某个 key 可以显式传 {} 或新的完整 dict。
        if payload.privacy is not None:
            merged_privacy: JsonDict = dict(record.privacy or {})
            merged_privacy.update(payload.privacy)
            record.privacy = merged_privacy
        if payload.preferences is not None:
            merged_preferences: JsonDict = dict(record.preferences or {})
            merged_preferences.update(payload.preferences)
            record.preferences = merged_preferences

    await db.flush()
    # 镜像到家庭级 SystemConfig('settings')：同步快照、设备下发都从这里读。
    settings_doc = {
        "privacy": dict(record.privacy or {}),
        "preferences": dict(record.preferences or {}),
    }
    await _upsert_config_doc(db, home_id=home.id, key="settings", value=settings_doc, source="miniapp", merge=True)
    await record_server_event(
        db,
        home_id=home.id,
        user_id=user.id,
        domain="settings",
        op="upsert",
        payload=settings_doc,
    )
    await db.commit()
    await db.refresh(record)

    # 下发设备（离线则由设备重连补发，不阻塞响应）。
    await push_cloud_snapshot_to_devices(
        db,
        home_id=home.id,
        server_revision=(await get_status(db, home.id)).server_revision,
        domains={"settings"},
        request_device_inventory=False,
    )
    return ApiResponse[UserSettingsData](data=_record_to_data(record))
