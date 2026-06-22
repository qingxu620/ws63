from __future__ import annotations

from PySide6.QtCore import QObject, Signal


class EventBus(QObject):
    log_message = Signal(str, str)
    status_message = Signal(str)

    def emit_log(self, channel: str, message: str) -> None:
        self.log_message.emit(channel, message)

    def emit_status(self, message: str) -> None:
        self.status_message.emit(message)
