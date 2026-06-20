from __future__ import annotations

from PySide6.QtWidgets import QLabel


class StatusBadge(QLabel):
    def __init__(self, label: str, value: str, parent=None) -> None:
        super().__init__(parent)
        self.label = label
        self.setProperty("badge", True)
        self.set_value(value)

    def set_value(self, value: str) -> None:
        self.setText(f"{self.label}: {value}")
