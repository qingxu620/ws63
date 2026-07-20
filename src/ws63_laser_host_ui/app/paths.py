"""Application-owned paths for source and frozen Windows builds."""
from __future__ import annotations

import os
import sys
from pathlib import Path


APP_NAME = "WS63 Laser Host"
APP_VENDOR = "fbb_ws63"
APP_DATA_ENV = "WS63_LASER_HOST_DATA_DIR"


def source_root() -> Path:
    """Return the project root when running from source."""
    return Path(__file__).resolve().parent.parent


def resource_path(*parts: str) -> Path:
    """Resolve a read-only asset in source or a PyInstaller bundle."""
    bundle_root = getattr(sys, "_MEIPASS", None)
    root = Path(bundle_root) if bundle_root else source_root()
    return root.joinpath(*parts)


def user_data_dir() -> Path:
    """Return the writable per-user application data directory."""
    override = os.environ.get(APP_DATA_ENV, "").strip()
    if override:
        return Path(override).expanduser()

    if os.name == "nt":
        local_app_data = os.environ.get("LOCALAPPDATA", "").strip()
        base = Path(local_app_data) if local_app_data else Path.home() / "AppData" / "Local"
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        xdg_data_home = os.environ.get("XDG_DATA_HOME", "").strip()
        base = Path(xdg_data_home).expanduser() if xdg_data_home else Path.home() / ".local" / "share"

    return base / APP_VENDOR / APP_NAME


def config_file_path() -> Path:
    return user_data_dir() / "config" / "host_ui_config.json"


def session_log_dir() -> Path:
    return user_data_dir() / "logs"


def generated_images_dir() -> Path:
    return user_data_dir() / "generated_images"


def self_test_report_path() -> Path:
    return user_data_dir() / "self_test_report.json"


def legacy_config_file_path() -> Path:
    """Return the pre-packaging source-relative config location."""
    return source_root() / "config" / "host_ui_config.json"


def application_icon_path() -> Path:
    return resource_path("assets", "app_icon.ico")

