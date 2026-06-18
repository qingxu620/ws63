"""AI 智能创作中枢入口。"""

from __future__ import annotations

import sys
from pathlib import Path

if __package__ in (None, ""):
    # 兼容直接执行 `python3 main.py` 的场景。
    current_dir = Path(__file__).resolve().parent
    parent_dir = current_dir.parent
    if str(parent_dir) not in sys.path:
        sys.path.insert(0, str(parent_dir))
    from ai_studio.main_window import MainWindow  # type: ignore
    from ai_studio.qt_compat import QtWidgets  # type: ignore
else:
    from .main_window import MainWindow
    from .qt_compat import QtWidgets


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
