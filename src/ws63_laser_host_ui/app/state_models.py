from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum


class ConnectionMode(str, Enum):
    SLE_VIA_TX = "SLE"
    WIFI_TCP = "WiFi"


class LinkState(str, Enum):
    DISCONNECTED = "DISCONNECTED"
    CONNECTING = "CONNECTING"
    CONNECTED = "CONNECTED"
    ERROR = "ERROR"


RX_STATE_NAMES = {
    0: "空闲",
    1: "数据传输中",
    2: "任务就绪",
    3: "正在执行",
    5: "已终止",
    6: "错误",
}


@dataclass
class AppState:
    mode: ConnectionMode = ConnectionMode.SLE_VIA_TX
    tx_state: LinkState = LinkState.DISCONNECTED
    rx_state: LinkState = LinkState.DISCONNECTED
    rx_state_code: int = -1
    prev_rx_state_code: int = -1
    rx_job_id: int = 0
    rx_received: int = 0
    rx_job_total: int = 0
    rx_cache_free: int = 0
    execution_expected: bool = False
    termination_requested: bool = False
    execution_complete: bool = False
    focus_active: bool = False
    gcode_path: str = ""
    gcode_bytes: bytes = field(default_factory=bytes)

    @property
    def rx_state_name(self) -> str:
        if self.rx_state_code < 0:
            return "待机"
        return RX_STATE_NAMES.get(self.rx_state_code, f"UNKNOWN({self.rx_state_code})")

    def as_labels(self) -> dict[str, str]:
        return {
            "mode": self.mode.value,
            "tx": self.tx_state.value,
            "rx": self.rx_state.value,
            "focus": "ON" if self.focus_active else "OFF",
        }
