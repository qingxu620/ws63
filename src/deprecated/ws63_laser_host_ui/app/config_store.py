from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass(slots=True)
class SerialRoleConfig:
    rx_direct_port: str = ""
    tx_cmd_port: str = ""
    tx_log_port: str = ""
    rx_log_port: str = ""
    baudrate: int = 115200


@dataclass(slots=True)
class WifiConfig:
    ip: str = "192.168.43.1"
    port: int = 5000


@dataclass(slots=True)
class HostConfig:
    default_mode: str = "SLE via TX"
    serial: SerialRoleConfig = field(default_factory=SerialRoleConfig)
    wifi: WifiConfig = field(default_factory=WifiConfig)


class ConfigStore:
    def __init__(self, path: Path | None = None) -> None:
        project_dir = Path(__file__).resolve().parents[1]
        self.path = path or (project_dir / "config" / "host_ui_config.json")

    def load(self) -> HostConfig:
        if not self.path.exists():
            return HostConfig()

        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return HostConfig()

        return self._from_dict(data)

    def save(self, config: HostConfig) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(
            json.dumps(asdict(config), indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )

    def _from_dict(self, data: dict[str, Any]) -> HostConfig:
        serial_data = data.get("serial") if isinstance(data.get("serial"), dict) else {}
        wifi_data = data.get("wifi") if isinstance(data.get("wifi"), dict) else {}

        serial = SerialRoleConfig(
            rx_direct_port=str(serial_data.get("rx_direct_port", serial_data.get("rx_port", ""))),
            tx_cmd_port=str(serial_data.get("tx_cmd_port", "")),
            tx_log_port=str(serial_data.get("tx_log_port", "")),
            rx_log_port=str(serial_data.get("rx_log_port", "")),
            baudrate=self._safe_int(serial_data.get("baudrate"), 115200),
        )
        wifi = WifiConfig(
            ip=str(wifi_data.get("ip", "192.168.43.1")),
            port=self._safe_int(wifi_data.get("port"), 5000),
        )
        return HostConfig(
            default_mode=str(data.get("default_mode", "SLE via TX")),
            serial=serial,
            wifi=wifi,
        )

    @staticmethod
    def _safe_int(value: Any, default: int) -> int:
        try:
            return int(value)
        except (TypeError, ValueError):
            return default
