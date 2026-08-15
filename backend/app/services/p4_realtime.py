"""家庭级 WebSocket 广播中心。

这里只保存进程内连接；断线期间由客户端使用 serverRevision 通过 REST 补齐。
"""

from __future__ import annotations

import asyncio
from collections import defaultdict
from uuid import UUID

from fastapi import WebSocket


class P4RealtimeHub:
    def __init__(self) -> None:
        self._connections: dict[UUID, set[WebSocket]] = defaultdict(set)
        self._lock = asyncio.Lock()

    async def add(self, home_ids: set[UUID], websocket: WebSocket) -> None:
        async with self._lock:
            for home_id in home_ids:
                self._connections[home_id].add(websocket)

    async def remove(self, websocket: WebSocket) -> None:
        async with self._lock:
            for home_id in list(self._connections):
                self._connections[home_id].discard(websocket)
                if not self._connections[home_id]:
                    self._connections.pop(home_id, None)

    async def publish(self, home_id: UUID, event: dict) -> None:
        async with self._lock:
            targets = list(self._connections.get(home_id, ()))
        stale: list[WebSocket] = []
        for websocket in targets:
            try:
                await websocket.send_json(event)
            except Exception:  # noqa: BLE001 - 连接清理由 finally/下一轮完成
                stale.append(websocket)
        for websocket in stale:
            await self.remove(websocket)

    def connection_count(self) -> int:
        return sum(len(items) for items in self._connections.values())


p4_realtime_hub = P4RealtimeHub()

