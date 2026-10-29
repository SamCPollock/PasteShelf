from __future__ import annotations

import html as html_module
import os
import re
import shutil
import tempfile
import time
import uuid
from pathlib import Path
from urllib.parse import unquote

from PySide6.QtCore import QByteArray, QBuffer, QIODevice, QMimeData, QUrl
from PySide6.QtGui import QImage, QPixmap, QTextDocument

from .entities import ShelfGroup, ShelfItem, StoredRepresentation
from .storage import ShelfStorage


INTERNAL_MIME_TYPE = "application/x-pasteshelf-item"


class UnsupportedMimeError(ValueError):
    pass


class PayloadTooLargeError(ValueError):
    pass


class MimeService:
    """Convert live Qt MIME offers to persistent shelf entries and back."""

    def __init__(
        self,
        storage: ShelfStorage,
        *,
        ephemeral_roots: list[Path] | None = None,
        max_owned_bytes: int = 100 * 1024 * 1024,
    ):
        self.storage = storage
        self.max_owned_bytes = max_owned_bytes
        if ephemeral_roots is None:
            roots = [Path(tempfile.gettempdir())]
            runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
            if runtime_dir:
                roots.append(Path(runtime_dir))
            self.ephemeral_roots = roots
        else:
            self.ephemeral_roots = [Path(root) for root in ephemeral_roots]

    @staticmethod
    def can_import(mime_data: QMimeData) -> bool:
        if mime_data.hasFormat(INTERNAL_MIME_TYPE):
            return False
        return bool(
            mime_data.hasUrls()
            or mime_data.hasImage()
            or mime_data.hasHtml()
            or mime_data.hasText()
            or any(fmt.startswith("image/") for fmt in mime_data.formats())
        )

    def import_mime(self, mime_data: QMimeData) -> ShelfGroup:
        if not self.can_import(mime_data):
            raise UnsupportedMimeError("This drag does not contain a supported item.")

        # Prefer real image content to a browser or screenshot tool's incidental URL.
        if mime_data.hasImage() or any(
            fmt.startswith("image/") for fmt in mime_data.formats()
        ):
            group = self._import_image(mime_data)
        elif mime_data.hasUrls():
            group = self._import_urls(mime_data)
        elif mime_data.hasHtml() or mime_data.hasText():
            group = self._import_text(mime_data)
        else:
            raise UnsupportedMimeError("This drag does not contain a supported item.")

        try:
            self.storage.add_group(group)
        except Exception:
            for item in group.items:
                if item.owned_path:
                    self.storage._remove_owned_paths([Path(item.owned_path)])
            raise
        return group

    def _new_group(self, *, kind: str, title: str, items: list[ShelfItem]) -> ShelfGroup:
        return ShelfGroup(
            id=str(uuid.uuid4()),
            created_at=time.time(),
            position=self.storage.next_position(),
            kind=kind,
            title=title,
            items=items,
        )

    def _import_urls(self, mime_data: QMimeData) -> ShelfGroup:
        items: list[ShelfItem] = []
        for position, url in enumerate(mime_data.urls()):
            item_id = str(uuid.uuid4())
            if url.isLocalFile():
                original = Path(url.toLocalFile())
                title = original.name or str(original)
                kind = "folder" if original.is_dir() else "file"
                source_url = url
                owned_path: str | None = None
                if original.is_file() and self._is_ephemeral(original):
                    size = original.stat().st_size
                    self._check_size(size)
                    destination = self.storage.allocate_owned_path(item_id, title)
                    partial = destination.with_suffix(destination.suffix + ".part")
                    shutil.copy2(original, partial)
                    os.replace(partial, destination)
                    source_url = QUrl.fromLocalFile(str(destination))
                    owned_path = str(destination)
                item = ShelfItem(
                    id=item_id,
                    position=position,
                    kind=kind,
                    title=title,
                    source_uri=source_url.toString(),
                    owned_path=owned_path,
                    representations=[
                        StoredRepresentation(
                            mime_type="text/uri-list",
                            storage_kind="uri",
                            value=source_url.toString(),
                        )
                    ],
                )
            else:
                display = unquote(url.fileName()) or url.host() or url.toDisplayString()
                item = ShelfItem(
                    id=item_id,
                    position=position,
                    kind="link",
                    title=display,
                    source_uri=url.toString(),
                    representations=[
                        StoredRepresentation(
                            mime_type="text/uri-list",
                            storage_kind="uri",
                            value=url.toString(),
                        )
                    ],
                )
            items.append(item)

        if not items:
            raise UnsupportedMimeError("The drag offered an empty URL list.")

        # URL drags often include useful rich/plain labels. Keep them on a single link.
        if len(items) == 1:
            self._append_text_representations(items[0], mime_data)

        if len(items) > 1:
            return self._new_group(kind="stack", title=f"{len(items)} items", items=items)
        return self._new_group(kind=items[0].kind, title=items[0].title, items=items)

    def _import_text(self, mime_data: QMimeData) -> ShelfGroup:
        item_id = str(uuid.uuid4())
        plain = mime_data.text() if mime_data.hasText() else ""
        rich = mime_data.html() if mime_data.hasHtml() else ""
        if not plain and rich:
            document = QTextDocument()
            document.setHtml(rich)
            plain = document.toPlainText()
        title = self._text_title(plain) or "Rich text"
        item = ShelfItem(
            id=item_id,
            position=0,
            kind="html" if rich else "text",
            title=title,
        )
        self._append_text_representations(item, mime_data, plain_override=plain)
        return self._new_group(kind=item.kind, title=title, items=[item])

    def _import_image(self, mime_data: QMimeData) -> ShelfGroup:
        item_id = str(uuid.uuid4())
        image = self._extract_image(mime_data)
        if image.isNull():
            raise UnsupportedMimeError("The offered image could not be decoded.")

        payload = QByteArray()
        buffer = QBuffer(payload)
        buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        if not image.save(buffer, "PNG"):
            raise UnsupportedMimeError("The offered image could not be encoded.")
        png_bytes = bytes(payload)
        self._check_size(len(png_bytes))

        destination = self.storage.allocate_owned_path(item_id, "image.png")
        partial = destination.with_suffix(".png.part")
        partial.write_bytes(png_bytes)
        os.replace(partial, destination)
        file_url = QUrl.fromLocalFile(str(destination)).toString()
        item = ShelfItem(
            id=item_id,
            position=0,
            kind="image",
            title=f"Image {image.width()}×{image.height()}",
            source_uri=file_url,
            owned_path=str(destination),
            representations=[
                StoredRepresentation(
                    mime_type="image/png", storage_kind="file", value=str(destination)
                ),
                StoredRepresentation(
                    mime_type="text/uri-list", storage_kind="uri", value=file_url
                ),
            ],
        )
        self._append_text_representations(item, mime_data)
        return self._new_group(kind="image", title=item.title, items=[item])

    @staticmethod
    def _extract_image(mime_data: QMimeData) -> QImage:
        value = mime_data.imageData() if mime_data.hasImage() else None
        if isinstance(value, QImage):
            return value
        if isinstance(value, QPixmap):
            return value.toImage()
        for mime_type in mime_data.formats():
            if mime_type.startswith("image/"):
                image = QImage.fromData(mime_data.data(mime_type))
                if not image.isNull():
                    return image
        return QImage()

    def _append_text_representations(
        self,
        item: ShelfItem,
        mime_data: QMimeData,
        *,
        plain_override: str | None = None,
    ) -> None:
        if mime_data.hasHtml():
            rich = mime_data.html()
            self._check_size(len(rich.encode("utf-8")))
            item.representations.append(
                StoredRepresentation(
                    mime_type="text/html", storage_kind="inline", data=rich.encode("utf-8")
                )
            )
        plain = plain_override if plain_override is not None else (
            mime_data.text() if mime_data.hasText() else ""
        )
        if plain:
            encoded = plain.encode("utf-8")
            self._check_size(len(encoded))
            item.representations.append(
                StoredRepresentation(
                    mime_type="text/plain", storage_kind="inline", data=encoded
                )
            )

    def build_mime(self, group: ShelfGroup, item_id: str | None = None) -> QMimeData:
        selected = (
            [item for item in group.items if item.id == item_id]
            if item_id
            else group.items
        )
        if not selected:
            raise KeyError(f"No matching shelf item in group {group.id}")

        result = QMimeData()
        urls: list[QUrl] = []
        for item in selected:
            if item.missing:
                continue
            for representation in item.representations:
                if representation.storage_kind == "uri" and representation.value:
                    url = QUrl(representation.value)
                    if url.isValid() and url not in urls:
                        urls.append(url)
                    continue
                if representation.storage_kind == "inline":
                    data = representation.data or b""
                elif representation.storage_kind == "file" and representation.value:
                    try:
                        data = Path(representation.value).read_bytes()
                    except OSError:
                        continue
                else:
                    continue
                if representation.mime_type == "text/plain":
                    result.setText(data.decode("utf-8", errors="replace"))
                elif representation.mime_type == "text/html":
                    result.setHtml(data.decode("utf-8", errors="replace"))
                else:
                    result.setData(representation.mime_type, QByteArray(data))
        if urls:
            result.setUrls(urls)
        if not result.formats():
            raise FileNotFoundError("The selected item is no longer available.")
        marker = f"{group.id}\n{item_id or ''}".encode("utf-8")
        result.setData(INTERNAL_MIME_TYPE, QByteArray(marker))
        return result

    def _is_ephemeral(self, path: Path) -> bool:
        try:
            resolved = path.resolve()
        except OSError:
            resolved = path.absolute()
        for root in self.ephemeral_roots:
            try:
                if resolved == root.resolve() or root.resolve() in resolved.parents:
                    return True
            except OSError:
                continue
        return False

    def _check_size(self, size: int) -> None:
        if size > self.max_owned_bytes:
            limit = self.max_owned_bytes // (1024 * 1024)
            raise PayloadTooLargeError(
                f"This item is larger than PasteShelf's {limit} MiB content limit."
            )

    @staticmethod
    def _text_title(text: str) -> str:
        compact = re.sub(r"\s+", " ", html_module.unescape(text)).strip()
        if len(compact) <= 64:
            return compact
        return compact[:61].rstrip() + "…"
