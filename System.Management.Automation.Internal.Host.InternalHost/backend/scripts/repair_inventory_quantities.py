"""修复早期设备同步造成的库存数量/单位污染。

用途：
    python backend/scripts/repair_inventory_quantities.py

效果：
    - 清理 InventoryItem.unit 中的 "1200g"、"200gg"、"11111200g" 等污染；
    - 同步修复 extra.deviceQuantityText，避免后续下发设备时继续携带旧值；
    - 可重复执行，只修改 status != deleted 的库存行。
"""

from __future__ import annotations

import asyncio
import re
import sys
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any

from sqlalchemy import select

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from app.db.session import AsyncSessionLocal  # noqa: E402
from app.models.inventory import InventoryItem  # noqa: E402


KNOWN_UNITS = ["毫升", "克", "个", "份", "盒", "瓶", "袋", "根", "把", "颗", "杯", "块"]


def _collapse_repeated_unit(unit: str) -> str:
    if unit and set(unit) == {"g"}:
        return "g"
    for known in KNOWN_UNITS:
        if len(unit) > len(known) and unit == known * (len(unit) // len(known)):
            return known
    return unit


def _clean_quantity_text(value: Any) -> str:
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


def _split_quantity_text(text: str) -> tuple[Decimal, str]:
    match = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*([^\d\s].*)?\s*$", text)
    if not match:
        return Decimal("1"), text[:24] or "份"
    try:
        quantity = Decimal(match.group(1))
    except InvalidOperation:
        quantity = Decimal("1")
    unit = (match.group(2) or "份").strip()[:24]
    return quantity, unit or "份"


def _is_textual_quantity(value: str) -> bool:
    return bool(re.match(r"^\s*[0-9]+(?:\.[0-9]+)?\s*[^\d\s]+", value or ""))


async def main() -> None:
    repaired = 0
    scanned = 0
    async with AsyncSessionLocal() as db:
        result = await db.execute(select(InventoryItem).where(InventoryItem.status != "deleted"))
        for item in result.scalars().all():
            scanned += 1
            extra = dict(item.extra or {})
            candidates = [
                extra.get("deviceQuantityText"),
                f"{item.quantity:g}{item.unit}" if item.quantity is not None and item.unit else None,
                item.unit if _is_textual_quantity(str(item.unit or "")) else None,
            ]
            cleaned = ""
            for candidate in candidates:
                cleaned = _clean_quantity_text(candidate)
                if cleaned:
                    break
            if not cleaned:
                continue

            quantity, unit = _split_quantity_text(cleaned)
            changed = False
            if item.quantity != quantity:
                item.quantity = quantity
                changed = True
            if item.unit != unit:
                item.unit = unit
                changed = True
            if extra.get("deviceQuantityText") != cleaned:
                extra["deviceQuantityText"] = cleaned
                item.extra = extra
                changed = True
            if changed:
                repaired += 1

        await db.commit()
    print(f"inventory quantity repair complete: scanned={scanned} repaired={repaired}")


if __name__ == "__main__":
    asyncio.run(main())
