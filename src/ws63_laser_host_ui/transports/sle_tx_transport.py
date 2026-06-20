from __future__ import annotations

from dataclasses import dataclass

from .base import BaseTransport, TransportResult, TransportState
from .serial_transport import SerialTransport, SerialTransportConfig


@dataclass(slots=True)
class SleTxConfig:
    tx_cmd_port: str
    tx_log_port: str = ""
    rx_log_port: str = ""
    baudrate: int = 115200


class SleTxTransport(BaseTransport):
    def __init__(self, config: SleTxConfig) -> None:
        super().__init__("SLE via TX")
        self.config = config
        self.tx_cmd = SerialTransport(
            "TX command",
            SerialTransportConfig(port=config.tx_cmd_port, baudrate=config.baudrate),
        )

    def connect(self) -> TransportResult:
        self.state = TransportState.CONNECTING
        result = self.tx_cmd.connect()
        self.state = TransportState.CONNECTED if result.ok else TransportState.ERROR
        if not result.ok:
            return result
        return TransportResult(
            True,
            f"SLE via TX: command link ready on {self.config.tx_cmd_port} @ {self.config.baudrate}",
        )

    def disconnect(self) -> TransportResult:
        result = self.tx_cmd.disconnect()
        self.state = TransportState.DISCONNECTED
        return TransportResult(result.ok, "SLE via TX: disconnected")

    def send_line(self, line: str) -> TransportResult:
        return self.tx_cmd.send_line(line)
