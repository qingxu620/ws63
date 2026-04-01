"""
Qt 兼容层。

优先使用 PySide6；如果本机没有安装，则回退到 PyQt5。
这样可以降低环境依赖冲突，便于比赛现场快速切换。
"""

from __future__ import annotations

try:
    from PySide6 import QtCore, QtGui, QtWidgets
    Signal = QtCore.Signal
    Slot = QtCore.Slot
    QT_API = "PySide6"
except ImportError:  # pragma: no cover - 运行环境相关
    from PyQt5 import QtCore, QtGui, QtWidgets
    Signal = QtCore.pyqtSignal
    Slot = QtCore.pyqtSlot
    QT_API = "PyQt5"

__all__ = [
    "QtCore",
    "QtGui",
    "QtWidgets",
    "Signal",
    "Slot",
    "QT_API",
]

