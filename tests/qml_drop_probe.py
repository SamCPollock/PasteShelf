"""Standalone Qt Quick drag/drop probe used by the unit test suite.

This must run in its own process because the other tests intentionally create
a QCoreApplication, while loading Main.qml requires a QApplication.
"""

from __future__ import annotations

import os
import sys
import tempfile
import atexit
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import (  # noqa: E402
    QCoreApplication,
    QMimeData,
    QPoint,
    QPointF,
    Qt,
)
from PySide6.QtGui import QDragEnterEvent, QDragLeaveEvent, QDropEvent  # noqa: E402
from PySide6.QtTest import QTest  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402
from shiboken6 import delete  # noqa: E402

from pasteshelf.desktop import DesktopController  # noqa: E402
from pasteshelf.dbus_service import PasteShelfDBusService  # noqa: E402
from pasteshelf.drawer_controller import DrawerController  # noqa: E402
from pasteshelf.mime_service import MimeService  # noqa: E402
from pasteshelf.mime_service import INTERNAL_MIME_TYPE  # noqa: E402
from pasteshelf.position_controller import PositionController  # noqa: E402
from pasteshelf.shelf_model import ShelfModel  # noqa: E402
from pasteshelf.storage import ShelfStorage  # noqa: E402
from pasteshelf.window import PasteShelfWindow  # noqa: E402

def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def find_quick_item(root, object_name: str):
    pending = [root]
    while pending:
        item = pending.pop()
        if item.objectName() == object_name:
            return item
        child_items = getattr(item, "childItems", None)
        if child_items is not None:
            pending.extend(child_items())
    return None


def find_object_child(root, object_name: str):
    pending = [root]
    while pending:
        item = pending.pop()
        if item.objectName() == object_name:
            return item
        pending.extend(item.children())
    return None


