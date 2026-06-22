from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QPushButton, QVBoxLayout, QWidget


class SidebarNav(QWidget):
    """Vertical navigation sidebar with icon-style buttons."""

    page_changed = Signal(int)

    _ITEMS = [
        ("⌁   连接与设置", 0),
        ("▣   代码编辑", 1),
        ("▶   任务控制", 2),
        ("▤   运行日志", 3),
    ]

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("sidebar")
        self.setFixedWidth(176)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 18, 12, 12)
        layout.setSpacing(6)

        self._buttons: list[QPushButton] = []
        for text, idx in self._ITEMS:
            btn = QPushButton(text)
            btn.setObjectName("sidebarBtn")
            btn.setCheckable(True)
            btn.setCursor(self.cursor())
            btn.clicked.connect(lambda checked, i=idx: self._on_click(i))
            layout.addWidget(btn)
            self._buttons.append(btn)

        layout.addStretch(1)

        # Brand
        brand = QPushButton("WS63 激光主控 V2")
        brand.setObjectName("sidebarBrand")
        brand.setEnabled(False)
        layout.addWidget(brand)

        self.set_current(0)

    def set_current(self, index: int) -> None:
        for i, btn in enumerate(self._buttons):
            btn.setChecked(i == index)
        self.page_changed.emit(index)

    def _on_click(self, index: int) -> None:
        self.set_current(index)
