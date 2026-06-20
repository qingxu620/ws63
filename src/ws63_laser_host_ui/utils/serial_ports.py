from __future__ import annotations

from dataclasses import dataclass


try:
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - runtime dependency check
    list_ports = None


@dataclass(frozen=True, slots=True)
class SerialPortInfo:
    device: str
    description: str
    hwid: str

    @property
    def display_name(self) -> str:
        detail = self.description if self.description and self.description != "n/a" else self.hwid
        return f"{self.device} - {detail}" if detail else self.device


def list_serial_ports() -> list[SerialPortInfo]:
    if list_ports is None:
        return []

    ports: list[SerialPortInfo] = []
    for port in list_ports.comports():
        ports.append(
            SerialPortInfo(
                device=str(port.device or ""),
                description=str(port.description or ""),
                hwid=str(port.hwid or ""),
            )
        )
    return sorted(ports, key=lambda item: item.device)
