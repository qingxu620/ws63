from __future__ import annotations

from PySide6.QtCore import QObject, Signal


class EventBus(QObject):
    log_message = Signal(str, str)
    status_message = Signal(str)
    state_changed = Signal()

    def emit_log(self, category: str, message: str) -> None:
        self.log_message.emit(category, message)

    def emit_status(self, message: str) -> None:
        self.status_message.emit(message)
        self.log_message.emit("status", message)
