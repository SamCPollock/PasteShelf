from __future__ import annotations

from PySide6.QtCore import QObject, Property, QSettings, QTimer, Signal, Slot


class DrawerController(QObject):
    expandedChanged = Signal()
    pinnedChanged = Signal()

    def __init__(
        self,
        parent: QObject | None = None,
        *,
        close_delay_ms: int = 1500,
        persist_settings: bool = True,
    ):
        super().__init__(parent)
        self._settings = QSettings() if persist_settings else None
        self._pinned = (
            bool(self._settings.value("window/pinned", False, type=bool))
            if self._settings
            else False
        )
        self._expanded = self._pinned
        self._hovered = False
        self._drop_active = False
        self._outbound_active = False
        self._menu_active = False
        self._close_timer = QTimer(self)
        self._close_timer.setSingleShot(True)
        self._close_timer.setInterval(close_delay_ms)
        self._close_timer.timeout.connect(self._close_if_idle)

    @Property(bool, notify=expandedChanged)
    def expanded(self) -> bool:
        return self._expanded

    @Property(bool, notify=pinnedChanged)
    def pinned(self) -> bool:
        return self._pinned

    @Slot()
    def open(self) -> None:
        self._close_timer.stop()
        self._set_expanded(True)

    @Slot()
    def close(self) -> None:
        if not self._has_blocker():
            self._close_timer.stop()
            self._set_expanded(False)

    @Slot()
    def scheduleClose(self) -> None:
        if not self._has_blocker():
            self._close_timer.start()

    @Slot(bool)
    def setHovered(self, active: bool) -> None:
        self._hovered = active
        self._update_for_activity(active)

    @Slot(bool)
    def setDropActive(self, active: bool) -> None:
        self._drop_active = active
        self._update_for_activity(active)

    @Slot(bool)
    def setOutboundActive(self, active: bool) -> None:
        self._outbound_active = active
        self._update_for_activity(active)

    @Slot(bool)
    def setMenuActive(self, active: bool) -> None:
        self._menu_active = active
        self._update_for_activity(active)

    @Slot(bool)
    def setPinned(self, active: bool) -> None:
        if self._pinned == active:
            return
        self._pinned = active
        if self._settings:
            self._settings.setValue("window/pinned", active)
        self.pinnedChanged.emit()
        if active:
            self.open()
        else:
            self.scheduleClose()

    @Slot()
    def togglePinned(self) -> None:
        self.setPinned(not self._pinned)

    def _update_for_activity(self, active: bool) -> None:
        if active:
            self.open()
        else:
            self.scheduleClose()

    def _has_blocker(self) -> bool:
        return (
            self._hovered
            or self._drop_active
            or self._outbound_active
            or self._menu_active
            or self._pinned
        )

    def _close_if_idle(self) -> None:
        if not self._has_blocker():
            self._set_expanded(False)

    def _set_expanded(self, expanded: bool) -> None:
        if self._expanded == expanded:
            return
        self._expanded = expanded
        self.expandedChanged.emit()
