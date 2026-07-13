"""FastAPI dependency helpers: authentication and active home resolution."""

from uuid import UUID

from fastapi import Depends, Header, HTTPException, status
from sqlalchemy import select, update
from sqlalchemy.ext.asyncio import AsyncSession

from app.db.session import get_db
from app.models.device import Device, DeviceBinding
from app.models.home import Home, HomeMember
from app.models.user import User
from app.services.token import JWTError, decode_access_token


# 鉴权统一在依赖层完成，路由代码不再关心 Authorization 头解析。
# 失败时直接抛 401/404，与 ApiResponse 壳层无关（FastAPI 会自动序列化 HTTPException）。
DEMO_DEVICE_SN = "DEMO-FRIDGE-001"
DEMO_HOME_NAME = "演示厨房"


async def get_current_user(
    authorization: str | None = Header(default=None, alias="Authorization"),
    db: AsyncSession = Depends(get_db),
) -> User:
    """从 Authorization: Bearer <token> 解析 user。"""
    if not authorization or not authorization.lower().startswith("bearer "):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="missing bearer token")
    token = authorization.split(" ", 1)[1].strip()
    try:
        payload = decode_access_token(token)
    except JWTError as exc:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail=f"invalid token: {exc}") from exc

    sub = payload.get("sub")
    if not sub:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="token missing sub")

    try:
        user_id = UUID(sub)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid user id in token") from exc

    user = await db.get(User, user_id)
    if user is None:
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="user not found")
    return user


async def get_active_home(
    user: User = Depends(get_current_user),
    db: AsyncSession = Depends(get_db),
) -> Home:
    """返回当前用户的"活跃家庭"。

    比赛演示版固定为“所有微信账号共用同一个 DEMO-FRIDGE-001 家庭”。
    这样固件 MQTT 里保存的 home_id、小程序当前 token 和后端库存表不会分叉；
    等后续做正式多用户时，再恢复按用户选择活跃家庭。
    """
    demo_home = await _get_or_create_demo_home(db, user)
    if demo_home is not None:
        return demo_home

    result = await db.execute(
        select(Home)
        .join(HomeMember, HomeMember.home_id == Home.id)
        .where(HomeMember.user_id == user.id, HomeMember.role == "owner")
        .order_by(Home.created_at.asc())
        .limit(1)
    )
    home = result.scalar_one_or_none()
    if home is not None:
        return home

    # 自动创建默认家庭，避免比赛演示阶段卡在"先创建家庭"这一步。
    home = Home(name="我的冰箱", owner_user_id=user.id)
    db.add(home)
    await db.flush()
    db.add(HomeMember(home_id=home.id, user_id=user.id, role="owner"))
    await db.commit()
    await db.refresh(home)
    return home


async def _get_or_create_demo_home(db: AsyncSession, user: User) -> Home | None:
    """比赛演示家庭收敛：所有账号都进入 DEMO-FRIDGE-001 的 active 家庭。"""
    changed = False
    device_row = await db.execute(select(Device).where(Device.device_sn == DEMO_DEVICE_SN))
    device = device_row.scalar_one_or_none()
    if device is None:
        device = Device(
            device_sn=DEMO_DEVICE_SN,
            name="演示冰箱",
            model="DEMO",
            status="offline",
        )
        db.add(device)
        await db.flush()
        changed = True

    binding_row = await db.execute(
        select(DeviceBinding)
        .where(DeviceBinding.device_id == device.id, DeviceBinding.status == "active")
        .order_by(DeviceBinding.created_at.asc())
        .limit(1)
    )
    binding = binding_row.scalar_one_or_none()
    if binding is not None:
        # 历史联调可能已经留下多个 active 绑定；比赛版统一收敛到第一条。
        await db.execute(
            update(DeviceBinding)
            .where(
                DeviceBinding.device_id == device.id,
                DeviceBinding.id != binding.id,
                DeviceBinding.status == "active",
            )
            .values(status="inactive")
        )
        changed = True
        home = await db.get(Home, binding.home_id)
        if home is None:
            return None
    else:
        # 设备还没有 active 绑定时，优先复用当前用户已有家庭；否则创建演示家庭。
        home_row = await db.execute(
            select(Home)
            .join(HomeMember, HomeMember.home_id == Home.id)
            .where(HomeMember.user_id == user.id, HomeMember.role == "owner")
            .order_by(Home.created_at.asc())
            .limit(1)
        )
        home = home_row.scalar_one_or_none()
        if home is None:
            home = Home(name=DEMO_HOME_NAME, owner_user_id=user.id)
            db.add(home)
            await db.flush()
            changed = True
        existing_binding = (
            await db.execute(
                select(DeviceBinding).where(
                    DeviceBinding.device_id == device.id,
                    DeviceBinding.home_id == home.id,
                )
            )
        ).scalar_one_or_none()
        if existing_binding is None:
            db.add(
                DeviceBinding(
                    device_id=device.id,
                    home_id=home.id,
                    bound_by_user_id=user.id,
                    status="active",
                )
            )
        else:
            existing_binding.status = "active"
            existing_binding.unbound_at = None
        await db.flush()
        changed = True

    member_row = await db.execute(
        select(HomeMember)
        .where(HomeMember.home_id == home.id, HomeMember.user_id == user.id)
        .limit(1)
    )
    if member_row.scalar_one_or_none() is None:
        db.add(HomeMember(home_id=home.id, user_id=user.id, role="owner"))
        changed = True
    if changed:
        await db.commit()
        await db.refresh(home)
    return home


__all__ = ["get_current_user", "get_active_home"]
