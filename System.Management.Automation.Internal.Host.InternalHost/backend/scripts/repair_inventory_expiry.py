"""修复设备同步时被误清空的库存过期日期。

用途：
    python backend/scripts/repair_inventory_expiry.py

效果：
    - 从 extra.deviceDaysLeft / extra.deviceExpireText 恢复 expire_date；
    - 不覆盖已有 expire_date；
    - 可重复执行，只处理 status != deleted 的库存行。
"""

from __future__ import annotations

import asyncio
import re
import sys
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Any

from sqlalchemy import select

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app.db.session import AsyncSessionLocal  # noqa: E402
from app.models.inventory import InventoryItem  # noqa: E402


def _today() -> date:
    return datetime.now(timezone.utc).date()


def _parse_expire(extra: dict[str, Any]) -> date | None:
    days_raw = extra.get("deviceDaysLeft")
    try:
        return _today() + timedelta(days=max(int(days_raw), 0))
    except (TypeError, ValueError):
        pass

    text = str(extra.get("deviceExpireText") or "").strip()
    if not text:
        return None
    try:
        return date.fromisoformat(text[:10])
    except ValueError:
        pass
    if "今天" in text:
        return _today()
    if "明天" in text:
        return _today() + timedelta(days=1)
    match = re.search(r"(\d+)\s*天后", text)
    if match:
        return _today() + timedelta(days=int(match.group(1)))
    return None


async def main() -> None:
    scanned = 0
    repaired = 0
    async with AsyncSessionLocal() as db:
        result = await db.execute(select(InventoryItem).where(InventoryItem.status != "deleted"))
        for item in result.scalars().all():
            scanned += 1
            if item.expire_date is not None:
                continue
            extra = item.extra if isinstance(item.extra, dict) else {}
            restored = _parse_expire(extra)
            if restored is None:
                continue
            item.expire_date = restored
            repaired += 1
        await db.commit()
    print(f"inventory expiry repair complete: scanned={scanned} repaired={repaired}")


if __name__ == "__main__":
    asyncio.run(main())
