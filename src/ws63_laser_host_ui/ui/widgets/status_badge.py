from __future__ import annotations

from PySide6.QtWidgets import QHBoxLayout, QLabel, QWidget


class StatusBadge(QWidget):
    """Compact status indicator: label + value capsule."""

    _COLORS = {
        "default": ("#475569", "#F1F5F9", "#E2E8F0"),
        "ok": ("#047857", "rgba(16, 185, 129, 0.08)", "rgba(16, 185, 129, 0.2)"),
        "warn": ("#B45309", "rgba(245, 158, 11, 0.08)", "rgba(245, 158, 11, 0.2)"),
        "error": ("#B91C1C", "rgba(239, 68, 68, 0.08)", "rgba(239, 68, 68, 0.2)"),
        "info": ("#0369A1", "rgba(37, 99, 235, 0.05)", "rgba(37, 99, 235, 0.2)"),
    }

    def __init__(self, label: str, value: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self._label = QLabel(label)
        self._label.setObjectName("badgeLabel")
        self._value = QLabel(value)
        self._value.setObjectName("badgeValue")

        layout.addWidget(self._label)
        layout.addWidget(self._value)
        self._apply_style("default")

    def set_value(self, value: str, tone: str = "default") -> None:
        self._value.setText(value)
        self._apply_style(tone)

    def _apply_style(self, tone: str) -> None:
        fg, bg, border = self._COLORS.get(tone, self._COLORS["default"])
        self.setStyleSheet(f"""
            QLabel#badgeLabel {{
                color: #475569; 
                background: #F1F5F9;
                border: 1px solid #E2E8F0; 
                border-right: none;
                border-radius: 6px 0 0 6px;
                padding: 4px 10px; 
                font-size: 11px; 
                font-weight: 600;
                letter-spacing: 0.5px;
            }}
            QLabel#badgeValue {{
                color: {fg}; 
                background: {bg};
                border: 1px solid {border};
                border-left: 1px solid #E2E8F0;
                border-radius: 0 6px 6px 0;
                padding: 4px 12px; 
                font-size: 11px; 
                font-weight: 700;
            }}
        """)
