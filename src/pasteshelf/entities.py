from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal


StorageKind = Literal["inline", "file", "uri"]


@dataclass(slots=True)
class StoredRepresentation:
    mime_type: str
    storage_kind: StorageKind
    data: bytes | None = None
    value: str | None = None


@dataclass(slots=True)
class ShelfItem:
    id: str
    position: int
    kind: str
    title: str
    source_uri: str | None = None
    owned_path: str | None = None
    representations: list[StoredRepresentation] = field(default_factory=list)

    @property
    def local_path(self) -> Path | None:
        if self.owned_path:
            return Path(self.owned_path)
        if self.source_uri and self.source_uri.startswith("file://"):
            from PySide6.QtCore import QUrl

            path = QUrl(self.source_uri).toLocalFile()
            return Path(path) if path else None
        return None

    @property
    def missing(self) -> bool:
        path = self.local_path
        return path is not None and not path.exists()


@dataclass(slots=True)
class ShelfGroup:
    id: str
    created_at: float
    position: int
    kind: str
    title: str
    pinned: bool = False
    items: list[ShelfItem] = field(default_factory=list)

    @property
    def missing(self) -> bool:
        return bool(self.items) and all(item.missing for item in self.items)

    @property
    def subtitle(self) -> str:
        if len(self.items) > 1:
            missing = sum(item.missing for item in self.items)
            base = f"{len(self.items)} items"
            return f"{base} · {missing} unavailable" if missing else base
        if not self.items:
            return "Empty"
        item = self.items[0]
        if item.missing:
            return "Original is unavailable"
        labels = {
            "file": "File",
            "folder": "Folder",
            "image": "Image",
            "text": "Text snippet",
            "html": "Rich text",
            "link": "Link",
        }
        return labels.get(item.kind, item.kind.capitalize())
