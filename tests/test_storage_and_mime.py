from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from PySide6.QtCore import QMimeData, QUrl
from PySide6.QtGui import QImage

from pasteshelf.app import _migrate_legacy_state
from pasteshelf.mime_service import (
    INTERNAL_MIME_TYPE,
    MimeService,
    PayloadTooLargeError,
)
from pasteshelf.storage import SCHEMA_VERSION, ShelfStorage


class StorageAndMimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.storage = ShelfStorage(self.root / "data")
        self.mime = MimeService(
            self.storage,
            ephemeral_roots=[self.root / "ephemeral"],
            max_owned_bytes=1024 * 1024,
        )

    def tearDown(self) -> None:
        self.storage.close()
        self.temp.cleanup()

    def test_schema_is_versioned(self) -> None:
        with sqlite3.connect(self.storage.db_path) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], SCHEMA_VERSION)

    def test_storage_permissions_are_private(self) -> None:
        self.assertEqual(self.storage.root.stat().st_mode & 0o077, 0)
        self.assertEqual(self.storage.content_dir.stat().st_mode & 0o077, 0)
        self.assertEqual(self.storage.db_path.stat().st_mode & 0o077, 0)
        for sidecar in self.storage.root.glob("items.sqlite3-*"):
            self.assertEqual(sidecar.stat().st_mode & 0o077, 0)

    def test_legacy_state_migrates_to_pasteshelf(self) -> None:
        data_home = self.root / "xdg-data"
        config_home = self.root / "xdg-config"
        legacy_data = data_home / "PasteTray" / "PasteTray"
        legacy_data.mkdir(parents=True)
        (legacy_data / "items.sqlite3").write_bytes(b"saved cards")
        legacy_settings = config_home / "PasteTray" / "PasteTray.conf"
        legacy_settings.parent.mkdir(parents=True)
        legacy_settings.write_text("[window]\nplacement=top-right\n", encoding="utf-8")

        with patch.dict(
            "os.environ",
            {
                "XDG_DATA_HOME": str(data_home),
                "XDG_CONFIG_HOME": str(config_home),
            },
        ):
            fallback = _migrate_legacy_state()

        self.assertIsNone(fallback)
        self.assertEqual(
            (data_home / "PasteShelf" / "PasteShelf" / "items.sqlite3").read_bytes(),
            b"saved cards",
        )
        self.assertIn(
            "placement=top-right",
            (config_home / "PasteShelf" / "PasteShelf.conf").read_text(
                encoding="utf-8"
            ),
        )
        self.assertFalse(legacy_data.exists())
        self.assertFalse(legacy_settings.exists())

    def test_version_one_database_migrates_with_unpinned_default(self) -> None:
        legacy_root = self.root / "legacy"
        legacy_root.mkdir()
        legacy_db_path = legacy_root / "items.sqlite3"
        with sqlite3.connect(legacy_db_path) as db:
            db.executescript(
                """
                CREATE TABLE groups (
                    id TEXT PRIMARY KEY,
                    created_at REAL NOT NULL,
                    position INTEGER NOT NULL,
                    kind TEXT NOT NULL,
                    title TEXT NOT NULL
                );
                INSERT INTO groups(id, created_at, position, kind, title)
                VALUES ('old-card', 1.0, 0, 'text', 'Old card');
                PRAGMA user_version = 1;
                """
            )

        legacy_storage = ShelfStorage(legacy_root)
        try:
            with sqlite3.connect(legacy_storage.db_path) as db:
                self.assertEqual(
                    db.execute("PRAGMA user_version").fetchone()[0],
                    SCHEMA_VERSION,
                )
                columns = {
                    row[1]: row
                    for row in db.execute("PRAGMA table_info(groups)").fetchall()
                }
                self.assertIn("pinned", columns)
                self.assertEqual(
                    db.execute(
                        "SELECT pinned FROM groups WHERE id = 'old-card'"
                    ).fetchone()[0],
                    0,
                )
        finally:
            legacy_storage.close()

    def test_plain_and_rich_text_round_trip(self) -> None:
        offer = QMimeData()
        offer.setText("A useful sentence")
        offer.setHtml("<p><b>A useful sentence</b></p>")

        group = self.mime.import_mime(offer)
        restored = self.mime.build_mime(group)

        self.assertEqual(group.kind, "html")
        self.assertEqual(restored.text(), "A useful sentence")
        self.assertIn("<b>", restored.html())
        self.assertEqual(len(self.storage.load_groups()), 1)

    def test_card_pin_state_persists(self) -> None:
        offer = QMimeData()
        offer.setText("keep this card")
        group = self.mime.import_mime(offer)

        self.storage.set_group_pinned(group.id, True)

        restored = self.storage.get_group(group.id)
        self.assertIsNotNone(restored)
        self.assertTrue(restored.pinned)

    def test_multiple_files_become_one_stack_and_remain_references(self) -> None:
        first = self.root / "first.txt"
        second = self.root / "second.txt"
        first.write_text("first", encoding="utf-8")
        second.write_text("second", encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(first)), QUrl.fromLocalFile(str(second))])

        group = self.mime.import_mime(offer)
        restored = self.mime.build_mime(group)

        self.assertEqual(group.kind, "stack")
        self.assertEqual(len(group.items), 2)
        self.assertTrue(all(item.owned_path is None for item in group.items))
        self.assertEqual(
            [url.toLocalFile() for url in restored.urls()],
            [str(first), str(second)],
        )

    def test_single_stack_member_can_be_dragged_alone(self) -> None:
        paths = [self.root / "one", self.root / "two"]
        for path in paths:
            path.write_text(path.name, encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(path)) for path in paths])
        group = self.mime.import_mime(offer)

        restored = self.mime.build_mime(group, group.items[1].id)

        self.assertEqual([url.toLocalFile() for url in restored.urls()], [str(paths[1])])

    def test_outbound_drag_is_marked_as_internal(self) -> None:
        offer = QMimeData()
        offer.setText("do not clone me")
        group = self.mime.import_mime(offer)

        outbound = self.mime.build_mime(group)

        self.assertTrue(outbound.hasFormat(INTERNAL_MIME_TYPE))
        self.assertFalse(self.mime.can_import(outbound))

    def test_ephemeral_files_are_materialized_and_cleaned_up(self) -> None:
        ephemeral_dir = self.root / "ephemeral"
        ephemeral_dir.mkdir()
        source = ephemeral_dir / "shot.png"
        source.write_bytes(b"temporary screenshot")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(source))])

        group = self.mime.import_mime(offer)
        owned = Path(group.items[0].owned_path or "")
        source.unlink()

        self.assertTrue(owned.exists())
        self.assertEqual(self.mime.build_mime(group).urls()[0].toLocalFile(), str(owned))
        self.storage.delete_group(group.id)
        self.assertFalse(owned.exists())

    def test_image_is_stored_as_png(self) -> None:
        image = QImage(12, 7, QImage.Format.Format_ARGB32)
        image.fill(0xFF336699)
        offer = QMimeData()
        offer.setImageData(image)

        group = self.mime.import_mime(offer)
        restored = self.mime.build_mime(group)

        self.assertEqual(group.kind, "image")
        self.assertTrue(Path(group.items[0].owned_path or "").exists())
        self.assertTrue(restored.hasFormat("image/png"))
        self.assertTrue(restored.hasUrls())

    def test_missing_referenced_file_is_reported_and_not_exported(self) -> None:
        source = self.root / "vanishing.txt"
        source.write_text("gone soon", encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(source))])
        group = self.mime.import_mime(offer)
        source.unlink()

        self.assertTrue(group.missing)
        with self.assertRaises(FileNotFoundError):
            self.mime.build_mime(group)

    def test_owned_payload_limit(self) -> None:
        ephemeral_dir = self.root / "ephemeral"
        ephemeral_dir.mkdir()
        source = ephemeral_dir / "large.bin"
        source.write_bytes(b"x" * (1024 * 1024 + 1))
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(source))])

        with self.assertRaises(PayloadTooLargeError):
            self.mime.import_mime(offer)

    def test_clear_keeps_original_referenced_files(self) -> None:
        source = self.root / "keep.txt"
        source.write_text("keep me", encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(source))])
        self.mime.import_mime(offer)

        self.storage.clear()

        self.assertTrue(source.exists())
        self.assertEqual(self.storage.load_groups(), [])

    def test_delete_one_stack_member_updates_persisted_group(self) -> None:
        paths = [self.root / "one", self.root / "two", self.root / "three"]
        for path in paths:
            path.write_text(path.name, encoding="utf-8")
        offer = QMimeData()
        offer.setUrls([QUrl.fromLocalFile(str(path)) for path in paths])
        group = self.mime.import_mime(offer)
        removed = group.items[1]
        remaining = [group.items[0], group.items[2]]

        self.storage.delete_item(
            group.id,
            removed.id,
            group_kind="stack",
            group_title="2 items",
            remaining_item_ids=[item.id for item in remaining],
        )

        restored = self.storage.get_group(group.id)
        self.assertIsNotNone(restored)
        self.assertEqual(restored.title, "2 items")
        self.assertEqual([item.id for item in restored.items], [item.id for item in remaining])
        self.assertEqual([item.position for item in restored.items], [0, 1])


if __name__ == "__main__":
    unittest.main()
