"""幂等创建 P4 Agent v2 表并登记 schema 版本。

运行：
    python scripts/migrate_p4_v2.py

当前迁移只新增表，不修改或导入任何 S3/旧 v1 数据。
"""

from __future__ import annotations

import asyncio

from sqlalchemy import text

import app.models  # noqa: F401 -- 注册全部 ORM 模型
from app.db.base import Base
from app.db.session import engine


async def main() -> None:
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
        await conn.execute(
            text(
                """
                CREATE TABLE IF NOT EXISTS backend_schema_migrations (
                    version VARCHAR(80) PRIMARY KEY,
                    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
                )
                """
            )
        )
        await conn.execute(
            text(
                """
                INSERT INTO backend_schema_migrations(version)
                VALUES ('20260726_p4_agent_v2')
                ON CONFLICT (version) DO NOTHING
                """
            )
        )
    await engine.dispose()
    print("P4 Agent v2 migration applied: 20260726_p4_agent_v2")


if __name__ == "__main__":
    asyncio.run(main())

