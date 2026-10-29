from __future__ import annotations

import os
from pathlib import Path

from PySide6.QtCore import QLockFile, QObject, QStandardPaths
from PySide6.QtNetwork import QLocalServer, QLocalSocket


class SingleInstance:
    """A lock plus local socket used to focus the already-running shelf."""

    def __init__(self, on_show, parent: QObject | None = None):
        runtime = QStandardPaths.writableLocation(
            QStandardPaths.StandardLocation.RuntimeLocation
        )
        if not runtime:
            runtime = "/tmp"
        uid = os.getuid() if hasattr(os, "getuid") else 0
        self.name = f"pasteshelf-{uid}"
        self.lock = QLockFile(str(Path(runtime) / f"{self.name}.lock"))
        self.server = QLocalServer(parent)
        self.on_show = on_show

    def acquire(self) -> bool:
        if not self.lock.tryLock(150):
            if self.notify_existing():
                return False

            # A process can disappear without releasing its QLockFile (for
            # example after a crash).  If there is no server to notify, clear
            # only a lock that Qt itself confirms is stale, then retry once.
            if not self.lock.removeStaleLockFile() or not self.lock.tryLock(150):
                return False
        QLocalServer.removeServer(self.name)
        if not self.server.listen(self.name):
            self.lock.unlock()
            return False
        self.server.newConnection.connect(self._read_messages)
        return True

    def notify_existing(self) -> bool:
        socket = QLocalSocket()
        socket.connectToServer(self.name)
        if socket.waitForConnected(500):
            socket.write(b"show\n")
            socket.flush()
            socket.waitForBytesWritten(250)
            socket.disconnectFromServer()
            return True
        return False

    def _read_messages(self) -> None:
        while self.server.hasPendingConnections():
            socket = self.server.nextPendingConnection()
            socket.waitForReadyRead(100)
            if b"show" in bytes(socket.readAll()):
                self.on_show()
            socket.disconnectFromServer()
