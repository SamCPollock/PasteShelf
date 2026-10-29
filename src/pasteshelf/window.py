from __future__ import annotations

import re
from importlib.resources import files
from urllib.parse import unquote

from PySide6.QtCore import QMimeData, QUrl, Qt
from PySide6.QtGui import QColor
from PySide6.QtQuickWidgets import QQuickWidget

from .desktop import DesktopController
from .drawer_controller import DrawerController
from .mime_service import INTERNAL_MIME_TYPE, MimeService
from .position_controller import PositionController
from .shelf_model import PreviewProvider, ShelfModel


class PasteShelfWindow(QQuickWidget):
    """Native QWidget drop target with the shelf rendered by Qt Quick."""

    def __init__(
        self,
        model: ShelfModel,
        mime_service: MimeService,
        drawer: DrawerController,
        desktop: DesktopController,
        position: PositionController,
        *,
        debug_dnd: bool = False,
        auto_show: bool = True,
    ):
        super().__init__()
        self.model = model
        self.mime_service = mime_service
        self.drawer = drawer
        self.position = position
        self.debug_dnd = debug_dnd
        self._root = None

        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setWindowFlags(
            Qt.WindowType.Window
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setClearColor(QColor(Qt.GlobalColor.transparent))
        self.setResizeMode(QQuickWidget.ResizeMode.SizeRootObjectToView)

        engine = self.engine()
        engine.addImageProvider("previews", PreviewProvider(model))
        context = self.rootContext()
        context.setContextProperty("shelfModel", model)
        context.setContextProperty("drawerController", drawer)
        context.setContextProperty("desktopController", desktop)
        context.setContextProperty("positionController", position)
        context.setContextProperty("debugDnd", debug_dnd)
        qml_path = files("pasteshelf").joinpath("qml", "Main.qml")
        self.setSource(QUrl.fromLocalFile(str(qml_path)))

        if self.status() == QQuickWidget.Status.Ready:
            self._root = self.rootObject()
            self._root.surfaceExpandedChanged.connect(self._sync_surface_size)
            position.placementChanged.connect(self._sync_surface_size)
            position.screenChanged.connect(self._sync_surface_size)
            position.screensChanged.connect(self._sync_surface_size)
            self._sync_surface_size()
            if auto_show:
                self.show()

    @property
    def ready(self) -> bool:
        return self.status() == QQuickWidget.Status.Ready and self._root is not None

    def dragEnterEvent(self, event) -> None:
        mime_data = event.mimeData()
        self._debug("entered", mime_data)
        if mime_data.hasFormat(INTERNAL_MIME_TYPE):
            event.ignore()
            return
        if not self.mime_service.can_import(mime_data):
            event.ignore()
            return
        event.setDropAction(Qt.DropAction.CopyAction)
        event.accept()
        self._show_drop_preview(mime_data)
        self.drawer.setDropActive(True)

    def dragMoveEvent(self, event) -> None:
        mime_data = event.mimeData()
        if (
            mime_data.hasFormat(INTERNAL_MIME_TYPE)
            or not self.mime_service.can_import(mime_data)
        ):
            event.ignore()
            return
        event.setDropAction(Qt.DropAction.CopyAction)
        event.accept()

    def dragLeaveEvent(self, event) -> None:
        self._debug("exited")
        self._hide_drop_preview()
        self.drawer.setDropActive(False)
        event.accept()

    def dropEvent(self, event) -> None:
        mime_data = event.mimeData()
        self._debug("dropped", mime_data)
        if (
            mime_data.hasFormat(INTERNAL_MIME_TYPE)
            or not self.mime_service.can_import(mime_data)
        ):
            event.ignore()
            self._hide_drop_preview()
            self.drawer.setDropActive(False)
            return
        self.model.importMimeData(mime_data)
        event.setDropAction(Qt.DropAction.CopyAction)
        event.accept()
        self._hide_drop_preview()
        self.drawer.setDropActive(False)
        self.drawer.open()

    def enterEvent(self, event) -> None:
        self.drawer.setHovered(True)
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self.drawer.setHovered(False)
        super().leaveEvent(event)

    def _sync_surface_size(self) -> None:
        if not self._root:
            return
        expanded = bool(self._root.property("surfaceExpanded"))
        if expanded:
            width = int(self._root.property("expandedWidth"))
            height = int(self._root.property("panelHeight"))
        else:
            width = int(self._root.property("collapsedWidth"))
            height = int(self._root.property("collapsedHeight"))
        self.setFixedSize(width, height)
        self._sync_position_title(width, height)

    def _sync_position_title(self, width: int, height: int) -> None:
        placement = self.position.placement
        if "left" in placement:
            x = self.position.screenX + 12
        elif "right" in placement:
            x = self.position.screenX + self.position.screenWidth - width - 12
        else:
            x = self.position.screenX + round((self.position.screenWidth - width) / 2)

        if "top" in placement:
            y = self.position.screenY + 12
        elif "bottom" in placement:
            y = self.position.screenY + self.position.screenHeight - height - 12
        else:
            y = self.position.screenY + round((self.position.screenHeight - height) / 2)

        self.setWindowTitle(f"PasteShelf@{x},{y},{width},{height}")
        # Useful on X11/offscreen; native Wayland positioning is performed by
        # the temporary KWin helper using the same encoded title.
        self.move(x, y)

    def _show_drop_preview(self, mime_data: QMimeData) -> None:
        if not self._root:
            return
        title, detail, glyph = self._preview(mime_data)
        self._root.setProperty("dropPreviewTitle", title)
        self._root.setProperty("dropPreviewDetail", detail)
        self._root.setProperty("dropPreviewGlyph", glyph)
        self._root.setProperty("externalDragActive", True)

    def _hide_drop_preview(self) -> None:
        if self._root:
            self._root.setProperty("externalDragActive", False)

    @staticmethod
    def _preview(mime_data: QMimeData) -> tuple[str, str, str]:
        if mime_data.hasUrls():
            urls = mime_data.urls()
            first = urls[0]
            title = unquote(first.fileName()) or first.host() or first.toDisplayString()
            detail = (
                f"{len(urls)} files or links · Release to add here"
                if len(urls) > 1
                else "File or link · Release to add here"
            )
            return title, detail, "▦" if len(urls) > 1 else "□"
        if mime_data.hasImage() or any(
            mime_type.startswith("image/") for mime_type in mime_data.formats()
        ):
            return "Image", "Image content · Release to add here", "▣"
        if mime_data.hasHtml() or mime_data.hasText():
            compact = re.sub(r"\s+", " ", mime_data.text() or "Rich text").strip()
            title = compact if len(compact) <= 64 else compact[:61].rstrip() + "…"
            detail = (
                "Rich text · Release to add here"
                if mime_data.hasHtml()
                else "Text snippet · Release to add here"
            )
            return title, detail, "≡"
        return "Unsupported item", "PasteShelf cannot store this drag", "!"

    def _debug(self, event_name: str, mime_data: QMimeData | None = None) -> None:
        if not self.debug_dnd:
            return
        formats = list(mime_data.formats()) if mime_data else []
        print(f"PasteShelf native DND {event_name}: {formats}", flush=True)
