"""Packaged startup and serial-backend smoke test."""
from __future__ import annotations

import argparse
import json
import platform
import ssl
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"{json.dumps(report, indent=2, ensure_ascii=False)}\n",
        encoding="utf-8",
    )


def run_self_test(argv: list[str]) -> int:
    from PySide6 import __version__ as pyside_version
    from PySide6.QtWidgets import QApplication
    import requests
    import serial

    from app.paths import (
        config_file_path,
        generated_images_dir,
        self_test_report_path,
        session_log_dir,
        user_data_dir,
    )
    from transports.sle_tx_transport import available_ports
    from ui.main_window import MainWindow

    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--self-test-report", type=Path, default=self_test_report_path())
    args, _ = parser.parse_known_args(argv)
    report_path = args.self_test_report.expanduser().resolve()

    report: dict[str, Any] = {
        "status": "error",
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "executable": sys.executable,
        "frozen": bool(getattr(sys, "frozen", False)),
        "app_data_dir": str(user_data_dir()),
        "config_path": str(config_file_path()),
        "log_dir": str(session_log_dir()),
        "generated_images_dir": str(generated_images_dir()),
        "pyside6": pyside_version,
        "pyserial": serial.VERSION,
        "requests": requests.__version__,
    }

    try:
        for directory in (user_data_dir(), session_log_dir(), generated_images_dir()):
            directory.mkdir(parents=True, exist_ok=True)

        ca_bundle = Path(requests.certs.where())
        if not ca_bundle.is_file():
            raise RuntimeError(f"requests CA bundle is missing: {ca_bundle}")
        ssl.create_default_context(cafile=str(ca_bundle))
        report["https_ca_bundle"] = str(ca_bundle)
        report["https_backend"] = "ok"

        app = QApplication.instance() or QApplication([])
        app.setApplicationName("WS63 Laser Host")
        app.setOrganizationName("fbb_ws63")
        window = MainWindow()
        report["ui_page_count"] = window.pages.count()
        report["serial_ports"] = available_ports()
        report["serial_enumeration"] = "ok"
        window.close()
        app.processEvents()

        if report["ui_page_count"] != 4:
            raise RuntimeError(
                f"unexpected page count: {report['ui_page_count']} (expected 4)"
            )
        report["status"] = "ok"
        _write_report(report_path, report)
        return 0
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        try:
            _write_report(report_path, report)
        except OSError:
            pass
        return 1
