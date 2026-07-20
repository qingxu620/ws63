from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

from app.paths import config_file_path, legacy_config_file_path


@dataclass
class HostConfig:
    tx_cmd_port: str = ""
    tx_log_port: str = ""
    rx_log_port: str = ""
    baudrate: int = 115200
    default_mode: str = "SLE"
    timeout_cmd: float = 8.0
    timeout_upload: float = 20.0
    preroll_bytes: int = 4096
    job_id: int = 1
    focus_power: int = 10

    def __post_init__(self) -> None:
        self.focus_power = max(1, min(100, int(self.focus_power)))


class ConfigStore:
    def __init__(
        self,
        path: Path | None = None,
        legacy_path: Path | None = None,
    ) -> None:
        using_default_path = path is None
        self.path = Path(path) if path is not None else config_file_path()
        if legacy_path is not None:
            self.legacy_path: Path | None = Path(legacy_path)
        elif using_default_path:
            self.legacy_path = legacy_config_file_path()
        else:
            self.legacy_path = None

    def load(self) -> HostConfig:
        source_path = self.path
        migrating_legacy = False
        if not source_path.exists() and self.legacy_path is not None and self.legacy_path.exists():
            source_path = self.legacy_path
            migrating_legacy = True
        if not source_path.exists():
            return HostConfig()
        try:
            data = json.loads(source_path.read_text(encoding="utf-8"))
            config = HostConfig(**data)
        except Exception:
            return HostConfig()
        if migrating_legacy:
            try:
                self.save(config)
            except OSError:
                # A read-only or restricted profile must not prevent startup.
                pass
        return config

    def save(self, config: HostConfig) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = self.path.with_suffix(f"{self.path.suffix}.tmp")
        temp_path.write_text(
            f"{json.dumps(asdict(config), indent=2, ensure_ascii=False)}\n",
            encoding="utf-8",
        )
        temp_path.replace(self.path)
