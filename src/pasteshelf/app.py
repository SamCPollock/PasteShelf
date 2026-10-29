from __future__ import annotations

import os
import shutil
import sys
from importlib.resources import files
from pathlib import Path

from PySide6.QtCore import QStandardPaths
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication

from .desktop import DesktopController
from .dbus_service import PasteShelfDBusService
from .drawer_controller import DrawerController
from .kwin_positioner import KWinPositioner
from .mime_service import MimeService
from .position_controller import PositionController
from .shelf_model import ShelfModel
from .single_instance import SingleInstance
from .storage import ShelfStorage
from .window import PasteShelfWindow


def _migrate_legacy_state() -> Path | None:
    """Move PasteTray-era state to PasteShelf without losing saved cards.

    Returns the legacy data directory only when moving it failed, allowing the
    renamed app to keep using the existing shelf rather than starting empty.
    """
    data_home = Path(
        os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")
    )
    config_home = Path(
        os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")
    )
    legacy_data = data_home / "PasteTray" / "PasteTray"
    current_data = data_home / "PasteShelf" / "PasteShelf"
    fallback_data: Path | None = None

    if legacy_data.exists() and not current_data.exists():
        try:
            current_data.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(legacy_data), str(current_data))
            try:
                legacy_data.parent.rmdir()
            except OSError:
                pass
        except OSError as error:
            print(
                f"PasteShelf could not migrate saved cards: {error}. "
                "Continuing with the existing data in place.",
                file=sys.stderr,
            )
            fallback_data = legacy_data

    legacy_settings = config_home / "PasteTray" / "PasteTray.conf"
    current_settings = config_home / "PasteShelf" / "PasteShelf.conf"
    if legacy_settings.exists() and not current_settings.exists():
        try:
            current_settings.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(legacy_settings), str(current_settings))
            try:
                legacy_settings.parent.rmdir()
            except OSError:
                pass
        except OSError as error:
            print(
                f"PasteShelf could not migrate settings: {error}",
                file=sys.stderr,
            )

    return fallback_data


def _configure_platform() -> None:
    if os.environ.get("XDG_SESSION_TYPE") == "wayland":
        # Use a regular native Wayland top-level so KWin routes external drag
        # offers to it. A temporary KWin script handles edge positioning.
        os.environ.setdefault("QT_QPA_PLATFORM", "wayland")


def main(argv: list[str] | None = None) -> int:
    _configure_platform()
    app = QApplication(argv if argv is not None else sys.argv)
    app.setOrganizationName("PasteShelf")
    app.setOrganizationDomain("io.github.pasteshelf")
    app.setApplicationName("PasteShelf")
    app.setApplicationDisplayName("PasteShelf")
    app.setDesktopFileName("io.github.pasteshelf.PasteShelf")
    icon_path = files("pasteshelf").joinpath(
        "assets", "io.github.pasteshelf.PasteShelf.png"
    )
    app.setWindowIcon(QIcon(str(icon_path)))
    app.setQuitOnLastWindowClosed(False)

    legacy_data_fallback = _migrate_legacy_state()

    drawer = DrawerController(app)
    instance = SingleInstance(drawer.open, app)
    if os.environ.get("PASTESHELF_DISABLE_SINGLE_INSTANCE") != "1":
        if not instance.acquire():
            return 0

    data_path = legacy_data_fallback or Path(
        QStandardPaths.writableLocation(
            QStandardPaths.StandardLocation.AppDataLocation
        )
    )
    storage = ShelfStorage(data_path)
    storage.cleanup_orphans()
    mime_service = MimeService(storage)
    model = ShelfModel(storage, mime_service, app)
    desktop = DesktopController(model, app)
    dbus_service = PasteShelfDBusService(desktop, app)
    position = PositionController(app)
    kwin_positioner = KWinPositioner()
    if os.environ.get("QT_QPA_PLATFORM") == "wayland":
        kwin_positioner.start()

    if os.environ.get("PASTESHELF_START_EXPANDED") == "1":
        drawer.open()

    model.showRequested.connect(drawer.open)
    model.dragActiveChanged.connect(
        lambda: drawer.setOutboundActive(model.dragActive)
    )

    window = PasteShelfWindow(
        model,
        mime_service,
        drawer,
        desktop,
        position,
        debug_dnd="--debug-dnd" in app.arguments(),
    )
    if not window.ready:
        storage.close()
        return 1

    app.aboutToQuit.connect(kwin_positioner.stop)
    app.aboutToQuit.connect(dbus_service.close)
    app.aboutToQuit.connect(storage.close)
    return app.exec()
