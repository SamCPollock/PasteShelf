from __future__ import annotations

from PySide6.QtCore import QObject, Property, QSettings, Signal, Slot
from PySide6.QtGui import QGuiApplication, QScreen


PLACEMENTS: list[tuple[str, str]] = [
    ("top-left", "Top left"),
    ("top-center", "Top center"),
    ("top-right", "Top right"),
    ("right-center", "Right center"),
    ("bottom-right", "Bottom right"),
    ("bottom-center", "Bottom center"),
    ("bottom-left", "Bottom left"),
    ("left-center", "Left center"),
]


class PositionController(QObject):
    placementChanged = Signal()
    screensChanged = Signal()
    screenChanged = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._settings = QSettings()
        saved_placement = self._settings.value("window/placement", "right-center")
        valid_ids = {placement_id for placement_id, _label in PLACEMENTS}
        self._placement = (
            str(saved_placement) if str(saved_placement) in valid_ids else "right-center"
        )
        self._saved_screen_name = str(self._settings.value("window/screen", ""))
        self._screen: QScreen | None = self._resolve_screen(self._saved_screen_name)

        app = QGuiApplication.instance()
        if app:
            app.screenAdded.connect(self._screens_updated)
            app.screenRemoved.connect(self._screens_updated)

    @Property(str, notify=placementChanged)
    def placement(self) -> str:
        return self._placement

    @Property("QStringList", constant=True)
    def placementLabels(self) -> list[str]:
        return [label for _placement_id, label in PLACEMENTS]

    @Property(int, notify=placementChanged)
    def currentPlacementIndex(self) -> int:
        return next(
            (
                index
                for index, (placement_id, _label) in enumerate(PLACEMENTS)
                if placement_id == self._placement
            ),
            3,
        )

    @Property("QStringList", notify=screensChanged)
    def screenLabels(self) -> list[str]:
        return [self._screen_label(screen) for screen in self._screens()]

    @Property(int, notify=screenChanged)
    def currentScreenIndex(self) -> int:
        screens = self._screens()
        return screens.index(self._screen) if self._screen in screens else 0

    @Property(QObject, notify=screenChanged)
    def screen(self) -> QObject | None:
        return self._screen

    @Property(int, notify=screenChanged)
    def screenX(self) -> int:
        return self._screen.geometry().x() if self._screen else 0

    @Property(int, notify=screenChanged)
    def screenY(self) -> int:
        return self._screen.geometry().y() if self._screen else 0

    @Property(int, notify=screenChanged)
    def screenWidth(self) -> int:
        return self._screen.geometry().width() if self._screen else 1920

    @Property(int, notify=screenChanged)
    def screenHeight(self) -> int:
        return self._screen.geometry().height() if self._screen else 1080

    @Slot(int)
    def setPlacementIndex(self, index: int) -> None:
        if not 0 <= index < len(PLACEMENTS):
            return
        placement = PLACEMENTS[index][0]
        if placement == self._placement:
            return
        self._placement = placement
        self._settings.setValue("window/placement", placement)
        self.placementChanged.emit()

    @Slot(int)
    def setScreenIndex(self, index: int) -> None:
        screens = self._screens()
        if not 0 <= index < len(screens):
            return
        screen = screens[index]
        if screen is self._screen:
            return
        self._screen = screen
        self._saved_screen_name = screen.name()
        self._settings.setValue("window/screen", self._saved_screen_name)
        self.screenChanged.emit()

    def _screens(self) -> list[QScreen]:
        app = QGuiApplication.instance()
        return list(app.screens()) if app else []

    def _resolve_screen(self, name: str) -> QScreen | None:
        screens = self._screens()
        if name:
            match = next((screen for screen in screens if screen.name() == name), None)
            if match:
                return match
        app = QGuiApplication.instance()
        return app.primaryScreen() if app else (screens[0] if screens else None)

    def _screens_updated(self, _screen: QScreen) -> None:
        screens = self._screens()
        if self._screen not in screens:
            self._screen = self._resolve_screen(self._saved_screen_name)
            self.screenChanged.emit()
        self.screensChanged.emit()

    @staticmethod
    def _screen_label(screen: QScreen) -> str:
        manufacturer = screen.manufacturer().strip()
        model = screen.model().strip()
        detail = " ".join(part for part in (manufacturer, model) if part)
        return f"{screen.name()} — {detail}" if detail else screen.name()
