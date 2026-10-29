from __future__ import annotations

import contextlib
import os
import shutil
import sqlite3
from pathlib import Path
from typing import Iterable

from .entities import ShelfGroup, ShelfItem, StoredRepresentation


SCHEMA_VERSION = 2


class ShelfStorage:
    """SQLite metadata store with file-backed storage for owned payloads."""

    def __init__(self, root: Path):
        self.root = Path(root)
        self.content_dir = self.root / "content"
        self.root.mkdir(parents=True, exist_ok=True)
        self.content_dir.mkdir(parents=True, exist_ok=True)
        os.chmod(self.root, 0o700)
        os.chmod(self.content_dir, 0o700)
        self.db_path = self.root / "items.sqlite3"
        self._db = sqlite3.connect(self.db_path)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA foreign_keys = ON")
        self._db.execute("PRAGMA journal_mode = WAL")
        self._initialize_schema()
        self._harden_permissions()

    def _harden_permissions(self) -> None:
        """Keep clipboard-derived metadata and payloads private to the user."""
        for path in (
            self.db_path,
            Path(f"{self.db_path}-wal"),
            Path(f"{self.db_path}-shm"),
        ):
            if path.exists():
                os.chmod(path, 0o600)

    def close(self) -> None:
        self._db.close()

    def _initialize_schema(self) -> None:
        current = int(self._db.execute("PRAGMA user_version").fetchone()[0])
        if current not in (0, 1, SCHEMA_VERSION):
            raise RuntimeError(
                f"PasteShelf data uses unsupported schema version {current}; "
                f"this build supports {SCHEMA_VERSION}."
            )
        if current == SCHEMA_VERSION:
            return
        if current == 1:
            with self._db:
                self._db.execute(
                    "ALTER TABLE groups ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0"
                )
                self._db.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")
            return
        with self._db:
            self._db.executescript(
                """
                CREATE TABLE groups (
                    id TEXT PRIMARY KEY,
                    created_at REAL NOT NULL,
                    position INTEGER NOT NULL,
                    kind TEXT NOT NULL,
                    title TEXT NOT NULL,
                    pinned INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE items (
                    id TEXT PRIMARY KEY,
                    group_id TEXT NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
                    position INTEGER NOT NULL,
                    kind TEXT NOT NULL,
                    title TEXT NOT NULL,
                    source_uri TEXT,
                    owned_path TEXT
                );
                CREATE TABLE representations (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    item_id TEXT NOT NULL REFERENCES items(id) ON DELETE CASCADE,
                    mime_type TEXT NOT NULL,
                    storage_kind TEXT NOT NULL CHECK(storage_kind IN ('inline', 'file', 'uri')),
                    data BLOB,
                    value TEXT
                );
                CREATE INDEX items_group_position ON items(group_id, position);
                CREATE INDEX groups_position ON groups(position);
                PRAGMA user_version = 2;
                """
            )

    def next_position(self) -> int:
        row = self._db.execute("SELECT COALESCE(MAX(position), -1) + 1 FROM groups").fetchone()
        return int(row[0])

    def add_group(self, group: ShelfGroup) -> None:
        with self._db:
            self._db.execute(
                "INSERT INTO groups(id, created_at, position, kind, title, pinned) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (
                    group.id,
                    group.created_at,
                    group.position,
                    group.kind,
                    group.title,
                    int(group.pinned),
                ),
            )
            for item in group.items:
                self._db.execute(
                    """
                    INSERT INTO items(id, group_id, position, kind, title, source_uri, owned_path)
                    VALUES (?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        item.id,
                        group.id,
                        item.position,
                        item.kind,
                        item.title,
                        item.source_uri,
                        item.owned_path,
                    ),
                )
                self._db.executemany(
                    """
                    INSERT INTO representations(item_id, mime_type, storage_kind, data, value)
                    VALUES (?, ?, ?, ?, ?)
                    """,
                    [
                        (
                            item.id,
                            representation.mime_type,
                            representation.storage_kind,
                            representation.data,
                            representation.value,
                        )
                        for representation in item.representations
                    ],
                )

    def load_groups(self) -> list[ShelfGroup]:
        groups: list[ShelfGroup] = []
        group_rows = self._db.execute(
            "SELECT id, created_at, position, kind, title, pinned "
            "FROM groups ORDER BY position, created_at"
        ).fetchall()
        for group_row in group_rows:
            items: list[ShelfItem] = []
            item_rows = self._db.execute(
                """
                SELECT id, position, kind, title, source_uri, owned_path
                FROM items WHERE group_id = ? ORDER BY position
                """,
                (group_row["id"],),
            ).fetchall()
            for item_row in item_rows:
                representation_rows = self._db.execute(
                    """
                    SELECT mime_type, storage_kind, data, value
                    FROM representations WHERE item_id = ? ORDER BY id
                    """,
                    (item_row["id"],),
                ).fetchall()
                representations = [
                    StoredRepresentation(
                        mime_type=row["mime_type"],
                        storage_kind=row["storage_kind"],
                        data=bytes(row["data"]) if row["data"] is not None else None,
                        value=row["value"],
                    )
                    for row in representation_rows
                ]
                items.append(
                    ShelfItem(
                        id=item_row["id"],
                        position=item_row["position"],
                        kind=item_row["kind"],
                        title=item_row["title"],
                        source_uri=item_row["source_uri"],
                        owned_path=item_row["owned_path"],
                        representations=representations,
                    )
                )
            groups.append(
                ShelfGroup(
                    id=group_row["id"],
                    created_at=group_row["created_at"],
                    position=group_row["position"],
                    kind=group_row["kind"],
                    title=group_row["title"],
                    pinned=bool(group_row["pinned"]),
                    items=items,
                )
            )
        return groups

    def get_group(self, group_id: str) -> ShelfGroup | None:
        return next((group for group in self.load_groups() if group.id == group_id), None)

    def delete_group(self, group_id: str) -> None:
        group = self.get_group(group_id)
        if group is None:
            return
        owned_paths = [Path(item.owned_path) for item in group.items if item.owned_path]
        with self._db:
            self._db.execute("DELETE FROM groups WHERE id = ?", (group_id,))
        self._remove_owned_paths(owned_paths)

    def set_group_pinned(self, group_id: str, pinned: bool) -> None:
        with self._db:
            self._db.execute(
                "UPDATE groups SET pinned = ? WHERE id = ?",
                (int(pinned), group_id),
            )

    def delete_item(
        self,
        group_id: str,
        item_id: str,
        *,
        group_kind: str,
        group_title: str,
        remaining_item_ids: list[str],
    ) -> None:
        row = self._db.execute(
            "SELECT owned_path FROM items WHERE id = ? AND group_id = ?",
            (item_id, group_id),
        ).fetchone()
        if row is None:
            return
        owned_paths = [Path(row[0])] if row[0] else []
        with self._db:
            self._db.execute(
                "DELETE FROM items WHERE id = ? AND group_id = ?",
                (item_id, group_id),
            )
            self._db.execute(
                "UPDATE groups SET kind = ?, title = ? WHERE id = ?",
                (group_kind, group_title, group_id),
            )
            self._db.executemany(
                "UPDATE items SET position = ? WHERE id = ? AND group_id = ?",
                [
                    (position, remaining_id, group_id)
                    for position, remaining_id in enumerate(remaining_item_ids)
                ],
            )
        self._remove_owned_paths(owned_paths)

    def clear(self) -> None:
        owned_paths = [
            Path(row[0])
            for row in self._db.execute(
                "SELECT owned_path FROM items WHERE owned_path IS NOT NULL"
            ).fetchall()
        ]
        with self._db:
            self._db.execute("DELETE FROM groups")
        self._remove_owned_paths(owned_paths)

    def _remove_owned_paths(self, paths: Iterable[Path]) -> None:
        content_root = self.content_dir.resolve()
        for path in paths:
            with contextlib.suppress(FileNotFoundError, OSError):
                resolved = path.resolve()
                if resolved != content_root and content_root in resolved.parents:
                    if resolved.is_dir():
                        shutil.rmtree(resolved)
                    else:
                        resolved.unlink()

    def allocate_owned_path(self, item_id: str, source_name: str) -> Path:
        safe_name = Path(source_name).name or "payload"
        return self.content_dir / f"{item_id}-{safe_name}"

    def cleanup_orphans(self) -> None:
        referenced = {
            Path(row[0]).resolve()
            for row in self._db.execute(
                "SELECT owned_path FROM items WHERE owned_path IS NOT NULL"
            ).fetchall()
        }
        for path in self.content_dir.iterdir():
            with contextlib.suppress(OSError):
                if path.resolve() not in referenced:
                    if path.is_dir():
                        shutil.rmtree(path)
                    else:
                        os.unlink(path)
