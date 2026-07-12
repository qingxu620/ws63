from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path


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
    def __init__(self, path: Path | None = None) -> None:
        if path is None:
            path = Path(__file__).resolve().parent.parent / "config" / "host_ui_config.json"
        self.path = path

    def load(self) -> HostConfig:
        if not self.path.exists():
            return HostConfig()
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            return HostConfig(**data)
        except Exception:
            return HostConfig()

    def save(self, config: HostConfig) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(asdict(config), indent=2, ensure_ascii=False), encoding="utf-8")
