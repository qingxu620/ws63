from __future__ import annotations

import os
import tempfile
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication

from app.doubao_image_service import DoubaoImageService
from workers.doubao_image_worker import DoubaoImageWorker


class DoubaoImageWorkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def test_cancel_aborts_active_request_and_finishes_immediately(self) -> None:
        service = DoubaoImageService(
            endpoint="http://127.0.0.1:9/slow-generation",
            timeout_s=60.0,
        )
        cancelled: list[bool] = []
        finished: list[bool] = []
        errors: list[str] = []
        with tempfile.TemporaryDirectory() as output_dir:
            worker = DoubaoImageWorker(
                "test prompt",
                "2k",
                False,
                output_dir,
                service=service,
            )
            worker.cancelled.connect(lambda: cancelled.append(True))
            worker.finished.connect(lambda: finished.append(True))
            worker.error.connect(errors.append)
            with patch.dict(os.environ, {"ARK_API_KEY": "test-key"}):
                worker.start()
                self.assertTrue(worker.isRunning())
                worker.cancel()

            self.assertFalse(worker.isRunning())
            self.assertEqual(cancelled, [True])
            self.assertEqual(finished, [True])
            self.assertEqual(errors, [])
            self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
