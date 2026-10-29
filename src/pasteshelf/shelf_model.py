from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import (
    QAbstractListModel,
    QByteArray,
    QFileInfo,
    QModelIndex,
    QObject,
    Property,
    QSize,
    Qt,
    QMimeData,
    QTimer,
    QUrl,
    Signal,
    Slot,
)
from PySide6.QtGui import QDrag, QIcon, QImage, QPixmap
from PySide6.QtQuick import QQuickImageProvider
from PySide6.QtWidgets import QFileIconProvider

from .entities import ShelfGroup, ShelfItem
from .mime_service import MimeService, UnsupportedMimeError
from .storage import ShelfStorage


class ShelfModel(QAbstractListModel):
    GroupIdRole = int(Qt.ItemDataRole.UserRole) + 1
    TitleRole = GroupIdRole + 1
    SubtitleRole = GroupIdRole + 2
    KindRole = GroupIdRole + 3
    CountRole = GroupIdRole + 4
    PreviewUrlRole = GroupIdRole + 5
    MissingRole = GroupIdRole + 6
    ExpandedRole = GroupIdRole + 7
    ItemsRole = GroupIdRole + 8
    DetailTextRole = GroupIdRole + 9
    PinnedRole = GroupIdRole + 10

    countChanged = Signal()
    dragActiveChanged = Signal()
    dragSourceChanged = Signal()
    groupPoofRequested = Signal(str)
    itemPoofRequested = Signal(str, str)
    notification = Signal(str)
    showRequested = Signal()
    clearConfirmationRequested = Signal()

    def __init__(
        self,
        storage: ShelfStorage,
        mime_service: MimeService,
        parent: QObject | None = None,
    ):
        super().__init__(parent)
        self.storage = storage
        self.mime_service = mime_service
        self.groups: list[ShelfGroup] = self.storage.load_groups()
        self._expanded: set[str] = set()
        self._drag_active = False
        self._dragging_group_id = ""
        self._dragging_item_id = ""

    def roleNames(self) -> dict[int, bytes]:
        return {
            self.GroupIdRole: b"groupId",
            self.TitleRole: b"title",
            self.SubtitleRole: b"subtitle",
            self.KindRole: b"kind",
            self.CountRole: b"itemCount",
            self.PreviewUrlRole: b"previewUrl",
            self.MissingRole: b"missing",
            self.ExpandedRole: b"expanded",
            self.ItemsRole: b"items",
            self.DetailTextRole: b"detailText",
            self.PinnedRole: b"pinned",
        }

    def rowCount(self, parent: QModelIndex = QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self.groups)

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole):
        if not index.isValid() or not 0 <= index.row() < len(self.groups):
            return None
        group = self.groups[index.row()]
        if role == Qt.ItemDataRole.DisplayRole or role == self.TitleRole:
            return group.title
        if role == self.GroupIdRole:
            return group.id
        if role == self.SubtitleRole:
            return group.subtitle
        if role == self.KindRole:
            return group.kind
        if role == self.CountRole:
            return len(group.items)
        if role == self.PreviewUrlRole:
            return f"image://previews/{group.id}"
        if role == self.MissingRole:
            return group.missing
        if role == self.ExpandedRole:
            return group.id in self._expanded
        if role == self.ItemsRole:
            return [self._item_map(group, item) for item in group.items]
        if role == self.DetailTextRole:
            return self._detail_text(group)
        if role == self.PinnedRole:
            return group.pinned
        return None

    @Property(int, notify=countChanged)
    def count(self) -> int:
        return len(self.groups)

    @Property(bool, notify=dragActiveChanged)
    def dragActive(self) -> bool:
        return self._drag_active

    @Property(str, notify=dragSourceChanged)
    def draggingGroupId(self) -> str:
        return self._dragging_group_id

    @Property(str, notify=dragSourceChanged)
    def draggingItemId(self) -> str:
        return self._dragging_item_id

    def importMimeData(self, mime_data) -> None:
        try:
            group = self.mime_service.import_mime(mime_data)
        except (UnsupportedMimeError, ValueError, OSError) as error:
            self.notification.emit(str(error))
            return
        row = len(self.groups)
        self.beginInsertRows(QModelIndex(), row, row)
        self.groups.append(group)
        self.endInsertRows()
        self.countChanged.emit()
        label = "stack" if len(group.items) > 1 else "item"
        self.notification.emit(f"Added {label} to PasteShelf")

    @Slot("QVariantList", str, str, str, QByteArray)
    def importDrop(
        self,
        urls: list,
        text: str,
        rich_text: str,
        image_mime: str,
        image_data: QByteArray,
    ) -> None:
        """Import the stable values exposed by Qt Quick's native DropArea."""
        mime_data = QMimeData()
        converted_urls: list[QUrl] = []
        for value in urls:
            url = value if isinstance(value, QUrl) else QUrl(str(value))
            if url.isValid():
                converted_urls.append(url)
        if converted_urls:
            mime_data.setUrls(converted_urls)
        if rich_text:
            mime_data.setHtml(rich_text)
        if text:
            mime_data.setText(text)
        if image_mime and image_data:
            mime_data.setData(image_mime, image_data)
        self.importMimeData(mime_data)

    @Slot(str)
    def removeGroup(self, group_id: str) -> None:
        row = self._row_for_group(group_id)
        if row < 0:
            return
        self.beginRemoveRows(QModelIndex(), row, row)
        self.storage.delete_group(group_id)
        self.groups.pop(row)
        self._expanded.discard(group_id)
        self.endRemoveRows()
        self.countChanged.emit()

    @Slot(str)
    def animateRemoveGroup(self, group_id: str) -> None:
        if self._row_for_group(group_id) < 0:
            return
        self.groupPoofRequested.emit(group_id)
        QTimer.singleShot(180, lambda: self.removeGroup(group_id))

    @Slot(str, str)
    def removeItem(self, group_id: str, item_id: str) -> None:
        group = self._group_for_id(group_id)
        if group is None:
            return
        item = next((candidate for candidate in group.items if candidate.id == item_id), None)
        if item is None:
            return
        if len(group.items) <= 1:
            self.removeGroup(group_id)
            return

        remaining = [candidate for candidate in group.items if candidate.id != item_id]
        if len(remaining) == 1:
            group_kind = remaining[0].kind
            group_title = remaining[0].title
        else:
            group_kind = "stack"
            group_title = f"{len(remaining)} items"
        self.storage.delete_item(
            group_id,
            item_id,
            group_kind=group_kind,
            group_title=group_title,
            remaining_item_ids=[candidate.id for candidate in remaining],
        )
        group.items = remaining
        group.kind = group_kind
        group.title = group_title
        for position, remaining_item in enumerate(group.items):
            remaining_item.position = position
        if len(group.items) == 1:
            self._expanded.discard(group_id)
        row = self._row_for_group(group_id)
        index = self.index(row, 0)
        self.dataChanged.emit(
            index,
            index,
            [
                self.TitleRole,
                self.SubtitleRole,
                self.KindRole,
                self.CountRole,
                self.PreviewUrlRole,
                self.MissingRole,
                self.ExpandedRole,
                self.ItemsRole,
                self.DetailTextRole,
            ],
        )

    @Slot()
    def clearAll(self) -> None:
        if not self.groups:
            return
        self.beginResetModel()
        self.storage.clear()
        self.groups.clear()
        self._expanded.clear()
        self.endResetModel()
        self.countChanged.emit()
        self.notification.emit("PasteShelf cleared")

    @Slot()
    def requestClearConfirmation(self) -> None:
        if self.groups:
            self.clearConfirmationRequested.emit()

    @Slot(str)
    def toggleExpanded(self, group_id: str) -> None:
        row = self._row_for_group(group_id)
        if row < 0:
            return
        if group_id in self._expanded:
            self._expanded.remove(group_id)
        else:
            self._expanded.add(group_id)
        index = self.index(row, 0)
        self.dataChanged.emit(index, index, [self.ExpandedRole])

    @Slot(str)
    def togglePinned(self, group_id: str) -> None:
        row = self._row_for_group(group_id)
        if row < 0:
            return
        group = self.groups[row]
        group.pinned = not group.pinned
        self.storage.set_group_pinned(group_id, group.pinned)
        index = self.index(row, 0)
        self.dataChanged.emit(index, index, [self.PinnedRole])
        state = "Pinned" if group.pinned else "Unpinned"
        self.notification.emit(f"{state} {group.title}")

    @Slot(str, str)
    def beginDrag(self, group_id: str, item_id: str = "") -> None:
        group = self._group_for_id(group_id)
        if group is None:
            return
        try:
            mime_data = self.mime_service.build_mime(group, item_id or None)
        except (KeyError, FileNotFoundError, OSError) as error:
            self.notification.emit(str(error))
            self.refreshMissing()
            return

        drag = QDrag(self)
        drag.setMimeData(mime_data)
        pixmap = PreviewProvider.render_group(group, QSize(128, 96))
        if not pixmap.isNull():
            drag.setPixmap(QPixmap.fromImage(pixmap))
            drag.setHotSpot(pixmap.rect().center())
        self._set_drag_source(group_id, item_id)
        self._set_drag_active(True)
        result = Qt.DropAction.IgnoreAction
        try:
            result = drag.exec(Qt.DropAction.CopyAction, Qt.DropAction.CopyAction)
        finally:
            self._set_drag_active(False)
            self.refreshMissing()
            drag.deleteLater()
        if result == Qt.DropAction.CopyAction:
            self._schedule_successful_drag_removal(group_id, item_id)
        else:
            self._set_drag_source("", "")

    @Slot()
    def refreshMissing(self) -> None:
        if not self.groups:
            return
        top = self.index(0, 0)
        bottom = self.index(len(self.groups) - 1, 0)
        self.dataChanged.emit(
            top,
            bottom,
            [self.MissingRole, self.SubtitleRole, self.ItemsRole, self.PreviewUrlRole],
        )

    def _set_drag_active(self, active: bool) -> None:
        if self._drag_active == active:
            return
        self._drag_active = active
        self.dragActiveChanged.emit()

    def _set_drag_source(self, group_id: str, item_id: str) -> None:
        if (
            self._dragging_group_id == group_id
            and self._dragging_item_id == item_id
        ):
            return
        self._dragging_group_id = group_id
        self._dragging_item_id = item_id
        self.dragSourceChanged.emit()

    def _schedule_successful_drag_removal(
        self, group_id: str, item_id: str
    ) -> None:
        group = self._group_for_id(group_id)
        if group is None:
            self._set_drag_source("", "")
            return
        if group.pinned:
            self.notification.emit("Copied pinned item · kept in PasteShelf")
            self._set_drag_source("", "")
            return
        # Let the delegate recover from its translucent in-drag state before
        # removal. Whole cards then use ListView's normal remove transition,
        # exactly like the × button, instead of animating from 42% opacity.
        self._set_drag_source("", "")
        if item_id and len(group.items) > 1:
            QTimer.singleShot(
                95,
                lambda: self._start_item_drag_removal(group_id, item_id),
            )
            return
        QTimer.singleShot(95, lambda: self.animateRemoveGroup(group_id))

    def _start_item_drag_removal(self, group_id: str, item_id: str) -> None:
        group = self._group_for_id(group_id)
        if group is None or not any(item.id == item_id for item in group.items):
            return
        self.itemPoofRequested.emit(group_id, item_id)
        QTimer.singleShot(
            180,
            lambda: self._finish_item_drag_removal(group_id, item_id),
        )

    def _finish_item_drag_removal(self, group_id: str, item_id: str) -> None:
        self.removeItem(group_id, item_id)

    def _row_for_group(self, group_id: str) -> int:
        return next(
            (index for index, group in enumerate(self.groups) if group.id == group_id),
            -1,
        )

    def _group_for_id(self, group_id: str) -> ShelfGroup | None:
        row = self._row_for_group(group_id)
        return self.groups[row] if row >= 0 else None

    @staticmethod
    def _item_map(group: ShelfGroup, item: ShelfItem) -> dict:
        return {
            "itemId": item.id,
            "title": item.title,
            "kind": item.kind,
            "missing": item.missing,
            "previewUrl": f"image://previews/{group.id}/{item.id}",
        }

    @staticmethod
    def _detail_text(group: ShelfGroup) -> str:
        if len(group.items) != 1:
            return ""
        item = group.items[0]
        for representation in item.representations:
            if representation.mime_type != "text/plain":
                continue
            if representation.storage_kind == "inline":
                return (representation.data or b"").decode(
                    "utf-8", errors="replace"
                )
            if representation.storage_kind == "file" and representation.value:
                try:
                    return Path(representation.value).read_text(
                        encoding="utf-8", errors="replace"
                    )
                except OSError:
                    return ""
        return ""


