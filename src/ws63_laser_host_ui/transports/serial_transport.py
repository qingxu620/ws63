from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .base import BaseTransport, TransportResult, TransportState


try:
    import serial
except ImportError:  # pragma: no cover - runtime dependency check
    serial = None


@dataclass(slots=True)
class SerialTransportConfig:
    port: str
    baudrate: int = 115200
    timeout_s: float = 0.2


class SerialTransport(BaseTransport):
    def __init__(self, name: str, config: SerialTransportConfig) -> None:
        super().__init__(name)
        self.config = config
        self._serial: Any | None = None

    def connect(self) -> TransportResult:
        try:
            self.open()
        except SerialTransportError as exc:
            self.state = TransportState.ERROR
            return TransportResult(False, str(exc))
        self.state = TransportState.CONNECTED
        return TransportResult(True, f"{self.name}: connected {self.config.port} @ {self.config.baudrate}")

    def disconnect(self) -> TransportResult:
        self.close()
        return TransportResult(True, f"{self.name}: disconnected")

    def open(self) -> None:
        if serial is None:
            self.state = TransportState.ERROR
            raise SerialTransportError("pyserial is not installed")
        if not self.config.port:
            self.state = TransportState.ERROR
            raise SerialTransportError(f"{self.name}: no serial port selected")

        self.state = TransportState.CONNECTING
        try:
            self._serial = serial.Serial(
                self.config.port,
                self.config.baudrate,
                timeout=self.config.timeout_s,
                write_timeout=self.config.timeout_s,
            )
        except Exception as exc:  # pragma: no cover - hardware dependent
            self.state = TransportState.ERROR
            raise SerialTransportError(f"{self.name}: connect failed: {exc}") from exc

        self.state = TransportState.CONNECTED

    def close(self) -> None:
        if self._serial is not None:
            try:
                self._serial.close()
            finally:
                self._serial = None
        self.state = TransportState.DISCONNECTED

    def send_line(self, line: str) -> TransportResult:
        try:
            self.write_line(line)
        except SerialTransportError as exc:
            return TransportResult(False, str(exc))
        return TransportResult(True, f"{self.name}: sent line")

    def write_line(self, line: str) -> None:
        if self._serial is None or self.state != TransportState.CONNECTED:
            raise SerialTransportError(f"{self.name}: not connected")
        data = (line.rstrip("\r\n") + "\n").encode("utf-8")
        try:
            self._serial.write(data)
        except Exception as exc:  # pragma: no cover - hardware dependent
            self.state = TransportState.ERROR
            raise SerialTransportError(f"{self.name}: write failed: {exc}") from exc


class SerialTransportError(RuntimeError):
    pass
