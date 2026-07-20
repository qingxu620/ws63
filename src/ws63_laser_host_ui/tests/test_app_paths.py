from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from app.config_store import ConfigStore, HostConfig
from app.paths import (
    APP_DATA_ENV,
    config_file_path,
    generated_images_dir,
    session_log_dir,
    user_data_dir,
)


class AppPathTests(unittest.TestCase):
    def test_override_controls_all_writable_paths(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            with patch.dict(os.environ, {APP_DATA_ENV: str(root)}):
                self.assertEqual(user_data_dir(), root)
                self.assertEqual(
                    config_file_path(),
                    root / "config" / "host_ui_config.json",
                )
                self.assertEqual(session_log_dir(), root / "logs")
                self.assertEqual(
                    generated_images_dir(),
                    root / "generated_images",
                )

    def test_config_store_atomically_saves_to_explicit_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "nested" / "host_ui_config.json"
            store = ConfigStore(path)

            store.save(HostConfig(job_id=23))

            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["job_id"], 23)
            self.assertFalse(path.with_suffix(".json.tmp").exists())

    def test_legacy_config_is_migrated_once(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            current = root / "current" / "host_ui_config.json"
            legacy = root / "legacy" / "host_ui_config.json"
            legacy.parent.mkdir(parents=True)
            legacy.write_text('{"job_id": 17, "focus_power": 7}', encoding="utf-8")

            config = ConfigStore(current, legacy_path=legacy).load()

            self.assertEqual(config.job_id, 17)
            self.assertEqual(config.focus_power, 7)
            self.assertTrue(current.exists())


if __name__ == "__main__":
    unittest.main()