class PreviewProvider(QQuickImageProvider):
    def __init__(self, model: ShelfModel):
        super().__init__(QQuickImageProvider.ImageType.Image)
        self.model = model

    def requestImage(self, image_id: str, size: QSize, requested_size: QSize) -> QImage:
        parts = image_id.split("/", 1)
        group = self.model._group_for_id(parts[0])
        if group is None:
            return QImage()
        item = None
        if len(parts) == 2:
            item = next((entry for entry in group.items if entry.id == parts[1]), None)
        image = self.render_group(group, requested_size, item=item)
        if size is not None:
            size.setWidth(image.width())
            size.setHeight(image.height())
        return image

    @staticmethod
    def render_group(
        group: ShelfGroup,
        requested_size: QSize,
        *,
        item: ShelfItem | None = None,
    ) -> QImage:
        target = requested_size if requested_size.isValid() else QSize(96, 72)
        target = QSize(max(target.width(), 32), max(target.height(), 32))
        entry = item or (group.items[0] if group.items else None)
        if entry is None:
            return QImage()

        local_path = entry.local_path
        if local_path and local_path.is_file():
            image = QImage(str(local_path))
            if not image.isNull():
                return image.scaled(
                    target,
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation,
                )
            icon = QFileIconProvider().icon(QFileInfo(local_path.as_posix()))
        else:
            icon_names = {
                "folder": "folder",
                "link": "internet-web-browser",
                "html": "text-html",
                "text": "text-x-generic",
                "image": "image-x-generic",
                "file": "text-x-generic",
            }
            icon = QIcon.fromTheme(icon_names.get(entry.kind, "unknown"))
        if icon.isNull():
            icon = QIcon.fromTheme("edit-paste")
        return icon.pixmap(target).toImage()
