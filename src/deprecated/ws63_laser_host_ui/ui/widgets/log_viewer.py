from __future__ import annotations

from datetime import datetime

from PySide6.QtWidgets import (
    QCheckBox,
    QFrame,
    QHBoxLayout,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)


LOG_CATEGORIES = ("host", "tx_log", "rx_log", "status", "error", "job")


class LogViewer(QWidget):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.category_checks: dict[str, QCheckBox] = {}

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(12)

        toolbar = QFrame()
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        toolbar_layout.setSpacing(10)

        for category in LOG_CATEGORIES:
            check = QCheckBox(category)
            check.setChecked(True)
            self.category_checks[category] = check
            toolbar_layout.addWidget(check)

        toolbar_layout.addStretch(1)
        self.pause_scroll_check = QCheckBox("Pause auto-scroll")
        self.clear_button = QPushButton("Clear")
        toolbar_layout.addWidget(self.pause_scroll_check)
        toolbar_layout.addWidget(self.clear_button)

        self.text = QPlainTextEdit()
        self.text.setReadOnly(True)
        self.text.setObjectName("logView")
        self.text.setMaximumBlockCount(8000)

        layout.addWidget(toolbar)
        layout.addWidget(self.text, 1)
        self.clear_button.clicked.connect(self.text.clear)

    def append_log(self, category: str, message: str) -> None:
        if category not in LOG_CATEGORIES:
            category = "host"
        if not self.category_checks[category].isChecked():
            return
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.text.appendPlainText(f"{stamp} {category.upper():7} {message}")
        if not self.pause_scroll_check.isChecked():
            scrollbar = self.text.verticalScrollBar()
            scrollbar.setValue(scrollbar.maximum())
