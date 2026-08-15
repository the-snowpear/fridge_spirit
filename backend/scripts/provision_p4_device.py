"""预置一台 P4 设备的 bootstrap 凭据。

示例：
    python scripts/provision_p4_device.py --device-sn FS-P4-001

命令只在终端显示一次原始 secret；数据库仅保存 bootstrap secret 摘要。
"""

from __future__ import annotations

import argparse
import asyncio
import secrets

from sqlalchemy import select

import app.models  # noqa: F401
from app.db.session import AsyncSessionLocal, engine
from app.models.device import Device
from app.models.p4_sync import P4DeviceCredential
from app.services.p4_binding import secret_hash


async def provision(device_sn: str, name: str | None, model: str) -> None:
    key_id = f"bootstrap-{device_sn}"
    secret = secrets.token_urlsafe(32)
    async with AsyncSessionLocal() as db:
        device = (
            await db.execute(select(Device).where(Device.device_sn == device_sn))
        ).scalar_one_or_none()
        if device is None:
            device = Device(
                device_sn=device_sn,
                name=name,
                model=model,
                status="offline",
            )
            db.add(device)
            await db.flush()
        credential = (
            await db.execute(
                select(P4DeviceCredential).where(P4DeviceCredential.device_id == device.id)
            )
        ).scalar_one_or_none()
        if credential is None:
            credential = P4DeviceCredential(
                device_id=device.id,
                bootstrap_key_id=key_id,
                bootstrap_secret_hash=secret_hash(secret),
                binding_epoch=0,
                active=True,
            )
            db.add(credential)
        else:
            credential.bootstrap_key_id = key_id
            credential.bootstrap_secret_hash = secret_hash(secret)
            credential.active = True
        await db.commit()
    await engine.dispose()
    print(f"deviceSn={device_sn}")
    print(f"keyId={key_id}")
    print(f"bootstrapSecret={secret}")
    print("请立即写入设备 NVS；原始 secret 不会再次显示。")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device-sn", required=True)
    parser.add_argument("--name")
    parser.add_argument("--model", default="ESP32-P4")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    asyncio.run(provision(args.device_sn.strip(), args.name, args.model))

