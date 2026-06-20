from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from enum import Enum


class TransportState(str, Enum):
    DISCONNECTED = "DISCONNECTED"
    CONNECTING = "CONNECTING"
    CONNECTED = "CONNECTED"
    ERROR = "ERROR"


@dataclass(slots=True)
class TransportResult:
    ok: bool
    message: str


class BaseTransport(ABC):
    def __init__(self, name: str) -> None:
        self.name = name
        self.state = TransportState.DISCONNECTED

    @abstractmethod
    def connect(self) -> TransportResult:
        raise NotImplementedError

    @abstractmethod
    def disconnect(self) -> TransportResult:
        raise NotImplementedError

    def send_line(self, line: str) -> TransportResult:
        return TransportResult(False, f"{self.name}: send_line is not implemented yet: {line!r}")
