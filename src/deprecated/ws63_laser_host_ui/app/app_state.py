from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class ConnectionMode(str, Enum):
    USART_DIRECT = "USART"
    SLE_VIA_TX = "SLE"
    WIFI_TCP = "WiFi"


class LinkState(str, Enum):
    DISCONNECTED = "DISCONNECTED"
    CONNECTING = "CONNECTING"
    CONNECTED = "CONNECTED"
    ERROR = "ERROR"


class SleState(str, Enum):
    IDLE = "IDLE"
    CONNECTING = "CONNECTING"
    CONNECTED = "CONNECTED"
    STREAMING = "STREAMING"
    ERROR = "ERROR"


class LaserState(str, Enum):
    OFF = "OFF"
    ON = "ON"
    FAULT = "FAULT"


@dataclass(slots=True)
class AppState:
    mode: ConnectionMode = ConnectionMode.SLE_VIA_TX
    rx_state: LinkState = LinkState.DISCONNECTED
    tx_state: LinkState = LinkState.DISCONNECTED
    sle_state: SleState = SleState.IDLE
    laser_state: LaserState = LaserState.OFF

    def as_labels(self) -> dict[str, str]:
        return {
            "mode": self.mode.value,
            "rx": self.rx_state.value,
            "tx": self.tx_state.value,
            "sle": self.sle_state.value,
            "laser": self.laser_state.value,
        }
