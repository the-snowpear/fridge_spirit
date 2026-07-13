"""收敛比赛演示设备绑定。

用途：
    python backend/scripts/repair_demo_binding.py

效果：
    - 确保 demo_openid_seed_001、演示家庭、DEMO-FRIDGE-001 存在；
    - 同一台 DEMO 设备只保留演示家庭一个 active 绑定；
    - 把历史多账号/多家庭联调留下的其它 active 绑定置为 inactive。
"""

from __future__ import annotations

import asyncio
import sys
from pathlib import Path

from sqlalchemy import select, update

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app.db.session import AsyncSessionLocal  # noqa: E402
from app.models.device import Device, DeviceBinding  # noqa: E402
from app.models.home import Home, HomeMember  # noqa: E402
from app.models.user import User  # noqa: E402

DEMO_OPENID = "demo_openid_seed_001"
DEMO_DEVICE_SN = "DEMO-FRIDGE-001"


async def main() -> None:
    async with AsyncSessionLocal() as db:
        user = (
            await db.execute(select(User).where(User.primary_openid == DEMO_OPENID))
        ).scalar_one_or_none()
        if user is None:
            user = User(primary_openid=DEMO_OPENID, display_name="演示用户")
            db.add(user)
            await db.flush()

        home = (
            await db.execute(select(Home).where(Home.owner_user_id == user.id).limit(1))
        ).scalar_one_or_none()
        if home is None:
            home = Home(name="演示厨房", owner_user_id=user.id)
            db.add(home)
            await db.flush()
            db.add(HomeMember(home_id=home.id, user_id=user.id, role="owner"))

        device = (
            await db.execute(select(Device).where(Device.device_sn == DEMO_DEVICE_SN))
        ).scalar_one_or_none()
        if device is None:
            device = Device(device_sn=DEMO_DEVICE_SN, name="演示冰箱", model="DEMO", status="offline")
            db.add(device)
            await db.flush()

        await db.execute(
            update(DeviceBinding)
            .where(
                DeviceBinding.device_id == device.id,
                DeviceBinding.home_id != home.id,
                DeviceBinding.status == "active",
            )
            .values(status="inactive")
        )

        binding = (
            await db.execute(
                select(DeviceBinding).where(
                    DeviceBinding.device_id == device.id,
                    DeviceBinding.home_id == home.id,
                )
            )
        ).scalar_one_or_none()
        if binding is None:
            db.add(
                DeviceBinding(
                    device_id=device.id,
                    home_id=home.id,
                    bound_by_user_id=user.id,
                    status="active",
                )
            )
        else:
            binding.status = "active"
            binding.unbound_at = None
            binding.bound_by_user_id = user.id

        await db.commit()
        print(f"demo binding repaired: home={home.id} device={device.device_sn}")


if __name__ == "__main__":
    asyncio.run(main())
