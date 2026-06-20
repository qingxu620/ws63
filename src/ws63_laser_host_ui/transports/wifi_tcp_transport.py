from __future__ import annotations

import socket
from dataclasses import dataclass

from .base import BaseTransport, TransportResult, TransportState


@dataclass(slots=True)
class WifiTcpConfig:
    host: str = "192.168.43.1"
    port: int = 5000
    timeout_s: float = 2.0


class WifiTcpTransport(BaseTransport):
    def __init__(self, config: WifiTcpConfig) -> None:
        super().__init__("WiFi TCP")
        self.config = config
        self._socket: socket.socket | None = None

    def connect(self) -> TransportResult:
        self.state = TransportState.CONNECTING
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(self.config.timeout_s)
        try:
            sock.connect((self.config.host, self.config.port))
        except OSError as exc:  # pragma: no cover - network dependent
            sock.close()
            self.state = TransportState.ERROR
            return TransportResult(False, f"WiFi TCP: connect failed: {exc}")

        self._socket = sock
        self.state = TransportState.CONNECTED
        return TransportResult(True, f"WiFi TCP: connected {self.config.host}:{self.config.port}")

    def disconnect(self) -> TransportResult:
        if self._socket is not None:
            try:
                self._socket.close()
            finally:
                self._socket = None
        self.state = TransportState.DISCONNECTED
        return TransportResult(True, "WiFi TCP: disconnected")

    def send_line(self, line: str) -> TransportResult:
        if self._socket is None or self.state != TransportState.CONNECTED:
            return TransportResult(False, "WiFi TCP: not connected")
        data = (line.rstrip("\r\n") + "\n").encode("utf-8")
        try:
            self._socket.sendall(data)
        except OSError as exc:  # pragma: no cover - network dependent
            self.state = TransportState.ERROR
            return TransportResult(False, f"WiFi TCP: send failed: {exc}")
        return TransportResult(True, f"WiFi TCP: sent {len(data)} bytes")
