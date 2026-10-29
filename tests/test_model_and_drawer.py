from __future__ import annotations

import tempfile
import unittest
import os
import subprocess
import sys
from pathlib import Path
from unittest.mock import patch

from PySide6.QtCore import QByteArray, QCoreApplication, QMimeData, Qt, QUrl
from PySide6.QtGui import QImage
from PySide6.QtTest import QSignalSpy, QTest

from pasteshelf.drawer_controller import DrawerController
from pasteshelf.mime_service import MimeService
from pasteshelf.shelf_model import ShelfModel
from pasteshelf.single_instance import SingleInstance
from pasteshelf.storage import ShelfStorage


APP = QCoreApplication.instance() or QCoreApplication([])


class ModelAndDrawerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.storage = ShelfStorage(Path(self.temp.name) / "data")
        self.mime = MimeService(self.storage, ephemeral_roots=[])
        self.model = ShelfModel(self.storage, self.mime)

    def tearDown(self) -> None:
        self.storage.close()
        self.temp.cleanup()

    def test_single_instance_recovers_when_lock_owner_is_gone(self) -> None:
        class FakeLock:
            def __init__(self) -> None:
                self.attempts = 0
                self.removed_stale_lock = False

            def tryLock(self, _timeout: int) -> bool:
                self.attempts += 1
                return self.attempts == 2

            def removeStaleLockFile(self) -> bool:
                self.removed_stale_lock = True
                return True

            def unlock(self) -> None:
                pass

        class FakeSignal:
            def connect(self, _callback) -> None:
                pass

        class FakeServer:
            def __init__(self) -> None:
                self.newConnection = FakeSignal()

            def listen(self, _name: str) -> bool:
                return True

        instance = SingleInstance(lambda: None)
        fake_lock = FakeLock()
        instance.lock = fake_lock
        instance.server = FakeServer()

        with (
            patch.object(instance, "notify_existing", return_value=False),
            patch("pasteshelf.single_instance.QLocalServer.removeServer"),
        ):
            self.assertTrue(instance.acquire())

        self.assertTrue(fake_lock.removed_stale_lock)
        self.assertEqual(fake_lock.attempts, 2)

    def test_model_inserts_toggles_and_removes(self) -> None:
        offer = QMimeData()
        offer.setText("hello")
        count_spy = QSignalSpy(self.model.countChanged)

        self.model.importMimeData(offer)
        group_id = self.model.data(self.model.index(0, 0), self.model.GroupIdRole)
        self.model.toggleExpanded(group_id)

        self.assertEqual(self.model.count, 1)
        self.assertTrue(
            self.model.data(self.model.index(0, 0), self.model.ExpandedRole)
        )
        self.model.removeGroup(group_id)
        self.assertEqual(self.model.count, 0)
        self.assertEqual(count_spy.count(), 2)

    def test_animated_remove_poofs_before_deleting_the_card(self) -> None:
        offer = QMimeData()
        offer.setText("poof me")
        self.model.importMimeData(offer)
        group_id = self.model.data(self.model.index(0, 0), self.model.GroupIdRole)
        poof_spy = QSignalSpy(self.model.groupPoofRequested)

        self.model.animateRemoveGroup(group_id)

        self.assertEqual(poof_spy.count(), 1)
        self.assertEqual(self.model.count, 1)
        QTest.qWait(220)
        self.assertEqual(self.model.count, 0)

    def test_quick_drop_bridge_imports_text(self) -> None:
        self.model.importDrop([], "dragged text", "", "", QByteArray())

        self.assertEqual(self.model.count, 1)
        self.assertEqual(
            self.model.data(self.model.index(0, 0), self.model.TitleRole),
            "dragged text",
        )

    def test_model_exposes_full_text_for_card_preview(self) -> None:
        long_text = (
            "A full paragraph belongs in the preview, including its line breaks.\n\n"
            "This second paragraph must remain readable instead of being reduced "
            "to the card title."
        )
        offer = QMimeData()
        offer.setText(long_text)

        self.model.importMimeData(offer)

        self.assertEqual(
            self.model.data(self.model.index(0, 0), self.model.DetailTextRole),
            long_text,
        )

    def test_drawer_stays_open_while_activity_is_present(self) -> None:
        drawer = DrawerController(close_delay_ms=20, persist_settings=False)
        spy = QSignalSpy(drawer.expandedChanged)

        drawer.setHovered(True)
        drawer.setHovered(False)
        drawer.setDropActive(True)
        QTest.qWait(35)

        self.assertTrue(drawer.expanded)
        drawer.setDropActive(False)
        QTest.qWait(35)
        self.assertFalse(drawer.expanded)
        self.assertGreaterEqual(spy.count(), 2)

    def test_outbound_drag_prevents_collapse(self) -> None:
        drawer = DrawerController(close_delay_ms=15, persist_settings=False)
        drawer.open()
        drawer.setOutboundActive(True)
        drawer.scheduleClose()
        QTest.qWait(25)
        self.assertTrue(drawer.expanded)
        drawer.setOutboundActive(False)
        QTest.qWait(25)
        self.assertFalse(drawer.expanded)

    def test_pin_prevents_collapse_until_released(self) -> None:
        drawer = DrawerController(close_delay_ms=15, persist_settings=False)
        drawer.setPinned(True)
        drawer.scheduleClose()
        QTest.qWait(25)
        self.assertTrue(drawer.expanded)

        drawer.setPinned(False)
        QTest.qWait(25)
        self.assertFalse(drawer.expanded)

    def test_main_qml_accepts_and_stores_a_text_drop(self) -> None:
        environment = os.environ.copy()
        source_root = str(Path(__file__).resolve().parents[1] / "src")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (source_root, environment.get("PYTHONPATH", ""))
            if part
        )
        environment["QT_QPA_PLATFORM"] = "offscreen"
        result = subprocess.run(
            [sys.executable, str(Path(__file__).with_name("qml_drop_probe.py"))],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
            env=environment,
        )
        self.assertEqual(
            result.returncode,
            0,
            msg=f"QML drop probe failed:\n{result.stdout}\n{result.stderr}",
        )

    def test_successful_outbound_drag_removes_the_shelf_copy(self) -> None:
        offer = QMimeData()
        offer.setText("drag me out")
        self.model.importMimeData(offer)
        group_id = self.model.data(self.model.index(0, 0), self.model.GroupIdRole)

        class SuccessfulDrag:
            def __init__(self, _parent):
                pass

            def setMimeData(self, _mime_data):
                pass

            def exec(self, *_arguments):
                return Qt.DropAction.CopyAction

            def deleteLater(self):
                pass

        with (
            patch("pasteshelf.shelf_model.QDrag", SuccessfulDrag),
            patch(
                "pasteshelf.shelf_model.PreviewProvider.render_group",
                return_value=QImage(),
            ),
        ):
            self.model.beginDrag(group_id, "")
            self.assertEqual(self.model.count, 1)
            self.assertEqual(self.model.draggingGroupId, "")
            QTest.qWait(330)

        self.assertEqual(self.model.count, 0)
        self.assertEqual(self.storage.load_groups(), [])

    def test_ignored_outbound_drag_keeps_the_shelf_copy(self) -> None:
        offer = QMimeData()
        offer.setText("keep me")
        self.model.importMimeData(offer)
        group_id = self.model.data(self.model.index(0, 0), self.model.GroupIdRole)

        class IgnoredDrag:
            def __init__(self, _parent):
                pass

            def setMimeData(self, _mime_data):
                pass

            def exec(self, *_arguments):
                return Qt.DropAction.IgnoreAction

            def deleteLater(self):
                pass

        with (
            patch("pasteshelf.shelf_model.QDrag", IgnoredDrag),
            patch(
                "pasteshelf.shelf_model.PreviewProvider.render_group",
                return_value=QImage(),
            ),
        ):
            self.model.beginDrag(group_id, "")
            QTest.qWait(30)

        self.assertEqual(self.model.count, 1)
        self.assertEqual(self.model.draggingGroupId, "")

    def test_file_drag_advertises_move_but_text_and_pinned_files_do_not(self) -> None:
        source = Path(self.temp.name) / "movable.txt"
        source.write_text("move me", encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(source))])
        self.model.importMimeData(offer)
        group = self.model.groups[0]
        advertised_actions = []

        class InspectingDrag:
            def __init__(self, _parent):
                pass

            def setMimeData(self, _mime_data):
                pass

            def exec(self, actions, default_action):
                advertised_actions.append((actions, default_action))
                return Qt.DropAction.IgnoreAction

            def deleteLater(self):
                pass

        with (
            patch("pasteshelf.shelf_model.QDrag", InspectingDrag),
            patch(
                "pasteshelf.shelf_model.PreviewProvider.render_group",
                return_value=QImage(),
            ),
        ):
            self.model.beginDrag(group.id, "")
            self.assertEqual(
                advertised_actions.pop(),
                (
                    Qt.DropAction.CopyAction | Qt.DropAction.MoveAction,
                    Qt.DropAction.CopyAction,
                ),
            )

            self.model.togglePinned(group.id)
            self.model.beginDrag(group.id, "")
            self.assertEqual(
                advertised_actions.pop(),
                (Qt.DropAction.CopyAction, Qt.DropAction.CopyAction),
            )

        text_offer = QMimeData()
        text_offer.setText("copy only")
        self.model.importMimeData(text_offer)
        self.assertEqual(
            self.model._outbound_drag_actions(self.model.groups[-1]),
            Qt.DropAction.CopyAction,
        )

    def test_pinned_card_is_persisted_and_retained_after_successful_drag(self) -> None:
        offer = QMimeData()
        offer.setText("copy me repeatedly")
        self.model.importMimeData(offer)
        group_id = self.model.data(self.model.index(0, 0), self.model.GroupIdRole)
        self.model.togglePinned(group_id)

        class SuccessfulDrag:
            def __init__(self, _parent):
                pass

            def setMimeData(self, _mime_data):
                pass

            def exec(self, *_arguments):
                return Qt.DropAction.CopyAction

            def deleteLater(self):
                pass

        with (
            patch("pasteshelf.shelf_model.QDrag", SuccessfulDrag),
            patch(
                "pasteshelf.shelf_model.PreviewProvider.render_group",
                return_value=QImage(),
            ),
        ):
            self.model.beginDrag(group_id, "")
            QTest.qWait(30)

        self.assertEqual(self.model.count, 1)
        self.assertTrue(
            self.model.data(self.model.index(0, 0), self.model.PinnedRole)
        )
        stored = self.storage.get_group(group_id)
        self.assertIsNotNone(stored)
        self.assertTrue(stored.pinned)
        self.assertEqual(self.model.draggingGroupId, "")

    def test_successful_member_drag_poofs_only_that_stack_member(self) -> None:
        paths = [Path(self.temp.name) / "first", Path(self.temp.name) / "second"]
        for path in paths:
            path.write_text(path.name, encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(path)) for path in paths])
        self.model.importMimeData(offer)
        group = self.model.groups[0]
        removed_item_id = group.items[1].id
        poof_spy = QSignalSpy(self.model.itemPoofRequested)

        class SuccessfulDrag:
            def __init__(self, _parent):
                pass

            def setMimeData(self, _mime_data):
                pass

            def exec(self, *_arguments):
                return Qt.DropAction.CopyAction

            def deleteLater(self):
                pass

        with (
            patch("pasteshelf.shelf_model.QDrag", SuccessfulDrag),
            patch(
                "pasteshelf.shelf_model.PreviewProvider.render_group",
                return_value=QImage(),
            ),
        ):
            self.model.beginDrag(group.id, removed_item_id)
            self.assertEqual(len(self.model.groups[0].items), 2)
            self.assertEqual(self.model.draggingGroupId, "")
            QTest.qWait(320)

        self.assertEqual(poof_spy.count(), 1)
        self.assertEqual(self.model.count, 1)
        self.assertEqual(len(self.model.groups[0].items), 1)
        self.assertEqual(self.model.groups[0].items[0].title, "first")
        self.assertEqual(len(self.storage.load_groups()[0].items), 1)


if __name__ == "__main__":
    unittest.main()