def main() -> int:
    app = QApplication(sys.argv)
    app.setOrganizationName("PasteShelfTests")
    app.setApplicationName("QmlDropProbe")

    with tempfile.TemporaryDirectory() as temporary_directory:
        storage = ShelfStorage(Path(temporary_directory) / "data")
        mime_service = MimeService(storage, ephemeral_roots=[])
        model = ShelfModel(storage, mime_service, app)
        drawer = DrawerController(app, persist_settings=False)
        position = PositionController(app)
        desktop = DesktopController(model, app)

        window = PasteShelfWindow(
            model,
            mime_service,
            drawer,
            desktop,
            position,
            auto_show=False,
        )
        require(window.ready, "Main.qml failed to load in the native drop widget")
        root = window.rootObject()
        cleaned_up = False

        def cleanup() -> None:
            nonlocal cleaned_up
            if cleaned_up:
                return
            cleaned_up = True
            window.close()
            delete(window)
            storage.close()

        # An assertion failure must still dismantle QML before QApplication's
        # global PySide cleanup. This prevents a failed probe from producing a
        # misleading system crash notification.
        atexit.register(cleanup)
        window.show()
        QCoreApplication.processEvents()
        if "--empty-screenshot" in sys.argv:
            screenshot_index = sys.argv.index("--empty-screenshot") + 1
            require(screenshot_index < len(sys.argv), "missing empty screenshot path")
            drawer.open()
            QTest.qWait(230)
            require(
                window.grabFramebuffer().save(sys.argv[screenshot_index]),
                "could not save empty-state screenshot",
            )
            drawer.close()
            QTest.qWait(180)

        mime_data = QMimeData()
        mime_data.setText("Text dragged from another application")
        enter = QDragEnterEvent(
            QPoint(10, 10),
            Qt.DropAction.CopyAction,
            mime_data,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        QCoreApplication.sendEvent(window, enter)
        QCoreApplication.processEvents()

        require(enter.isAccepted(), "QML DropArea did not accept text drag-enter")
        require(drawer.expanded, "drawer did not expand for an accepted drag")
        require(
            bool(root.property("externalDragActive")),
            "drop preview was not activated on drag-enter",
        )
        require(
            root.property("dropPreviewTitle")
            == "Text dragged from another application",
            "drop preview did not show the dragged text",
        )
        require(
            bool(root.property("surfaceExpanded")),
            "animated surface did not expand for the drag",
        )
        QTest.qWait(65)
        opening_progress = float(root.property("panelProgress"))
        require(
            0.05 < opening_progress < 0.99,
            "panel did not visibly progress through its slide-open animation "
            f"(progress={opening_progress})",
        )
        QTest.qWait(175)
        require(
            float(root.property("panelProgress")) > 0.99,
            "panel slide-open animation did not complete",
        )
        drop_preview = find_quick_item(root, "dropPreview")
        require(drop_preview is not None, "drop placeholder was not created")
        require(
            float(drop_preview.property("y")) > float(root.property("height")) * 0.65,
            "empty-tray drop placeholder was not anchored near the bottom",
        )
        if "--screenshot" in sys.argv:
            screenshot_index = sys.argv.index("--screenshot") + 1
            require(screenshot_index < len(sys.argv), "missing screenshot path")
            QCoreApplication.processEvents()
            require(
                window.grabFramebuffer().save(sys.argv[screenshot_index]),
                "could not save drop-preview screenshot",
            )

        drop = QDropEvent(
            QPointF(10, 10),
            Qt.DropAction.CopyAction,
            mime_data,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        QCoreApplication.sendEvent(window, drop)
        QCoreApplication.processEvents()

        require(drop.isAccepted(), "QML DropArea did not accept text drop")
        require(model.count == 1, "accepted text drop did not create one shelf card")
        require(
            model.data(model.index(0, 0), model.TitleRole)
            == "Text dragged from another application",
            "stored shelf card did not preserve the dropped text",
        )
        require(
            not bool(root.property("externalDragActive")),
            "drop preview remained visible after the drop",
        )
        QTest.qWait(80)
        text_preview = find_quick_item(root, "textPreviewFrame")
        require(text_preview is not None, "text card preview was not created")
        require(
            not bool(text_preview.property("visible")),
            "text card opened its full preview by default",
        )
        shelf_card = find_quick_item(root, "shelfCard")
        require(shelf_card is not None, "text shelf card was not created")
        shelf_card.setProperty("textPreviewExpanded", True)
        QCoreApplication.processEvents()
        require(
            bool(text_preview.property("visible")),
            "text card could not reveal its full preview",
        )
        shelf_card.setProperty("textPreviewExpanded", False)
        QCoreApplication.processEvents()

        hover_preview = find_object_child(shelf_card, "cardHoverPreview")
        require(hover_preview is not None, "hover preview was not created")
        require(
            int(hover_preview.property("delay")) == 425,
            "hover preview delay changed unexpectedly",
        )
        shelf_card.setProperty("hoverPreviewRequested", True)
        QTest.qWait(520)
        require(
            bool(hover_preview.property("opened")),
            "floating card preview could not be opened",
        )
        if "--hover-screenshot" in sys.argv:
            screenshot_index = sys.argv.index("--hover-screenshot") + 1
            require(screenshot_index < len(sys.argv), "missing hover screenshot path")
            require(
                window.grabFramebuffer().save(sys.argv[screenshot_index]),
                "could not save hover-preview screenshot",
            )
        shelf_card.setProperty("hoverPreviewRequested", False)
        QCoreApplication.processEvents()

        populated_enter = QDragEnterEvent(
            QPoint(10, 10),
            Qt.DropAction.CopyAction,
            mime_data,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        QCoreApplication.sendEvent(window, populated_enter)
        QTest.qWait(140)
        require(populated_enter.isAccepted(), "populated tray rejected text drag-enter")
        require(
            float(drop_preview.property("y")) > float(root.property("height")) * 0.65,
            "populated-tray drop placeholder was not anchored near the bottom",
        )
        QCoreApplication.sendEvent(window, QDragLeaveEvent())
        QCoreApplication.processEvents()

        internal_mime = QMimeData()
        internal_mime.setText("existing shelf card")
        internal_mime.setData(INTERNAL_MIME_TYPE, b"group\nitem")
        internal_enter = QDragEnterEvent(
            QPoint(10, 10),
            Qt.DropAction.CopyAction,
            internal_mime,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        QCoreApplication.sendEvent(window, internal_enter)
        QCoreApplication.processEvents()
        require(
            not internal_enter.isAccepted(),
            "PasteShelf accepted a card dragged back onto itself",
        )
        require(model.count == 1, "self-drag changed the shelf contents")

        clipboard_text = (
            "Captured explicitly from clipboard. This is deliberately long enough "
            "to wrap across several lines so the card preview can be inspected.\n\n"
            "A second paragraph verifies that line breaks and the complete text are "
            "available in the drawer instead of only the short title."
        )
        QApplication.clipboard().setText(clipboard_text)
        shortcut_service = PasteShelfDBusService(desktop, app, register=False)
        shortcut_service.CaptureClipboard()
        QCoreApplication.processEvents()
        require(model.count == 2, "explicit clipboard capture did not create a card")
        require(
            model.data(model.index(1, 0), model.DetailTextRole) == clipboard_text,
            "clipboard capture did not preserve the full copied text",
        )
        clipboard_group_id = model.data(model.index(1, 0), model.GroupIdRole)
        model.togglePinned(clipboard_group_id)
        QCoreApplication.processEvents()
        require(
            bool(model.data(model.index(1, 0), model.PinnedRole)),
            "pin state was not exposed to the QML card",
        )
        if "--card-screenshot" in sys.argv:
            screenshot_index = sys.argv.index("--card-screenshot") + 1
            require(screenshot_index < len(sys.argv), "missing card screenshot path")
            QTest.qWait(2600)
            require(
                window.grabFramebuffer().save(sys.argv[screenshot_index]),
                "could not save card-preview screenshot",
            )

        drawer.close()
        QTest.qWait(55)
        closing_progress = float(root.property("panelProgress"))
        require(
            0.01 < closing_progress < 0.999,
            "panel did not visibly progress through its slide-close animation "
            f"(progress={closing_progress})",
        )
        QTest.qWait(130)
        require(
            not bool(root.property("surfaceExpanded")),
            "panel slide-close animation did not release the expanded surface",
        )
        require(
            not bool(root.property("handleVisible")),
            "collapsed handle appeared before native resize/reposition settled",
        )
        QTest.qWait(140)
        require(
            bool(root.property("handleVisible")),
            "collapsed handle did not appear after resize/reposition settled",
        )
        # Tear down the QML object tree while the context model is still
        # alive. Letting QApplication destroy its children in creation order
        # can otherwise leave QQuickListView reacting to a deleted model.
        cleanup()
        atexit.unregister(cleanup)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
