from __future__ import annotations

import shutil
import subprocess
from importlib.resources import files


class KWinPositioner:
    """Load a session-scoped KWin script for native Wayland positioning."""

    PLUGIN_NAME = "pasteshelf-positioner"

    def __init__(self):
        self._gdbus = shutil.which("gdbus")
        self._loaded = False

    def start(self) -> bool:
        if not self._gdbus:
            return False
        script = files("pasteshelf").joinpath("kwin", "pasteshelf-positioner.js")
        self.stop()
        loaded = self._call(
            "org.kde.kwin.Scripting.loadScript",
            str(script),
            self.PLUGIN_NAME,
        )
        if loaded is None:
            return False
        started = self._call("org.kde.kwin.Scripting.start")
        self._loaded = started is not None
        return self._loaded

    def stop(self) -> None:
        if not self._gdbus:
            return
        self._call("org.kde.kwin.Scripting.unloadScript", self.PLUGIN_NAME)
        self._loaded = False

    def _call(self, method: str, *arguments: str) -> str | None:
        if not self._gdbus:
            return None
        command = [
            self._gdbus,
            "call",
            "--session",
            "--dest",
            "org.kde.KWin",
            "--object-path",
            "/Scripting",
            "--method",
            method,
            *arguments,
        ]
        try:
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=3,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        return result.stdout.strip() if result.returncode == 0 else None
