from __future__ import annotations

from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from ui.widgets.log_viewer import LogViewer


class LogsPage(QWidget):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(28, 24, 28, 28)
        layout.setSpacing(18)

        title = QLabel("Logs")
        title.setObjectName("pageTitle")
        caption = QLabel("Multi-channel runtime log viewer.")
        caption.setObjectName("pageCaption")
        caption.setWordWrap(True)

        self.viewer = LogViewer()
        layout.addWidget(title)
        layout.addWidget(caption)
        layout.addWidget(self.viewer, 1)

    def append_log(self, category: str, message: str) -> None:
        self.viewer.append_log(category, message)
