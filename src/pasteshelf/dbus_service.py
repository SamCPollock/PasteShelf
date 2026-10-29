from __future__ import annotations

from PySide6.QtCore import ClassInfo, QObject, Slot
from PySide6.QtDBus import QDBusConnection

from .desktop import DesktopController


@ClassInfo({"D-Bus Interface": "io.github.pasteshelf.PasteShelf"})
class PasteShelfDBusService(QObject):
    """Small local endpoint used by the KWin global-shortcut callback."""

    SERVICE = "io.github.pasteshelf.PasteShelf"
    PATH = "/PasteShelf"

    def __init__(
        self,
        desktop: DesktopController,
        parent: QObject | None = None,
        *,
        register: bool = True,
    ):
        super().__init__(parent)
        self.desktop = desktop
        self.connection = QDBusConnection.sessionBus()
        self.registered = False
        if register and self.connection.isConnected():
            service_ok = self.connection.registerService(self.SERVICE)
            object_ok = service_ok and self.connection.registerObject(
                self.PATH,
                self,
                QDBusConnection.RegisterOption.ExportAllSlots,
            )
            self.registered = bool(service_ok and object_ok)
            if service_ok and not object_ok:
                self.connection.unregisterService(self.SERVICE)

    @Slot()
    def CaptureClipboard(self) -> None:
        self.desktop.captureClipboard()

    def close(self) -> None:
        if not self.registered:
            return
        self.connection.unregisterObject(self.PATH)
        self.connection.unregisterService(self.SERVICE)
        self.registered = False
