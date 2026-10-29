from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

from PySide6.QtCore import QObject, Property, Signal, Slot
from PySide6.QtGui import QAction, QIcon
from PySide6.QtWidgets import QApplication, QMenu, QSystemTrayIcon

from .shelf_model import ShelfModel


DESKTOP_FILENAME = "io.github.pasteshelf.PasteShelf.desktop"


class DesktopController(QObject):
    autostartEnabledChanged = Signal()

    def __init__(self, model: ShelfModel, parent: QObject | None = None):
        super().__init__(parent)
        self.model = model
        config_home = Path(
            os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")
        )
        data_home = Path(
            os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")
        )
        self.autostart_path = config_home / "autostart" / DESKTOP_FILENAME
        self.launcher_path = data_home / "applications" / DESKTOP_FILENAME
        self._tray_menu: QMenu | None = None
        self._tray = self._create_tray()

    @Property(bool, notify=autostartEnabledChanged)
    def autostartEnabled(self) -> bool:
        return self.autostart_path.exists()

    @Slot(bool)
    def setAutostartEnabled(self, enabled: bool) -> None:
        try:
            if enabled:
                self.autostart_path.parent.mkdir(parents=True, exist_ok=True)
                if self.launcher_path.exists():
                    shutil.copy2(self.launcher_path, self.autostart_path)
                else:
                    self.autostart_path.write_text(
                        self._desktop_entry(), encoding="utf-8"
                    )
            else:
                self.autostart_path.unlink(missing_ok=True)
        except OSError as error:
            self.model.notification.emit(f"Could not update login startup: {error}")
            return
        self.autostartEnabledChanged.emit()
        if self._tray:
            action = self._tray.contextMenu().findChild(QAction, "autostartAction")
            if action:
                action.blockSignals(True)
                action.setChecked(enabled)
                action.blockSignals(False)

    @Slot()
    def quit(self) -> None:
        QApplication.quit()

    @Slot()
    def captureClipboard(self) -> None:
        mime_data = QApplication.clipboard().mimeData()
        if mime_data is None or not self.model.mime_service.can_import(mime_data):
            self.model.notification.emit(
                "The clipboard does not contain supported text, files, links, or an image."
            )
            self.model.showRequested.emit()
            return
        self.model.importMimeData(mime_data)
        self.model.showRequested.emit()

    def _desktop_entry(self) -> str:
        executable = str(Path(sys.executable).resolve()).replace('"', '\\"')
        return (
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=PasteShelf\n"
            "Comment=Temporary shelf for dragged files and snippets\n"
            f'Exec="{executable}" -m pasteshelf\n'
            "Icon=io.github.pasteshelf.PasteShelf\n"
            "Terminal=false\n"
            "Categories=Utility;\n"
            "StartupNotify=false\n"
            "X-KDE-autostart-after=panel\n"
        )

    def _create_tray(self) -> QSystemTrayIcon | None:
        if not QSystemTrayIcon.isSystemTrayAvailable():
            return None
        icon = QApplication.windowIcon()
        if icon.isNull():
            icon = QIcon.fromTheme("io.github.pasteshelf.PasteShelf")
        if icon.isNull():
            icon = QIcon.fromTheme("edit-paste")
        tray = QSystemTrayIcon(icon, self)
        tray.setToolTip("PasteShelf")
        menu = QMenu()
        capture_action = menu.addAction("Add Clipboard to PasteShelf")
        capture_action.setShortcut("Meta+Shift+V")
        capture_action.triggered.connect(
            lambda _checked=False: self.captureClipboard()
        )
        menu.addSeparator()
        show_action = menu.addAction("Show PasteShelf")
        show_action.triggered.connect(lambda _checked=False: self.model.showRequested.emit())
        clear_action = menu.addAction("Clear…")
        clear_action.triggered.connect(
            lambda _checked=False: self.model.requestClearConfirmation()
        )
        menu.addSeparator()
        autostart_action = menu.addAction("Launch at login")
        autostart_action.setObjectName("autostartAction")
        autostart_action.setCheckable(True)
        autostart_action.setChecked(self.autostartEnabled)
        autostart_action.toggled.connect(self.setAutostartEnabled)
        menu.addSeparator()
        quit_action = menu.addAction("Quit")
        quit_action.triggered.connect(QApplication.quit)
        tray.setContextMenu(menu)
        self._tray_menu = menu
        tray.activated.connect(self._tray_activated)
        tray.show()
        return tray

    def _tray_activated(self, reason: QSystemTrayIcon.ActivationReason) -> None:
        if reason in (
            QSystemTrayIcon.ActivationReason.Trigger,
            QSystemTrayIcon.ActivationReason.DoubleClick,
        ):
            self.model.showRequested.emit()
